#!/usr/bin/env python3
"""H2 self-authored NT-equal heap/string fixture; no payloads or Wine test copies.

Wine runs once in an isolated prefix, sequentially, and its server is stopped.
Never use this to bless Windows 98 differences. H1 must run separately before
the comparison can claim agreement; an absent H1 result is reported as pending.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright 2026 CiukiOS contributors.
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[4]
sys.path.insert(0,str(ROOT))
from tests.host.win32.adapter import build
from scripts.win32 import stubs
ADAPTER=Path(__file__).resolve().parent

PROBES={'oracle':'self-authored NT-equal heap/string','seh_probe':'self-authored x86 SEH dispatch/unwind/faults'}

def build_fixture(out,name='oracle'):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    build.run(['clang','--target=i686-pc-windows-msvc','-march=pentiumpro','-ffreestanding',
               '-fno-stack-protector','-mno-sse','-mno-sse2','-O1','-c',ADAPTER/(name+'.c'),'-o',out/(name+'.obj')])
    build.run(['llvm-dlltool','-m','i386','-k','-d',ADAPTER/(name+'.def'),'-l',out/(name+'-import.lib')])
    build.run(['lld-link','/machine:x86','/nodefaultlib','/safeseh:no','/entry:entry','/subsystem:console,4.0',
               '/timestamp:0','/incremental:no','/out:'+str(out/(name+'.exe')),out/(name+'.obj'),out/(name+'-import.lib')])
    return out/(name+'.exe')

def run_wine(image,out,timeout=45):
    env=os.environ.copy();env.update(WINEPREFIX=str(Path(out)/'wine-prefix'),WINEDEBUG='-all',
                                    WINEDLLOVERRIDES='winemenubuilder.exe=d')
    try:
        process=subprocess.Popen(['wine',str(image)],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
        try:
            stdout,stderr=process.communicate(timeout=timeout)
            return {'exit':process.returncode,'stdout':stdout[-4096:],'stderr':stderr[-4096:]}
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGKILL);stdout,stderr=process.communicate()
            return {'timeout':True,'stdout':stdout[-4096:],'stderr':stderr[-4096:]}
    except OSError as e: return {'unavailable':str(e)}
    finally:
        try: subprocess.run(['wineserver','-k'],env=env,timeout=10,capture_output=True)
        except (OSError,subprocess.TimeoutExpired): pass

def compare(out,adapter=None,core_directory=None,timeout=45,probe='oracle'):
    """Run one self-authored probe under Wine and under the adapter with the PE
    ntdll. Agreement means both exit 0 (every check passed) in both runs."""
    out=Path(out).resolve();image=build_fixture(out,probe)
    result={'fixture':PROBES[probe],'wine_version':None,'wine':None,'adapter':None,'agreement':False}
    try: result['wine_version']=subprocess.run(['wine','--version'],capture_output=True,text=True,timeout=10).stdout.strip()
    except (OSError,subprocess.TimeoutExpired) as e: result['wine']={'unavailable':str(e)}
    if result['wine'] is None: result['wine']=run_wine(image,out,timeout)
    if adapter and core_directory:
        stubs.verify_store(Path(core_directory)/'builtin-store.json')
        p=subprocess.run([str(adapter),'exec-core',str(image),str(core_directory)],capture_output=True,text=True,timeout=15)
        result['adapter']={'exit':p.returncode,'stdout':p.stdout[-4096:],'stderr':p.stderr[-4096:]}
        result['agreement']=result['wine'].get('exit')==0 and p.returncode==0 and 'RESULT 0' in p.stdout
    (out/(probe+'-comparison.json')).write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    return result

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',type=Path,default=ROOT/'build/host/win32/oracle')
    ap.add_argument('--adapter',type=Path);ap.add_argument('--core-directory',type=Path)
    ap.add_argument('--probe',choices=sorted(PROBES),default='oracle');args=ap.parse_args()
    print(json.dumps(compare(args.output,args.adapter,args.core_directory,probe=args.probe),indent=2))
