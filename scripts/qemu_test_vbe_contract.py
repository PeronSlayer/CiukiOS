#!/usr/bin/env python3
"""Exercise real VBE calls and banked VRAM through the installed DOS kernel."""
import argparse, json, shutil, subprocess
from pathlib import Path
from qemu_test_full_display_profile import VM
p=argparse.ArgumentParser();p.add_argument('--image',type=Path,required=True)
p.add_argument('--kernel',type=Path);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
o=a.output.resolve();o.mkdir(parents=True,exist_ok=True);d=o/'target.img';shutil.copyfile(a.image,d)
f=o/'vbecheck.com';subprocess.run(['nasm','-f','bin','scripts/fixtures/vbe_contract.asm','-o',str(f)],check=True)
files=[(f,'APPS/VBECHECK.COM')]
if a.kernel:files.append((a.kernel,'SYSTEM/CIUKIDOS.SYS'))
for source,target in files:subprocess.run(['mcopy','-o','-i',f'{d}@@32256',str(source),'::'+target],check=True)
v=VM(d,o)
try:
 v.wait('CiukiOS SHELL C:\\APPS>',timeout=90)
 marker='[VBE-CHECK] PASS DX preserved, bank 2 queried, VRAM retained'
 v.command('vbecheck',marker)
 v.command('dir \\sbemu','VSBHDA.EXE');v.command('comdemo.com','COM demo via INT21h')
 (o/'result.json').write_text(json.dumps({'observed':marker,'DOS_after_video':True},indent=2)+'\n')
 print(marker)
finally:v.shot('final');v.close()
