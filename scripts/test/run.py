#!/usr/bin/env python3
"""Canonical-image F0/F1 runner. Python standard library only; never builds images."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import re
import struct
from contextlib import contextmanager
from pathlib import Path
import secrets
import select
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0,str(Path(__file__).resolve().parent))
from evidence import Parser, EvidenceError, f2_metadata
from loader_model import selector, F1_PROBES
from qmp import QMP, writes
import resources as res

ROOT=Path(__file__).resolve().parents[2]
REGRESSION_SUITES = ('f0-smoke','f0-core','f0-panic','f0-runner',
                     'f1-input','f1-storage','f1-fat32','f1-safe')
F2_SUITES = ('f2-process','f2-runtime','f2-desktop','f2-app')


def load_suite(name):
    """Resolve runner aliases and prerequisites once, in declared order."""
    names = REGRESSION_SUITES if name == 'all' else (*REGRESSION_SUITES, *F2_SUITES) if name == 'f2-all' else (name,)
    suite = {'schema_version':1, 'image':'full', 'cases':[]}
    seen = set(); confirmations = set()
    def add(part_name, prerequisites=True):
        if part_name in seen:return
        part = load(ROOT/'tests/suites'/f'{part_name}.json')
        if part.get('image') != 'full':raise res.Refusal('only canonical full HDD suites are supported')
        if prerequisites:
            for required in part.get('prerequisites',[]):add(required)
        seen.add(part_name)
        confirmations.update(part.get('operator_confirmation_cases',[]))
        additions = expand_cases(part)
        if part_name == 'f0-smoke':additions = [{**c, '_smoke':True} for c in additions]
        suite['cases'].extend(additions)
        # Preserve the existing physical import behavior: prerequisite physical
        # gates remain separate, while alias and requested-suite imports apply.
        if part_name in names:
            for flag in ('host_tests','physical_import_required'):
                if part.get(flag):suite[flag] = True
            suite.setdefault('physical_cases',[]).extend(part.get('physical_cases',[]))
    for part_name in names:add(part_name, name != 'all')
    for case in suite['cases']:
        if case.get('id') in confirmations:case['operator_confirmation'] = True
    return suite


def failed_prerequisite(case, result):
    return result['outcome'] != 'pass' and not (
        case.get('operator_confirmation') is True and result.get('operator_confirmation') is True)


def record_f2_result(result, parser):
    empty = f2_metadata([])
    result.update({k:v for k,v in empty.items() if k != 'missing_fields'})
    result['missing_f2_fields'] = empty['missing_fields']
    result['application_output'] = parser.application.result()
    metadata = f2_metadata(parser.records)
    result.update({k:v for k,v in metadata.items() if k != 'missing_fields'})
    result['missing_f2_fields'] = metadata['missing_fields']
    result['application_output'] = parser.application.result()
    result['payload_hash_comparisons'] = []
    manifest_path = result['build_manifest'].get('path')
    manifest = json.loads(Path(manifest_path).read_text()) if manifest_path != 'unknown' else {}
    payloads = {p['path']:p['sha256'] for p in manifest.get('payloads',[])}
    measured = []
    for record in parser.records:
        if record.get('event') == 'DATA' and record.get('group') == 'payload':
            measured.append((record.get('path'), record.get('sha256')))
    if isinstance(metadata['elf_hashes'],dict):measured.extend(metadata['elf_hashes'].items())
    seen = set()
    for path,actual in measured:
        if path in seen:raise EvidenceError('duplicate guest-loaded payload hash: '+str(path))
        seen.add(path)
        expected = payloads.get(path)
        match = bool(expected and isinstance(actual,str) and re.fullmatch('[0-9a-fA-F]{64}',actual) and actual.lower() == expected.lower())
        result['payload_hash_comparisons'].append({'path':path,'guest_sha256':actual,
                                                  'host_sha256':expected or 'unknown','match':match})
        if not match:raise EvidenceError('guest-loaded payload hash differs from host manifest: '+str(path))
    result['missing_payload_hashes'] = sorted(set(payloads)-{r['path'] for r in result['payload_hash_comparisons']})
    if metadata['sdk_manifest_sha256'] != 'unknown' and manifest.get('sdk_manifest_sha256') != metadata['sdk_manifest_sha256']:
        raise EvidenceError('guest SDK hash differs from host manifest')
    archives = manifest.get('lua',{}).get('archives',{})
    for field,archive in (('application_source_sha256','source'),('application_tests_sha256','tests')):
        if metadata[field] != 'unknown' and metadata[field] != archives.get(archive,{}).get('sha256'):
            raise EvidenceError('guest application provenance differs from host manifest: '+field)
    if result['probe'] == 'app-gate' and result['outcome'] == 'pass':
        if result['missing_f2_fields']:raise EvidenceError('missing application provenance/setup fields')
        required = {'/bin/lua','/system/tests/ciuki-f2.lua'} | {p for p in payloads if p.startswith('/system/tests/lua-5.4.8-tests/')}
        if required-seen:raise EvidenceError('missing guest-loaded application/test payload hashes')
        if metadata['abi_version'] != '1' or metadata['application_wait_status'] != '0':
            raise EvidenceError('application ABI or wait status mismatch')
        if metadata['argv'] != ['lua','-e','_U=true','all.lua'] or metadata['cwd'] != '/system/tests/lua-5.4.8-tests':
            raise EvidenceError('application argv/cwd mismatch')
        environment = metadata['env']
        if not isinstance(environment,dict) or any(environment.get(k)!=v for k,v in {'LC_ALL':'C','TZ':'UTC0','HOME':'/home','TMPDIR':'/tmp'}.items()):
            raise EvidenceError('application environment mismatch')
        fds = metadata['fd_setup']
        if not isinstance(fds,dict) or fds.get('inherited') != [0,1,2] or fds.get('stdin') not in ('fixture','/dev/null') or any(fds.get(s)!='bounded' for s in ('stdout','stderr')):
            raise EvidenceError('application fd setup mismatch')
        if not parser.application.final_success_indication or parser.application.assertion_indications:
            raise EvidenceError('upstream final output missing or assertion observed')


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


def digest_json(data):
    return hashlib.sha256(json.dumps(data,sort_keys=True,separators=(',',':')).encode()).hexdigest()


def expand_cases(suite):
    """An F1 all/core runner alias is always individual selectors across boots."""
    expanded=[]
    for case in suite['cases']:
        if case['probe'] not in ('all','core'):
            expanded.append(case);continue
        templates=case.get('probe_cases')
        if not templates or [c['probe'] for c in templates]!=list(F1_PROBES):
            raise res.Refusal('F1 alias requires ordered individual probe_cases')
        for template in templates:
            selected={**case,**template};selected.pop('probe_cases',None)
            selected['selector']='f1:'+selected['probe']+' run={run_id}'
            expanded.append(selected)
    return expanded


def prepare_fixtures(host,case,directory):
    manifests=[]
    for index,fixture in enumerate(case.get('fixtures',[])):
        item=dict(fixture)
        if fixture.get('generator')=='mkfs.fat':
            item.update(host.fat_fixture(fixture,directory,index))
        elif 'path' in fixture:
            path=(ROOT/fixture['path']).resolve()
            if not path.is_relative_to(ROOT) or not path.is_file():raise res.Refusal('fixture missing/outside worktree')
            actual=sha(path)
            if actual!=fixture.get('sha256'):raise res.Refusal('fixture SHA-256 mismatch')
            item.update(path=str(path),size=path.stat().st_size,sha256=actual)
        manifests.append(item)
    return {'sha256':digest_json(manifests),'declared':case.get('fixtures',[]),'manifest':manifests}


def boot_cfg_extent(image):
    """Locate existing short-name BOOT.CFG in the canonical FAT32 volume.
    No allocation/metadata changes: patch only its existing bounded contents.
    """
    with Path(image).open('rb') as stream:
        def read(offset,length):
            stream.seek(offset);data=stream.read(length)
            if len(data)!=length:raise res.Refusal('truncated BOOT.CFG fixture')
            return data
        mbr=read(0,512)
        if mbr[510:]!=b'\x55\xaa':raise res.Refusal('BOOT.CFG fixture lacks MBR signature')
        start,sectors=struct.unpack_from('<II',mbr,454);base=start*512
        bpb=read(base,512);sector=struct.unpack_from('<H',bpb,11)[0];spc=bpb[13]
        reserved=struct.unpack_from('<H',bpb,14)[0];fats=bpb[16];fat_sectors=struct.unpack_from('<I',bpb,36)[0]
        root_cluster=struct.unpack_from('<I',bpb,44)[0];cluster_size=sector*spc
        if sector!=512 or not spc or spc & (spc-1) or not reserved or not fats or not fat_sectors:
            raise res.Refusal('invalid FAT32 fixture geometry')
        data_base=base+(reserved+fats*fat_sectors)*sector
        cluster_count=(sectors-reserved-fats*fat_sectors)//spc
        def offset(cluster):
            if not 2<=cluster<cluster_count+2:raise res.Refusal('BOOT.CFG cluster out of range')
            return data_base+(cluster-2)*cluster_size
        def find(cluster,name):
            seen=set()
            while cluster<0x0ffffff8:
                if cluster in seen or len(seen)>=4096:raise res.Refusal('BOOT.CFG directory walk exceeds bound')
                seen.add(cluster);block=read(offset(cluster),cluster_size)
                for i in range(0,len(block),32):
                    entry=block[i:i+32]
                    if entry[0]==0:raise res.Refusal('BOOT.CFG fixture not found')
                    if entry[:11]==name and entry[11]!=15:
                        first=struct.unpack_from('<H',entry,26)[0] | struct.unpack_from('<H',entry,20)[0]<<16
                        return first,struct.unpack_from('<I',entry,28)[0],entry[11]
                cluster=struct.unpack('<I',read(base+reserved*sector+cluster*4,4))[0]&0x0fffffff
            raise res.Refusal('BOOT.CFG fixture not found')
        system,_,attr=find(root_cluster,b'SYSTEM     ')
        if not attr & 16:raise res.Refusal('SYSTEM fixture is not a directory')
        cluster,size,attr=find(system,b'BOOT    CFG')
        if attr & 16 or not 1<=size<=127 or size>cluster_size:raise res.Refusal('invalid BOOT.CFG extent')
        return offset(cluster),size


def patch_overlay(host,image,overlay,directory,patches):
    manifest=[]
    for index,patch in enumerate(patches):
        if patch.get('file')!='SYSTEM/BOOT.CFG':raise res.Refusal('only BOOT.CFG preboot patches are allowed')
        base,size=boot_cfg_extent(image);relative=patch.get('offset',0)
        before=bytes.fromhex(patch['before_hex']);after=bytes.fromhex(patch['after_hex'])
        if type(relative) is not int or relative<0 or not before or len(before)!=len(after) or relative+len(after)>size:
            raise res.Refusal('BOOT.CFG patch must preserve size and remain inside file')
        offset=base+relative;actual=host.overlay_read(overlay,offset,len(before))
        if actual!=before:raise res.Refusal('BOOT.CFG patch preimage mismatch')
        payload=directory/f'patch-{index}.bin';payload.write_bytes(after)
        host.overlay_write(overlay,offset,payload,len(after))
        actual_after=host.overlay_read(overlay,offset,len(after))
        if actual_after!=after:raise res.Refusal('BOOT.CFG patch readback mismatch')
        manifest.append({'file':patch['file'],'offset':offset,'file_offset':relative,'length':len(after),
                         'before_hex':before.hex(),'after_hex':after.hex(),
                         'sha256_before':hashlib.sha256(actual).hexdigest(),'sha256_after':hashlib.sha256(actual_after).hexdigest()})
    (directory/'patch-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


def blkdebug_config(case,directory):
    fault=case.get('fault')
    if not fault:return None
    if fault.get('layer')!='host-block-backend':raise res.Refusal('blkdebug fault layer must be host-block-backend')
    event=fault.get('event')
    if event not in ('read_aio','write_aio','flush_to_os','flush_to_disk') or type(fault.get('errno')) is not int or not 1<=fault['errno']<=4095:
        raise res.Refusal('invalid blkdebug event/errno')
    # read failures need a postboot sector filter to preserve loader prerequisites.
    if event=='read_aio' and 'sector' not in fault:raise res.Refusal('read fault requires a postboot fixture sector')
    # A read filter above qcow2 sees guest LBAs (including backing reads).
    # Activate at open, but restrict both sector and I/O type; no metadata offsets.
    injected_event='none' if event=='read_aio' else event
    lines=['[inject-error]',f'event = "{injected_event}"',
           f'errno = "{fault["errno"]}"','once = "on"']
    if event=='read_aio':lines.append('iotype = "read"')
    if 'sector' in fault:
        if type(fault['sector']) is not int or fault['sector']<0:raise res.Refusal('invalid fault sector')
        lines.append(f'sector = "{fault["sector"]}"')
    path=directory/'blkdebug.conf';path.write_text('\n'.join(lines)+'\n');return path


class Actions:
    """Nonblocking paced stimuli; QMP success is recorded, guest evidence is required."""
    def __init__(self,declared):
        self.declared=declared;self.index=0;self.batch=0;self.repeat=0;self.due=0;self.observed=[]
        for action in declared:
            if action.get('after',{}).get('event') not in ('READY','ARM'):
                raise res.Refusal('actions require READY/ARM synchronization')
            if action.get('type') not in ('input','cut'):raise res.Refusal('unknown runner action')
            if action['type']=='input':
                if not 1<=action.get('repeat',1)<=1000 or not action.get('batches'):
                    raise res.Refusal('invalid input repetition/batches')
                for batch in action['batches']:
                    if not 1<=batch.get('pause_ms',0)<=100:raise res.Refusal('input pacing must be 1..100 ms')
            elif action.get('mode') not in ('guest-termination',):
                raise res.Refusal('device power loss requires a driver-boundary fixture; guest termination is distinct')
    @property
    def complete(self):return self.index==len(self.declared)
    def step(self,parser,qmp,now):
        if self.complete or not qmp or now<self.due:return None
        action=self.declared[self.index]
        matches=[r for r in parser.records if all(r.get(k)==str(v) for k,v in action['after'].items())]
        if len(matches)>1:raise EvidenceError('duplicate action synchronization record')
        if not matches:return None
        if action['type']=='cut':
            if parser.terminal:raise EvidenceError('cut point already passed before termination')
            self.observed.append({'type':'cut','mode':action['mode'],'record':matches[0]})
            self.index+=1;return action
        batch=action['batches'][self.batch];qmp.input_events(batch['events'])
        self.observed.append({'type':'input','batch':self.batch,'repeat':self.repeat,'events':batch['events'],
                              'host_monotonic':now,'sync_seq':matches[0]['seq']})
        self.due=now+batch['pause_ms']/1000
        self.batch+=1
        if self.batch==len(action['batches']):
            self.batch=0;self.repeat+=1
            if self.repeat==action.get('repeat',1):self.index+=1;self.repeat=0
        return None


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

    def fat_fixture(self,fixture,directory,index):
        kind=fixture['fat_type'];sizes={12:4*1024**2,16:32*1024**2,32:64*1024**2}
        if kind not in sizes or fixture.get('seed')!=1:raise res.Refusal('unsupported FAT fixture type/seed')
        image=directory/f'fixture-{index}.img'
        with image.open('wb') as stream:stream.truncate(sizes[kind])
        geometry=self.checker(['mkfs.fat','--invariant','-v','-F',str(kind),'-i','00000001',str(image)],directory)
        if geometry['returncode']:raise res.Refusal('mkfs.fat fixture generation failed')
        source=directory/f'fixture-{index}.txt';source.write_bytes(b'Ciuki F1 independent fixture seed=1\n'*128)
        os.utime(source,(946684800,946684800))
        copied=self.checker(['mcopy','-i',str(image),str(source),'::/Ciuki long fixture.txt'],directory)
        if copied['returncode']:raise res.Refusal('mtools fixture copy failed')
        listing=self.checker(['mdir','-i',str(image),'::/'],directory)
        if listing['returncode']:raise res.Refusal('mtools fixture listing failed')
        checked=self.checker(['fsck.fat','-n',str(image)],directory)
        if checked['returncode']:raise res.Refusal('independent fixture fsck failed')
        return {'path':str(image),'sha256':sha(image),'size':image.stat().st_size,'geometry':geometry,
                'listing':listing,'checker':checked}
    def sector_digest(self,image,directory,lba,count,fmt):
        # qemu-img dd opens its input read-only; export only the measured sectors.
        if type(lba) is not int or type(count) is not int or lba<0 or not 1<=count<=128:
            raise res.Refusal('invalid digest sector range')
        info=json.loads(subprocess.check_output(['qemu-img','info','-f',fmt,'--output=json',str(image)],timeout=10))
        if (lba+count)*512>info['virtual-size']:raise res.Refusal('digest sector range outside image')
        target=directory/'digest-sectors.raw'
        try:
            view={'driver':'raw','offset':lba*512,'size':count*512,
                  'file':{'driver':fmt,'file':{'driver':'file','filename':str(image)}}}
            subprocess.run(['qemu-img','dd','bs=512',f'count={count}',
                            'if=json:'+json.dumps(view,separators=(',',':')),'of='+str(target)],
                           check=True,capture_output=True,timeout=10)
            if target.stat().st_size!=count*512:raise res.Refusal('short digest sector export')
            return {'size':target.stat().st_size,'sha256':sha(target)}
        finally:target.unlink(missing_ok=True)
    def file_digest(self,volume,directory,path):
        # Hash bytes, including NUL/non-UTF8, without decoding or logging content.
        import resource
        def limits():resource.setrlimit(resource.RLIMIT_FSIZE,(16*1024**2,16*1024**2))
        target=directory/'digest-file.bin'
        try:
            with target.open('wb') as output:
                subprocess.run(['mtype','-i',str(volume),'::'+path],stdout=output,stderr=subprocess.PIPE,
                               check=True,timeout=30,preexec_fn=limits)
            return {'size':target.stat().st_size,'sha256':sha(target)}
        finally:target.unlink(missing_ok=True)
    def overlay_read(self,overlay,offset,length):
        output=subprocess.check_output(['qemu-io','-r','-f','qcow2','-c',f'read -v {offset} {length}',str(overlay)],text=True,timeout=10)
        data=bytearray()
        for line in output.splitlines():
            if re.match(r'^[0-9a-fA-F]+:',line):
                data.extend(bytes.fromhex(line.split(':',1)[1].lstrip().split('  ',1)[0].strip()))
        if len(data)!=length:raise res.Refusal('qemu-io readback length mismatch')
        return bytes(data)
    def overlay_write(self,overlay,offset,payload,length):
        # qemu-io parses its -c string itself; use relative generated payload names.
        subprocess.run(['qemu-io','-f','qcow2','-c',f'write -s {payload.name} {offset} {length}','-c','flush',str(overlay)],
                       cwd=payload.parent,check=True,stdout=subprocess.DEVNULL,timeout=10)
    @contextmanager
    def export_readonly(self,overlay,directory,offset,size):
        """A FUSE raw slice, never an image conversion or writable mount."""
        target=directory/'check-volume.raw';target.touch()
        args=['qemu-storage-daemon','--blockdev',json.dumps({'driver':'qcow2','node-name':'overlay','read-only':True,
              'file':{'driver':'file','filename':str(overlay)}}),'--blockdev',json.dumps({'driver':'raw','node-name':'volume',
              'file':'overlay','offset':offset,'size':size,'read-only':True}),
              '--export',f'type=fuse,id=checker,node-name=volume,mountpoint={target},writable=off,allow-other=off']
        with (directory/'export.log').open('wb') as log:
            process=subprocess.Popen(args,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                deadline=time.monotonic()+5
                while target.stat().st_size!=size:
                    if process.poll() is not None or time.monotonic()>=deadline:raise res.Refusal('read-only FUSE export unavailable')
                    time.sleep(.025)
                yield target
            finally:
                process.terminate()
                try:process.wait(timeout=2)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=2)
                target.unlink(missing_ok=True)
    def checker(self,args,directory):
        # Bound output on disk, using the same cap as the guest collector.
        import resource
        def limits():resource.setrlimit(resource.RLIMIT_FSIZE,(res.LOG_CAP,res.LOG_CAP))
        with (directory/'checker.log').open('wb') as log:
            checked=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,timeout=30,preexec_fn=limits)
        output=(directory/'checker.log').read_text(errors='replace')
        return {'arguments':args,'returncode':checked.returncode,'output':output,'output_sha256':sha(directory/'checker.log')}


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
    request=f"f{requested['phase']}:{requested['probe']} run={requested['run']}"
    platform=profile.get('platform') or requested['platform']
    if platform:request+=' platform='+platform
    if requested['safe']:request+=' safe=1'
    cache=case.get('disk_cache','writeback')
    if cache not in ('writeback','writethrough','none','directsync'):raise res.Refusal('unsafe/unknown disk cache mode forbidden')
    disk={'driver':'qcow2','file':{'driver':'file','filename':str(overlay)}}
    read_fault=case.get('fault',{}).get('event')=='read_aio'
    if read_fault:
        disk={'driver':'raw','file':{'driver':'blkdebug','config':str(overlay.parent/'blkdebug.conf'),'image':disk}}
    elif case.get('fault'):
        disk['file']={'driver':'blkdebug','config':str(overlay.parent/'blkdebug.conf'),
                      'image':{'driver':'file','filename':str(overlay)}}
    disk_format='raw' if read_fault else 'qcow2'
    drive='file='+('json:'+json.dumps(disk,separators=(',',':')) if case.get('fault') else str(overlay))
    selector(request,'fw_cfg',True)
    devices=case.get('device_exceptions',{})
    args=[executable,'-machine',profile['machine'],'-cpu',profile['cpu'],'-accel',profile['accelerator'],
          '-m',str(profile['ram_mib']),'-smp','1','-bios',str(firmware),'-display','none',
          '-monitor','none','-nic','none','-no-shutdown','-S',
          '-drive',drive.replace(',',',,')+',format='+disk_format+',if=ide,index=0,media=disk,cache='+cache+',rerror=report,werror=report',
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
    if case.get('fixtures'):
        slot=1
        for index,fixture in enumerate(case['fixtures']):
            if fixture.get('generator')=='mkfs.fat':
                if slot>3:raise res.Refusal('too many IDE fixture disks')
                args+=['-drive',f'file=fixture-{index}.img,format=raw,if=ide,index={slot},cache='+cache]
                slot+=1
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


def _run_boot(root,suite,case,profile,image,executable,firmware,host=None,keep=False,qemu_img='qemu-img',shared_overlay=None,retain_overlay=False):
    """Caller holds the common lock. Every failure after creation has result.json."""
    case=json.loads(json.dumps(case))
    host=host or Host();runs=root/'build/test-runs';res.check_budget(runs)
    memory=host.preflight(runs)
    run_id=secrets.token_hex(4);directory=runs/suite/run_id
    overlay=shared_overlay or directory/'run.qcow2';fifo=directory/'serial.fifo';gate=directory/'gate'
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
    actions=Actions(case.get('actions',[]))
    result.update(stimulus={'sha256':digest_json(case.get('actions',[])),'declared':case.get('actions',[]),'observed':[]},
                  fixtures={'sha256':digest_json(case.get('fixtures',[])),'declared':case.get('fixtures',[])},
                  fault=case.get('fault'),cut_point=None,disk_cache_mode=case.get('disk_cache','writeback'),
                  checkers=[],durability_observations=[],patch_manifest=[],digests=[])
    result['operator_confirmation'] = False
    result['operator_confirmation_required'] = case.get('operator_confirmation',False)
    directory.mkdir(parents=True,exist_ok=False)
    launched=None
    process=qmp=cgroup=None;parser=Parser(run_id,case['probe']);fd=None;dirfd=None;logs=[]
    pending=b'';panic_start=None;armed_stats=None;terminal_time=None
    restart_performed=False;expected_resets=0
    try:
        if shared_overlay is None:
            subprocess.run([qemu_img,'create','-f','qcow2','-b',str(image),'-F','raw',str(overlay)],check=True,stdout=subprocess.DEVNULL,timeout=10)
        blkdebug_config(case,overlay.parent)
        if case.get('patches'):
            result['patch_manifest']=patch_overlay(host,image,overlay,directory,case['patches'])
        result['overlay_path']=str(overlay)
        result['fixtures']=prepare_fixtures(host,case,directory)
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
                if source==fd:
                    pending+=chunk
                    while b'\n' in pending:
                        line,pending=pending.split(b'\n',1);line+=b'\n'
                        try:record=parser.feed(line)
                        except EvidenceError:
                            # Retain the first malformed controller line for
                            # diagnosis too; never silently repair its bytes.
                            serial.write(line[:max(0,res.LOG_CAP-serial.tell())]);serial.flush()
                            raise
                        # Application frames are hashed/scanned in full and
                        # retained as bounded head/tail in result.json. The
                        # serial log budget is reserved for controller evidence.
                        if not record or record.get('group') not in ('app','app_digest'):
                            if serial.tell()+len(line)>res.LOG_CAP:raise EvidenceError('serial/stderr log cap reached')
                            serial.write(line);serial.flush()
                        if parser.records and parser.records[-1]['event']=='ARM' and armed_stats is None and qmp:
                            armed_stats=writes(qmp.command('query-blockstats'));result['observed_blockstats_armed']=armed_stats
                    if serial.tell()+len(pending)>res.LOG_CAP:
                        serial.write(pending[:max(0,res.LOG_CAP-serial.tell())]);serial.flush()
                        raise EvidenceError('unterminated serial line exceeds cap')
                else:
                    remaining=res.LOG_CAP-target.tell()
                    target.write(chunk[:remaining]);target.flush()
                    if len(chunk)>remaining:raise EvidenceError('serial/stderr log cap reached')
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
            if parser.terminal and parser.terminal.get('status')=='not_run':
                result['outcome']='not_run';raise EvidenceError('missing phase probe: not_run')
            cut=actions.step(parser,qmp,now)
            if cut:
                parser.check(case['expected']);result['cut_point']=actions.observed[-1]
                host.kill_scope(unit,'SIGKILL')
                result['durability_observations'].append({'mode':'guest-termination','device_power_loss':False})
                break
            if parser.terminal:
                if parser.terminal['event']=='NOT_RUN':
                    result['outcome']='not_run';result['reason']='prerequisite failed: '+parser.terminal['after'];break
                if not actions.complete:raise EvidenceError('terminal evidence before declared stimuli completed')
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
                result['operator_confirmation'] = case.get('operator_confirmation') is True
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
        if result['outcome']=='pass' and result['cut_point'] is None and result['cleanup'].get('wrapper_returncode')!=0:
            result['outcome']='fail';result['reason']='QEMU/scope exited abnormally after observation'
        if cgroup is not None:
            try:
                oom=(cgroup/'memory.events').read_text()
                result['host']['memory_events']=oom
                if any(int(l.split()[1]) for l in oom.splitlines() if l.split()[0] in ('oom','oom_kill')):
                    result['outcome']='fail';result['reason']='scope OOM'
            except OSError:result['host']['memory_events']='scope removed'
        if (directory/'qmp.log').exists():result['qmp_transcript_sha256']=sha(directory/'qmp.log')
        result['stimulus']['observed']=actions.observed
        result['stimulus']['observed_sha256']=digest_json(actions.observed)
        result['durability_observations'].extend(r for r in parser.records if r.get('group') in ('durability','barrier','persisted'))
        result['observed']=parser.records
        if request.startswith('f2:'):
            try:record_f2_result(result,parser)
            except (OSError,ValueError,RuntimeError) as e:
                result['outcome']='fail';result['reason']=str(e)
        if result['outcome']=='pass' and (case.get('checks') or case.get('digests')):
            try:
                if not result['cleanup']['clean']:raise EvidenceError('QEMU must stop before overlay export')
                if case.get('digests'):check_digests(host,overlay,directory,case['digests'],result)
                if case.get('checks'):check_overlay(host,overlay,directory,case['checks'],result)
            except (OSError,ValueError,RuntimeError,subprocess.SubprocessError) as e:
                result['outcome']='fail';result['reason']=str(e)
        result['image']['sha256_after']=sha(image)
        if result['image']['sha256_after']!=result['image']['sha256']:
            result['outcome']='fail';result['reason']='canonical image changed during run'
        if not result['cleanup']['clean'] or result['reason'] != 'screen captured; serial-disabled subcase requires externally verified screen evidence':
            result['operator_confirmation'] = False
        result['timing']={'host_monotonic_elapsed_seconds':time.monotonic()-launched if launched is not None else None,
                          'guest_domain':'icount' if profile.get('icount') else 'non-icount',
                          'guest_records':[r for r in parser.records if 'timing_domain' in r]}
        result['utc_end']=utc()
        for p in directory.iterdir():
            if p.is_file() and p.name!='result.json':result['artifacts'][p.name]={'sha256':sha(p),'size':p.stat().st_size}
        # --keep retains failure evidence for review, not passing overlays;
        # all passing runs always retain exactly these two contract artifacts.
        if result['outcome']=='pass':
            for p in directory.iterdir():
                if p.name!='serial.log' and not (retain_overlay and p==overlay):p.unlink()
            result['artifacts']={k:v for k,v in result['artifacts'].items() if k=='serial.log' or (retain_overlay and k=='run.qcow2')}
        else:
            for name in ('q','serial.fifo','gate','file-limit'):
                (directory/name).unlink(missing_ok=True)
        result['artifacts']={k:v for k,v in result['artifacts'].items() if (directory/k).is_file()}
        result['retention']={'keep_requested':keep,'passing_files':['result.json','serial.log']}
        (directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        res.prune(directory.parent)
    return result,directory


def check_digests(host,overlay,directory,declarations,result):
    """Independently compare guest measurements after verified teardown."""
    for item in declarations:
        matches=[r for r in result['observed'] if all(r.get(k)==str(v) for k,v in item['where'].items())]
        if len(matches)!=1:raise EvidenceError('missing or duplicate digest evidence')
        record=matches[0]
        source=item.get('source','overlay')
        if source=='fixture':
            index=item['fixture'];manifest=result['fixtures']['manifest']
            if type(index) is not int or not 0<=index<len(manifest):raise res.Refusal('invalid digest fixture')
            image=Path(manifest[index]['path']);fmt='raw'
            if sha(image)!=manifest[index]['sha256']:raise EvidenceError('fixture image changed during boot')
        elif source=='overlay':image=overlay;fmt='qcow2'
        elif source=='backing':image=Path(result['image']['path']);fmt='raw'
        else:raise res.Refusal('unknown digest source')
        if item['kind']=='sector':
            lba=item['lba'];count=item.get('count',1)
            if record.get('lba')!=str(lba) or record.get('count',str(count))!=str(count):
                raise EvidenceError('guest digest sector range mismatch')
            measured=host.sector_digest(image,directory,lba,count,fmt)
        elif item['kind']=='file':
            path=item['path']
            if not path.startswith('/') or record.get('name_hex')!=path.encode('utf-8').hex():
                raise EvidenceError('guest digest file name mismatch')
            if source=='fixture':measured=host.file_digest(image,directory,path)
            else:
                offset=item['offset'];size=item['size']
                if type(offset) is not int or type(size) is not int or offset<0 or size<=0 or offset+size>result['image']['size']:
                    raise res.Refusal('invalid digest partition extent')
                # Existing FUSE export exposes only a read-only partition view.
                if source=='backing':volume=str(image)+'@@'+str(offset);measured=host.file_digest(volume,directory,path)
                else:
                    with host.export_readonly(image,directory,offset,size) as volume:
                        measured=host.file_digest(volume,directory,path)
        else:raise res.Refusal('unknown digest kind')
        result['digests'].append({'declaration':item,'measured':measured,'guest':record})
        if record.get('size',str(measured['size']) if item['kind']=='sector' else None)!=str(measured['size']) or record.get('sha256')!=measured['sha256']:
            raise EvidenceError('guest digest mismatch')


def check_overlay(host,overlay,directory,checks,result):
    """Called only after verified QEMU teardown. Export exactly one partition."""
    offset=checks['offset'];size=checks['size']
    if type(offset) is not int or type(size) is not int or offset<0 or size<=0 or offset+size>result['image']['size']:
        raise res.Refusal('invalid checker partition extent')
    with host.export_readonly(overlay,directory,offset,size) as volume:
        for args in (['mdir','-V'],):
            version=host.checker(args,directory);result['checkers'].append({'kind':'version',**version})
            if version['returncode']!=0:raise EvidenceError('checker version unavailable')
        checked=host.checker(['fsck.fat','-n',str(volume)],directory)
        result['checkers'].append({'kind':'version','tool':'fsck.fat','version':checked['output'].splitlines()[0] if checked['output'] else 'unknown'})
        result['checkers'].append({'kind':'fsck.fat',**checked})
        allowed=checks.get('fsck_exit_codes',[0])
        if checked['returncode'] not in allowed:raise EvidenceError('undeclared fsck.fat result')
        if checked['returncode']!=0:
            patterns=checks.get('interrupted_patterns',[])
            lines=[line for line in checked['output'].splitlines() if line.strip()]
            if not patterns or any(not any(re.fullmatch(p,line) for p in patterns) for line in lines):
                raise EvidenceError('unclassified interrupted filesystem discrepancy')
        listing=host.checker(['mdir','-i',str(volume),checks.get('listing_path','::/SYSTEM/TESTS')],directory)
        result['checkers'].append({'kind':'mtools',**listing})
        if listing['returncode']!=0:raise EvidenceError('mtools listing failed')
        for expected in checks.get('listing_contains',[]):
            if expected not in listing['output']:raise EvidenceError('mtools names/sizes mismatch')
        for item in checks.get('files',[]):
            checked=host.checker(['mtype','-i',str(volume),item['path']],directory)
            result['checkers'].append({'kind':'mtools-hash',**checked})
            if checked['returncode'] or checked['output_sha256']!=item['sha256']:
                raise EvidenceError('mtools file digest mismatch')
    result['durability_observations'].append({'read_only_export':True,'qemu_stopped':True,'checker_passed':True})


def run_case(root,suite,case,profile,image,executable,firmware,host=None,keep=False,qemu_img='qemu-img'):
    boots=case.get('boots')
    if not boots and any(a['type']=='cut' for a in case.get('actions',[])):
        raise res.Refusal('crash cut requires a declared reboot sequence')
    if not boots:return _run_boot(root,suite,case,profile,image,executable,firmware,host,keep,qemu_img)
    if not 2<=len(boots)<=5:raise res.Refusal('declared reboot sequence needs 2..5 boots')
    if any(a['type']=='cut' for a in boots[-1].get('actions',case.get('actions',[]))):
        raise res.Refusal('reboot sequence must finish with independent observation')
    shared=None;sequence=[];directories=[];baseline=sha(image)
    for index,boot in enumerate(boots):
        selected={**case,**boot};selected.pop('boots',None)
        selected['id']=case['id']+f'-boot-{index+1}'
        result,directory=_run_boot(root,suite,selected,profile,image,executable,firmware,host,keep,qemu_img,
                                   shared_overlay=shared,retain_overlay=True)
        directories.append(directory)
        if shared is None:shared=directory/'run.qcow2'
        sequence.append({'boot':index+1,'run_id':result['run_id'],'overlay':str(shared),
                         'image_sha256':result['image']['sha256'],'outcome':result['outcome'],
                         'observed':result['observed'],'cut_point':result['cut_point'],'cleanup':result['cleanup'],
                         'stimulus':result['stimulus'],'fixtures':result['fixtures'],'fault':result['fault'],
                         'disk_cache_mode':result['disk_cache_mode'],'patch_manifest':result['patch_manifest'],
                         'checkers':result['checkers'],'digests':result['digests'],'durability_observations':result['durability_observations']})
        if result['outcome']!='pass' or result['image']['sha256']!=baseline:break
    if result['outcome']=='pass':shared.unlink(missing_ok=True)
    if shared.exists() or result['outcome']=='pass':
        first=json.loads((directories[0]/'result.json').read_text());first['artifacts'].pop('run.qcow2',None)
        if shared.exists():first['artifacts']['run.qcow2']={'sha256':sha(shared),'size':shared.stat().st_size}
        (directories[0]/'result.json').write_text(json.dumps(first,indent=2)+'\n')
    result['case']=case['id'];result['reboot_sequence']=sequence
    result['unattempted_boots']=len(boots)-len(sequence)
    (directory/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result,directory


def main(argv=None):
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--physical-capture',type=Path,action='append',default=[])
    ap.add_argument('suite');ap.add_argument('--image',type=Path);ap.add_argument('--profile');ap.add_argument('--keep',action='store_true')
    options=ap.parse_args(argv)
    try:
        if not all(c.isalnum() or c in '-_' for c in options.suite):raise res.Refusal('invalid suite name')
        suite=load_suite(options.suite)
        if suite.get('image')!='full':raise res.Refusal('only canonical full HDD suites are supported in F0')
        with res.ExclusiveLock(res.common_lock(ROOT)):
            Host().preflight(ROOT/'build/test-runs')
            host_evidence=None
            if suite.get('host_tests') or options.suite in ('all','f2-all') or options.suite.startswith(('f1-','f2-')):
                started=time.monotonic()
                checked=subprocess.run([sys.executable,'-m','unittest','discover','-s','tests/host','-v'],cwd=ROOT,capture_output=True,timeout=180)
                output=checked.stdout+checked.stderr
                if len(output)>res.LOG_CAP:raise res.Refusal('host fixture evidence exceeds log cap')
                print(output.decode(errors='replace'),end='',flush=True)
                host_evidence={'outcome':'pass' if checked.returncode==0 else 'fail','returncode':checked.returncode,
                               'duration_seconds':time.monotonic()-started,'output_sha256':hashlib.sha256(output).hexdigest(),
                               'fixture_outcomes':output.decode(errors='replace')}
                if checked.returncode:raise res.Refusal('host runner prerequisite failed')
            image=(options.image or ROOT/'build/f0/ciukios.img').resolve()
            if not image.is_file():raise res.Refusal('canonical image missing; QEMU/physical runner qualification not_run')
            executable=shutil.which('qemu-system-i386')
            if not executable:raise res.Refusal('qemu-system-i386 unavailable')
            toolchain=json.loads((ROOT/'config/toolchain.json').read_text())
            firmware=find_firmware(executable)
            cases=expand_cases(suite);summary=[]
            for index,case in enumerate(cases):
                name=options.profile or case['profile']
                if not all(c.isalnum() or c in '-_' for c in name):raise res.Refusal('invalid profile name')
                profile=load(ROOT/'tests/profiles'/f'{name}.json')
                if profile['machine']!=toolchain['qemu_machine']:raise res.Refusal('profile machine does not match pinned toolchain')
                if case.get('selector','').startswith(('f1:','f2:')) and profile.get('icount')!='shift=1,sleep=on':raise res.Refusal('F1/F2 evidence requires pinned icount profile')
                if options.suite!='f0-smoke' and profile['accelerator']!='tcg' and not case.get('_smoke'):raise res.Refusal('CPU correctness evidence requires TCG')
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
                if failed_prerequisite(case,result):
                    summary.extend({'case':c.get('id',c['probe']),'outcome':'not_run','reason':'prerequisite failed'} for c in cases[index+1:]);break
            if suite.get('physical_import_required'):
                from physical import import_evidence
                imports=[]
                for capture in options.physical_capture:
                    acquisition=capture/'acquisition.json'
                    if acquisition.stat().st_size>128*1024:raise res.Refusal('physical acquisition exceeds 128 KiB limit')
                    metadata=json.loads(acquisition.read_text())
                    probe=selector(metadata['selector'],metadata.get('selector_source','menu'))['probe']
                    physical_cases=suite.get('physical_cases',[])
                    selected=next((c for c in physical_cases if c['probe']==probe and
                                   c['target'] in metadata.get('model','').lower()),None)
                    if selected is None and physical_cases and probe=='safe':
                        raise res.Refusal('physical safe capture does not match declared T23/E500 target')
                    expected=selected['expected'] if selected else next((c['expected'] for c in suite['cases'] if c['probe']==probe),None)
                    if expected is None:raise res.Refusal('physical probe is not declared by the suite')
                    imported=import_evidence(capture,sha(image),expected)
                    imported['case']=selected['id'] if selected else 'physical-selector/evidence-import'
                    imports.append(imported)
                if suite.get('physical_cases'):
                    for physical_case in suite['physical_cases']:
                        matching=[r for r in imports if r['case']==physical_case['id']]
                        summary.append({'case':physical_case['id'],'outcome':'pass' if matching else 'not_run',
                                        'reason':'operator-confirmed menu capture imported' if matching else 'physical menu evidence required',
                                        'imports':matching})
                else:
                    summary.append({'case':'physical-selector/evidence-import','outcome':'pass' if imports else 'not_run',
                                    'reason':'operator-confirmed evidence imported' if imports else 'operator-confirmed physical records required','imports':imports})
            dest=ROOT/'build/test-runs'/options.suite/'summary.json'
            dest.write_text(json.dumps({'schema_version':1,'suite':options.suite,'cases':summary},indent=2)+'\n')
            return 0 if all(r['outcome']=='pass' for r in summary) else 1
    except (res.Refusal,OSError,ValueError,subprocess.SubprocessError) as e:
        print('REFUSED: '+str(e),file=sys.stderr);return 2


if __name__=='__main__':sys.exit(main())
