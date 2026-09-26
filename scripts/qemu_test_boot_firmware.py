#!/usr/bin/env python3
"""Exercise bad firmware responses against real QEMU mode sets and PS/2 input."""
import argparse
from pathlib import Path
import subprocess
import time
from qemu_test_desktop_capabilities import HardwareVM, disk_copy

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--case',choices=('video','audio'),default='video')
    args=ap.parse_args()
    if args.case=='audio':
        out=args.output.resolve()
        disk=disk_copy(out)
        fixture=out/'IRQFAULT.COM'
        subprocess.run(['nasm','-f','bin','scripts/fixtures/audio_irq_fault.asm','-o',str(fixture)],check=True)
        subprocess.run(['mcopy','-o','-i',str(disk),str(fixture),'::APPS/IRQFAULT.COM'],check=True)
        vm=HardwareVM(disk,out,('-audiodev','none,id=snd','-device','AC97,audiodev=snd',
                                '-debugcon',f'file:{out}/fault.log'))
        try:
            vm.wait('[DESKTOP] READY',timeout=90)
            vm.key('f4');vm.wait('[DESKTOP] DOS')
            vm.wait('CiukiOS SHELL C:\\APPS>')
            vm.command('irqfault','[AUDIO-FAULT] installed')
            started=time.monotonic()
            vm.command('bootsnd.com /Q',timeout=45)
            assert (out/'fault.log').read_bytes()==b'T','fault did not reach codec clock query'
            vm.command('echo IRQ READY','IRQ READY\r\nCiukiOS SHELL')
            offset=vm.offset();vm.text('exit');vm.wait('[DESKTOP] READY',offset,90)
            vm.edges((1280,800))
            print(f'[firmware] PASS stalled audio timer and masked IRQs: bounded return, keyboard/mouse ({time.monotonic()-started:.1f}s including GUI checks)',flush=True)
        finally:vm.close()
        return
    for fault in (1,2,3,4,5):
        out=args.output.resolve()/str(fault)
        disk=disk_copy(out)
        fixture=out/'VBEFAULT.COM'
        subprocess.run(['nasm','-f','bin','scripts/fixtures/vbe_fault.asm',
                        '-D',f'TEST_FAULT={fault}','-o',str(fixture)],check=True)
        subprocess.run(['mcopy','-o','-i',str(disk),str(fixture),'::APPS/VBEFAULT.COM'],check=True)
        vm=HardwareVM(disk,out)
        try:
            vm.wait('[DESKTOP] READY',timeout=90)
            vm.key('f4');vm.wait('[DESKTOP] DOS')
            vm.wait('CiukiOS SHELL C:\\APPS>')
            vm.command('vbefault','[VBE-FAULT] installed')
            offset=vm.offset();vm.text('shell.com')
            vm.wait('[DESKTOP] READY',offset,90)
            size=(1280,800) if fault in (1,4) else (640,480)
            vm.shot('desktop',size)
            vm.edges(size)
            offset=vm.offset();vm.key('f4');vm.wait('[DESKTOP] DOS',offset)
            vm.text('echo FIRMWARE READY');vm.wait('FIRMWARE READY\r\nCiukiOS SHELL',offset)
            print(f'[firmware] PASS fault={fault}, geometry={size}, cursor bounds and keyboard',flush=True)
        finally:
            vm.close()

if __name__=='__main__':main()
