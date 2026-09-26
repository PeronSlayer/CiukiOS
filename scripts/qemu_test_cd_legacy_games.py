#!/usr/bin/env python3
import argparse,os,sys,time
from pathlib import Path
import numpy as np
from PIL import Image
sys.path.insert(0,str(Path('scripts').resolve()))
from qemu_test_native_desktop import DesktopVM
from analyze_audio_wav import pcm_payload
ap=argparse.ArgumentParser(description='Exercise Wolf3D and DoomVan gameplay, audio and return from the release CD.')
ap.add_argument('--iso',type=Path,default=Path('build/full/CiukiOS_full_cd_0-7-1.iso'))
ap.add_argument('--output',type=Path,required=True)
args=ap.parse_args()
os.environ.setdefault('CIUKIOS_SETUP_QEMU_ACCEL','kvm')
out=args.output.resolve()
v=DesktopVM([],args.iso.resolve(),out)
audio_windows=[]
try:
 v.ready();v.dos()
 for name,cmd,cleanup in [('wolf3d','wolf3d nowait tedlevel 0 normal','[WOLF3D] AUDIO CLEANUP COMPLETE'),('doomvan','doomvan -devparm -warp 1 1','[DOOMVAN] AUDIO CLEANUP COMPLETE')]:
  start=(out/'audio.wav').stat().st_size
  off=v.offset();v.text(cmd)
  v.wait('VSBHDA transient child:',off,90)
  time.sleep(15)
  deadline=time.monotonic()+60
  while True:
   a=np.array(Image.open(v.shot(name+'-game')))
   if a.shape[:2] in ((200,320),(400,640)) and len(np.unique(a.reshape(-1,3),axis=0))>32:break
   assert time.monotonic()<deadline,'game screen missing'
   time.sleep(1)
  v.hmp('sendkey up 1200');time.sleep(2)
  b=np.array(Image.open(v.shot(name+'-walk')))
  assert a.shape==b.shape
  region=slice(0,int(a.shape[0]*.75))
  changed=np.any(a[region]!=b[region],axis=2).sum()
  assert changed>a.shape[0]*a.shape[1]*.01,f'{name} view did not respond: {changed}'
  v.hmp('sendkey ctrl 700');time.sleep(2)
  v.hmp('mouse_move 30 0 0');time.sleep(1);v.shot(name+'-input')
  v.hmp('sendkey f10 120');time.sleep(1);v.shot(name+'-quit')
  v.hmp('sendkey y 120')
  v.wait(cleanup,off,60)
  v.wait('CiukiOS SHELL D:\\APPS>',off,60)
  v.command('comdemo','COM demo via INT21h')
  v.desktop();v.pointer();v.shot(name+'-return',(1280,800));v.dos()
  end=(out/'audio.wav').stat().st_size
  audio_windows.append((name,start,end))
  print(f'[legacy-games] PASS {name}: rendered gameplay, movement, quit, COM execution, desktop return',flush=True)
except:
 v.shot('failure');(out/'registers.log').write_bytes(v.hmp('info registers'));raise
finally:v.close()
raw=(out/'audio.wav').read_bytes()
for name,start,end in audio_windows:
 data=raw[max(44,start+4096):end-4096]
 data=data[:len(data)//4*4]
 samples=np.frombuffer(data,dtype='<i2').astype(float)
 assert len(samples)>5000
 rms=np.std(samples);assert rms>20,(name,rms)
 print(f'[legacy-games] PASS {name} PCM RMS={rms:.1f}',flush=True)
