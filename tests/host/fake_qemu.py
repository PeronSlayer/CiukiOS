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
scenario_path=Path(os.environ['CIUKI_FAKE_SCRIPT'])
scenario=json.loads(scenario_path.read_text())
boot=0
if scenario.get('boot_records'):
    counter=scenario_path.with_suffix('.count')
    boot=int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(boot+1))
    scenario['records']=scenario['boot_records'][boot]
gates=scenario.get('boot_gates',[[]]*(boot+1))[boot]
gate_index=0;gate_armed=False;gate_suspended=False;crash_arm_emitted=False;setup_writes=0
selector=sys.argv[sys.argv.index('-fw_cfg')+1].split('string=',1)[1]
run_id=selector.split('run=')[1][:8];probe=selector.split(':')[1].split()[0]
Path('fake-arguments.json').write_text(json.dumps(sys.argv[1:]))
commands=open('commands.fifo','rb',buffering=0)
responses=open('responses.fifo','wb',buffering=0)
class Connection:
    def sendall(self,data):responses.write(data)
    def makefile(self,mode):return commands
conn=Connection();Path('q').touch()
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

def suspend_gate():
    global gate_armed,gate_suspended
    assert gate_armed and not gate_suspended
    gate_armed=False;gate_suspended=True
    print("blkdebug: Suspended request 'ciuki-write'",flush=True)

for raw in stream:
    request=json.loads(raw);cmd=request['execute'];reply={}
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
        screen=scenario.get('desktop_screens',{}).get('after' if scenario.get('received_input') else 'before')
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
    if cmd=='human-monitor-command' and gates:
        if text=='resume ciuki-write' and gate_index<len(gates):emit(gates[gate_index])
        if gate_index<len(gates) and not gate_suspended:suspend_gate()
    if cmd=='input-send-event':
        count=scenario.setdefault('received_input',0)+1;scenario['received_input']=count
        if count==scenario.get('finish_after_input'):
            emit(scenario.get('after_input',[]))
            if scenario.get('delayed_terminal'):
                def finish():
                    time.sleep(.5);emit(scenario['delayed_terminal'])
                threading.Thread(target=finish,daemon=True).start()
    if cmd=='quit':Path('fake-stopped').touch()
    if scenario.get('reset') and cmd=='query-status':conn.sendall(('{"timestamp":{"seconds":%d,"microseconds":0},"event":"RESET","data":{"guest":true,"reason":"guest-reset"}}\n'%int(time.time())).encode())
if scenario.get('child'):
    try:child.wait(timeout=.1)
    except subprocess.TimeoutExpired:pass
