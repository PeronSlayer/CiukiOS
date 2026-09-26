#!/usr/bin/env python3
"""Actual clock instructions against modeled PIT2/PIT3 and delayed BIOS ticks.

This is a CPU unit test, not evidence of physical T23/PentiumIII performance.
QEMU's independent elapsed-time comparison is in qemu_test_graphics_window.py.
"""
import argparse,json,struct,subprocess
from pathlib import Path
from unicorn import Uc,UC_ARCH_X86,UC_MODE_16,UC_HOOK_INSN
from unicorn.x86_const import *

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,required=True)
    a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    source=Path('src/com/dos_window_graphics.inc').read_text()
    body=source[source.index('cg_update_clock:'):source.index('cg_convert_palette:')]
    names=['cg_update_clock','cg_clock_ready','cg_previous_cycles','cg_clock_remainder','cg_descriptor']
    asm='bits 16\norg 0\nCG_MILLISECONDS equ 28\nCG_STATUS equ 36\n'+body+'''
cg_clock_ready db 0
cg_previous_cycles dd 0
cg_clock_remainder dd 0
cg_descriptor times 48 db 0
db 'CGCLOCK1'
'''+''.join('dw '+n+'\n' for n in names)
    path=a.output/'clock.asm';path.write_text(asm)
    subprocess.run(['nasm','-f','bin',str(path),'-o',str(a.output/'clock.bin')],check=True)
    code=(a.output/'clock.bin').read_bytes();index=code.index(b'CGCLOCK1')+8
    syms=dict(zip(names,struct.unpack_from('<5H',code,index)));results=[]
    for mode in (2,3):
        u=Uc(UC_ARCH_X86,UC_MODE_16);u.mem_map(0,0x100000);u.mem_write(0x20000,code)
        model={'reads':[],'irr':0,'writes':[]}
        def read(uc,port,size,userdata):
            assert size==1
            if port==0x40:return model['reads'].pop(0)
            assert port==0x20;return model['irr']
        def write(uc,port,size,value,userdata):
            assert (port,value) in ((0x43,0xC2),(0x20,0x0A)),(port,value)
            model['writes'].append((port,value))
        u.hook_add(UC_HOOK_INSN,read,None,1,0,UC_X86_INS_IN)
        u.hook_add(UC_HOOK_INSN,write,None,1,0,UC_X86_INS_OUT)
        def field(offset,size):return int.from_bytes(u.mem_read(0x20000+syms['cg_descriptor']+offset,size),'little')
        def sample(cycles,lag=False,pending=False):
            tick,phase=divmod(cycles,65536)
            count=(65536-((phase*(2 if mode==3 else 1))%65536))&65535
            out=(phase<32768) if mode==3 else (phase!=65535)
            model['reads']=[(int(out)<<7)|0x30|(mode<<1),count&255,count>>8]
            model['irr']=int(pending)
            u.mem_write(0x46C,struct.pack('<I',tick-int(lag)))
            u.reg_write(UC_X86_REG_CS,0x2000);u.reg_write(UC_X86_REG_DS,0x2000)
            u.reg_write(UC_X86_REG_ES,0x4444);u.reg_write(UC_X86_REG_SS,0x8000)
            u.reg_write(UC_X86_REG_SP,0xF000);u.reg_write(UC_X86_REG_EFLAGS,2)
            u.mem_write(0x8F000,struct.pack('<H',0xFFF0))
            u.emu_start(0x20000+syms['cg_update_clock'],0x2FFF0,count=300)
            assert u.reg_read(UC_X86_REG_IP)==0xFFF0
            assert not model['reads'] and u.reg_read(UC_X86_REG_ES)==0x4444
            return field(28,4)
        origin=100*65536+1000
        samples=[origin,100*65536+32766,100*65536+32768,100*65536+65534,
                 101*65536,101*65536+2,102*65536+100,105*65536+50000]
        for cycles in samples:
            got=sample(cycles);expected=(cycles-origin)*1000//1193182
            assert abs(got-expected)<=1,(mode,cycles,got,expected)
        # Acknowledged edge with stale BDA must not become a one-hour jump.
        before=sample(106*65536-20)
        assert sample(106*65536+10,lag=True)==before
        assert field(40,4)==1 and field(36,2)==0
        # Pending unacknowledged edge is compensated; later BDA catches up.
        for cycles,lag,pending in ((106*65536+20,True,True),(106*65536+100,False,False)):
            got=sample(cycles,lag,pending);expected=(cycles-origin)*1000//1193182
            assert abs(got-expected)<=1,(mode,cycles,got,expected)
        results.append(dict(mode=mode,passed=True,samples=len(samples)+4,delayed_edge_deferred=True,
                            pending_edge_compensated=True,no_timer_reprogramming=True))
    (a.output/'report.json').write_text(json.dumps(dict(passed=True,scope=__doc__,cases=results),indent=2)+'\n')
    print('[graphics-clock] PASS PIT2/PIT3 phase, edge deferral and pending IRQ compensation')
if __name__=='__main__':main()
