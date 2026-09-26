#!/usr/bin/env python3
"""Launch the two source ports from their actual application-library tiles."""
import argparse,json,shutil,hashlib,time
from pathlib import Path
from qemu_test_graphics_window import Graphics
from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--listing',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    assert not (a.output/'report.json').exists()
    disk=a.output/'private.img';shutil.copyfile(a.image,disk)
    report=dict(passed=False,events=[],guest_memory_writes=False,
        image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),games=[])
    vm=WindowVM(disk,a.output,'std',memory=128,palette='platinum')
    try:
        vm.ready();ui=Graphics(vm,FAT16(disk).read('SYSTEM/SHELL.COM'),a.listing,report)
        for game,tile in (('Doom',33),('Wolf',34)):
            ui.click(1);ui.click(21,0);ui.screen(game+'-library')
            ui.click(tile,0);offset=vm.offset();vm.key('ret')
            ui.until(lambda:ui.b('dos_host_active')==1,'library did not start a DOS window',20)
            ui.segment=ui.w('dw_segment')
            state=ui.await_graphics(game+' library launch became visible',90)
            vm.wait('[CGFX] '+game+' source port 320x200',offset,10)
            time.sleep(.5);ui.screen(game+'-library-running')
            ui.click(18,11);ui.finish(game+' library close')
            report['games'].append(dict(game=game,tile=tile,state=state,clean_close=True))
        report['passed']=True
    except Exception as e:
        report['error']=repr(e)
        try:ui.screen('failure')
        except Exception:pass
        raise
    finally:
        vm.close();(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
