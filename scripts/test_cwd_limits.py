#!/usr/bin/env python3
"""Execute production CHDIR/GETCWD with a modeled directory-lookup boundary.

Path parsing, copies, per-drive CWD and error rollback run as assembled code.
The fixture supplies directory metadata at int21_lookup_in_dir; this does not
claim to test FAT sector I/O or physical storage.
"""
import argparse
import json
from pathlib import Path
import re
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.x86_const import *

def symbol(listing, name):
    lines=listing.splitlines()
    for i,line in enumerate(lines):
        if re.search(r'\b'+re.escape(name)+r'(?=:|\s+(?:d[bwd]|times)\b)',line):
            for instruction in lines[i:]:
                match=re.match(r'\s*\d+\s+([0-9A-F]{8})\s',instruction)
                if match:
                    return int(match[1],16)
    raise AssertionError(f'missing symbol {name}')


class DirectoryFixture:
    def __init__(self, binary, listing):
        self.listing = listing
        self.base, self.segment, self.stop = 0x9000, 0x900, 0xF000
        self.cpu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.cpu.mem_map(0, 0x100000)
        self.cpu.mem_write(self.base, binary)
        self.lookup = self.base + symbol(listing, 'int21_lookup_in_dir')
        self.cpu.mem_write(self.lookup, b'\xc3')
        self.cpu.hook_add(UC_HOOK_CODE, self.on_code)
        self.cpu.hook_add(UC_HOOK_INTR, lambda *_: (_ for _ in ()).throw(AssertionError('unexpected interrupt')))
        self.paths = {'': 0}
        self.edges = {}
        self.symbols = {}
        self.write('dos_default_drive', b'\2')

    def addr(self, name):
        if name not in self.symbols:
            self.symbols[name] = self.base + symbol(self.listing, name)
        return self.symbols[name]

    def write(self, name, data):
        self.cpu.mem_write(self.addr(name), data)

    def readword(self, name):
        return int.from_bytes(self.cpu.mem_read(self.addr(name), 2), 'little')

    def text(self, name):
        return bytes(self.cpu.mem_read(self.addr(name), 64)).split(b'\0', 1)[0].decode()

    def add(self, path):
        parent = ''
        for component in path.split('\\'):
            current = parent+'\\'+component if parent else component
            if current not in self.paths:
                self.paths[current] = len(self.paths)+1
                self.edges[(self.paths[parent], component.ljust(11))] = self.paths[current]
            parent = current

    def on_code(self, cpu, address, _size, _data):
        if address != self.lookup:
            return
        parent = cpu.reg_read(UC_X86_REG_AX)
        source = cpu.reg_read(UC_X86_REG_DS)*16+cpu.reg_read(UC_X86_REG_SI)
        name = bytes(cpu.mem_read(source, 11)).decode()
        cluster = self.edges.get((parent, name))
        flags = cpu.reg_read(UC_X86_REG_EFLAGS)
        cpu.reg_write(UC_X86_REG_EFLAGS, (flags & ~1) | int(cluster is None))
        if cluster is not None:
            self.write('search_found_cluster', struct.pack('<H', cluster))
            self.write('search_found_attr', b'\x10')

    def call(self, routine, *, path=None, drive=0):
        cpu = self.cpu
        for reg, value in ((UC_X86_REG_CS,self.segment), (UC_X86_REG_DS,0x5000),
                           (UC_X86_REG_ES,self.segment), (UC_X86_REG_SS,0x7000),
                           (UC_X86_REG_SP,0xFFF0), (UC_X86_REG_EFLAGS,0x202),
                           (UC_X86_REG_DX,0x100 if path is not None else drive),
                           (UC_X86_REG_SI,0x200)):
            cpu.reg_write(reg,value)
        cpu.mem_write(0x7FFF0,struct.pack('<H',self.stop))
        if path is not None:
            cpu.mem_write(0x50100,path.encode()+b'\0')
        cpu.mem_write(0x50200,b'?'*65)
        cpu.emu_start(self.base+symbol(self.listing,routine),self.base+self.stop,count=200000)
        assert cpu.reg_read(UC_X86_REG_IP)==self.stop,'routine failed to return'
        return bool(cpu.reg_read(UC_X86_REG_EFLAGS)&1)


def check(binary, listing):
    vm = DirectoryFixture(binary, listing)
    path = '\\'.join(['ABCDEFG']+['ABCDEFGH']*6+['Z'])
    assert len(path)==63
    too_long = '\\'.join(['ABCDEFGH']*7+['Z'])
    assert len(too_long)==64
    shorter = 'PROGRAMS\\TOOLS\\UTILS\\BIN'
    other = 'WINDOWS\\SYSTEM\\DRIVERS\\AUDIO\\LEGACY'
    for p in (path,too_long,shorter,other,path+'\\EXTRA'):
        vm.add(p)
    checks = []
    def chdir(p, expected, fail=False):
        before = vm.readword('cwd_cluster')
        assert vm.call('int21_chdir_impl',path=p)==fail, ('CHDIR status',p)
        assert vm.text('cwd_buf')==expected, ('CWD',p,vm.text('cwd_buf'))
        assert vm.readword('cwd_cluster')==(before if fail else vm.paths[expected])
        checks.append(p)
    chdir('\\'+shorter,shorter)
    chdir('MISSING',shorter,True)
    chdir('\\'+path,path)
    assert not vm.call('int21_getcwd',drive=3)
    result = bytes(vm.cpu.mem_read(0x50200,65))
    assert result[:64]==path.encode()+b'\0' and result[64:]==b'?', 'GETCWD boundary overwrite'
    parent = path.rsplit('\\',1)[0]
    chdir('..',parent)
    chdir('Z',path)
    chdir('EXTRA',path,True)
    chdir('\\'+too_long,path,True)
    assert not vm.call('int21_chdir_impl',path='D:\\'+other)
    assert vm.text('cwd_buf')==path and vm.text('cwd_d_buf')==other
    assert not vm.call('int21_getcwd',drive=4)
    assert bytes(vm.cpu.mem_read(0x50200,64)).split(b'\0',1)[0]==other.encode()
    return checks+['GETCWD 63-byte guard','qualified D: preserves C:','GETCWD D:']


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('kernel','listing','report'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--old-kernel',type=Path)
    p.add_argument('--old-listing',type=Path)
    a=p.parse_args()
    cases=check(a.kernel.read_bytes(),a.listing.read_text())
    old_error=None
    if a.old_kernel:
        try:
            check(a.old_kernel.read_bytes(),a.old_listing.read_text())
        except AssertionError as exc:
            old_error=str(exc)
        assert old_error,'old CWD negative control unexpectedly passed'
    a.report.write_text(json.dumps({'passed':len(cases),'cases':cases,'old_failure':old_error},indent=2)+'\n')
    print(f'PASS {len(cases)} CWD/GETCWD checks; old failure: {old_error}')


if __name__=='__main__':
    main()
