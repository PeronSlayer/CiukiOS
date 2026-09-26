#!/usr/bin/env python3
"""Exercise default selection after AUTO, real previews, rollback and DOS paths."""
import argparse,hashlib,json,shutil,subprocess,time
from pathlib import Path
from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('--image',type=Path,required=True)
p.add_argument('--vgasetup',type=Path);p.add_argument('--shell',type=Path)
p.add_argument('--expect-invalid-selection',action='store_true')
p.add_argument('--output',type=Path,required=True);a=p.parse_args()
o=a.output.resolve();o.mkdir(parents=True,exist_ok=True);d=o/'target.img';shutil.copyfile(a.image,d)
files=[]
if a.vgasetup:files.append((a.vgasetup,'SYSTEM/DRIVERS/VGASETUP.COM'))
if a.shell:files.append((a.shell,'SYSTEM/SHELL.COM'))
profile=o/'display.cfg';profile.write_bytes(b'AUTO');files.append((profile,'SYSTEM/VIDEO/DISPLAY.CFG'))
for source,target in files:subprocess.run(['mcopy','-o','-i',f'{d}@@32256',str(source),'::'+target],check=True)
source=FAT16(d);ini=source.read('WINDOWS/SYSTEM.INI');report={}
v=VM(d,o)
try:
 v.wait('CiukiOS SHELL C:\\APPS>',timeout=90)
 off=v.offset();v.text('vgasetup');v.wait('[VGASETUP] MENU READY',off,30)
 Image.open(v.shot('menu')).save(o/'menu.png')
 off=v.offset();v.key('ret')
 if a.expect_invalid_selection:
  v.wait('[VGASETUP] PREVIEW',off,30)
  v.wait('ENTER keeps this resolution',off,30)
  raw=subprocess.check_output(['scripts/serial_log_normalize.py','--offset',str(off),str(v.serial)])
  label=raw.split(b'[VGASETUP] PREVIEW',1)[1].split(b'ENTER keeps',1)[0]
  bad=sum(byte>126 or (byte<32 and byte not in (10,13)) for byte in label)
  assert bad>0, 'old default selection did not reproduce the out-of-table label read'
  Image.open(v.shot('corrupt-preview')).save(o/'corrupt-preview.png')
  report['observed']='AUTO default Enter read its label outside menu_options'
  report['invalid_text_bytes']=bad
 else:
  v.wait('[VGASETUP] PREVIEW',off,30);time.sleep(.4)
  Image.open(v.shot('default-preview',(800,600))).save(o/'default-preview.png')
  # Let the real preview timer expire and check both on-disk settings.
  v.wait('Preview cancelled',off,20);v.wait('[VGASETUP] MENU READY',off,30)
  assert FAT16(d).read('SYSTEM/VIDEO/DISPLAY.CFG')==b'AUTO'
  assert FAT16(d).read('WINDOWS/SYSTEM.INI')==ini
  off=v.offset();v.key('2');v.key('ret');v.wait('[VGASETUP] PREVIEW',off,30)
  time.sleep(.5);v.shot('800-preview',(800,600));off=v.offset();v.key('ret')
  v.wait('Shared resolution saved',off,30);v.wait('CiukiOS SHELL C:\\APPS>',off,30)
  assert FAT16(d).read('SYSTEM/VIDEO/DISPLAY.CFG')==b'0800'
  assert b'Width=800' in FAT16(d).read('WINDOWS/SYSTEM.INI')
  Image.open(v.shot('applied',(800,600))).save(o/'applied.png')
  for path in ('SBEMU','WINDOWS','SYSTEM/DRIVERS'):
   off=v.offset();v.text('dir \\'+path);v.wait('CiukiOS SHELL C:\\APPS>',off,60)
   raw=subprocess.check_output(['scripts/serial_log_normalize.py','--offset',str(off),str(v.serial)]).decode()
   current=FAT16(d)
   names=current.entries(current.entry(path)[1])
   original=source.entries(source.entry(path)[1])
   assert set(names)==set(original)|({'SYSTEM.BAK'} if path=='WINDOWS' else set()), 'unexpected on-disk entries'
   expected=[n+(' <DIR>' if e[0]&16 else '') for n,e in names.items()]
   # The profile transaction also renames WINDOWS/SYSTEM.INI to SYSTEM.BAK.
   actual=[line for line in raw.split('Directory listing\r\n',1)[1].split('CiukiOS SHELL',1)[0].splitlines() if line]
   assert actual==expected,(path,actual,expected)
  v.command('run \\command.com','HELP lists commands. WHERE shows launch targets.')
  v.command('dir \\sbemu','HDPMI32I.EXE');v.command('comdemo.com','COM demo via INT21h')
  report['checks']=['AUTO Enter preview 800','real timeout','both settings preserved','800 confirmation','shared files committed','three complete directories','COMMAND.COM and COM execution']
 print(json.dumps(report),flush=True);(o/'result.json').write_text(json.dumps(report,indent=2)+'\n')
finally:v.shot('final');v.close()
