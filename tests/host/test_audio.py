"""F4 waveform controls and runner retention, with no QEMU invocation."""
import array
import copy
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import run as runner
import test_runner as fixture
from loader_model import selector


def expectations():
    case = runner.load(ROOT / 'tests/suites/f4-audio.json')['cases'][0]
    result = copy.deepcopy(case['audio_expectations'])
    # The same DFT checker; narrower bands keep repeated host controls cheap.
    for window in result['windows']:
        if 'frequency' in window:
            f = window['frequency']
            window['dft_band'] = [f - 20, f + 20]
    return result


def capture(path, rate=48000, *, swap=False, frequency=440, amplitude=1,
            gap=False, seconds=4):
    samples = array.array('h')
    for frame in range(round((seconds + .2) * rate)):
        t = frame / rate - .1
        left = right = 0
        if 0 <= t < seconds:
            if t < 1 or t >= 2:
                left = round(6000 * amplitude * (1 if t < 2 else .5) * math.sin(2 * math.pi * frequency * t))
            if t >= 1:
                right = round(6000 * (1 if t < 2 else .25) * math.sin(2 * math.pi * 880 * t))
            if gap and 2.5 < t < 2.51:
                left = 0  # right keeps playing: channel-specific gap must fail
        samples.extend((right, left) if swap else (left, right))
    if sys.byteorder != 'little':
        samples.byteswap()
    with wave.open(str(path), 'wb') as output:
        output.setparams((2, 2, rate, 0, 'NONE', 'not compressed'))
        output.writeframes(samples.tobytes())


class AudioWaveTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build/audio-host-tests'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.path = Path(self.temp.name) / 'audio.wav'

    def tearDown(self):
        self.temp.cleanup()

    def test_patterns_and_capture_rates(self):
        for rate in (44100, 48000):
            capture(self.path, rate)
            observed = runner.check_audio_wav(self.path, expectations())
            self.assertEqual(observed['failures'], [], observed)
            self.assertAlmostEqual(observed['duration_seconds'], 4, places=2)
            self.assertEqual([w.get('dominant_hz') for w in observed['windows']],
                             [440, None, None, 880, 440, 880])

    def test_waveform_negative_controls(self):
        for options, failure in (({'swap': True}, 'channel RMS'),
                                 ({'frequency': 452}, 'frequency'),
                                 ({'amplitude': .5}, 'channel RMS'),
                                 ({'gap': True}, 'channel gap'),
                                 ({'seconds': 4.3}, 'duration')):
            with self.subTest(options=options):
                capture(self.path, **options)
                observed = runner.check_audio_wav(self.path, expectations())
                self.assertIn(failure, observed['failures'])
        with wave.open(str(self.path), 'wb') as output:
            output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
            output.writeframes(bytes(48000 * 4))
        with self.assertRaisesRegex(runner.EvidenceError, 'silent'):
            runner.check_audio_wav(self.path, expectations())

    def test_selector_and_device_exception(self):
        selected = selector('f4:audio-native run=12345678', 'fw_cfg', True)
        self.assertEqual(selected['phase'], 4)
        self.assertEqual(selected['probe'], 'audio-native')
        for text in ('f4:boot run=12345678', 'f4:all run=12345678', 'f3:audio-native run=12345678'):
            with self.assertRaises(ValueError):
                selector(text, 'fw_cfg', True)
        suite = runner.load(ROOT / 'tests/suites/f4-audio.json')
        for case in suite['cases']:
            profile = runner.load(ROOT / f"tests/profiles/{case['profile']}.json")
            self.assertEqual(profile['ram_mib'], 512)
            args, request = runner.qemu_args('fixture', profile, case, '12345678', self.path, self.path)
            self.assertEqual(request, 'f4:audio-native run=12345678')
            self.assertIn('none,id=silent', args)
            self.assertIn('wav,id=cap,path=audio.wav', args)
            self.assertEqual(args.count('AC97,audiodev=cap'), 1)
            self.assertNotIn('AC97,audiodev=silent', args)
            broken = {**case, 'device_exceptions': {'audio': 'unknown'}}
            with self.assertRaises(runner.res.Refusal):
                runner.qemu_args('fixture', profile, broken, '12345678', self.path, self.path)

    def test_declared_evidence_and_absent_backend(self):
        records = [dict(event='BEGIN'),
                   dict(event='DATA',case='memory',usable_kib=524288,free_pages=120000,reserved_pages=800,audio_pages=13),
                   dict(event='DATA',case='identity',vendor='8086',device='2415',generation=1,codec='83847600'),
                   dict(event='DATA',case='transport',accepted_rate=48000,period_frames=256,published_periods=8,mute=0,master_left=50,master_right=50),
                   dict(event='DATA',case='clock',fifo=0,halts=0,deadlines=0,quarantine=0),
                   dict(event='DATA',case='source',index=0,pid=2,accepted=192000,consumed=192000,queued=0,starvation=0,error=0),
                   dict(event='DATA',case='source',index=1,pid=3,accepted=192000,consumed=192000,queued=0,starvation=0,error=0),
                   dict(event='DATA',case='service',max_service_us=100,max_irq_us=10),
                   dict(event='DATA',case='cleanup',before_pages=13,after_pages=13),
                   dict(event='DATA',case='codec_paths',pcm_unity='0000',nam_bytes=1024,nabm_bytes=256),
                   dict(event='END',status='PASS')]
        def parsed(rows):
            parser = runner.Parser('12345678','audio-native')
            for sequence, row in enumerate(rows,1):
                parser.feed((f'CIUKI_TEST v=1 run=12345678 seq={sequence:06d} probe=audio-native '+
                             ' '.join(f'{key}={value}' for key,value in row.items())).encode())
            return parser
        expected = runner.load(ROOT/'tests/suites/f4-audio.json')['cases'][0]['expected']
        self.assertTrue(parsed(records).check(expected))
        for index,field,value in ((5,'consumed',191999),(7,'max_service_us',1001),(8,'after_pages',14)):
            broken=copy.deepcopy(records);broken[index][field]=value
            with self.assertRaises(runner.EvidenceError):
                parsed(broken).check(expected)
        missing=records[:1]+records[2:]
        with self.assertRaises(runner.EvidenceError):
            parsed(missing).check(expected)
        absent=parsed([dict(event='BEGIN'),dict(event='READY',case='backend',status='not_run',reason='device_absent'),
                       dict(event='ERROR',status='not_run',reason='device_absent')])
        self.assertEqual(absent.outcome,'not_run')
        self.assertEqual(absent.terminal['reason'],'device_absent')
        for reason in ('activation_failed','unknown'):
            with self.assertRaises(runner.EvidenceError):
                parsed([dict(event='BEGIN'),dict(event='READY',case='backend',status='not_run',reason=reason),
                        dict(event='ERROR',status='not_run',reason=reason)])


