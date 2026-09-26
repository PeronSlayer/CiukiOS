#!/usr/bin/env python3
"""Measure actual BIOS-text workload/callback/input costs on an image copy.

KVM does not emulate a Pentium III clock rate. These are relative same-host,
same-QEMU observations, not physical-machine benchmarks or DOS-game FPS.
Submitted BIOS batches and completed UI callbacks are reported separately.
The guest keeps running; there are no injected counters or guest RAM writes.
"""
import argparse
import hashlib
import json
import re
import shutil
import statistics
import struct
import subprocess
import time
from pathlib import Path

from PIL import Image

from qemu_test_dos_window import Session
from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM
from qemu_test_ui_regressions import cpu_seconds


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Observer:
    """After setup, sample only the 128-byte runtime header and small metrics."""
    def __init__(self, ui):
        self.ui=ui;self.vm=ui.vm
        ui.refresh();self.segment=ui.w('dw_segment')
        assert self.segment,'missing live DOS runtime'
        self.read_calls=0;self.read_bytes=0;self.read_seconds=0.
        self.start=time.perf_counter()
        prefix=self.read(self.segment*16,16)
        self.abi_version,self.header_bytes=struct.unpack_from('<HH',prefix,12)
        assert 128<=self.header_bytes<=512,self.header_bytes

    def read(self,address,size):
        path=self.vm.output/'perf-observed.bin'
        start=time.perf_counter()
        self.vm.hmp(f'pmemsave {address} {size} "{path}"')
        data=path.read_bytes()
        self.read_seconds+=time.perf_counter()-start
        self.read_calls+=1;self.read_bytes+=size
        assert len(data)==size
        return data

    def sample(self,settled=False):
        begin=time.perf_counter()
        deadline=begin+3
        while True:
            data=self.read(self.segment*16,self.header_bytes)
            if not settled:break
            if not data[35]:
                following=self.read(self.segment*16,self.header_bytes)
                if (not following[35] and data[52:56]==following[52:56]
                    and data[80:116]==following[80:116]):
                    data=following
                    break
            if time.perf_counter()>=deadline:raise AssertionError('no stable completed-callback counter observation')
            time.sleep(.005)
        assert data[4:12]==b'CWRT0001','runtime allocation changed'
        result=dict(host_seconds=time.perf_counter()-self.start,
                    cpu_seconds=cpu_seconds(self.vm.process),
                    irq_ticks=struct.unpack_from('<I',data,48)[0],
                    completed_callbacks=struct.unpack_from('<I',data,52)[0],
                    busy=data[35],live=data[37],errors=struct.unpack_from('<H',data,44)[0],
                    mouse=list(struct.unpack_from('<HH',data,62)),
                    mouse_buttons=struct.unpack_from('<H',data,66)[0],
                    sample_seconds=time.perf_counter()-begin)
        if self.abi_version>=2 and self.header_bytes>=140:
            result['metrics']=dict(completed_paints=struct.unpack_from('<I',data,80)[0],
                callback_elapsed_tsc_total=struct.unpack_from('<Q',data,84)[0],
                callback_elapsed_tsc_max=struct.unpack_from('<Q',data,92)[0],
                paint_elapsed_tsc_total=struct.unpack_from('<Q',data,100)[0],
                paint_elapsed_tsc_max=struct.unpack_from('<Q',data,108)[0],
                int16_host_services=struct.unpack_from('<I',data,132)[0],
                physical_mouse_changes=struct.unpack_from('<I',data,136)[0])
        assert not result['errors'],result
        return result

    def interval(self,name,seconds):
        before=self.sample(settled=True);samples=[before]
        deadline=time.perf_counter()+seconds
        while time.perf_counter()<deadline:
            time.sleep(min(.5,max(0,deadline-time.perf_counter())))
            samples.append(self.sample())
        samples.append(self.sample(settled=True))
        return summarize(name,before,samples[-1],samples)

    def mouse_trial(self,dx=6):
        before=self.sample()
        idle_deadline=time.perf_counter()+3
        while before['busy'] and time.perf_counter()<idle_deadline:
            time.sleep(.005);before=self.sample()
        begin=time.perf_counter()
        self.vm.hmp(f'mouse_move {dx} 0')
        delivered=time.perf_counter()
        polls=0;last=before
        while time.perf_counter()-begin<3:
            last=self.sample();polls+=1
            if (last['mouse']!=before['mouse'] and not last['busy']
                and last['completed_callbacks']>before['completed_callbacks']):
                return dict(completed=True,completion_upper_bound_ms=(time.perf_counter()-begin)*1000,
                            input_command_ms=(delivered-begin)*1000,polls=polls,
                            before=before,after=last,relative_mouse_dx=dx)
            time.sleep(.005)
        return dict(completed=False,completion_upper_bound_ms=None,polls=polls,
                    input_command_ms=(delivered-begin)*1000,before=before,after=last,
                    relative_mouse_dx=dx)


