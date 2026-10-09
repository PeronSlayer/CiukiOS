#!/usr/bin/env python3
"""Build pinned Doomgeneric and a genuine DOS/DPMI graphics bridge exercise.

This produces a new silent source-port executable. It never replaces existing
fullscreen Doom or its audio launchers and never modifies a disk image.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,default=ROOT/'build/full/doom-window')
    ap.add_argument('--watcom',type=Path,default=Path(os.environ.get('WATCOM','/opt/watcom')))
    ap.add_argument('--smoke-only',action='store_true')
    args=ap.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    port=ROOT/'src/ports/doomgeneric'
    package=ROOT/'third_party/doomgeneric'
    meta=json.loads((package/'UPSTREAM.json').read_text())
    archive=package/meta['archive']
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==meta['sha256'],'upstream archive mismatch'
    work=out/'source'
    work.mkdir(exist_ok=True)
    with tarfile.open(archive) as tf:tf.extractall(work,filter='data')
    source=work/'doomgeneric'
    subprocess.run(['patch','--batch','-p1','-i',str(port/'patches/openwatcom.patch')],cwd=source,check=True)
    game=source/'doomgeneric'
    compiler=args.watcom/'binl64/wcc386'
    linker=args.watcom/'binl64/wlink'
    env=os.environ.copy();env['WATCOM']=str(args.watcom);env['INCLUDE']=str(args.watcom/'h')
    env['PATH']=str(args.watcom/'binl64')+os.pathsep+env.get('PATH','')
    objects=out/'obj';objects.mkdir(exist_ok=True)
    common=['-q','-bt=dos','-za99','-5','-zp1','-ox','-dCIUKIOS=1','-dCMAP256',
            '-dDOOMGENERIC_RESX=320','-dDOOMGENERIC_RESY=200',
            '-i='+str(port/'compat'),'-i='+str(port),'-i='+str(game)]
    log=out/'build.log'
    with log.open('w') as fh:
        def compile_file(path):
            obj=objects/(path.stem+'.obj')
            cmd=[str(compiler),*common,'-fo='+str(obj),'-fr='+str(objects/(path.stem+'.err')),str(path)]
            fh.write(' '.join(cmd)+'\n');fh.flush()
            result=subprocess.run(cmd,env=env,stdout=fh,stderr=subprocess.STDOUT)
            if result.returncode:raise RuntimeError(f'Compilation failed: {path.name}; see {log}')
            return obj
        def link(name,objs):
            response=out/(name+'.lnk')
            response.write_text('system dos4g\noption quiet\noption stack=65536\nname '+str(out/name)+
                '\noption map='+str(out/(name+'.map'))+'\n'+''.join('file '+str(o)+'\n' for o in objs))
            result=subprocess.run([str(linker),'@'+str(response)],env=env,stdout=fh,stderr=subprocess.STDOUT)
            if result.returncode:raise RuntimeError(f'Link failed: {name}; see {log}')
        bridge=compile_file(port/'graphics_bridge.c')
        smoke=compile_file(port/'graphics_smoke.c')
        link('CGSMOKE.EXE',[bridge,smoke])
        if not args.smoke_only:
            names=re.search(r'^SRC_DOOM\s*=\s*(.+)$',(game/'Makefile').read_text(),re.M).group(1).split()
            names=[n[:-2]+'.c' for n in names if n!='doomgeneric_xlib.o']
            objs=[compile_file(game/n) for n in names]
            objs.extend([bridge,compile_file(port/'doomgeneric_ciuki.c')])
            link('DOOMWIN.EXE',objs)
    report={'upstream':meta,'runtime_tested':False,'audio':'silent source port',
            'artifacts':{p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                         for p in out.glob('*.EXE')}}
    (out/'build.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report['artifacts'],indent=2))

if __name__=='__main__':main()
