#!/usr/bin/env python3
import argparse, sys, subprocess as sp, shutil, json, re, hashlib, struct
from pathlib import Path
sys.path.insert(0, 'scripts')
from qemu_test_full_display_profile import VM
parser=argparse.ArgumentParser(description='Compare real installed-HDD EXEC/InDOS behavior before and after a kernel fix.')
parser.add_argument('--image',type=Path,required=True,help='Installed MBR disk; never modified')
parser.add_argument('--before-sys',type=Path,required=True)
parser.add_argument('--after-sys',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--partition-offset',type=int,default=32256)
args=parser.parse_args()
root=args.output.resolve()
root.mkdir(parents=True,exist_ok=True)
for source,name in [(args.before_sys,'before.sys'),(args.after_sys,'after.sys'),
                    (Path('src/probes/execstate/parent.asm'),'execstate.asm'),
                    (Path('src/probes/execstate/child.asm'),'execleaf.asm')]:
 shutil.copyfile(source,root/name)
for idx,(name,mode,mz,nest) in enumerate([
 ('C4C.COM',0x4c,False,False),('C20.COM',0x20,False,False),
 ('CRET.COM',0xff,False,False),('C00.COM',0,False,False),
 ('M4C.EXE',0x4c,True,False),('M20.EXE',0x20,True,False),
 ('M00.EXE',0,True,False),('NEST.COM',0x4c,False,True),('C31.COM',0x31,False,False)]):
 cmd=['nasm','-f','bin',str(root/'execleaf.asm'),'-o',str(root/name),'-D',f'EXIT={mode}','-D',f'ID={idx}']
 if mz:cmd+=['-D','MZ=1']
 if nest:cmd+=['-D','NEST=1']
 sp.run(cmd,check=True)
(root/'BROKEN.EXE').write_bytes(b'MZ invalid incomplete header')
# A valid MZ overlay has known payload bytes; the parent verifies the actual copy.
payload=b'OVERLAY-EXEC-IN-DOS-TEST\0'
(root/'OVERLAY.EXE').write_bytes(struct.pack('<14H',0x5a4d,32+len(payload),1,0,2,0,0,0,0,0,0,0,0x1c,0)+b'\0'*4+payload)
(root/'NOMEM.EXE').write_bytes(struct.pack('<14H',0x5a4d,33,1,0,2,0xa000,0xffff,0,0,0,0,0,0x1c,0)+b'\0'*4+b'\xc3')
sp.run(['nasm','-f','bin',str(root/'execstate.asm'),'-l',str(root/'execstate.lst'),'-o',str(root/'EXECSTAT.COM')],check=True)
for src,dst in [('ciukpstk.asm','CIUKPST.COM'),('ciukptrm.asm','CIUKTRM.EXE'),('ciukpcom.asm','CIUKPCOM.COM')]:
 sp.run(['nasm','-f','bin',f'src/com/{src}','-D','CIUKIDOS_RUNTIME_SEG=0x0300','-o',str(root/dst)],check=True)
results={}
for variant in ['before','after']:
 out=root/variant;out.mkdir(exist_ok=True)
 disk=out/'test.img';shutil.copyfile(args.image,disk)
 img=f'{disk}@@{args.partition_offset}'
 sp.run(['mcopy','-o','-i',img,str(root/f'{variant}.sys'),'::SYSTEM/CIUKIDOS.SYS'],check=True)
 for file in list(root.glob('*.COM'))+list(root.glob('*.EXE')):
  sp.run(['mcopy','-o','-i',img,str(file),f'::APPS/{file.name}'],check=True)
 dbg=out/'debug.log';dbg.unlink(missing_ok=True)
 vm=VM(disk,out,('-debugcon',f'file:{dbg}'))
 try:
  vm.wait('CiukiOS SHELL C:\\APPS>',timeout=50)
  vm.command('EXECSTAT.COM',timeout=90)
  vm.shot('execstat-return')
  vm.command('CIUKPST.COM','[PSTACK:C] ALL=PASS',timeout=90)
  vm.command('CIUKPST.COM /ROOT','[PSTACK:R] ALL=PASS',timeout=60)
  vm.command('C4C.COM')
  vm.command('echo EXEC-REGRESSION-ALIVE','EXEC-REGRESSION-ALIVE')
  vm.shot('process-tests-return')
 finally:vm.close()
 data=dbg.read_text(errors='replace')
 rows=[]
 for match in re.finditer(r'@EXSTATE ((?:[0-9A-F]{4} ){16})',data):
  fields=[int(x,16) for x in match[1].split()]
  names=['kind','case','indos','es','ds','di','si','bp','sp','bx','dx','cx','ax','flags','caller_ds','caller_bx']
  rows.append(dict(zip(names,fields)))
 results[variant]={'sys_sha256':hashlib.sha256((root/f'{variant}.sys').read_bytes()).hexdigest(),
 'records':rows,'child_indos':re.findall(r'@L(\d)(\d)',data),'nested_indos':re.findall(r'@N(\d)',data),'in_dos_during_bios_read':re.findall(r'@I(\d)',data),'overlay_contents_ok':'!' not in data,'pstack':'PASS'}
 (out/'result.json').write_text(json.dumps(results[variant],indent=2))
 print(f'[exec-state] {variant}: collected {len(rows)} records; process/TSR probe PASS',flush=True)
(root/'result.json').write_text(json.dumps(results,indent=2))

# All assertions apply to observations from actual executable machine code.
# Before/after use the same source, disk and guest actions; only the SYS differs.
fixed=results['after']
assert len(fixed['records']) == 44, 'missing EXEC input/return/exit observations'
assert len(fixed['child_indos']) == 11, 'missing child or post-TSR execution'
assert all(depth == '0' for _,depth in fixed['child_indos']), 'suspended EXEC counted as DOS activity'
assert fixed['nested_indos'] == ['0'], 'grandchild return left DOS active'
assert fixed['in_dos_during_bios_read'], 'no actual BIOS I/O observed'
assert set(fixed['in_dos_during_bios_read']) == {'1'}, 'DOS must be active during BIOS calls'
assert fixed['overlay_contents_ok'], 'AL03 overlay content mismatch or nested child failed'
for row in fixed['records']:
 assert row['indos'] == 0, row
for case in range(14):
 rows={row['kind']:row for row in fixed['records'] if row['case']==case}
 before,after,exit_status=rows[1],rows[2],rows[3]
 for reg in ['es','ds','di','si','bp','sp','bx','dx','cx']:
  assert after[reg] == before[reg], (case,reg,before,after)
 assert after['caller_ds']==before['ds'], (case,'DOSMGR caller DS',after)
 assert after['caller_bx']==before['bx'], (case,'DOSMGR caller BX',after)
 assert (after['flags']&0x40)==(before['flags']&0x40), (case,'ZF changed',after)
 error={8:2,9:11,11:1,12:8}.get(case,0)
 assert after['ax']==error and bool(after['flags']&1)==bool(error), (case,'EXEC result',after)
 if case in (0,4,7):expected=0x5a
 elif case in (1,2,3,5,6):expected=0
 elif case==13:expected=0x035a
 else:expected=0x5a  # Failed EXEC and AL03 preserve the previous child status.
 assert exit_status['ax']==expected, (case,'AH4D result',exit_status)
old=results['before']
assert all(depth != '0' for _,depth in old['child_indos']), 'baseline did not reproduce InDOS regression'
old_returns=[r for r in old['records'] if r['kind']==2 and r['case']<8]
assert any(r['caller_ds']!=r['ds'] or r['caller_bx']!=r['bx'] for r in old_returns), 'baseline metadata regression absent'
results['validation']='PASS: 14 EXEC cases, live BIOS InDOS, before/after DOSMGR metadata, exact registers, overlay bytes and independent process/TSR checks'
(root/'result.json').write_text(json.dumps(results,indent=2))
print('[exec-state] '+results['validation'],flush=True)
