#!/usr/bin/env python3
"""DOS/4GW programs inside the native DOS window with the session's devices.

An unchanged DOS/4GW program (default: doom-vanille PCDMCORE.EXE; the original
id DOOM is APPS\\DOOM\\DOOMCORE.EXE) runs in the desktop DOS window through
DPMIRUN.COM and the session-bound patched HDPMI 3.24:

  JEMM386 (V86 monitor) + JLOAD CVSESS.DLL (VGA model, scheduler, SB16/OPL/
  PIC/keyboard/mouse model, AC'97 output) + DPMIRUN -> HDPMI32I -c<desc> -r

Gate checks (game mode): no DOS/4GW or HDPMI fatal error; the game draws
(protected-mode VGA faults grow, no DAMAGE geometry error); the keyboard
moves the view; OPL writes (unless -nomusic) and SB blocks reach the model;
SB interrupts are delivered to the protected-mode handler (the adapter's
CVPMIRQ counters); the recorded AC'97 output carries sound while keys fire
the pistol / open the menu; F10+Y quits and DPMIRUN releases the device IRQs
and unloads the host. --probe runs DPMIPORT.EXE (VGA register/memory forms)
and passes on its own verdict.

QEMU evidence only; no physical-hardware claim is made.
"""
import argparse
import array
import collections
import hashlib
import json
import math
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_installed_hdd import FAT16                     # noqa: E402
from qemu_test_native_windows import WindowVM                 # noqa: E402
from qemu_test_dos_window import Session                      # noqa: E402
from qemu_test_vga_window import Harness, launch               # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DEVICE_FIELDS = ['magic', 'version_bytes', 'active', 'caps', 'focused', 'audio',
                 'keys_forwarded', 'keys_dropped', 'aux_bytes', 'lazy_pulls',
                 'irqs_raised', 'irq_failures', 'port_reads', 'port_writes',
                 'unclaimed_io', 'model_errors', 'last_error', 'last_port',
                 'buffers_rendered', 'underruns', 'polls', 'sb_blocks',
                 'opl_writes', 'dsp_commands', 'ac97_nam', 'ac97_nabm',
                 'bridge_calls', 'rate', 'ac97_civ', 'ac97_status', 'ac97_queued', 'pm_state']
FATAL = ('DOS/4GW error', 'hdpmi: fatal exit', 'Jemm386: exception', 'DPMIRUN: ')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def find_all(blob, magic, words):
    """Every copy of a counter block (magic + dwords) in a RAM image."""
    found, at = [], blob.find(magic)
    while at >= 0:
        found.append([int.from_bytes(blob[at + len(magic) + 4 * i:at + len(magic) + 4 + 4 * i], 'little')
                      for i in range(words)])
        at = blob.find(magic, at + 1)
    return found


