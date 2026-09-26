#!/usr/bin/env python3
"""Actual RMDIR/DEL instructions with modeled sector/FAT/lookup boundaries.

This isolates refusal-before-write and chain release. Actual disk-format
creation/move behavior is checked separately in qemu_test_native_utilities.py.
"""
import argparse, json, struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE
from unicorn.x86_const import *
from test_cwd_limits import symbol

def entry(name, attr=0x20, cluster=30):
    e = bytearray(32); e[:11] = name.ljust(11).encode(); e[11] = attr
    struct.pack_into('<H', e, 26, cluster)
    return bytes(e)

class Fixture:
    def __init__(self, data, listing, directory, chain=None, unreadable=None):
        self.u = Uc(UC_ARCH_X86, UC_MODE_16); self.u.mem_map(0, 0x100000)
        # The on-disk image initially loads at 0900h, then relocates to 0300h.
        # Running FAT I/O at the temporary address would overlap its scratch.
        self.base=0x3000; self.seg=0x300; self.listing=listing
        self.u.mem_write(self.base, data); self.writes=[]; self.reads=0
        self.chain = {20:0xFFF8} if chain is None else chain.copy()
        self.sectors={329:entry('DEST',0x10,20)+bytes(480)}
        for cluster, content in directory.items():
            content = content.ljust(4096,b'\0')
            for i in range(8):self.sectors[361+(cluster-2)*8+i]=content[i*512:(i+1)*512]
        self.unreadable=unreadable
        for n in ('int21_resolve_and_find_path','int21_load_fat_cache','fat12_get_entry_cached',
                  'fat12_set_entry_cached','fat12_flush_cache','read_sector_lba32','write_sector_lba32'):
            address=self.addr(n); self.u.mem_write(address,b'\xc3')
            self.u.hook_add(UC_HOOK_CODE,self.hook,n,address,address)
    def addr(self,n):return self.base+symbol(self.listing,n)
    def put(self,n,v):self.u.mem_write(self.addr(n),struct.pack('<H',v))
    def cf(self,x):self.u.reg_write(UC_X86_REG_EFLAGS,(self.u.reg_read(UC_X86_REG_EFLAGS)&~1)|int(x))
    def hook(self,u,_a,_s,name):
        self.cf(False)
        ax=u.reg_read(UC_X86_REG_AX)
        if name=='int21_resolve_and_find_path':
            for n,v in [('search_found_cluster',20),('search_found_root_lba',329),('search_found_root_lba_hi',0),('search_found_root_off',0)]:self.put(n,v)
            u.mem_write(self.addr('search_found_attr'),b'\x10')
        elif name=='fat12_get_entry_cached':
            if ax not in self.chain:self.cf(True)
            else:u.reg_write(UC_X86_REG_AX,self.chain[ax])
        elif name=='fat12_set_entry_cached':
            self.chain[ax]=u.reg_read(UC_X86_REG_DX);self.writes.append(('FAT',ax))
        elif name in ('read_sector_lba32','write_sector_lba32'):
            lba=ax+(u.reg_read(UC_X86_REG_DX)<<16)
            ptr=(u.reg_read(UC_X86_REG_ES)<<4)+u.reg_read(UC_X86_REG_BX)
            if name.startswith('read'):
                self.reads+=1
                if lba==self.unreadable or lba not in self.sectors:self.cf(True)
                else:u.mem_write(ptr,self.sectors[lba])
            else:self.sectors[lba]=bytes(u.mem_read(ptr,512));self.writes.append(('sector',lba))
    def run(self,routine='int21_rmdir'):
        u=self.u
        for r,v in ((UC_X86_REG_CS,self.seg),(UC_X86_REG_DS,0x5000),(UC_X86_REG_ES,0x5100),
                    (UC_X86_REG_SS,0x7000),(UC_X86_REG_SP,0xFFF0),(UC_X86_REG_DX,0x100),(UC_X86_REG_EFLAGS,0x202)):
            u.reg_write(r,v)
        u.mem_write(0x50100,b'\\DEST\0');u.mem_write(0x7FFF0,b'\0\xf0')
        u.emu_start(self.addr(routine),self.base+0xF000,count=150000000)
        assert u.reg_read(UC_X86_REG_IP)==0xF000,'unbounded traversal'
        assert u.reg_read(UC_X86_REG_DS)==0x5000 and u.reg_read(UC_X86_REG_ES)==0x5100,'caller segments changed'
        return bool(u.reg_read(UC_X86_REG_EFLAGS)&1)

def checks(data,listing):
    dots=entry('.',0x10,20)+entry('..',0x10,0)
    deleted=b'\xe5'+bytes(31)
    cases=[]
    def refused(name, directory, **kw):
        f=Fixture(data,listing,directory,**kw)
        assert f.run(),name+' incorrectly succeeded'
        assert not f.writes,name+' wrote before refusal'
        cases.append(name)
    refused('ordinary file',{20:dots+entry('FILE    TXT')})
    refused('hidden system file',{20:dots+entry('HIDDEN  SYS',6)})
    refused('child directory',{20:dots+entry('CHILD',0x10)})
    refused('legacy missing dots',{20:entry('MOVED   BIN')})
    refused('file in later sector',{20:deleted*16+entry('LATER   BIN')})
    refused('file in second cluster',{20:deleted*128,21:entry('LATER   BIN')},chain={20:21,21:0xFFF8})
    refused('unreadable directory',{20:dots},unreadable=361+(20-2)*8)
    refused('invalid FAT link',{20:deleted*128},chain={20:1})
    refused('cyclic FAT link',{20:deleted*128},chain={20:20})
    refused('cycle after logical end',{20:dots},chain={20:20})
    refused('bad link after logical end',{20:dots},chain={20:1})
    for name,content,chain in [('empty dots',dots,{20:0xFFF8}),('legacy empty',bytes(4096),{20:0xFFF8}),
                               ('empty chained',{20:deleted*128,21:deleted*128},{20:21,21:0xFFF8})]:
        f=Fixture(data,listing,content if isinstance(content,dict) else {20:content},chain=chain)
        assert not f.run(),name+' refused'
        assert f.sectors[329][0]==0xE5 and all(v==0 for v in f.chain.values()),name+' leaked chain'
        cases.append(name)
    f=Fixture(data,listing,{20:dots});f.put('cwd_cluster',20)
    assert f.run() and not f.writes;cases.append('current directory refused')
    f=Fixture(data,listing,{20:dots});f.u.mem_write(f.addr('search_found_name'),b'.          ')
    assert f.run() and not f.writes;cases.append('dot alias refused')
    f=Fixture(data,listing,{20:dots})
    assert f.run('int21_delete') and not f.writes;cases.append('DEL cannot bypass RMDIR')
    return cases

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('kernel','listing','report'):p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--old-kernel',type=Path);p.add_argument('--old-listing',type=Path)
    a=p.parse_args();cases=checks(a.kernel.read_bytes(),a.listing.read_text());old=None
    if a.old_kernel:
        try: checks(a.old_kernel.read_bytes(),a.old_listing.read_text())
        except AssertionError as e:old=str(e)
        assert old,'negative control passed'
    a.report.write_text(json.dumps({'cases':cases,'negative_control':old,'boundary':'modeled lookup, sectors and FAT'},indent=2)+'\n')
    print(f'PASS {len(cases)} directory mutation cases; old failure: {old}')
if __name__=='__main__':main()