class AudioRunnerTests(unittest.TestCase):
    setUp = fixture.RunnerTests.setUp
    tearDown = fixture.RunnerTests.tearDown
    run_fake = fixture.RunnerTests.run_fake

    def test_capture_numbers_and_pass_cleanup_failure_retention(self):
        self.case.update(probe='audio-native', selector='f4:audio-native run={run_id}',
                         device_exceptions={'audio': 'ac97'},
                         expected={'terminal': 'END', 'predicates': []},
                         audio_expectations=expectations())
        launch = fixture.FakeHost.launch
        for bad in (False, True):
            def captured(host, args, cwd):
                capture(cwd / 'audio.wav', frequency=452 if bad else 440)
                return launch(host, args, cwd)
            with patch.object(fixture.FakeHost, 'launch', captured):
                result, directory = self.run_fake(records=[{'event': 'BEGIN'}, {'event': 'END', 'status': 'PASS'}])
            self.assertEqual(result['outcome'], 'fail' if bad else 'pass', result['reason'])
            self.assertTrue(result['cleanup']['clean'])
            self.assertEqual((directory / 'audio.wav').exists(), bad)
            self.assertEqual(result['audio']['sample_rate'], 48000)
            self.assertEqual(bool(result['audio']['failures']), bad)
            if not bad:
                self.assertEqual({p.name for p in directory.iterdir()}, {'serial.log', 'result.json'})

    def test_malformed_capture_is_recorded_as_failure(self):
        self.case.update(probe='audio-native', selector='f4:audio-native run={run_id}',
                         device_exceptions={'audio': 'ac97'},
                         expected={'terminal': 'END', 'predicates': []},
                         audio_expectations=expectations())
        launch = fixture.FakeHost.launch
        def malformed(host, args, cwd):
            (cwd / 'audio.wav').write_bytes(b'not a RIFF capture')
            return launch(host, args, cwd)
        with patch.object(fixture.FakeHost, 'launch', malformed):
            result, directory = self.run_fake(records=[{'event': 'BEGIN'}, {'event': 'END', 'status': 'PASS'}])
        self.assertEqual(result['outcome'], 'fail')
        self.assertTrue((directory / 'audio.wav').is_file())
        self.assertTrue(result['cleanup']['clean'])


if __name__ == '__main__':
    unittest.main()