def audio_windows(path, seconds=0.25):
    """RMS per window of the QEMU wav capture (header sizes may be unfinalized)."""
    raw = Path(path).read_bytes()[44:]
    samples = array.array('h')
    samples.frombytes(raw[:len(raw) // 4 * 4])
    step = int(44100 * seconds)
    result = []
    for start in range(0, len(samples) // 2 - step, step):
        chunk = samples[start * 2:(start + step) * 2:6]
        result.append(int(math.sqrt(sum(x * x for x in chunk) / max(1, len(chunk)))))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('image', 'kernel', 'shell', 'listing', 'runtime', 'jemm', 'jload', 'module',
                 'hdpmi', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--launcher', type=Path, help='DPMIRUN.COM (default: assembled from src/com/dpmirun.asm)')
    parser.add_argument('--program', default='\\APPS\\DOOMVAN\\PCDMCORE.EXE',
                        help='DOS/4GW program run through DPMIRUN (default: doom-vanille)')
    parser.add_argument('--game-args', default='-warp 1 1')
    parser.add_argument('--probe', type=Path, help='run this DPMIPORT.EXE instead of the game')
    parser.add_argument('--samples', type=int, default=4, help='5-second gameplay samples')
    parser.add_argument('--keys', default='esc,esc,ctrl,ctrl,ctrl,ctrl',
                        help='QEMU keys sent after walking forward (sound-producing input)')
    parser.add_argument('--min-audio-rms', type=int, default=400,
                        help='loudest 0.25 s window of the game audio must reach this RMS')
    parser.add_argument('--auto', action='store_true',
                        help='main-build flow: the desktop started the VM at boot; run the program '
                             'directly in a DOS window (its DPMI host comes with the window)')
    parser.add_argument('--image-vm', action='store_true',
                        help='use the image\'s own C:\\VM profile: \\VM\\VMSTART.COM, \\VM\\DPMIRUN.COM')
    parser.add_argument('--profile', type=int, default=0, help='register samples for a CPU profile')
    parser.add_argument('--stop-on', default=None, help='freeze on this serial text and dump RAM (debug)')
    parser.add_argument('--jemm-options',
                        default='NOEMS NOHI X=A000-CCFF I=CD00-EBFF X=EC00-FFFF NODYN MAX=32M MIN=32M NOVME')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    before = sha(args.image)
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    launcher = args.launcher
    if not launcher:
        launcher = output / 'DPMIRUN.COM'
        subprocess.run(['nasm', '-f', 'bin', '-I', str(ROOT) + '/', str(ROOT / 'src/com/dpmirun.asm'),
                        '-o', str(launcher)], check=True, cwd=ROOT)
    files = [(args.kernel, 'SYSTEM/CIUKIDOS.SYS'), (args.shell, 'SYSTEM/SHELL.COM'),
             (args.runtime, 'SYSTEM/DOSWIN.DRV')]
    if not (args.image_vm or args.auto):
        files += [(args.jemm, 'JEMM386.EXE'), (args.jload, 'JLOAD.EXE'), (args.module, 'CVSESS.DLL'),
                  (args.hdpmi, 'SBEMU/HDPMI32I.EXE'), (launcher, 'DPMIRUN.COM')]
    if args.probe:
        files.append((args.probe, 'APPS/DOOMVAN/DPMIPORT.EXE'))
    for path, target in files:
        subprocess.run(['mcopy', '-o', '-i', volume, str(path), '::' + target], check=True)
    record = dict(passed=False, checks={}, source_image_sha256=before, inputs={t: sha(p) for p, t in files},
                  program=args.program, game_args=args.game_args,
                  physical_hardware_qualified=False, events=[])
    vm = WindowVM(disk, output, 'std', boot_capture=True, memory=128, palette='platinum')
    record['qemu_command'] = vm.process.args
    h = Harness(vm, output)
    started = time.monotonic()

    def event(name, **details):
        record['events'].append(dict(name=name, seconds=round(time.monotonic() - started, 2), **details))
        print('[dpmi-window] ' + name, flush=True)

    def check(name, ok, **details):
        record['checks'][name] = dict(ok=bool(ok), **details)
        print(f'[dpmi-window] {"PASS" if ok else "FAIL"} {name} {details if details else ""}', flush=True)

    def ram():
        path = output / 'ram.bin'
        vm.hmp(f'pmemsave 0 0x8000000 "{path}"')
        blob = path.read_bytes()
        path.unlink()
        return blob

    try:
        vm.ready()
        if not args.auto:
            vm.key('f4')
            vm.wait('CiukiOS SHELL C:\\APPS>')
        if args.image_vm and not args.auto:
            h.console('run \\VM\\VMSTART.COM', 'Jemm386 loaded', 'loaded successfully', 'VMSTART: ready')
        elif not args.auto:
            h.console('run \\JEMM386.EXE LOAD ' + args.jemm_options, 'Jemm386 loaded')
            h.console('run \\JLOAD.EXE \\CVSESS.DLL', 'loaded successfully')
        dpmirun = '' if args.auto else '\\VM\\DPMIRUN.COM ' if args.image_vm else '\\DPMIRUN.COM '
        if not args.auto:
            offset = vm.offset()
            h.typed('exit')
            vm.ready(offset)
        shell = FAT16(disk).read('SYSTEM/SHELL.COM')
        ui = Session(vm, shell, args.listing, dict(events=record['events'], cases=[]))
        if args.auto:
            # The desktop started the VM manager at boot and stays as fast as
            # in real mode (CVSESSION maps its VBE bank window, no firmware).
            boot = h.serial(0)
            check('vm_started_at_boot', 'VMSTART: ready.' in boot and 'loaded successfully' in boot)
            opened = []
            for _ in range(3):
                start = time.monotonic()
                vm.key('f3')
                ui.until(lambda: ui.b('ui_active_window') == 1, 'Run did not open')
                opened.append(round(time.monotonic() - start, 3))
                vm.key('esc')
                time.sleep(1.5)
            check('desktop_fast_under_vm', max(opened[1:]) < 0.2, open_run_seconds=opened)
        offset = vm.offset()
        if args.probe:
            launch(ui, vm, f'run {dpmirun}\\APPS\\DOOMVAN\\DPMIPORT.EXE', event)
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and not any(
                    m in h.serial(offset) for m in ('[DPMIPORT] PASS', '[DPMIPORT] FAIL', '[DPMIRUN] EXIT')):
                time.sleep(.5)
            record['probe'] = [l for l in h.serial(offset).splitlines() if 'DPMIPORT' in l or 'DPMIRUN' in l]
            check('dpmiport', any('[DPMIPORT] PASS' in l for l in record['probe']),
                  verdict=next((l for l in record['probe'] if 'PASS' in l or 'FAIL' in l), None))
            raise SystemExit
        launch(ui, vm, f'run {dpmirun}' + args.program + ' ' + args.game_args, event)
        event('launched')
        if args.stop_on:
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and args.stop_on not in h.serial(offset):
                time.sleep(.05)
            vm.hmp('stop')
            record['stopped_registers'] = vm.hmp('info registers').decode('utf-8', 'replace')
            vm.hmp(f'pmemsave 0 0x8000000 "{output}/stop-ram.bin"')
            record['stop_serial'] = h.serial(offset)[-2500:]
            vm.hmp('cont')
            raise SystemExit
        def sample(tag):
            h.shot(f'game-{tag}')
            data, state = ui.module()
            share = data.find(b'CVSH')
            video = {}
            if share >= 0:
                physical = int.from_bytes(data[share + 8:share + 12], 'little')
                path = output / 'shared-header.bin'
                vm.hmp(f'pmemsave {physical} 4096 "{path}"')
                header = path.read_bytes()
                names = ('pm_faults', 'pm_instructions', 'pm_elements', 'pm_unsupported',
                         'pm_port_reads', 'pm_port_writes', 'pm_attached')
                video = {n: int.from_bytes(header[32 + 4 * i:36 + 4 * i], 'little') for i, n in enumerate(names)}
                video['device_irqs'] = int.from_bytes(header[320:324], 'little')
            video['damage_result'] = int.from_bytes(data[234:236], 'little')
            record.setdefault('video', []).append(dict(t=tag, seconds=round(time.monotonic() - started, 1),
                                                       paints=int.from_bytes(data[80:84], 'little'), **video))

        for k in range(args.samples):
            time.sleep(5)
            sample(k)
        if args.profile:
            hist = collections.Counter()
            for _ in range(args.profile):
                text = vm.hmp('info registers').decode('utf-8', 'replace')
                eip = re.search(r'EIP=([0-9a-f]{8})', text)
                cpl = re.search(r'CPL=(\d)', text)
                cs = re.search(r'\nCS =([0-9a-f]{4})', text)
                v86 = 'VM' if re.search(r'EFL=[0-9a-f]{3}[2367abef]', text) else ''
                if eip and cpl:
                    hist[(cpl.group(1), cs.group(1) if cs else '?', v86, eip.group(1)[:-2] + 'xx')] += 1
            record['profile'] = [dict(cpl=k[0], cs=k[1], v86=k[2], eip=k[3], samples=v)
                                 for k, v in hist.most_common(40)]
        vm.key('ret'); time.sleep(2); h.shot('after-enter')
        vm.key('ret'); time.sleep(2); vm.key('ret'); time.sleep(6)
        before_walk = h.shot('before-walk')
        vm.hmp('sendkey up 2000'); time.sleep(3)
        after_walk = h.shot('after-walk')
        changed = ImageChops.difference(Image.open(before_walk).convert('RGB'),
                                        Image.open(after_walk).convert('RGB')).getbbox()
        check('keyboard_moves_view', changed is not None, changed_box=changed)
        record['key_marks'] = []
        for index, key in enumerate(k for k in args.keys.split(',') if k):
            record['key_marks'].append(dict(key=key, seconds=round(time.monotonic() - started, 1)))
            vm.hmp(f'sendkey {key} 200')
            time.sleep(1.5)
            h.shot(f'key-{index}')
        sample('end')
        video = record['video']
        check('game_draws', video[-1].get('pm_faults', 0) > video[-2].get('pm_faults', 0) > 0
              and video[-1]['damage_result'] == 0,
              pm_faults=[v.get('pm_faults') for v in video], damage=video[-1]['damage_result'])

        blob = ram()
        reports = [dict(zip(DEVICE_FIELDS[1:], words)) for words in find_all(blob, b'CVDV', 31)
                   if words[0] & 0xffff == 0x0100]
        adapter = [dict(zip(('held', 'delivered'), w)) for w in find_all(blob, b'CVPMIRQ!', 2)]
        session = [dict(zip(('claimed', 'held', 'delivered', 'refused'), w)) for w in find_all(blob, b'CVPMHELD', 4)]
        record['device_reports'] = reports
        record['pm_irq'] = dict(adapter=adapter, session=session)
        live = max(reports, key=lambda r: r['polls'], default={})
        delivered = max((a['delivered'] for a in adapter), default=0)
        music = '-nomusic' not in args.game_args
        check('sb_blocks', live.get('sb_blocks', 0) > 0, sb_blocks=live.get('sb_blocks'))
        check('opl_music', not music or live.get('opl_writes', 0) > 0, opl_writes=live.get('opl_writes'))
        check('pm_irq_delivery', delivered > 0 and live.get('sb_blocks', 0) > 0,
              delivered=delivered, raised=live.get('irqs_raised'),
              ratio=round(delivered / max(1, live.get('irqs_raised', 1)), 2))
        check('model_errors', live.get('model_errors', 1) == 0 and live.get('underruns', 1) == 0,
              model_errors=live.get('model_errors'), underruns=live.get('underruns'))

        # Quit: F10, Y. DPMIRUN releases the held IRQs and unloads the host.
        done_marker = 'HDPMI32 uninstalled' if args.auto else '[DPMIRUN] EXIT'
        record['quit_attempts'] = 0
        for _ in range(3):
            record['quit_attempts'] += 1
            vm.key('f10'); time.sleep(1.5); vm.key('y')
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and done_marker not in h.serial(offset):
                time.sleep(.5)
            if done_marker in h.serial(offset):
                break
        time.sleep(3)
        h.shot('after-quit')
        serial = h.serial(offset)
        record['serial'] = serial[-4000:]
        fatal = [f for f in FATAL if f in serial]
        check('no_fatal_errors', not fatal, found=fatal)
        exit_line = next((l for l in serial.splitlines() if '[DPMIRUN] EXIT' in l), None)
        check('clean_exit', (args.auto or exit_line is not None) and 'HDPMI32 uninstalled' in serial,
              exit=exit_line)
        # The window's session ends: host binding released, runtime uninstalled.
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            data, state = ui.module()
            if not state['installed']:
                break
            time.sleep(1)
        check('window_session_ended', not state['installed'] and not state['errors'],
              installed=state['installed'], errors=state['errors'])
        blob = ram()
        session = [dict(zip(('claimed', 'held', 'delivered', 'refused'), w)) for w in find_all(blob, b'CVPMHELD', 4)]
        check('irqs_released', bool(session) and all(s['claimed'] == 0 for s in session), session=session)
        vm.close()
        rms = audio_windows(output / 'audio.wav')
        record['audio_rms'] = rms
        loud = max(rms[8:], default=0)  # skip the desktop's startup sound
        check('audio_output', loud >= args.min_audio_rms, loudest=loud)
        record['passed'] = all(c['ok'] for c in record['checks'].values())
    except SystemExit:
        record['passed'] = bool(record['checks']) and all(c['ok'] for c in record['checks'].values())
    except Exception as error:
        record['error'] = repr(error)
        try:
            h.shot('failure')
            record['registers_on_failure'] = vm.hmp('info registers').decode('utf-8', 'replace')
            record['serial'] = h.serial(0)[-4000:]
        except Exception as diagnostic:
            record['diagnostic_error'] = repr(diagnostic)
    finally:
        vm.close()
    (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: v for k, v in record.items() if k in ('passed', 'error')}, indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
