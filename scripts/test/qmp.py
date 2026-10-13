"""Small bounded synchronous QMP client; events are retained, never ignored."""
import json
import socket
import time
from resources import LOG_CAP


class QMP:
    def __init__(self,path,log,transport=None):
        self.socket=transport or socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        if transport is None:
            self.socket.settimeout(.3);self.socket.connect(str(path))
        self.buffer=b'';self.log=log;self.events=[];self.counter=0
        # Repeated identical query-status exchanges are not logged: the run loop
        # polls every 25 ms and 40k polls filled the 4 MiB log in a 17-minute
        # DOOM case (lead, 2026-10-12). The first reply and every change stay.
        self.deferred=None;self.deferred_id=None;self.last_status=None
        self.greeting=self.read()
        if 'QMP' not in self.greeting: raise RuntimeError('missing QMP greeting')
        self.command('qmp_capabilities')

    def read(self):
        while b'\n' not in self.buffer:
            chunk=self.socket.recv(4096)
            if not chunk: raise RuntimeError('QMP disconnected')
            self.buffer+=chunk
            if len(self.buffer)>LOG_CAP: raise RuntimeError('QMP frame exceeds cap')
        line,self.buffer=self.buffer.split(b'\n',1)
        result=json.loads(line)
        if self.deferred is not None and result.get('id')==self.deferred_id:
            same=result.get('return')==self.last_status and 'error' not in result
            self.last_status=result.get('return')
            payload=self.deferred;self.deferred=None;self.deferred_id=None
            if same: return result
            if self.log.tell()+len(payload)>LOG_CAP:raise RuntimeError('QMP log exceeds cap')
            self.log.write(payload)
        if self.log.tell()+len(line)+1>LOG_CAP: raise RuntimeError('QMP log exceeds cap')
        self.log.write(line+b'\n');self.log.flush()
        if 'event' in result: self.events.append(result)
        return result

    def command(self,name,arguments=None):
        self.counter+=1;identifier=self.counter
        data={'execute':name,'id':identifier}
        if arguments is not None:data['arguments']=arguments
        payload=json.dumps(data).encode()+b'\n'
        if name=='query-status' and arguments is None:
            self.deferred=payload;self.deferred_id=identifier
        else:
            if self.log.tell()+len(payload)>LOG_CAP:raise RuntimeError('QMP log exceeds cap')
            self.log.write(payload);self.log.flush()
        self.socket.sendall(payload)
        # A busy TCG guest (DOOM at full CPU under a loaded host) answers QMP
        # slower than the 0.3 s socket timeout: wait for the reply up to 10 s
        # (lead fix 2026-10-12: doom-qemu-t23 ended 'timed out' in loop 2).
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try:result=self.read()
            except socket.timeout:continue
            if result.get('id')==identifier:
                if 'error' in result: raise RuntimeError('QMP command failed: '+str(result['error']))
                return result.get('return')
        # TimeoutError: run.py classifies it as a case timeout (timeout.occurred),
        # like a socket timeout; a RuntimeError here bypassed that classification
        # (runner test test_timeout_identifies_pending_cut_and_arm, qmp phase).
        raise TimeoutError('QMP response deadline')

    def input_events(self,events):
        """QAPI InputEvent: qcode keys, relative motion and buttons (PS/2 or USB)."""
        if not isinstance(events,list) or not 1<=len(events)<=64:
            raise ValueError('input batch must contain 1..64 events')
        for event in events:
            kind=event.get('type');data=event.get('data',{})
            if kind=='key':
                key=data.get('key',{})
                valid=(type(data.get('down')) is bool and key.get('type')=='qcode'
                       and isinstance(key.get('data'),str) and key['data'].isascii())
            elif kind=='rel':
                valid=data.get('axis') in ('x','y') and type(data.get('value')) is int and abs(data['value'])<=32767
            elif kind=='btn':
                valid=data.get('button') in ('left','middle','right') and type(data.get('down')) is bool
            else:valid=False
            if not valid:raise ValueError('invalid input-send-event batch')
        return self.command('input-send-event',{'events':events})

    def close(self): self.socket.close()


def writes(stats):
    return {s.get('device',str(i)):{k:s['stats'].get(k,0) for k in
            ('rd_bytes','rd_operations','wr_bytes','wr_operations','flush_operations')} for i,s in enumerate(stats)}
