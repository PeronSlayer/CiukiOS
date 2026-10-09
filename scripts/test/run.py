#!/usr/bin/env python3
"""Canonical-image F0 runner. Python standard library only; never builds images."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import secrets
import select
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parent))
from evidence import Parser, EvidenceError
from loader_model import selector
from qmp import QMP, writes
import resources as res

ROOT=Path(__file__).resolve().parents[2]


def utc(): return datetime.now(timezone.utc).isoformat()


def sha(path):
    digest=hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):digest.update(block)
    return digest.hexdigest()


def git_identity(root):
    try:
        revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
        dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=root))
        return {'revision':revision,'dirty':dirty}
    except subprocess.CalledProcessError:
        return {'revision':'unknown','dirty':'unknown'}


def load(path):
    with Path(path).open() as stream: data=json.load(stream)
    if data.get('schema_version')!=1: raise res.Refusal('unsupported JSON schema: '+str(path))
    return data


class Host:
    """Production boundary; host tests substitute only this boundary."""
    def preflight(self,root): return res.preflight(root)
    def launch(self,args,cwd):
        return subprocess.Popen(args,cwd=cwd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
    def verify(self,unit): return res.verify_scope(unit)
    def connect_qmp(self,path,log): return QMP(path,log)
    def unowned_qemu_count(self): return res.qemu_count()
    def kill_scope(self,unit,sig):
        subprocess.run(['systemctl','--user','kill','--kill-whom=all','--signal='+sig,unit],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=3)
    def scope_pids(self,path):
        if path is None or not path.exists(): return []
        result=[]
        for p in [path/'cgroup.procs',*path.glob('**/cgroup.procs')]:
            try:result.extend(int(v) for v in p.read_text().split())
            except FileNotFoundError:continue
        return sorted(set(result))


def teardown(host,unit,process,qmp,cgroup):
    result={'qmp_quit':False,'terminated_scope':False,'killed_scope':False,'remaining_children':[], 'clean':False}
    if process is None:
        result['clean']=True;return result
    if qmp:
        result['qmp_quit_requested']=True
        try:qmp.command('quit');result['qmp_quit']=True
        except (OSError,RuntimeError,ValueError):pass
    def finished(): return process.poll() is not None and not host.scope_pids(cgroup)
    def wait_for(seconds):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            if finished(): return True
            time.sleep(.025)
        return finished()
    if not wait_for(2):
        try:host.kill_scope(unit,'SIGTERM')
        except (OSError,subprocess.SubprocessError) as error:result.setdefault('kill_errors',[]).append(str(error))
        result['terminated_scope']=True
        # Also reap the systemd-run wrapper we own (not another launcher).
        if process.poll() is None:
            try:os.killpg(process.pid,signal.SIGTERM)
            except ProcessLookupError:pass
        if not wait_for(2):
            try:host.kill_scope(unit,'SIGKILL')
            except (OSError,subprocess.SubprocessError) as error:result.setdefault('kill_errors',[]).append(str(error))
            result['killed_scope']=True
            try:os.killpg(process.pid,signal.SIGKILL)
            except ProcessLookupError:pass
            wait_for(2)
    try:process.wait(timeout=1)
    except subprocess.TimeoutExpired:pass
    result['remaining_children']=host.scope_pids(cgroup)
    result['wrapper_returncode']=process.poll()
    result['clean']=finished()
    return result


def qemu_args(executable,profile,case,run_id,overlay,firmware):
    if 'loader_options' in case:raise res.Refusal('loader_options are unsupported; use the selector')
    request=case.get('selector',f"f0:{case['probe']} run={{run_id}}").format(run_id=run_id)
    requested=selector(request,'fw_cfg',True)
    if requested['probe']!=case['probe']:raise res.Refusal('suite selector/probe mismatch')
    request=f"f0:{requested['probe']} run={requested['run']}"
    platform=profile.get('platform') or requested['platform']
    if platform:request+=' platform='+platform
    if requested['safe']:request+=' safe=1'
    selector(request,'fw_cfg',True)
    devices=case.get('device_exceptions',{})
    args=[executable,'-machine',profile['machine'],'-cpu',profile['cpu'],'-accel',profile['accelerator'],
          '-m',str(profile['ram_mib']),'-smp','1','-bios',str(firmware),'-display','none',
          '-monitor','none','-nic','none','-no-shutdown','-S',
          '-drive','file='+str(overlay).replace(',',',,')+',format=qcow2,if=ide,index=0,media=disk',
          '-vga',devices.get('vga',profile['vga']),'-qmp','unix:q,server=on,wait=off',
          '-fw_cfg','name=opt/it.alcybercloud.ciukios/test,string='+request]
    # -no-reboot turns a host system_reset into a shutdown (QEMU 'SHUTDOWN
    # reason=host-qmp-system-reset'), so warm-restart cases omit it; guest
    # resets are still caught by the RESET event count against expected_resets.
    if case.get('boot_kind')!='restart': args.append('-no-reboot')
    # Instruction counting makes guest time independent of host stalls and
    # TCG translation work (f0-acceptance.md, "Emulation profiles"); it is a
    # TCG-only option, so the fast KVM profile never sets it.
    if profile.get('icount'):
        if profile['accelerator']!='tcg':raise res.Refusal('icount requires the tcg accelerator')
        args+=['-icount',profile['icount']]
    # A FIFO file backend keeps COM1 file output bounded by the host collector.
    args+=['-serial','none' if devices.get('serial')=='none' else 'file:serial.fifo']
    if profile.get('audio') and not devices.get('audio')=='none':
        args+=['-audiodev','none,id=silent','-device',profile['audio']+',audiodev=silent']
    return args,request


def find_firmware(executable):
    base=Path(executable).resolve().parent.parent
    for p in (base/'share/qemu/bios-256k.bin',Path('/usr/share/qemu/bios-256k.bin'),Path('/usr/share/seabios/bios-256k.bin')):
        if p.is_file():return p.resolve()
    raise res.Refusal('SeaBIOS firmware not found; cannot record firmware hash')


def unexpected_resets(events,expected_host):
    """RESET events beyond the host-requested ones. After a host system_reset
    SeaBIOS performs one hard reboot of its own (QMP 'RESET reason=guest-reset'
    within the same second: 2026-10-09 f0-core evidence), so one guest reset
    within 3 s of each host reset is part of the warm restart; every other
    guest reset is unexpected."""
    def stamp(e):
        t=e.get('timestamp',{});return t.get('seconds',0)+t.get('microseconds',0)/1e6
    resets=[e for e in events if e.get('event')=='RESET']
    host=[stamp(e) for e in resets if e.get('data',{}).get('reason')=='host-qmp-system-reset']
    if len(host)>expected_host:return True
    used=set()
    for e in resets:
        if e.get('data',{}).get('reason')=='host-qmp-system-reset':continue
        ts=stamp(e);match=None
        for i,h in enumerate(host):
            if i not in used and 0<=ts-h<=3:match=i;break
        if match is None or e.get('data',{}).get('reason')!='guest-reset':return True
        used.add(match)
    return False


def host_loadavg():
    """1/5/15-minute host load at launch: under TCG a busy host inflates the
    guest's TSC-based critical-section and timer-gap measurements."""
    try:
        return [float(x) for x in Path('/proc/loadavg').read_text().split()[:3]]
    except (OSError,ValueError):
        return 'unknown'


