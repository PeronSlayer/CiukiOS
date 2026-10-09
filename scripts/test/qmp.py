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
        if self.log.tell()+len(line)+1>LOG_CAP: raise RuntimeError('QMP log exceeds cap')
        self.log.write(line+b'\n');self.log.flush()
        result=json.loads(line)
        if 'event' in result: self.events.append(result)
        return result

    def command(self,name,arguments=None):
        self.counter+=1;identifier=self.counter
        data={'execute':name,'id':identifier}
        if arguments is not None:data['arguments']=arguments
        self.socket.sendall(json.dumps(data).encode()+b'\n')
        deadline=time.monotonic()+1
        while time.monotonic()<deadline:
            result=self.read()
            if result.get('id')==identifier:
                if 'error' in result: raise RuntimeError('QMP command failed: '+str(result['error']))
                return result.get('return')
        raise RuntimeError('QMP response deadline')

    def close(self): self.socket.close()


def writes(stats):
    return {s.get('device',str(i)):{k:s['stats'].get(k,0) for k in
            ('rd_bytes','rd_operations','wr_bytes','wr_operations','flush_operations')} for i,s in enumerate(stats)}