def summarize(name,before,after,samples):
    wall=after['host_seconds']-before['host_seconds']
    cpu=after['cpu_seconds']-before['cpu_seconds']
    callbacks=(after['completed_callbacks']-before['completed_callbacks'])&0xffffffff
    ticks=(after['irq_ticks']-before['irq_ticks'])&0xffffffff
    result=dict(name=name,host_elapsed_seconds=wall,qemu_process_cpu_seconds=cpu,
                qemu_cpu_seconds_per_host_second=cpu/wall,
                completed_callbacks=callbacks,completed_callbacks_per_host_second=callbacks/wall,
                observed_irq_ticks=ticks,samples=samples)
    if 'metrics' in before and 'metrics' in after:
        a,b=before['metrics'],after['metrics']
        paints=(b['completed_paints']-a['completed_paints'])&0xffffffff
        callback_tsc=b['callback_elapsed_tsc_total']-a['callback_elapsed_tsc_total']
        paint_tsc=b['paint_elapsed_tsc_total']-a['paint_elapsed_tsc_total']
        result['metrics']=dict(completed_paints=paints,completed_paints_per_host_second=paints/wall,
            callback_elapsed_tsc_total=callback_tsc,paint_elapsed_tsc_total=paint_tsc,
            average_callback_elapsed_tsc=callback_tsc/callbacks if callbacks else None,
            average_paint_elapsed_tsc=paint_tsc/paints if paints else None,
            cumulative_callback_elapsed_tsc_max=b['callback_elapsed_tsc_max'],
            cumulative_paint_elapsed_tsc_max=b['paint_elapsed_tsc_max'],
            int16_host_services=(b['int16_host_services']-a['int16_host_services'])&0xffffffff,
            physical_mouse_changes=(b['physical_mouse_changes']-a['physical_mouse_changes'])&0xffffffff,
            elapsed_tsc_includes_nested_interrupts=True)
    return result


def marker_phase(observer,serial_offset,name):
    vm=observer.vm
    deadline=time.monotonic()+45
    begin=None;samples=[];mouse=[];next_sample=0.;next_mouse=0.
    pattern=re.compile(r'\[DOSPERF\] PHASE '+name+r' END batches=([0-9A-F]{8}) int10=([0-9A-F]{8}) ticks=([0-9A-F]{8}) tsc=([0-9A-F]{16})')
    while time.monotonic()<deadline:
        raw=vm.serial.read_bytes()[serial_offset:].decode('ascii','replace')
        assert '[DOSPERF] FAIL' not in raw,raw
        if begin is None and '[DOSPERF] PHASE '+name+' BEGIN' in raw:
            begin=observer.sample(settled=True);samples.append(begin)
            next_sample=time.perf_counter()+.5
            next_mouse=time.perf_counter()+2
        if begin is not None:
            end=pattern.search(raw)
            if end:
                last=observer.sample(settled=True);samples.append(last)
                result=summarize(name,begin,last,samples)
                result['guest_measurement']=dict(submitted_batches=int(end[1],16),
                    actual_int10_calls=int(end[2],16),elapsed_bios_ticks=int(end[3],16),
                    elapsed_serialized_tsc_ticks=int(end[4],16))
                result['mouse_trials']=mouse
                return result
            if time.perf_counter()>=next_sample:
                samples.append(observer.sample());next_sample=time.perf_counter()+.5
            if len(mouse)<3 and time.perf_counter()>=next_mouse:
                mouse.append(observer.mouse_trial(6 if len(mouse)%2==0 else -6))
                next_mouse=time.perf_counter()+2
        time.sleep(.04)
    raise AssertionError(f'{name}: workload did not finish in 45 host seconds')


