#!/usr/bin/env python3
"""Execute compiled AH=52h implementation and preserve modeled live DOS state.

No production instruction is replaced. The BDA, registered device header and
client-owned CDS/SFT fields are fixtures; this is not a full DOS driver test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from unicorn import Uc,UC_ARCH_X86,UC_MODE_16,UC_HOOK_INTR
from unicorn.x86_const import *
from test_xms_extended import symbol

ROOT=Path(__file__).resolve().parents[1]


def qualify(binary,listing):
    # The compiled kernel reserves SYSVARS at D900, immediately after its
    # 3000..D8FF runtime window. Use the real resident segment, not 0900.
    base,segment=0x3000,0x300
    cpu=Uc(UC_ARCH_X86,UC_MODE_16);cpu.mem_map(0,0x100000)
    cpu.mem_write(base,binary)
    cpu.mem_write(0x413,struct.pack('<H',640))
    cpu.mem_write(0x40e,struct.pack('<H',0x9fc0))
    def interrupt(_cpu,number,_):raise AssertionError(f'unexpected interrupt {number:02x}')
    cpu.hook_add(UC_HOOK_INTR,interrupt)
    entry=symbol(listing,'int21_get_list_of_lists')
    initialized=base+symbol(listing,'dos_sysvars_initialized')
    anchor=base+symbol(listing,'dos_list_of_lists')
    code_end=symbol(listing,'boot_drive')
    assert cpu.mem_read(initialized,1)==b'\0'

    def call():
        for reg,value in [(UC_X86_REG_CS,segment),(UC_X86_REG_DS,0x5000),
                          (UC_X86_REG_ES,0x5100),(UC_X86_REG_FS,0x5200),
                          (UC_X86_REG_GS,0x5300),(UC_X86_REG_SS,0x7000),
                          (UC_X86_REG_SP,0xfff0),(UC_X86_REG_AX,0x5200),
                          (UC_X86_REG_BX,0x1234),(UC_X86_REG_EFLAGS,0x202)]:cpu.reg_write(reg,value)
        cpu.mem_write(0x7fff0,struct.pack('<H',0xf000))
        cpu.emu_start(base+entry,base+0xf000,count=100000)
        assert cpu.reg_read(UC_X86_REG_IP)==0xf000
        assert cpu.reg_read(UC_X86_REG_SP)==0xfff2
        assert cpu.reg_read(UC_X86_REG_EFLAGS)&1==0
        assert cpu.reg_read(UC_X86_REG_DS)==0x5000
        assert cpu.reg_read(UC_X86_REG_FS)==0x5200 and cpu.reg_read(UC_X86_REG_GS)==0x5300
        assert bytes(cpu.mem_read(base,code_end))==binary[:code_end]
        return cpu.reg_read(UC_X86_REG_ES),cpu.reg_read(UC_X86_REG_BX)

    result=call();assert result==(0xd90,2),result
    assert cpu.mem_read(initialized,1)==b'\1'
    storage=result[0]*16;lol=storage+result[1]
    assert bytes(cpu.mem_read(lol+0x22+10,8))==b'NUL     '
    original_next=bytes(cpu.mem_read(lol+0x22,4))
    assert struct.unpack('<HH',original_next)==(0x80,0xd90)
    assert bytes(cpu.mem_read(storage+0x80+10,8))==b'CON     '
    assert bytes(cpu.mem_read(storage+0x1b0,3))==b'C:\\'
    assert int.from_bytes(cpu.mem_read(storage+0x204,2),'little')==20
    assert int.from_bytes(cpu.mem_read(storage,2),'little')==int.from_bytes(cpu.mem_read(anchor,2),'little')
    cases=['cold_initialization_publishes_NUL_CON_CDS_and_20_SFT_entries']
    # Model a real device's standard NUL-chain insertion and live DOS clients.
    device=original_next+struct.pack('<HHH8s',0xc000,0x300,0x400,b'EMMQXXX0')
    cpu.mem_write(0x25000,device)
    cpu.mem_write(lol+0x22,struct.pack('<HH',0,0x2500))
    cpu.mem_write(storage+0x1b0,b'C:\\LIVE\\SESSION\0')
    cpu.mem_write(storage+0x200,struct.pack('<HH',0x40,0x2600))
    live_sft=bytes((i*37+11)&255 for i in range(0x3b))
    cpu.mem_write(storage+0x206+5*0x3b,live_sft)
    expected=bytearray(cpu.mem_read(storage,1792))
    for new_anchor in [0x21ff,0x23ff,0x20ff]:
        cpu.mem_write(anchor,struct.pack('<H',new_anchor))
        expected[:2]=struct.pack('<H',new_anchor)
        assert call()==result
        actual=bytes(cpu.mem_read(storage,1792))
        assert actual==bytes(expected),'Repeated AH52 destroyed live SYSVARS/device/CDS/SFT bytes'
        assert bytes(cpu.mem_read(0x25000,len(device)))==device
    cases.extend(['registered_device_chain_survives_three_queries',
                  'live_CDS_and_SFT_link_and_record_survive_three_queries',
                  'first_MCB_anchor_refreshes_without_reinitialization',
                  'same_pointer_code_segments_and_stack_preserved'])
    return cases


def main():
    p=argparse.ArgumentParser(description=__doc__)
    base=ROOT/'build/full/vm-session-2026-09-26'
    p.add_argument('--kernel',type=Path,default=base/'ciukidos.sys')
    p.add_argument('--listing',type=Path,default=base/'ciukidos.lst')
    p.add_argument('--output',type=Path,default=base/'sysvars-lifetime-report.json')
    a=p.parse_args();binary=a.kernel.read_bytes();listing=a.listing.read_text()
    report={'status':'running','kernel_bytes':len(binary),'kernel_sha256':hashlib.sha256(binary).hexdigest(),
            'listing_sha256':hashlib.sha256(a.listing.read_bytes()).hexdigest(),
            'scope':'Compiled AH52+memory initialization; modeled BDA/live DOS metadata, no driver/device integration claim'}
    try:
        assert len(binary)<=0xa900
        report['cases']=qualify(binary,listing);report['status']='PASS'
    except Exception as error:
        report.update(status='FAIL',error=str(error));raise
    finally:
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
