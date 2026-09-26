#!/usr/bin/env python3
"""Build the pinned silent Wolf4SDL source port with Ciuki indexed surfaces."""
import argparse,hashlib,json,os,re,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',type=Path,default=ROOT/'build/full/wolf-window')
    args=ap.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    package=ROOT/'third_party/wolf4sdl';meta=json.loads((package/'UPSTREAM.json').read_text())
    archive=package/meta['archive'];assert hashlib.sha256(archive.read_bytes()).hexdigest()==meta['sha256']
    with tarfile.open(archive) as tf:tf.extractall(out,filter='data')
    source=out/'wolf4sdl';port=ROOT/'src/ports/wolfwindow';bridge=ROOT/'src/ports/doomgeneric'
    def patch(name,old,new):
        p=source/name;s=p.read_text();assert old in s,(name,old);p.write_text(s.replace(old,new))
    patch('id_vl.cpp','unsigned screenWidth = 640;','unsigned screenWidth = 320;')
    patch('id_vl.cpp','unsigned screenHeight = 400;','unsigned screenHeight = 200;')
    patch('id_vl.cpp','boolean aspect = true;','boolean aspect = false;')
    patch('wl_menu.cpp','mkdir(configdir, 0755)','mkdir(configdir)')
    patch('wl_main.cpp','char    configdir[256] = "";','char    configdir[256] = "WINCFG";')
    for name in ('wl_main.cpp','wl_menu.cpp'):
        patch(name,'"%s/%s"','"%s\\\\%s"')
    watcom=Path(os.environ.get('WATCOM','/opt/watcom'))
    env=os.environ.copy();env.update(WATCOM=str(watcom),INCLUDE=str(watcom/'h'))
    env['PATH']=str(watcom/'binl64')+os.pathsep+env.get('PATH','')
    objects=out/'obj';objects.mkdir(exist_ok=True);log=out/'build.log'
    names=re.findall(r'^SRCS \+= ([\w.]+)$',(source/'Makefile').read_text(),re.M)
    files=[source/n for n in names if n not in ('id_sd.cpp','opl3.c')]
    files += [port/'platform.cpp',port/'sound_absent.cpp',bridge/'graphics_bridge.c']
    with log.open('w') as f:
        for p in files:
            compiler='wcc386' if p.suffix=='.c' else 'wpp386'
            flags=['-za99'] if p.suffix=='.c' else ['-xs']
            cmd=[str(watcom/'binl64'/compiler),'-q','-bt=dos','-5','-zp1','-ox',*flags,
                 '-i='+str(port/'compat'),'-i='+str(bridge),'-i='+str(source),
                 '-fo='+str(objects/(p.stem+'.obj')),str(p)]
            f.write(' '.join(cmd)+'\n');f.flush()
            result=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT)
            if result.returncode:raise RuntimeError(f'Compile failed: {p.name}; see {log}')
        response=out/'wolf.lnk'
        response.write_text('system dos4g\noption quiet\noption stack=65536\nname '+str(out/'WOLFWIN.EXE')+
            '\noption map='+str(out/'wolf.map')+'\n'+''.join('file '+str(objects/(p.stem+'.obj'))+'\n' for p in files))
        result=subprocess.run([str(watcom/'binl64/wlink'),'@'+str(response)],env=env,stdout=f,stderr=subprocess.STDOUT)
        if result.returncode:raise RuntimeError(f'Link failed; see {log}')
    binary=out/'WOLFWIN.EXE'
    report=dict(upstream=meta,bytes=binary.stat().st_size,sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        runtime_tested=False,audio='absent',hardware_acceleration=False)
    (out/'build.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
