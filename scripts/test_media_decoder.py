#!/usr/bin/env python3
"""Exercise the production audio decoder with generated, redistributable tones."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    assert shutil.which('ffmpeg'), 'ffmpeg is required for codec fixtures'
    with tempfile.TemporaryDirectory(prefix='ciuki-media-') as work:
        work = Path(work)
        exe = work / 'decoder'
        subprocess.run(['cc', '-std=gnu99', '-O2', '-o', str(exe),
                        str(ROOT / 'scripts/fixtures/media_decoder_host.c'),
                        str(ROOT / 'src/media/decoder.c'), '-lm'], check=True)
        reports = {}
        for extension, rate, channels in [('wav', 8000, 1), ('Wav', 44100, 2),
                                          ('mp3', 44100, 2), ('flac', 96000, 2),
                                          ('ogg', 48000, 1)]:
            path = work / ('tone.' + extension)
            subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                            'sine=frequency=523:duration=1.25', '-ar', str(rate),
                            '-ac', str(channels), '-y', str(path)], check=True)
            report = json.loads(subprocess.check_output([str(exe), str(path)]))
            assert report['rate'] == 48000 and report['nonzero'] > 10000, report
            assert abs(report['actual'] - 60000) <= 3000, report
            assert abs(report['total'] - report['actual']) <= 3000, report
            assert report['seek_frames'] > 0, report
            reports[extension] = report
        for extension in ('wav', 'mp3', 'flac', 'ogg'):
            path = work / ('bad.' + extension)
            path.write_bytes(b'CiukiOS damaged media\0' * 3)
            result = subprocess.run([str(exe), str(path)], capture_output=True)
            assert result.returncode == 3, (extension, result.returncode)
        print(json.dumps(reports, indent=2))
        print('Production media decoder: PASS formats, resampling, mono, seek, invalid input')


if __name__ == '__main__':
    main()