def comparison(report,path):
    previous=json.loads(path.read_text())
    assert previous['passed'],'comparison baseline did not finish'
    assert previous['probe_sha256']==report['probe_sha256'],'benchmark program differs'
    old_configuration=previous.get('qemu_configuration')
    if old_configuration is None:
        # Earlier harness reports retain the complete actual QEMU command and
        # desktop screenshot; derive missing metadata from that evidence.
        command=previous['qemu']
        value=lambda option:command[command.index(option)+1]
        with Image.open(path.parent/'idle-command-before.png') as shot:resolution=list(shot.size)
        old_configuration=dict(accelerator=value('-accel'),machine=value('-machine'),
            cpu=value('-cpu'),memory_mib=int(value('-m')),vga=value('-vga'),native_resolution=resolution)
    assert old_configuration==report['qemu_configuration'],'QEMU configuration differs'
    result=dict(baseline_report=str(path),baseline_source_image_sha256=previous['source_image_sha256'],
                note='ratios are same-host KVM observations; lower CPU/latency/elapsed cycles is better')
    for phase in ('idle','bulk','scroll'):
        a,b=previous[phase],report[phase]
        result[phase]={}
        for key in ('qemu_cpu_seconds_per_host_second','completed_callbacks_per_host_second'):
            result[phase][key+'_ratio']=b[key]/a[key] if a[key] else None
        if 'metrics' in a and 'metrics' in b:
            for key in ('completed_paints_per_host_second','average_callback_elapsed_tsc','average_paint_elapsed_tsc'):
                old,new=a['metrics'][key],b['metrics'][key]
                result[phase][key+'_ratio']=new/old if old else None
        if 'guest_measurement' in a:
            result[phase]['submitted_bios_batches_ratio']=b['guest_measurement']['submitted_batches']/a['guest_measurement']['submitted_batches']
    for key in ('median_completion_upper_bound_ms','maximum_completion_upper_bound_ms'):
        old,new=previous['input_latency'][key],report['input_latency'][key]
        result[key+'_ratio']=new/old if old else None
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--listing',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--label',required=True)
    parser.add_argument('--probe',type=Path,default=Path('build/probes/doswindow/DWPERF.COM'))
    parser.add_argument('--idle-seconds',type=float,default=10)
    parser.add_argument('--compare',type=Path,help='prior report with the same QEMU configuration/probe')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    assert not (args.output/'report.json').exists(),'choose a fresh evidence directory'
    original=sha(args.image);disk=args.output/'private.img';shutil.copyfile(args.image,disk)
    subprocess.run(['mcopy','-o','-i',str(disk),str(args.probe),'::APPS/DWPERF.COM'],check=True)
    source=FAT16(disk);shell=source.read('SYSTEM/SHELL.COM')
    report=dict(label=args.label,passed=False,events=[],memory_mib=128,
                source_image_sha256=original,shell_sha256=hashlib.sha256(shell).hexdigest(),
                listing_sha256=sha(args.listing),probe_sha256=sha(args.probe),
                scope='relative same-host KVM BIOS text workload, not Pentium III hardware speed or game FPS',
                guest_memory_writes=False,guest_payload_override='only APPS/DWPERF.COM acceptance probe',
                callback_count_is_not_displayed_frames=True,paint_metrics=None,
                qemu_configuration=dict(accelerator='kvm',machine='pc,vmport=off,i8042=on',
                                        cpu='pentium3',memory_mib=128,vga='std'))
    vm=WindowVM(disk,args.output,'std',memory=128,palette='platinum');report['qemu']=vm.process.args
    observer=None
    try:
        vm.ready();ui=Session(vm,shell,args.listing,report)
        report['qemu_configuration']['native_resolution']=[ui.w('ui_width'),ui.w('ui_height')]
        ui.click(3);ui.until(lambda:ui.b('dos_host_active')==1,'COMMAND window did not launch')
        vm.position(1160,650);time.sleep(.5)
        ui.screen('idle-command-before')
        observer=Observer(ui)
        report['runtime_abi_version']=observer.abi_version
        report['runtime_header_bytes']=observer.header_bytes
        report['paint_metrics']='instrumented completed presents, not distinct game frames' if observer.abi_version>=2 else None
        report['idle']=observer.interval('idle COMMAND',args.idle_seconds)
        report['idle_mouse_trials']=[observer.mouse_trial(6 if i%2==0 else -6) for i in range(6)]
        actual_pointer=vm.pointer();sample=observer.sample()
        assert (abs(actual_pointer[0]-sample['mouse'][0])<=1 and abs(actual_pointer[1]-sample['mouse'][1])<=1), ('rendered pointer differs from completed callback',actual_pointer,sample)
        ui.screen('idle-command-after-input')
        offset=vm.offset();vm.text('run DWPERF.COM')
        vm.wait('[DOSPERF] START',offset,20)
        report['bulk']=marker_phase(observer,offset,'BULK')
        report['scroll']=marker_phase(observer,offset,'SCROLL')
        vm.wait('[DOSPERF] DONE',offset,10)
        ui.screen('workload-completed')
        trials=report['idle_mouse_trials']+report['bulk']['mouse_trials']+report['scroll']['mouse_trials']
        completed=[t['completion_upper_bound_ms'] for t in trials if t['completed']]
        report['input_latency']=dict(trials=len(trials),completed=len(completed),
            median_completion_upper_bound_ms=statistics.median(completed) if completed else None,
            maximum_completion_upper_bound_ms=max(completed) if completed else None,
            note='includes monitor delivery and read-only polling; verified rendered pointer after idle trials')
        vm.text('exit');ui.until(lambda:ui.b('dos_host_active')==0,'COMMAND EXIT failed',20)
        ui.unhooked();report['passed']=True
        if args.compare:report['comparison']=comparison(report,args.compare)
    except Exception as exc:
        report.update(passed=False,error=repr(exc))
        try:ui.screen('failure')
        except Exception:pass
        raise
    finally:
        vm.close()
        if observer:report['measurement_observer']=dict(small_memory_read_calls=observer.read_calls,
            small_memory_read_bytes=observer.read_bytes,monitor_read_wall_seconds=observer.read_seconds)
        if sha(args.image)!=original:report.update(passed=False,error='source image changed')
        (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    assert report['passed'],report.get('error')


if __name__=='__main__':main()
