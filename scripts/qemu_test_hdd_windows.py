#!/usr/bin/env python3
"""Exercise Win16 applications, repaint, WAV/MIDI and return on an installed HDD."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
from PIL import Image
from analyze_audio_wav import pcm_payload
from qemu_test_installed_hdd import InstalledVM, FAT16
from qemu_test_full_display_profile import VM


class WindowsVM(InstalledVM):
    def __init__(self, disk, output, audio='ac97', memory=512):
        devices = ['-device','AC97,audiodev=snd'] if audio == 'ac97' else [
            '-device','sb16,iobase=0x220,irq=7,dma=1,dma16=5,audiodev=snd',
            '-device','adlib,audiodev=snd']
        VM.__init__(self,disk,output,(
            '-m',str(memory),
            '-audiodev',f'wav,id=snd,path={output}/audio.wav', *devices,
            '-debugcon',f'file:{output}/debug.bin'))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, default=Path('build/full/t23-runtime-repair-2026-09-06/final-install/target.img'))
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--kernel', type=Path)
    ap.add_argument('--shell', type=Path)
    ap.add_argument('--audio', choices=('ac97','sb16'), default='ac97')
    ap.add_argument('--memory', type=int, default=512)
    ap.add_argument('--resolution', choices=('safe','800'), default='800')
    ap.add_argument('--banked', action='store_true', help='Diagnostic VBE banked profile')
    ap.add_argument('--unbuffered', action='store_true', help='Diagnostic VBE timer-free profile')
    args = ap.parse_args()
    if not os.environ.get('TESSDATA_PREFIX') and Path('build/tools/tessdata/eng.traineddata').exists():
        os.environ['TESSDATA_PREFIX'] = str(Path('build/tools/tessdata').resolve())
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'installed.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    for source, target in ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'), (args.shell, 'SYSTEM/SHELL.COM')):
        if source:
            subprocess.run(['mcopy','-o','-i',volume,str(source),'::'+target], check=True)
    profile = out/'DISPLAY.CFG'
    profile.write_bytes(b'0800')
    subprocess.run(['mcopy','-o','-i',volume,str(profile),'::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    if args.banked or args.unbuffered:
        ini=FAT16(disk).read('WINDOWS/SYSTEM.800')
        if args.banked: ini=ini.replace(b'PreferBankedModes=0',b'PreferBankedModes=1')
        if args.unbuffered: ini=ini.replace(b'SwapBuffersInterval=16',b'SwapBuffersInterval=0')
        modified=out/'SYSTEM.800';modified.write_bytes(ini)
        subprocess.run(['mcopy','-o','-i',volume,str(modified),'::WINDOWS/SYSTEM.800'],check=True)
    vm = WindowsVM(disk, out, args.audio, args.memory)
    launch_marker = ('VSBHDA transient child:' if args.audio == 'ac97'
                     else '[AUDIO] Native Sound Blaster - starting application')
    intervals = []

    def screen(name, required=()):
        path = vm.shot(name)
        # Keep the original screenshot. A separate thresholded copy lets OCR
        # distinguish the small black Win16 font from its blue title bar.
        pixels = np.array(Image.open(path))
        ocr_path = out/(name+'-ocr.png')
        Image.fromarray(np.where(pixels.min(axis=2)<60,0,255).astype('uint8')).resize(
            (pixels.shape[1]*3,pixels.shape[0]*3)).save(ocr_path)
        text = subprocess.check_output(['tesseract',str(ocr_path),'stdout','--psm','11'], stderr=subprocess.DEVNULL).decode()
        (out/(name+'.txt')).write_text(text)
        for expected in required:
            assert ''.join(expected.lower().split()) in ''.join(text.lower().split()), (name, expected, text)
        assert 'impossibile' not in text.lower() and 'errore nel file' not in text.lower(), (name, text)
        return np.array(Image.open(path))

    def run(command):
        vm.key('alt-f'); time.sleep(.5)
        vm.key('e'); time.sleep(.5)
        vm.text(command); time.sleep(4)

    def pcm_mark():
        return max(0, (out/'audio.wav').stat().st_size - 44)

    def active_window(pixels):
        blue = ((pixels[:,:,0]>=140)&(pixels[:,:,0]<=190)&
                (pixels[:,:,1]>=180)&(pixels[:,:,1]<=220)&(pixels[:,:,2]>=230))
        blue |= (pixels[:,:,0]<60)&(pixels[:,:,1]<60)&(pixels[:,:,2]>=100)
        rows = np.flatnonzero(blue.sum(axis=1)>100)
        assert len(rows), 'active Win16 title bar is missing'
        y = int(rows[0]); xs = np.flatnonzero(blue[y])
        return int(xs[0])-20, y-2, int(xs[-1])-int(xs[0])

    def calculator_value(pixels, name):
        x,y,_ = active_window(pixels)
        display = Image.fromarray(pixels[y+48:y+71,x+46:x+243])
        crop = out/(name+'-display.png')
        display.resize((788,92),Image.Resampling.NEAREST).save(crop)
        value = subprocess.check_output(['tesseract',str(crop),'stdout','--psm','7',
            '-c','tessedit_char_whitelist=0123456789,.-'],stderr=subprocess.DEVNULL).decode().strip()
        (out/(name+'-display.txt')).write_text(value)
        return value.rstrip(',.')

    try:
        vm.wait('[DESKTOP] READY',timeout=90)
        vm.key('f4'); vm.wait('CiukiOS SHELL C:\\APPS>')
        vm.result('vgasetup win '+args.resolution,'profile installed; restart Windows',timeout=90)
        offset=vm.offset(); vm.text('win')
        vm.wait(launch_marker,offset,90)
        time.sleep(20)
        before=screen('program-manager',('Program Manager',))
        assert before.shape[:2] == ((480,640) if args.resolution=='safe' else (600,800)), before.shape
        # Resize the top-level Program Manager using its system menu. The
        # original pixels outside the new rectangle must be repainted too.
        vm.key('alt-spc'); time.sleep(.5)
        screen('system-menu',('Ridimensiona',))
        vm.key('d'); vm.key('right')
        for _ in range(6): vm.key('right')
        vm.key('down')
        for _ in range(4): vm.key('down')
        vm.key('ret'); time.sleep(2)
        after=screen('resized',('Program Manager',))
        assert np.count_nonzero(np.any(before != after,axis=2)) > 1000, 'resize did not change the window'
        assert active_window(after)[2] > active_window(before)[2]+20, 'window frame did not grow'
        # Open/close a child, then verify the same background is reconstructed.
        run('calc')
        calculator = screen('calculator',('Calcolatrice',))
        assert calculator_value(calculator,'calculator') == '0', 'unexpected initial calculator value'
        vm.key('2'); vm.key('kp_add'); vm.key('3'); vm.key('ret'); time.sleep(1)
        calculator = screen('calculator-result',('Calcolatrice',))
        assert calculator_value(calculator,'calculator-result') == '5', '2+3 did not produce 5 in the display'
        vm.key('alt-f4'); time.sleep(2)
        restored=screen('calculator-closed',('Program Manager',))
        # Ignore pointer pixels/blink; extensive unrepainted client regions fail.
        difference=np.count_nonzero(np.any(after != restored,axis=2))
        assert difference < 800, f'child close left {difference} damaged background pixels'
        run('soundrec tada.wav')
        screen('soundrec',('Registratore',))
        start=pcm_mark(); vm.key('ret'); time.sleep(5)
        end=pcm_mark(); screen('soundrec-played',('Registratore',))
        intervals.append(('wave',start+17640,end-17640))
        vm.key('alt-f4'); time.sleep(2)
        screen('wave-closed',('Program Manager',))
        run('mplayer canyon.mid')
        screen('midi-open',('multimediale',))
        start=pcm_mark(); vm.key('spc'); time.sleep(4)
        a=screen('midi-playing-a',('multimediale',))
        time.sleep(5)
        b=screen('midi-playing-b',('multimediale',))
        end=pcm_mark()
        x,y,_ = active_window(a)
        track = np.any(a[y+66:y+82,x+18:x+373] != b[y+66:y+82,x+18:x+373],axis=2)
        assert np.count_nonzero(track)>20, 'MIDI transport thumb did not advance'
        intervals.append(('midi',start+352800,end-17640))
        vm.key('alt-f'); time.sleep(.5); vm.key('s'); time.sleep(2)
        screen('midi-closed',('Program Manager',))
        offset=vm.offset(); vm.key('alt-f4'); time.sleep(1); vm.key('ret')
        vm.wait('CiukiOS SHELL C:\\APPS>',offset,90)
        vm.result('comdemo','COM demo via INT21h')
        vm.result('dir \\windows','WINCORE.COM','WIN.COM',timeout=90)
        offset=vm.offset();vm.text('win');vm.wait(launch_marker,offset,90)
        time.sleep(20);screen('second-windows',('Program Manager',))
        offset=vm.offset();vm.key('alt-f4');time.sleep(1);vm.key('ret')
        vm.wait('CiukiOS SHELL C:\\APPS>',offset,90)
        vm.result('echo WINDOWS RETURN READY','WINDOWS RETURN READY')
    except Exception:
        vm.shot('failure')
        (out/'registers.log').write_bytes(vm.hmp('info registers'))
        vm.hmp(f'pmemsave 0 0x100000 "{out}/failure-ram.bin"')
        raise
    finally:
        vm.close()
    pcm,_=pcm_payload((out/'audio.wav').read_bytes())
    measured=[]
    for name,start,end in intervals:
        samples=np.frombuffer(pcm[start&~1:end&~1],dtype='<i2').astype(float)
        assert len(samples)>24000, (name,'missing capture interval')
        rms=float(np.std(samples));peak=float(np.max(np.abs(samples)))
        assert rms>20 and peak>200, (name,'no application audio',rms,peak)
        measured.append({'application':name,'rms':rms,'peak':peak,'samples':len(samples)})
    (out/'result.json').write_text(json.dumps({'audio':measured,'repaint_changed_pixels':int(difference)},indent=2))
    print('[installed-windows] PASS real applications, resize/repaint, WAV, MIDI, child close and two Windows sessions',flush=True)


if __name__=='__main__':main()
