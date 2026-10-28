"""Runner slot locks, conservative host preflight and bounded retention."""
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


SLOT_DEFAULT=2
SLOT_MAX=4
SCOPE_PREFIX='ciuki-test-'


def slot_count():
    raw=os.environ.get('CIUKI_RUNNER_SLOTS')
    if raw is None or raw=='': return SLOT_DEFAULT
    if not re.fullmatch('[0-9]+',raw) or not 1<=int(raw)<=SLOT_MAX:
        raise Refusal(f'CIUKI_RUNNER_SLOTS must be an integer 1..{SLOT_MAX}')
    return int(raw)


def slot_paths(base, count=None):
    """Slot lock files next to the common lock path (.qemu.lock -> .qemu.slotN)."""
    if count is not None and (type(count) is not int or not 1 <= count <= SLOT_MAX):
        raise Refusal(f'slot namespace must contain 1..{SLOT_MAX} locks')
    base=Path(base);stem=base.name[:-5] if base.name.endswith('.lock') else base.name
    return [base.with_name(f'{stem}.slot{i}') for i in range(slot_count() if count is None else count)]


class SlotLock(ExclusiveLock):
    """Takes the first free runner slot; at most one process owns each slot."""
    current=None
    def __init__(self,base): self.base=Path(base);self.index=None;self.fd=None;self.path=None
    def __enter__(self):
        paths=slot_paths(self.base);paths[0].parent.mkdir(parents=True,exist_ok=True)
        for _ in range(3):
            for index,path in enumerate(paths):
                fd=os.open(path,os.O_CREAT|os.O_RDWR,0o600)
                try: fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
                except BlockingIOError: os.close(fd);continue
                self.fd=fd;self.path=path;self.index=index;SlotLock.current=self
                return self
            time.sleep(.05)
        raise Refusal(f'all {len(paths)} runner slots are busy')
    def __exit__(self,*args):
        if SlotLock.current is self: SlotLock.current=None
        super().__exit__(*args)


def other_slots_held(slot=None):
    """Number of runner slots, other than ours, currently locked by someone."""
    slot=slot or SlotLock.current
    if slot is None: return 0
    held=0
    for path in slot_paths(slot.base):
        if path==slot.path: continue
        try: fd=os.open(path,os.O_CREAT|os.O_RDWR,0o600)
        except OSError: continue
        try:
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);fcntl.flock(fd,fcntl.LOCK_UN)
        except BlockingIOError: held+=1
        finally: os.close(fd)
    return held


def qemu_count(unit=None):
    """QEMU processes outside every runner scope (foreign); with a unit, only
    those inside that exact scope. Other slots' guests are never counted as foreign."""
    found=0
    for p in Path('/proc').glob('[0-9]*'):
        try:
            if not (p/'comm').read_text().startswith('qemu-system'): continue
            groups=(p/'cgroup').read_text()
        except OSError: continue
        if unit is not None: found+=unit in groups
        else: found+=SCOPE_PREFIX not in groups
    return found


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
    memory=available_memory();others=other_slots_held()
    if memory < 2*GIB*(1+others): raise Refusal(f'MemAvailable is below {2*(1+others)} GiB ({others} other runner slot(s) running)')
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
