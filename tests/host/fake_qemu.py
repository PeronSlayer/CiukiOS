#!/usr/bin/env python3
"""Scripted QEMU boundary for host tests; never executes an emulator."""
import json
import hashlib
import os
from pathlib import Path
import signal
import shlex
import socket
import subprocess
import sys
import time
import threading

if '--version' in sys.argv:
    print('QEMU emulator version 11.0.0 (scripted host fixture)');sys.exit(0)
if '-machine' in sys.argv and sys.argv[sys.argv.index('-machine')+1].endswith(',help'):
    print('  i8042=<bool> - Enable/disable Intel 8042 PS/2 controller emulation');sys.exit(0)
scenario_path=Path(os.environ['CIUKI_FAKE_SCRIPT'])
scenario=json.loads(scenario_path.read_text())
def exit_early():
    if scenario.get('exit_signal'):
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        os.kill(os.getpid(), getattr(signal, scenario['exit_signal']))
    os._exit(scenario.get('exit_code', 0))
if scenario.get('exit_on') == 'startup': exit_early()
boot=0
if scenario.get('boot_records'):
    counter=scenario_path.with_suffix('.count')
    boot=int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(boot+1))
    scenario['records']=scenario['boot_records'][boot]
gates=scenario.get('boot_gates',[[]]*(boot+1))[boot]
gate_index=0;gate_armed=False;gate_suspended=False;crash_arm_emitted=False;setup_writes=0
if '-fw_cfg' in sys.argv:
    selector=sys.argv[sys.argv.index('-fw_cfg')+1].split('string=',1)[1]
elif scenario.get('normal_boot'):
    # A normal boot carries no request at all: no fw_cfg item and the overlay
    # is never read for a BOOT.CFG probe line. Serial records are raw lines.
    assert not any('opt/it.alcybercloud.ciukios' in a for a in sys.argv)
    selector='f0:normal-boot run=00000000'
else:
    # The sweep fixture stores a tiny BOOT.CFG in the shared fake overlay.
    overlay=Path(sys.argv[sys.argv.index('-drive')+1].split(',format=',1)[0][5:])
    cfg=overlay.read_text()
    selector=next(line[6:] for line in cfg.splitlines() if line.startswith('probe='))
    assert '-no-reboot' in sys.argv and '-no-shutdown' not in sys.argv
run_id=selector.split('run=')[1][:8];probe=selector.split(':')[1].split()[0]
Path('fake-arguments.json').write_text(json.dumps(sys.argv[1:]))
commands=open('commands.fifo','rb',buffering=0)
responses=open('responses.fifo','wb',buffering=0)
Path('q').touch()
class Connection:
    def sendall(self,data):responses.write(data)
    def makefile(self,mode):return commands
conn=Connection()
conn.sendall(b'{"QMP":{"version":{"qemu":{"major":11,"minor":0,"micro":0}},"capabilities":[]}}\n')
if scenario.get('child'):
    child=subprocess.Popen([sys.executable,'-c','import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(60)'])
    Path('child.pid').write_text(str(child.pid))
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
serial=None
seq=0

def emit(items=None):
    global serial,seq,crash_arm_emitted
    if serial is None:serial=open('serial.fifo','wb',buffering=0)
    for item in (scenario.get('records',[]) if items is None else items):
        seq+=1
        if isinstance(item,str):line=item.replace('{run_id}',run_id).replace('{probe}',probe)
        else:
            record={'v':'1','run':run_id,'seq':f'{seq:06d}','probe':probe,**item}
            line='CIUKI_TEST '+' '.join(f'{k}={v}' for k,v in record.items())
            if item.get('event')=='ARM' and item.get('action')=='crash_cut':crash_arm_emitted=True
        serial.write(line.encode()+b'\n')
    if scenario.get('flood'):serial.write(b'x'*(4*1024*1024+65536))
    if scenario.get('application_bytes') and items is None:
        total=scenario['application_bytes'];digest=hashlib.sha256();offset=0
        pattern=b'CIUKI_TEST event=END status=PASS\n'
        while offset<total:
            data=pattern[:min(24,total-offset)]
            digest.update(data)
            seq+=1
            serial.write((f'CIUKI_TEST v=1 run={run_id} seq={seq:06d} probe={probe} event=DATA '
                          f'group=app pid=7 tid=9 stream=stdout offset={offset} bytes={len(data)} data_hex={data.hex()}\n').encode())
            offset+=len(data)
        emit([{'event':'DATA','group':'app_digest','total_bytes':str(total),'sha256':digest.hexdigest()},
              *scenario.get('after_application',[])])

