"""Shared lock, conservative host preflight and bounded retention."""
import fcntl
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

GIB = 1024**3
LOG_CAP = 4*1024**2
MEMORY_MAX = 1500*1024**2


class Refusal(RuntimeError):
    pass


def common_lock(root):
    common = Path(subprocess.check_output(['git','rev-parse','--path-format=absolute','--git-common-dir'],cwd=root,text=True).strip()).resolve()
    # Ordinary main checkout .git (also works from linked worktrees).
    if common.name != '.git':
        raise Refusal('cannot determine main checkout from git common directory')
    return common.parent/'build/test-runs/.qemu.lock'


class ExclusiveLock:
    def __init__(self,path): self.path=Path(path);self.fd=None
    def __enter__(self):
        self.path.parent.mkdir(parents=True,exist_ok=True)
        self.fd=os.open(self.path,os.O_CREAT|os.O_RDWR,0o600)
        try: fcntl.flock(self.fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(self.fd);self.fd=None
            raise Refusal('QEMU lock is owned by another launcher')
        return self
    def __exit__(self,*args):
        if self.fd is not None: os.close(self.fd);self.fd=None


def qemu_count():
    result=subprocess.run(['pgrep','-c','qemu-system'],capture_output=True,text=True,timeout=5)
    if result.returncode not in (0,1): raise Refusal('cannot check existing QEMU processes')
    return int(result.stdout.strip() or '0')


def heavy_builds():
    found=[]
    for p in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            args=p.read_bytes().split(b'\0');name=Path(args[0].decode()).name
            words=[x.decode(errors='replace') for x in args]
        except (OSError,IndexError): continue
        if name in ('make','gmake') and any(w in ('build-full','build-full-cd') for w in words) or any(Path(w).name in ('build_full.sh','build_full_cd.sh','build_full_f0.sh') for w in words[1:]):
            found.append(int(p.parent.name))
    return found


def available_memory():
    match=re.search(r'^MemAvailable:\s+(\d+) kB$',Path('/proc/meminfo').read_text(),re.M)
    if not match: raise Refusal('MemAvailable unavailable')
    return int(match[1])*1024


def usage(root):
    logical=allocated=0
    for p in Path(root).rglob('*'):
        if p.is_symlink(): raise Refusal('symlink in test-runs')
        if p.is_file():
            stat=p.stat();logical+=stat.st_size;allocated+=stat.st_blocks*512
    return {'logical':logical,'allocated':allocated}


def check_budget(root):
    sizes=usage(root)
    if max(sizes.values()) >= 2*GIB: raise Refusal('test-runs disk budget reached (2 GiB)')
    return sizes


def preflight(root, wait_seconds=1200):
    # Refuse contention under the lock. An unowned manual QEMU may finish;
    # poll at the owner's required 30-second interval, never kill it.
    until=time.monotonic()+wait_seconds
    while qemu_count():
        if time.monotonic() >= until: raise Refusal('another qemu-system is running')
        print('Another qemu-system is running; waiting 30 seconds.',flush=True)
        time.sleep(min(30,max(0,until-time.monotonic())))
    builds=heavy_builds()
    if builds: raise Refusal('heavy build active: '+str(builds))
    memory=available_memory()
    if memory < 2*GIB: raise Refusal('MemAvailable is below 2 GiB')
    check_budget(root)
    return memory


def prune(suite_root, keep=5):
    runs=sorted((p for p in Path(suite_root).iterdir() if p.is_dir()),key=lambda p:p.stat().st_mtime_ns,reverse=True)
    for p in runs[keep:]: shutil.rmtree(p)


def scope_properties(unit):
    cmd=['systemctl','--user','show',unit,'--property=MemoryMax,MemorySwapMax,ControlGroup,Result']
    result=subprocess.run(cmd,capture_output=True,text=True,timeout=3)
    if result.returncode: raise Refusal('cannot inspect systemd user scope')
    return dict(l.split('=',1) for l in result.stdout.splitlines() if '=' in l)


def verify_scope(unit):
    props=scope_properties(unit)
    if props.get('MemoryMax')!=str(MEMORY_MAX) or props.get('MemorySwapMax')!='0':
        raise Refusal('effective systemd scope limits are not 1500M/0')
    group=props.get('ControlGroup','')
    if not group.startswith('/') or '..' in group.split('/'):
        raise Refusal('invalid or missing scope cgroup')
    path=Path('/sys/fs/cgroup')/group.lstrip('/')
    try:
        actual={'memory.max':(path/'memory.max').read_text().strip(),
                'memory.swap.max':(path/'memory.swap.max').read_text().strip()}
    except OSError as e: raise Refusal('functioning cgroup v2 memory controller required') from e
    if actual!={'memory.max':str(MEMORY_MAX),'memory.swap.max':'0'}:
        raise Refusal('cgroup effective limits mismatch')
    return {**props,**actual},path
