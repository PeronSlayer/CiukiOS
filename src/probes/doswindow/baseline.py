#!/usr/bin/env python3
"""Fullscreen guest baseline only: explicitly not a concurrent-window pass."""
import argparse, hashlib, json, re, shutil, subprocess, sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'scripts'))
from qemu_test_native_windows import WindowVM
from qemu_test_installed_hdd import FAT16

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--programs',type=Path,default=Path('build/probes/doswindow'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    source_hash=hashlib.sha256(a.image.read_bytes()).hexdigest()
    disk=a.output/'fullscreen-baseline.img';shutil.copyfile(a.image,disk)
    programs=list(a.programs.glob('*.COM'))+list(a.programs.glob('*.EXE'))
    subprocess.run(['mcopy','-o','-i',str(disk),*[str(x) for x in programs],'::APPS/'],check=True)
    vm=WindowVM(disk,a.output,'std',memory=128,palette='platinum')
    report={'scope':'fullscreen only; no concurrent GUI acceptance',
            'source_image_sha256':source_hash,'memory_mib':128,'qemu':vm.process.args,
            'programs':{x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in programs},'cases':[]}
    try:
        vm.ready();off=vm.offset();vm.key('f4');vm.wait('[DESKTOP] DOS',off,30)
        for exe,kind,child in [('DWBIOST.COM','BIOS-COM','BIOS-CHILD'),
                               ('DWTEXT.EXE','DIRECT-MZ','DIRECT-CHILD')]:
            prefix=f'[DOSWIN:{kind}] '
            off=vm.offset();vm.text('run '+exe)
            vm.wait(prefix+'START psp=',off,30)
            vm.wait(prefix+'FILE OK',off,15)
            vm.wait(prefix+'LIVE ticks=',off,15)
            vm.shot(kind+'-live-early')
            vm.key('a');vm.wait(prefix+'KEY ax=1E61',off,15)
            vm.wait(prefix+'EXEC BEGIN',off,25)
            vm.wait(f'[DOSWIN:{child}] START psp=',off,15)
            vm.wait(f'[DOSWIN:{child}] END status=005A',off,15)
            vm.wait(prefix+'EXEC RETURN',off,15)
            print(exe+': nested EXEC and key input passed',flush=True)
            vm.wait(prefix+'FILE MIDRUN OK',off,20)
            vm.shot(kind+'-live-late')
            vm.wait(prefix+'END ticks=',off,30)
            vm.wait('CiukiOS SHELL C:\\APPS>',off,15)
            raw=vm.serial.read_bytes()[off:].decode('ascii','replace')
            assert prefix+'FAIL' not in raw,raw
            end=re.search(re.escape(prefix)+r'END ticks=([0-9A-F]{4}) frames=([0-9A-F]{4}) reason=TIMEOUT',raw)
            assert end and int(end[1],16)>=728 and int(end[2],16)>=300,raw
            live=[int(x,16) for x in re.findall(re.escape(prefix)+r'LIVE ticks=([0-9A-F]{4})',raw)]
            assert len(live)>=25 and live==sorted(set(live)),live
            parent=re.search(re.escape(prefix)+r'START psp=([0-9A-F]{4})',raw)[1]
            nested=re.search(re.escape(f'[DOSWIN:{child}] ')+r'START psp=([0-9A-F]{4})',raw)[1]
            assert parent!=nested,'nested child reused the parent PSP'
            report['cases'].append({'program':exe,'ticks':int(end[1],16),'frames':int(end[2],16),
                                    'live_samples':len(live),'parent_psp':parent,'child_psp':nested})
            print(exe+': full-duration fullscreen baseline passed',flush=True)
        report['passed']=True
    except Exception as e:
        report['passed']=False;report['error']=repr(e)
        try:vm.shot('failure')
        except Exception:pass
        raise
    finally:
        vm.close()
        (a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        assert hashlib.sha256(a.image.read_bytes()).hexdigest()==source_hash,'source image changed'
    disk_after=FAT16(disk)
    try:disk_after.read('APPS/DWCHECK.DAT')
    except (FileNotFoundError,KeyError):pass
    else:raise AssertionError('probe scratch file still exists')

if __name__=='__main__':main()
