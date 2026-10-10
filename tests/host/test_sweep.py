import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
from sweep_loader_model import config
import run as runner


class SweepTests(unittest.TestCase):
    def test_cfg_source_and_menu_precedence(self):
        cfg = 'safe=0 serial=1\nprobe=all:sweep run=12345678\n'
        self.assertEqual(config(cfg)['selection']['phase'], 3)
        self.assertEqual(config(cfg)['source'], 'cfg')
        self.assertIsNone(config(cfg, menu='N')['selection'])
        self.assertTrue(config(cfg, menu='S')['safe'])
        fw = {'probe':'boot'}
        self.assertEqual(config(cfg, fw_cfg=fw, menu='N')['selection'], fw)
        self.assertTrue(config('safe=1\nprobe=f1:sweep run=12345678\n', menu='N')['safe'])
        for suffix in (' platform=e500',' safe=1',' server=desktop',' step=64',' step=1 state=ffffffff'):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                config('probe=f1:sweep run=12345678' + suffix)
        for value in ('probe=f0:sweep run=12345678\n'*2, ' '*128, 'probe=f0:sweep run=12345678\0'):
            with self.assertRaises(ValueError): config(value)
        self.assertIsNotNone(config(' '*99+'probe=f0:sweep run=12345678\n')['selection'])
        with self.assertRaises(ValueError): runner.selector('f0:sweep run=12345678', 'fw_cfg', True)

    def test_production_parser_and_reboot(self):
        folder = ROOT / 'build/host/sweep'; folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=folder) as temp:
            binary = Path(temp) / 'sweep-test'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                            '-DREBOOT_HOST','-I'+str(ROOT/'src/kernel/include'),'-I'+str(ROOT/'src/kernel/probes'),
                            str(ROOT/'tests/host/sweep_test.c'),str(ROOT/'src/kernel/probes/selector.c'),
                            str(ROOT/'src/kernel/core/reboot.c'),'-o',str(binary)], check=True,
                           env={**os.environ, 'TMPDIR':temp})
            subprocess.run([str(binary)], check=True, env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})

    def test_sweep_smoke_uses_cfg_and_no_reboot(self):
        case = runner.load_suite('sweep-smoke')['cases'][0]
        profile = runner.load(ROOT/'tests/profiles/qemu-t23.json')
        args, request = runner.qemu_args('qemu', profile, case, '12345678', Path('run.qcow2'), Path('bios.bin'))
        self.assertEqual(request, 'f0:sweep run=12345678')
        self.assertIn('-no-reboot', args); self.assertNotIn('-fw_cfg', args)
        self.assertNotIn('-no-shutdown', args)
        self.assertEqual(case['max_boots'], 11)

    def test_live_sweep_records_require_cfg_and_keep_original_envelopes(self):
        parser = runner.SweepParser('12345678')
        parser.feed(b'L:SELECT_SOURCE=cfg\r\n')
        for seq, event in enumerate(('BEGIN', 'END status=PASS', 'SWEEP step=0 result=pass'), 1):
            parser.feed(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe=boot event={event}\n'.encode())
        self.assertTrue(parser.completed)
        parser.feed(b'CIUKI_TEST v=1 run=12345678 seq=000004 probe=sweep event=SWEEP_END passed=1 failed=0 not_run=0\n')
        self.assertTrue(parser.check({'passed':1,'failed':0,'not_run':0}))
        self.assertEqual(parser.records[0]['seq'], '000001')
        parser = runner.SweepParser('12345678')
        parser.feed(b'CIUKI_TEST v=1 run=12345678 seq=000001 probe=sweep event=SWEEP_END passed=0 failed=0 not_run=0\n')
        with self.assertRaisesRegex(ValueError, 'provenance'): parser.check({'passed':0,'failed':0,'not_run':0})

    def test_relaunches_share_one_overlay_and_run_id_and_are_bounded(self):
        folder = ROOT / 'build/host/sweep'; folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=folder) as temp:
            root = Path(temp); image = root / 'image.img'; image.write_bytes(b'canonical')
            case = runner.load_suite('sweep-smoke')['cases'][0]
            host = type('Host', (), {'preflight':lambda self,path:None})()
            seen = []; sequence = [
                {'sweep_continue':True,'observed':[{'event':'SWEEP'}]},
                {'observed':[{'event':'PANIC'}]},
                {'observed':[{'event':'SWEEP_END','passed':'9','failed':'0','not_run':'1'}]}]
            def boot(*args, **kwargs):
                selected = args[2]; seen.append((selected['_run_id'], kwargs['shared_overlay']))
                directory = root/'build/test-runs/sweep-smoke'/selected['_run_id']/('boot-'+str(selected['_sweep_boot']))
                directory.mkdir(parents=True)
                return {'outcome':'pass','reason':'observed',**sequence[len(seen)-1]}, directory
            with patch.object(runner, '_run_boot', side_effect=boot), patch.object(runner.subprocess, 'run'), \
                 patch.object(runner.secrets, 'token_hex', return_value='12345678'):
                result, _ = runner.run_sweep(root,'sweep-smoke',case,{},image,'qemu','bios',host)
            self.assertEqual(result['outcome'], 'pass');self.assertEqual(len(result['sweep_boots']),3)
            self.assertEqual(len(set(seen)),1);self.assertEqual(image.read_bytes(),b'canonical')
            seen.clear();sequence=[{'sweep_continue':True,'observed':[{'event':'SWEEP'}]}]*2
            with patch.object(runner, '_run_boot', side_effect=boot), patch.object(runner.subprocess, 'run'), \
                 patch.object(runner.secrets, 'token_hex', return_value='87654321'):
                result, _ = runner.run_sweep(root,'sweep-smoke',{**case,'max_boots':2},{},image,'qemu','bios',host)
            self.assertEqual(result['outcome'],'fail');self.assertIn('bounded',result['reason'])
