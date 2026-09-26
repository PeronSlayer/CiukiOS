#!/usr/bin/env bash
set -euo pipefail
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 - "$root_dir" "$@" <<'PY'
import argparse, hashlib, json, os, pathlib, subprocess, sys
root=pathlib.Path(sys.argv[1])
p=argparse.ArgumentParser(description='Build the experimental V86 session JLM; no installation.')
p.add_argument('--jemm-build',type=pathlib.Path,default=root/'build/external/jemm-monitor')
p.add_argument('--output',type=pathlib.Path,default=root/'build/full/vm-session')
a=p.parse_args(sys.argv[2:]);base=a.jemm_build.resolve();out=a.output.resolve()
if root/'build' not in out.parents: p.error('--output must be inside the ignored build directory')
current=(base/'CURRENT').read_text().strip();jemm_output=(base/current).resolve()
if base not in jemm_output.parents: raise SystemExit('Invalid Jemm CURRENT path')
manifest=json.loads((jemm_output/'manifest.json').read_text());work=jemm_output.parent
meta=json.loads((root/'third_party/jemm/UPSTREAM.json').read_text())
if manifest['upstream'] != meta: raise SystemExit('Pinned Jemm build metadata mismatch')
tools={}
def tree_under(directory):
    trees=[entry for entry in directory.iterdir() if entry.is_dir()]
    if len(trees)!=1: raise SystemExit('Unexpected extracted source layout: '+str(directory))
    return trees[0]
for name,spec in meta['toolchains'].items():
    tree=tree_under(work/name)
    tools[name]=tree/spec['binary']
    if hashlib.sha256(tools[name].read_bytes()).hexdigest()!=manifest['tools'][name]['sha256']:
        raise SystemExit('Built tool hash mismatch: '+str(tools[name]))
include=tree_under(work/'jemm')/'Include'
out.mkdir(parents=True,exist_ok=True)
obj=out/'session.obj';binary=out/'CVSESSION.DLL'
subprocess.run([str(tools['jwasm']),'-coff','-c','-nologo','-I'+str(include),'-I'+str(root/'src/vm'),
                '-Fo'+str(obj),'-Fl'+str(out/'session.lst'),str(root/'src/vm/session_jlm.asm')],check=True)
subprocess.run([str(tools['jwlink']),'format','win','nt','hx','dll','ru','native','file',str(obj),
                'name',str(binary),'op','q,MAP='+str(out/'session.map'),'export','_ddb.1'],check=True)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
(out/'manifest.json').write_text(json.dumps({'abi':1,'runtime_tested':False,'jemm_commit':meta['commit'],
    'sources':{str(path.relative_to(root)):sha(path) for path in
               [root/'src/vm/session_jlm.asm',root/'src/vm/session_abi.inc',root/'scripts/build_vm_session.sh']},
    'module':{'bytes':binary.stat().st_size,'sha256':sha(binary)},'jemm_build_manifest_sha256':sha(jemm_output/'manifest.json')},indent=2)+'\n')
print(binary)
PY