stream=conn.makefile('rb');started=False
input_pending=None
key_counts={};qmp_counts={}

def emit_mixed(items):
    """fx-accept hooks: serial lines (strings or records), QMP events
    ({"qmp_event": {...}}) and pauses ({"delay": seconds}), in order, on a
    thread so a pause never blocks the QMP loop."""
    def run():
        for item in items:
            if isinstance(item,dict) and 'delay' in item:time.sleep(item['delay'])
            elif isinstance(item,dict) and 'qmp_event' in item:
                event={'timestamp':{'seconds':int(time.time()),'microseconds':0},**item['qmp_event']}
                conn.sendall(json.dumps(event).encode()+b'\n')
            else:emit([item])
    threading.Thread(target=run,daemon=True).start()
input_motion=scenario.get('input_motion',0);input_buttons=scenario.get('input_buttons',0)

def suspend_gate():
    global gate_armed,gate_suspended
    assert gate_armed and not gate_suspended
    gate_armed=False;gate_suspended=True
    print("blkdebug: Suspended request 'ciuki-write'",flush=True)

for raw in stream:
    request=json.loads(raw);cmd=request['execute'];reply={}
    if scenario.get('exit_on') == cmd: exit_early()
    if input_pending and time.monotonic()>=input_pending['due'] and cmd=='query-status':
        counters={'event':'DATA','case':'gate','motion':str(input_motion),'buttons':str(input_buttons)}
        emit([f'[input] gate motion={input_motion} buttons={input_buttons}' if scenario.get('raw_input_counters') else counters])
        input_pending=None
        if scenario.get('received_input')==scenario.get('finish_after_input'):
            emit(scenario.get('after_input',[]))
    with Path('fake-qmp.jsonl').open('a') as log:log.write(json.dumps(request)+'\n')
    if cmd=='query-status':reply={'status':'running','running':True}
    elif cmd=='stop' and gates:raise AssertionError('QMP stop drains suspended requests')
    elif cmd=='human-monitor-command':
        target,text=shlex.split(request['arguments']['command-line'])[1:];reply=''
        # A block node would create/drain a temporary backend in real HMP.
        assert target=='ciuki-cut-drive', 'gate must target the persistent drive backend'
        assert any('id=ciuki-cut-drive' in a for a in sys.argv), 'missing named drive backend'
        if text=='break pwritev ciuki-write':
            assert crash_arm_emitted, 'breakpoint must follow crash ARM and write-enable setup'
            assert not gate_armed, 'breakpoints are one-shot'
            gate_armed=True
            if scenario.get('gate_qmp_timeout'):continue
        elif text=='resume ciuki-write':
            assert gate_armed and gate_suspended, 'next breakpoint must precede resume'
            gate_suspended=False;gate_index+=1
        else:raise AssertionError(text)
    elif cmd=='query-blockstats':reply=[{'device':'ide0','stats':{'wr_bytes':scenario.get('writes',0)+setup_writes*512 if started else 0,'wr_operations':setup_writes,'flush_operations':0}}]
    elif cmd=='quit' and not scenario.get('ignore_quit'):
        Path('fake-stopped').touch()
        conn.sendall(json.dumps({'return':{},'id':request['id']}).encode()+b'\n');break
    elif cmd=='screendump':
        if scenario.get('screens'):
            # f5-17B win32 tiers: one canned frame per screendump, in order; the
            # last frame repeats (a stalled scene) once the list is exhausted.
            shots=scenario.setdefault('screendumps',0);scenario['screendumps']=shots+1
            screen=scenario['screens'][min(shots,len(scenario['screens'])-1)]
        else:
            screen=scenario.get('screen') or scenario.get('desktop_screens',{}).get('after' if scenario.get('received_input') else 'before')
        Path(request['arguments']['filename']).write_bytes(Path(screen).read_bytes() if screen else b'P6\n1 1\n255\n\x00\x00\x00')
    conn.sendall(json.dumps({'return':reply,'id':request['id']}).encode()+b'\n')
    if cmd in ('cont','system_reset'):
        if cmd=='system_reset':
            seq=0;conn.sendall(('{"timestamp":{"seconds":%d,"microseconds":0},"event":"RESET","data":{"guest":false,"reason":"host-qmp-system-reset"}}\n'%int(time.time())).encode())
            # Real firmware answers a host reset with one hard reboot of its own.
            conn.sendall(('{"timestamp":{"seconds":%d,"microseconds":500000},"event":"RESET","data":{"guest":true,"reason":"guest-reset"}}\n'%int(time.time())).encode())
        started=True
        if gates and gate_index==0 and not scenario.get('gate_started'):
            scenario['gate_started']=True
            assert not gate_armed, 'pre-ARM writes must run without a breakpoint'
            setup_writes=scenario.get('setup_writes',5)
            emit(gates[0])
            # Host ARM receipt can lag writes. Exercise this race explicitly;
            # completed traces precede the next pending request/suspension.
            for _ in range(scenario.get('gate_arm_lag_writes',0)):
                gate_index+=1;emit(gates[gate_index])
        elif gates:pass
        elif scenario.get('flood') or scenario.get('application_bytes'):threading.Thread(target=emit,daemon=True).start()
        else:emit()
        if scenario.get('delayed_records'):
            # Serial lines that arrive later in the boot (normal-boot settle window).
            def later():
                time.sleep(scenario.get('delay',.3));emit(scenario['delayed_records'])
            threading.Thread(target=later,daemon=True).start()
        if scenario.get('boot_reboots', [False]*(boot+1))[boot]:
            # Persist the guest cursor before -no-reboot ends this process.
            overlay.write_text('safe=0 serial=1\nprobe=f0:sweep run='+run_id+' step='+str(boot+1)+'\n')
            conn.sendall(b'{"event":"SHUTDOWN","data":{"guest":true,"reason":"guest-reset"}}\n')
            serial.close()
            sys.exit(scenario.get('boot_exit_codes',[0]*(boot+1))[boot])
    if cmd=='human-monitor-command' and gates:
        if text=='resume ciuki-write' and gate_index<len(gates):emit(gates[gate_index])
        if gate_index<len(gates) and not gate_suspended:suspend_gate()
    if cmd=='input-send-event':
        if scenario.get('consume_input'):
            assert input_pending is None, 'runner sent another batch before guest consumption'
        count=scenario.setdefault('received_input',0)+1;scenario['received_input']=count
        if count==scenario.get('input_baseline_after'):
            emit([dict(event='DATA',case='gate',server='desktop',keys='0',motion=str(input_motion),buttons=str(input_buttons))])
        events=request['arguments']['events']
        motion=sum(e['type']=='rel' and e['data']['value']!=0 for e in events)
        buttons=sum(e['type']=='btn' for e in events)
        if scenario.get('consume_input') and (motion or buttons):
            input_motion+=motion;input_buttons+=buttons
            due=float('inf') if count==scenario.get('stall_input') else time.monotonic()+scenario.get('input_ack_delay',.15)
            input_pending={'due':due}
        elif count==scenario.get('finish_after_input'):
            emit(scenario.get('after_input',[]))
            if scenario.get('delayed_terminal'):
                def finish():
                    time.sleep(.5);emit(scenario['delayed_terminal'])
                threading.Thread(target=finish,daemon=True).start()
        # f5-17B: records the guest emits once the Nth input batch arrived
        # (segment ends, next ARM, summaries), keyed by the decimal batch count.
        if str(count) in scenario.get('after_inputs',{}):
            emit(scenario['after_inputs'][str(count)])
    if cmd=='input-send-event':
        # fx-accept: lines/events after the Nth press of a key ("ret#2"), or
        # after every press of it ("ret").
        for event in request['arguments']['events']:
            if event['type']=='key' and event['data']['down']:
                code=event['data']['key']['data'];key_counts[code]=key_counts.get(code,0)+1
                hooks=scenario.get('on_key',{})
                hook=hooks.get('%s#%d'%(code,key_counts[code]),hooks.get(code))
                if hook:emit_mixed(hook)
    if cmd in ('blockdev-change-medium','eject'):
        qmp_counts[cmd]=qmp_counts.get(cmd,0)+1
        with Path('fake-media.jsonl').open('a') as log:log.write(json.dumps(request['arguments'])+'\n')
        hook=scenario.get('on_qmp',{}).get('%s#%d'%(cmd,qmp_counts[cmd]))
        if hook:emit_mixed(hook)
    if cmd=='quit':Path('fake-stopped').touch()
    if scenario.get('reset') and cmd=='query-status':conn.sendall(('{"timestamp":{"seconds":%d,"microseconds":0},"event":"RESET","data":{"guest":true,"reason":"guest-reset"}}\n'%int(time.time())).encode())
if scenario.get('child'):
    try:child.wait(timeout=.1)
    except subprocess.TimeoutExpired:pass
