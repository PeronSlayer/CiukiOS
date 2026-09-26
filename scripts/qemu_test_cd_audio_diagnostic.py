#!/usr/bin/env python3
"""Qualify Live and Safe AC97 from an immutable CD; inject no guest payloads."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import uuid

import numpy as np
from analyze_audio_wav import pcm_payload
from qemu_test_cd_sessions import Session, digest
from qemu_test_installed_hdd import FAT16


class AudioSession(Session):
    def __init__(self, iso, output, case, accelerator):
        self.output, self.case = output, case
        output.mkdir()
        self.serial, self.audio = output / 'serial.log', output / 'audio.wav'
        self.monitor = Path(tempfile.gettempdir()) / f'ciukios-cd-audio-{uuid.uuid4().hex}.sock'
        self.command = [
            'qemu-system-i386', '-accel', accelerator,
            '-machine', 'pc,vmport=off,i8042=on', '-cpu', 'pentium3', '-smp', '1',
            '-m', '128', '-vga', 'std', '-nic', 'none',
            '-drive', f'file={iso},format=raw,if=ide,index=2,media=cdrom,readonly=on',
            '-boot', 'd', '-display', 'none', '-serial', f'file:{self.serial}',
            '-monitor', f'unix:{self.monitor},server=on,wait=off',
            '-audiodev', f'wav,id=snd0,path={self.audio}', '-device', 'AC97,audiodev=snd0',
            '-no-reboot', '-no-shutdown',
        ]
        (output / 'command.json').write_text(json.dumps(self.command, indent=2) + '\n')
        (output / 'command.txt').write_text(shlex.join(self.command) + '\n')
        self.errors = (output / 'qemu.stderr.log').open('wb')
        self.start = time.monotonic()
        self.process = subprocess.Popen(self.command, stdout=subprocess.DEVNULL, stderr=self.errors)
        self.report = dict(case=case, status='running', cpu='pentium3', ram_mib=128,
                           accelerator=accelerator, hard_disks=[], iso_readonly=True,
                           guest_payload_overrides=[], launch='F4 then PS/2 typed sound /d')

    def text(self, command):
        for character in command:
            self.key({' ': 'spc', '/': 'slash', '.': 'dot'}.get(character, character))
        self.key('ret')

    def run(self):
        self.connect()
        self.select_boot_entry(2 if self.case == 'safe' else 0)
        self.wait(b'[DESKTOP] READY', timeout=90)
        if self.case == 'safe':
            self.wait(b'[BOOT-SESSION] SAFE')
        self.settled_desktop('boot-desktop', safe=self.case == 'safe')
        mark = self.offset()
        self.key('f4')
        self.wait(b'CiukiOS SHELL ', mark)
        self.report['boot_audio_end'] = self.audio.stat().st_size
        mark = self.offset()
        self.text('sound /d')
        for stage in (b'Entered player; DOS resize', b'Probe native Sound Blaster',
                      b'PCI BIOS presence', b'Check output busy', b'Allocate and read PCM',
                      b'Configure codec (CAS)', b'DMA start and bounded poll'):
            self.wait(b'[SOUND:D] ' + stage, mark, 30)
        # Observe the BIOS text during playback: the shell redraws after EXEC.
        self.hmp('stop')
        self.hmp(f'pmemsave 0xb8000 4000 "{self.output}/diagnostic.vram"')
        visible = (self.output / 'diagnostic.vram').read_bytes()[::2].decode('cp437')
        assert '[SOUND:D] DMA start' in visible, 'BIOS diagnostic was not visible'
        self.shot('diagnostic-during-playback')
        self.hmp('cont')
        self.wait(b'[SOUND:D] DMA stopped; cleanup', mark, 30)
        self.wait(b'[SOUND:D] Restore PCI command', mark, 30)
        self.wait(b'startup melody played through AC97.', mark, 30)
        self.wait(b'CiukiOS SHELL ', mark, 30)
        self.report['manual_audio_end'] = self.audio.stat().st_size
        assert b'Explicit EC access' not in self.data(mark)
        mark = self.offset()
        self.text('comdemo')
        self.wait(b'COM demo via INT21h', mark)
        self.wait(b'CiukiOS SHELL ', mark)
        self.shot('comdemo-return')
        mark = self.offset()
        self.text('exit')
        self.wait(b'[DESKTOP] READY', mark)
        self.settled_desktop('desktop-return', safe=self.case == 'safe')
        self.report.update(status='passed', comdemo_and_desktop_return=True,
                           qemu_alive_before_cleanup=self.process.poll() is None)

    def close(self):
        self.report['elapsed_seconds'] = round(time.monotonic() - self.start, 3)
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.report['qemu_returncode_after_cleanup'] = self.process.returncode
        self.errors.close()
        self.monitor.unlink(missing_ok=True)
        if 'manual_audio_end' in self.report:
            raw = self.audio.read_bytes()
            pcm, bits = pcm_payload(raw)
            assert raw[36:40] == b'data' and len(raw) - len(pcm) == 44, 'unexpected WAV chunk layout'
            # A silent boot can leave QEMU's output at zero bytes until the
            # first PCM stream starts. Never interpret its later RIFF header
            # as manual audio when that initial file position was zero.
            boot_end = max(44, self.report['boot_audio_end'])
            boundaries = {'boot': (44, boot_end),
                          'manual': (boot_end, self.report['manual_audio_end'])}
            for name, (start, end) in boundaries.items():
                data = raw[start:end]
                samples = np.frombuffer(data[:len(data) // 2 * 2], dtype='<i2').astype(float)
                metric = dict(pcm_bytes=len(data), bits=bits,
                              rms=float(np.std(samples)) if len(samples) else 0,
                              peak=float(np.max(np.abs(samples))) if len(samples) else 0)
                self.report[name + '_audio'] = metric
                if name == 'boot' and self.case == 'safe':
                    assert not np.any(samples), 'Safe boot unexpectedly played sound'
                else:
                    assert bits == 16 and len(data) > 100000 and metric['rms'] > 100, f'{name} PCM missing'
        (self.output / 'serial.normalized.log').write_bytes(self.data())
        (self.output / 'result.json').write_text(json.dumps(self.report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--expect-sha256', required=True)
    parser.add_argument('--expect-sound-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    iso, out = args.iso.resolve(), args.output.resolve()
    assert not out.exists(), 'Use fresh output directory to preserve evidence'
    assert digest(iso) == args.expect_sha256, 'Unexpected ISO'
    out.mkdir(parents=True)
    # Read the actual ISO payload independently. Never substitute these files
    # into the VM: only the original read-only ISO is attached to QEMU.
    packed, payload = out / 'payload.img.gz', out / 'payload.img'
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(iso),
                    '-extract', '/ciukios-full-cd-disk.img.gz', str(packed)],
                   stdout=(out / 'extract.log').open('w'), stderr=subprocess.STDOUT, check=True)
    with gzip.open(packed, 'rb') as source, payload.open('wb') as target:
        shutil.copyfileobj(source, target)
    fat = FAT16(payload)
    hashes = {path: hashlib.sha256(fat.read(path)).hexdigest()
              for path in ('SYSTEM/BOOTSND.COM', 'SYSTEM/DRIVERS/SOUND.COM', 'SYSTEM/SHELL.COM')}
    assert hashes['SYSTEM/BOOTSND.COM'] == hashes['SYSTEM/DRIVERS/SOUND.COM'] == args.expect_sound_sha256
    del fat
    packed.unlink()
    payload.unlink()
    report = dict(iso=str(iso), iso_sha256=args.expect_sha256, payload_sha256=hashes, cases={})
    try:
        for case in ('live', 'safe'):
            session = AudioSession(iso, out / case, case, args.accel)
            try:
                session.run()
            except Exception as exc:
                session.report.update(status='failed', error=str(exc))
                if session.process.poll() is None:
                    session.shot('failure')
                raise
            finally:
                session.close()
                report['cases'][case] = session.report
                print(json.dumps(session.report, indent=2), flush=True)
    finally:
        report['iso_unchanged'] = digest(iso) == args.expect_sha256
        report['passed'] = (len(report['cases']) == 2 and report['iso_unchanged']
                            and all(case['status'] == 'passed' for case in report['cases'].values()))
        (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
