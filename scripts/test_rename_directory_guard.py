#!/usr/bin/env python3
"""Execute AH56's production rename with modeled lookup and sector boundaries."""
import argparse, json, struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_AX, UC_X86_REG_BP
from test_directory_empty import Fixture, entry

class Rename(Fixture):
    def __init__(self,data,listing,directory,cross):
        dots=entry('.',0x10,21)+entry('..',0x10,0)
        super().__init__(data,listing,{21:dots})
        self.directory=directory; self.cross=cross
        self.sectors[329]=entry('ORIGINAL',0x10 if directory else 0x20,20)+bytes(480)
        for name in ('int21_resolve_parent_dir','int21_lookup_in_dir','int21_find_free_dir_entry'):
            a=self.addr(name);self.u.mem_write(a,b'\xc3')
            self.u.hook_add(UC_HOOK_CODE,self.hook,name,a,a)
    def hook(self,u,a,s,name):
        self.cf(False)
        if name=='int21_resolve_parent_dir':
            u.reg_write(UC_X86_REG_AX,21 if self.cross else 0)
        elif name=='int21_lookup_in_dir':
            # A failed destination search does not preserve source metadata.
            self.u.mem_write(self.addr('search_found_attr'),b'\0')
            u.reg_write(UC_X86_REG_AX,2);self.cf(True)
        elif name=='int21_find_free_dir_entry':
            self.put('search_found_root_lba',513);self.put('search_found_root_lba_hi',0)
            self.put('search_found_root_off',64)
        else:
            super().hook(u,a,s,name)
            if name=='int21_resolve_and_find_path':
                self.put('tmp_lookup_dir',0)
                u.mem_write(self.addr('search_found_attr'),bytes([0x10 if self.directory else 0x20]))
    def rename(self):
        self.u.reg_write(UC_X86_REG_BP,0xF000)
        # The native INT21 wrapper's original ES,DS,DI,DX frame.
        self.u.mem_write(0x7F000,struct.pack('<7H',0,0x5100,0x5000,0,0x200,0,0x100))
        self.u.mem_write(0x51200,b'RENAMED\0')
        return self.run('int21_rename')

def checks(data,listing):
    f=Rename(data,listing,True,True)
    original=dict(f.sectors)
    assert f.rename(),'cross-directory folder move incorrectly succeeded'
    assert f.u.reg_read(UC_X86_REG_AX)==5,'wrong DOS error'
    assert not f.writes and f.sectors==original,'folder move wrote before refusal'
    f=Rename(data,listing,True,False)
    assert not f.rename(),'same-parent folder rename regressed'
    assert f.sectors[329][:11]==b'RENAMED    ' and f.sectors[329][11]==0x10
    f=Rename(data,listing,False,True)
    assert not f.rename(),'cross-directory regular file move regressed'
    assert f.sectors[329][0]==0xE5 and f.sectors[513][64:75]==b'RENAMED    '
    assert f.sectors[513][75]==0x20 and struct.unpack_from('<H',f.sectors[513],90)[0]==20
    return ['folder cross-parent refused without writes','folder same-parent rename','file cross-parent move']

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('kernel','listing','old-kernel','old-listing','report'):
        p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();cases=checks(a.kernel.read_bytes(),a.listing.read_text());old=None
    try:checks(a.old_kernel.read_bytes(),a.old_listing.read_text())
    except AssertionError as e:old=str(e)
    assert old,'old negative control passed'
    a.report.write_text(json.dumps({'cases':cases,'old_failure':old,'boundary':'modeled lookup and sectors'},indent=2)+'\n')
    print('PASS 3 AH56 cases; old failure:',old)
if __name__=='__main__':main()
