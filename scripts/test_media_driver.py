#!/usr/bin/env python3
"""Execute production MEDIA.DRV FAR entry against independent FAT/ISO fixtures.

Models BIOS/DOS file calls, asserts ABI/register preservation and no console,
keyboard, video calls. This is CPU/reader validation, not hardware qualification.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from test_media_readers import Machine, fixtures
from unicorn.x86_const import *

SIZE = 1192
PATH = 20
DEST = 212
MESSAGE = 276
ROWS = 404
PREVIEW = 680

class Driver(Machine):
    def __init__(self, program, devices, **kwargs):
        super().__init__(b'', '', devices, **kwargs)
        self.uc.mem_write(self.start, program)
        self.program = program
        assert program[4:12] == b'CMEDIA01'
        self.packet = struct.unpack_from('<H', program, 20)[0]
        assert struct.unpack_from('<H', program, 18)[0] == SIZE

    def request(self, op, device, path='/', destination='', first=0, in_place=False, selection=0):
        p = bytearray(self.uc.mem_read(self.start+self.packet,SIZE)) if op in (5,6,7,8) else bytearray(SIZE)
        struct.pack_into('<HHHHH', p, 0, 1, op, 0xeeee, device, first)
        struct.pack_into('<H',p,16,selection)
        p[PATH:PATH+192] = bytes(192)
        p[DEST:DEST+64] = bytes(64)
        p[PATH:PATH+len(path)] = path.encode('ascii')
        p[DEST:DEST+len(destination)] = destination.encode('ascii')
        seg, off = (self.segment, self.packet) if in_place else (0x8000, 0x400)
        address = seg*16+off
        self.uc.mem_write(address, bytes(p))
        registers = {UC_X86_REG_EAX:0x12345678,UC_X86_REG_EBX:0x87654321,
                     UC_X86_REG_ECX:0x13572468,UC_X86_REG_EDX:0x24681357,
                     UC_X86_REG_ESI:0xabbacd00,UC_X86_REG_EDI:0xabab0000|off,
                     UC_X86_REG_EBP:0x77774444,UC_X86_REG_DS:0x7000,
                     UC_X86_REG_ES:seg,UC_X86_REG_FS:0x6000,UC_X86_REG_GS:0x5000,
                     UC_X86_REG_SS:0x9000,UC_X86_REG_SP:0xfff0,UC_X86_REG_EFLAGS:0x202}
        for register, value in registers.items(): self.uc.reg_write(register,value)
        self.uc.reg_write(UC_X86_REG_CS,self.segment)
        self.uc.mem_write(0x9fff0,struct.pack('<HH',0x100,0x8000))
        self.uc.emu_start(self.start,0x80100,count=20000000)
        assert self.uc.reg_read(UC_X86_REG_CS)==0x8000 and self.uc.reg_read(UC_X86_REG_IP)==0x100,'module did not return'
        for register,value in registers.items():
            expected=value+4 if register==UC_X86_REG_SP else value
            assert self.uc.reg_read(register)==expected,(register,self.uc.reg_read(register),expected)
        assert not self.output,'native media wrote to DOS console'
        assert not self.handles,'native media leaked file handles'
        result=bytes(self.uc.mem_read(address,SIZE))
        values=struct.unpack_from('<10H',result)
        rows=[]
        for i in range(values[5]):
            row=result[ROWS+i*46:ROWS+(i+1)*46]
            rows.append({'name':row[:40].split(b'\0')[0].decode(),'size':struct.unpack_from('<I',row,40)[0],'directory':row[44]})
        return dict(status=values[2],count=values[5],total=values[6],more=values[8],drive=values[9],
                    message=result[MESSAGE:MESSAGE+128].split(b'\0')[0].decode(),rows=rows,
                    preview=result[PREVIEW:PREVIEW+values[7]])

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--program',type=Path,default=Path('build/full/native-media-2026-09-26/obj/media.drv'))
    ap.add_argument('--output',type=Path,default=Path('build/full/native-media-2026-09-26/driver-tests'))
    args=ap.parse_args(); args.output.mkdir(parents=True,exist_ok=True)
    paths,payload,binary=fixtures(args.output/'fixtures'); program=args.program.read_bytes(); checks=[]
    for kind,path in paths.items():
        optical=kind=='iso'; drive=0xe0 if optical else 0 if kind=='fat12' else 0x81
        device=3 if optical else 1 if drive==0 else 2
        vm=Driver(program,{drive:(path.read_bytes(),2048 if optical else 512,drive!=0)},dma_boundary=True,segment=0x3f00)
        result=vm.request(1,device,in_place=True)
        assert result['status']==0 and {row['name'] for row in result['rows']}=={'NESTED','PAYLOAD.BIN'},result
        index=[row['name'] for row in result['rows']].index('NESTED')
        result=vm.request(5,device,selection=index)
        assert result['status']==0 and result['rows'][0]['name']=='DEEP',result
        result=vm.request(5,device)
        assert result['status']==0 and result['rows'][0]['name']=='README.TXT',result
        result=vm.request(5,device)
        assert result['status']==0 and result['preview']==payload[:511],result
        result=vm.request(8,device)
        assert result['status']==0 and result['rows'][0]['name']=='README.TXT',result
        result=vm.request(7,device)
        assert result['status']==0 and result['rows'][0]['name']=='DEEP',result
        result=vm.request(7,device)
        assert result['status']==0 and result['total']==2,result
        index=[row['name'] for row in result['rows']].index('PAYLOAD.BIN')
        result=vm.request(6,device,destination='C:\\SELECTED.BIN',selection=index)
        assert result['status']==0 and vm.files['C:\\SELECTED.BIN']==binary and result['total']==2,result
        result=vm.request(2,device,'/NESTED/DEEP')
        assert result['status']==0 and result['rows'][0]['name']=='README.TXT',result
        result=vm.request(3,device,'/NESTED/DEEP/README.TXT')
        assert result['status']==0 and result['preview']==payload[:511] and result['more']==1,result
        result=vm.request(4,device,'/PAYLOAD.BIN','C:\\IMPORTED.BIN')
        assert result['status']==0 and vm.files['C:\\IMPORTED.BIN']==binary,result
        result=vm.request(4,device,'/PAYLOAD.BIN','C:\\IMPORTED.BIN')
        assert result['status']==1 and 'already exists' in result['message'] and vm.files['C:\\IMPORTED.BIN']==binary,result
        result=vm.request(3,device,'/PAYLOAD.BIN')
        assert result['status']==1 and not result['preview'] and 'binary' in result['message'],result
        result=vm.request(2,device,'/MISSING')
        assert result['status']==1 and not result['rows'],result
        result=vm.request(2,device,'/',first=1)
        assert result['status']==0 and result['total']==2 and result['count']==1,result
        result=vm.request(2,device,'/',first=2)
        assert result['status']==0 and result['total']==2 and not result['rows'],result
        vm.devices.clear()
        result=vm.request(2,device,'/')
        assert result['status']==1 and not result['rows'],result
        checks.append({'filesystem':kind,'status':'PASS','cases':['mount','nested listing','preview','exact binary import','no overwrite','binary preview rejection','missing path','pagination','empty final page','removed media','FAR registers/segments/flags','first request in exported packet']})
    vm=Driver(program,{})
    for dev in (1,2,3):
        result=vm.request(1,dev)
        assert result['status']==1 and not result['rows'],result
        if dev==2: assert 'before boot' in result['message'],result
    result=vm.request(2,1)
    assert result['status']==1 and 'Open the device' in result['message'],result
    checks.append({'status':'PASS','cases':['absent floppy','absent CD','absent BIOS USB disk','unmounted request']})
    report={'result':'PASS','program_sha256':hashlib.sha256(program).hexdigest(),'checks':checks,
            'scope':'Actual compiled 16-bit FAR module; modeled BIOS/DOS calls; not QEMU or physical hardware.'}
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report,indent=2))
if __name__=='__main__': main()