def manifest_identity(image):
    for name in ('build-manifest.json','manifest.json'):
        p=image.parent/name
        if p.is_file():
            data=json.loads(p.read_text())
            return {'path':str(p),'sha256':sha(p),'revision':data.get('git_revision',data.get('revision','unknown')),
                    'dirty':data.get('git_dirty',data.get('dirty','unknown'))}
    return {'path':'unknown','sha256':'unknown','revision':'unknown','dirty':'unknown'}


def run_case(root,suite,case,profile,image,executable,firmware,host=None,keep=False,qemu_img='qemu-img'):
    """Caller holds the common lock. Every failure after creation has result.json."""
    host=host or Host();runs=root/'build/test-runs';res.check_budget(runs)
    memory=host.preflight(runs)
    run_id=secrets.token_hex(4);directory=runs/suite/run_id
    directory.mkdir(parents=True,exist_ok=False)
    overlay=directory/'run.qcow2';fifo=directory/'serial.fifo';gate=directory/'gate'
    unit='ciuki-test-'+run_id+'.scope';identity=git_identity(root)
    args,request=qemu_args(executable,profile,case,run_id,overlay,firmware)
    scoped=['systemd-run','--user','--scope','--unit='+unit,'-p','MemoryMax=1500M','-p','MemorySwapMax=0','--',
            sys.executable,str(ROOT/'scripts/test/scope_exec.py'),str(gate),*args]
    result={'schema_version':1,'run_id':run_id,'suite':suite,'probe':case['probe'],'case':case.get('id',case['probe']),'attempt':case.get('attempt',1),'boot_kind':case.get('boot_kind','cold'),
            'utc_start':utc(),'utc_end':None,'outcome':'fail','reason':'incomplete',
            'image':{'path':str(image),'sha256':sha(image),'size':image.stat().st_size},
            'build_manifest':manifest_identity(image),'build_git_revision':'unknown','build_dirty':'unknown',
            'runner_revision':identity,'selector':request,'expected':case['expected'],'observed':[],
            'artifacts':{},'timeout':{'seconds':case['timeout'],'occurred':False},'cleanup':{},
            'host':{'mem_available':memory,'scope':unit,'loadavg':host_loadavg()},
            'qemu':{'version':'unknown','executable':executable,'executable_sha256':sha(executable),
                    'arguments':args,'scope_arguments':scoped,'machine':profile['machine'],'cpu':profile['cpu'],
                    'accelerator':profile['accelerator'],'ram_mib':profile['ram_mib'],'profile':profile,
                    'firmware_sha256':sha(firmware),'devices':{'ide':'PIIX','i8042':True,'vga':case.get('device_exceptions',{}).get('vga',profile['vga']),
                        'com1':case.get('device_exceptions',{}).get('serial','file'),'audio':profile.get('audio','none')}}}
    result['build_git_revision']=result['build_manifest']['revision'];result['build_dirty']=result['build_manifest']['dirty']
    process=qmp=cgroup=None;parser=Parser(run_id,case['probe']);fd=None;dirfd=None;logs=[]
    pending=b'';panic_start=None;armed_stats=None;terminal_time=None
    restart_performed=False;expected_resets=0
    try:
        subprocess.run([qemu_img,'create','-f','qcow2','-b',str(image),'-F','raw',str(overlay)],check=True,stdout=subprocess.DEVNULL,timeout=10)
        os.mkfifo(fifo,0o600);fd=os.open(fifo,os.O_RDWR|os.O_NONBLOCK)
        serial=(directory/'serial.log').open('wb');stderr=(directory/'stderr.log').open('wb');qlog=(directory/'qmp.log').open('wb');logs=[serial,stderr,qlog]
        launched=time.monotonic();deadline=launched+case['timeout']
        process=host.launch(scoped,directory)
        os.set_blocking(process.stdout.fileno(),False)
        # The wrapper cannot exec QEMU until both systemd properties and kernel
        # cgroup files are verified. No unrestricted fallback is possible.
        last_error=None
        while time.monotonic()<min(deadline,launched+5):
            if process.poll() is not None:break
            try:
                properties,cgroup=host.verify(unit);result['host']['effective_limits']=properties;break
            except res.Refusal as e:last_error=e;time.sleep(.05)
        else:raise res.Refusal('scope verification deadline: '+str(last_error))
        if cgroup is None:raise res.Refusal('scope failed before limit verification')
        remaining=2*res.GIB-max(res.usage(runs).values())-32*1024**2
        if remaining <= 0:raise res.Refusal('insufficient remaining artifact budget')
        (directory/'file-limit').write_text(str(remaining))
        gate.write_text('verified')
        dirfd=os.open(directory,os.O_RDONLY)
        while True:
            now=time.monotonic()
            if now>=deadline:
                result['timeout']['occurred']=True;raise EvidenceError('host monotonic deadline exceeded')
            res.check_budget(runs)
            ready,_,_=select.select([fd,process.stdout.fileno()],[],[],.025)
            for source in ready:
                chunk=os.read(source,65536)
                if not chunk:continue
                target=serial if source==fd else stderr
                remaining=res.LOG_CAP-target.tell()
                target.write(chunk[:remaining]);target.flush()
                if len(chunk)>remaining:raise EvidenceError('serial/stderr log cap reached')
                if source==fd:
                    pending+=chunk
                    while b'\n' in pending:
                        line,pending=pending.split(b'\n',1);parser.feed(line+b'\n')
                        if parser.records and parser.records[-1]['event']=='ARM' and armed_stats is None and qmp:
                            armed_stats=writes(qmp.command('query-blockstats'));result['observed_blockstats_armed']=armed_stats
                    if len(pending)>res.LOG_CAP:raise EvidenceError('unterminated serial line exceeds cap')
            if qmp is None and (directory/'q').exists():
                qmp=host.connect_qmp(Path('/proc/self/fd')/str(dirfd)/'q',qlog)
                version=qmp.greeting['QMP']['version']
                result['qemu']['version']=version
                pinned=json.loads((ROOT/'config/toolchain.json').read_text())['qemu_major']
                if version['qemu']['major']!=pinned:raise res.Refusal('QEMU major version differs from pinned toolchain')
                result['initial_blockstats']=writes(qmp.command('query-blockstats'))
                if case['probe']=='panic':
                    result['blockstats_baseline']='ARM receipt for all I/O; before CPU start for writes'
                qmp.command('cont')
                # Catch ARM received in the same batch before QMP connected.
                if any(r['event']=='ARM' for r in parser.records) and armed_stats is None:
                    armed_stats=writes(qmp.command('query-blockstats'));result['observed_blockstats_armed']=armed_stats
            if qmp:
                status=qmp.command('query-status')
                if unexpected_resets(qmp.events,expected_resets) or any(e.get('event')=='SHUTDOWN' and e.get('data',{}).get('reason')=='guest-reset' for e in qmp.events):
                    raise EvidenceError('unexpected reset')
                result['observed_qemu_status']=status
            now=time.monotonic()
            if now>=deadline:
                result['timeout']['occurred']=True;raise EvidenceError('host monotonic deadline exceeded')
            if parser.terminal:
                if parser.terminal['event']=='NOT_RUN':
                    result['outcome']='not_run';result['reason']='prerequisite failed: '+parser.terminal['after'];break
                parser.check(case['expected'])
                if qmp is None:raise EvidenceError('terminal evidence without QMP observation')
                if terminal_time is None:terminal_time=now
                if parser.terminal['event']=='PANIC':
                    arms=[r for r in parser.records if r['event']=='ARM']
                    if len(arms)!=1:raise EvidenceError('panic requires exactly one ARM record')
                    for name in ('error','eip','cr2'):
                        if int(parser.terminal.get(name,''),16)!=int(arms[0].get('expected_'+name,''),16):
                            raise EvidenceError('panic differs from armed '+name)
                    if armed_stats is None:raise EvidenceError('panic lacks armed block-stat baseline')
                    if panic_start is None:panic_start=now
                    if now-panic_start>=5:
                        final=writes(qmp.command('query-blockstats'));result['observed_blockstats_final']=final
                        if final!=armed_stats:raise EvidenceError('block I/O changed after panic arm')
                        before=result['initial_blockstats']
                        if any(final[device][key]!=values[key] for device,values in before.items() for key in ('wr_bytes','wr_operations','flush_operations')):
                            raise EvidenceError('block writes/flushes changed after CPU start')
                        result['panic_observation_seconds']=now-panic_start
                        break
                elif now-terminal_time>=.2:
                    if case.get('boot_kind')=='restart' and not restart_performed:
                        result['restart_prerequisite']=parser.records
                        expected_resets+=1;qmp.command('system_reset')
                        parser=Parser(run_id,case['probe']);terminal_time=None
                        restart_performed=True;pending=b''
                    else:
                        break
            if process.poll() is not None:
                if pending:parser.feed(pending)
                raise EvidenceError('QEMU exited before observation completed')
            if case.get('evidence_sink')=='screen' and now-launched>=case.get('screen_capture_after',10) and not (directory/'screen.ppm').exists() and qmp:
                qmp.command('screendump',{'filename':str(directory/'screen.ppm')})
                raise EvidenceError('screen captured; serial-disabled subcase requires externally verified screen evidence')
        if case.get('boot_kind')=='restart':
            host_resets=sum(1 for e in qmp.events if e.get('event')=='RESET' and e.get('data',{}).get('reason')=='host-qmp-system-reset') if qmp else 0
            if host_resets!=expected_resets:
                raise EvidenceError(f'host reset count {host_resets} differs from expected {expected_resets}')
        if result['outcome']!='not_run':
            result['outcome']='pass';result['reason']='all declared predicates and host observations passed'
    except (OSError,ValueError,RuntimeError,subprocess.SubprocessError,KeyboardInterrupt) as e:
        result['reason']=str(e) or type(e).__name__
    finally:
        # Ownership is held until this sequence finishes, including verification.
        result['cleanup']=teardown(host,unit,process,qmp,cgroup)
        result['cleanup']['qemu_process_count']=host.unowned_qemu_count()
        if result['cleanup']['qemu_process_count']:
            result['cleanup']['clean']=False
        if qmp:qmp.close()
        if process and process.stdout:process.stdout.close()
        if fd is not None:os.close(fd)
        if dirfd is not None:os.close(dirfd)
        for log in logs:log.close()
        if not (directory/'serial.log').exists():(directory/'serial.log').touch()
        if not result['cleanup']['clean']:
            result['outcome']='fail';result['reason']='owned children survived teardown'
        if result['outcome']=='pass' and result['cleanup'].get('wrapper_returncode')!=0:
            result['outcome']='fail';result['reason']='QEMU/scope exited abnormally after observation'
        if cgroup is not None:
            try:
                oom=(cgroup/'memory.events').read_text()
                result['host']['memory_events']=oom
                if any(int(l.split()[1]) for l in oom.splitlines() if l.split()[0] in ('oom','oom_kill')):
                    result['outcome']='fail';result['reason']='scope OOM'
            except OSError:result['host']['memory_events']='scope removed'
        result['observed']=parser.records
        result['image']['sha256_after']=sha(image)
        if result['image']['sha256_after']!=result['image']['sha256']:
            result['outcome']='fail';result['reason']='canonical image changed during run'
        result['utc_end']=utc()
        for p in directory.iterdir():
            if p.is_file() and p.name!='result.json':result['artifacts'][p.name]={'sha256':sha(p),'size':p.stat().st_size}
        # --keep retains failure evidence for review, not passing overlays;
        # all passing runs always retain exactly these two contract artifacts.
        if result['outcome']=='pass':
            for p in directory.iterdir():
                if p.name!='serial.log':p.unlink()
            result['artifacts']={k:v for k,v in result['artifacts'].items() if k=='serial.log'}
        else:
            for name in ('q','serial.fifo','gate','file-limit'):
                (directory/name).unlink(missing_ok=True)
        result['artifacts']={k:v for k,v in result['artifacts'].items() if (directory/k).is_file()}
        result['retention']={'keep_requested':keep,'passing_files':['result.json','serial.log']}
        (directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        res.prune(directory.parent)
    return result,directory


def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--physical-capture',type=Path,action='append',default=[])
    ap.add_argument('suite');ap.add_argument('--image',type=Path);ap.add_argument('--profile');ap.add_argument('--keep',action='store_true')
    options=ap.parse_args(argv)
    try:
        if not all(c.isalnum() or c in '-_' for c in options.suite):raise res.Refusal('invalid suite name')
        suite=load(ROOT/'tests/suites'/f'{options.suite}.json')
        if suite.get('image')!='full':raise res.Refusal('only canonical full HDD suites are supported in F0')
        host_evidence=None
        if suite.get('host_tests'):
            started=time.monotonic()
            checked=subprocess.run([sys.executable,'-m','unittest','discover','-s','tests/host','-v'],cwd=ROOT,capture_output=True,timeout=180)
            output=checked.stdout+checked.stderr
            if len(output)>res.LOG_CAP:raise res.Refusal('host fixture evidence exceeds log cap')
            print(output.decode(errors='replace'),end='',flush=True)
            host_evidence={'outcome':'pass' if checked.returncode==0 else 'fail','returncode':checked.returncode,
                           'duration_seconds':time.monotonic()-started,'output_sha256':hashlib.sha256(output).hexdigest(),
                           'fixture_outcomes':output.decode(errors='replace')}
            if checked.returncode:raise res.Refusal('host runner prerequisite failed')
        image=(options.image or ROOT/'build/full/ciukios-full.img').resolve()
        if not image.is_file():raise res.Refusal('canonical image missing; QEMU/physical runner qualification not_run')
        executable=shutil.which('qemu-system-i386')
        if not executable:raise res.Refusal('qemu-system-i386 unavailable')
        toolchain=json.loads((ROOT/'config/toolchain.json').read_text())
        firmware=find_firmware(executable)
        cases=suite['cases'];summary=[]
        with res.ExclusiveLock(res.common_lock(ROOT)):
            for index,case in enumerate(cases):
                name=options.profile or case['profile']
                if not all(c.isalnum() or c in '-_' for c in name):raise res.Refusal('invalid profile name')
                profile=load(ROOT/'tests/profiles'/f'{name}.json')
                if profile['machine']!=toolchain['qemu_machine']:raise res.Refusal('profile machine does not match pinned toolchain')
                if options.suite!='f0-smoke' and profile['accelerator']!='tcg':raise res.Refusal('CPU correctness evidence requires TCG')
                result,directory=run_case(ROOT,options.suite,case,profile,image,executable,firmware,keep=options.keep)
                if host_evidence is not None:
                    result['host_probe']=host_evidence
                    (directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
                summary.append({'case':case.get('id',case['probe']),'attempt':case.get('attempt',1),'boot_kind':case.get('boot_kind','cold'),'run_id':result['run_id'],'outcome':result['outcome'],'result':str(directory/'result.json'),'evidence':result})
                # Preserve essential textual evidence before the next run can
                # prune old directories (the ten-boot matrix exceeds five runs).
                checkpoint=ROOT/'build/test-runs'/options.suite/'summary.json'
                checkpoint.write_text(json.dumps({'schema_version':1,'suite':options.suite,'cases':summary},indent=2)+'\n')
                print(result['outcome'].upper()+': '+summary[-1]['case']+' — '+result['reason'],flush=True)
                if result['outcome']!='pass':
                    summary.extend({'case':c.get('id',c['probe']),'outcome':'not_run','reason':'prerequisite failed'} for c in cases[index+1:]);break
            if suite.get('physical_import_required'):
                from physical import import_evidence
                imports=[]
                for capture in options.physical_capture:
                    metadata=json.loads((capture/'acquisition.json').read_text())
                    probe=selector(metadata['selector'],metadata.get('selector_source','menu'))['probe']
                    expected=next((c['expected'] for c in suite['cases'] if c['probe']==probe),None)
                    if expected is None:raise res.Refusal('physical probe is not declared by the suite')
                    imports.append(import_evidence(capture,sha(image),expected))
                summary.append({'case':'physical-selector/evidence-import','outcome':'pass' if imports else 'not_run',
                                'reason':'operator-confirmed evidence imported' if imports else 'operator-confirmed physical records required','imports':imports})
            dest=ROOT/'build/test-runs'/options.suite/'summary.json'
            dest.write_text(json.dumps({'schema_version':1,'suite':options.suite,'cases':summary},indent=2)+'\n')
        return 0 if all(r['outcome']=='pass' for r in summary) else 1
    except (res.Refusal,OSError,ValueError,subprocess.SubprocessError) as e:
        print('REFUSED: '+str(e),file=sys.stderr);return 2


if __name__=='__main__':sys.exit(main())
