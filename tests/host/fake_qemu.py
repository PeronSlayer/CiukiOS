#!/usr/bin/env python3
"""Scripted QEMU boundary for host tests; never executes an emulator."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import threading

if '--version' in sys.argv:
    print('QEMU emulator version 11.0.0 (scripted host fixture)');sys.exit(0)
scenario=json.loads(Path(os.environ['CIUKI_FAKE_SCRIPT']).read_text())
selector=sys.argv[sys.argv.index('-fw_cfg')+1].split('string=',1)[1]
run_id=selector.split('run=')[1][:8];probe=selector.split(':')[1].split()[0]
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

def emit():
    global serial,seq
    if serial is None:serial=open('serial.fifo','wb',buffering=0)
    for item in scenario.get('records',[]):
        seq+=1
        if isinstance(item,str):line=item.replace('{run_id}',run_id).replace('{probe}',probe)
        else:
            record={'v':'1','run':run_id,'seq':f'{seq:06d}','probe':probe,**item}
            line='CIUKI_TEST '+' '.join(f'{k}={v}' for k,v in record.items())
        serial.write(line.encode()+b'\n')
    if scenario.get('flood'):serial.write(b'x'*(4*1024*1024+65536))

stream=conn.makefile('rb');started=False
for raw in stream:
    request=json.loads(raw);cmd=request['execute'];reply={}
    if cmd=='query-status':reply={'status':'running','running':True}
    elif cmd=='query-blockstats':reply=[{'device':'ide0','stats':{'wr_bytes':scenario.get('writes',0) if started else 0,'wr_operations':0,'flush_operations':0}}]
    elif cmd=='quit' and not scenario.get('ignore_quit'):
        conn.sendall(json.dumps({'return':{},'id':request['id']}).encode()+b'\n');break
    elif cmd=='screendump':Path(request['arguments']['filename']).write_bytes(b'P6\n1 1\n255\n\x00\x00\x00')
    conn.sendall(json.dumps({'return':reply,'id':request['id']}).encode()+b'\n')
    if cmd in ('cont','system_reset'):
        if cmd=='system_reset':
            seq=0;conn.sendall(('{"timestamp":{"seconds":%d,"microseconds":0},"event":"RESET","data":{"guest":false,"reason":"host-qmp-system-reset"}}\n'%int(time.time())).encode())
            # Real firmware answers a host reset with one hard reboot of its own.
            conn.sendall(('{"timestamp":{"seconds":%d,"microseconds":500000},"event":"RESET","data":{"guest":true,"reason":"guest-reset"}}\n'%int(time.time())).encode())
        started=True
        if scenario.get('flood'):threading.Thread(target=emit,daemon=True).start()
        else:emit()
    if scenario.get('reset') and cmd=='query-status':conn.sendall(('{"timestamp":{"seconds":%d,"microseconds":0},"event":"RESET","data":{"guest":true,"reason":"guest-reset"}}\n'%int(time.time())).encode())
if scenario.get('child'):
    try:child.wait(timeout=.1)
    except subprocess.TimeoutExpired:pass
