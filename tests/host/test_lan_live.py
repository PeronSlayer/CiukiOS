"""f6-28 host evidence only: scripted processes, no emulator or systemd."""
from contextlib import contextmanager
import copy
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
sys.path.insert(0, str(Path(__file__).parent))
import acceptance
import dos_fixtures
import lan_live as live
import lan_runner
import resources as res
import run as runner
from test_runner import FakeHost
from test_acceptance import FakeQMP, boot_lines

SCRATCH = ROOT / 'build/host/lan-live'
TOOLS = all(shutil.which(t) for t in ('mkfs.fat', 'mcopy', 'mtype', 'mmd', 'mdir', 'fsck.fat'))


def cases():
    return json.loads((ROOT / 'tests/suites/lan-t23.json').read_text())['cases']


class Base(unittest.TestCase):
    def setUp(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=SCRATCH); self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def fixtures(self):
        directory = self.root / live.PE_DIR; directory.mkdir(parents=True)
        source_dir = self.root / 'tests/win32/lan'; source_dir.mkdir(parents=True)
        for name in ('build.py', 'common.h', 'ipx.c', 'directplay.c'):
            (source_dir / name).write_bytes((ROOT / 'tests/win32/lan' / name).read_bytes())
        rows = {}
        for name in live.PE_NAMES:
            data = b'MZ' + bytes(range(64))
            (directory / name).write_bytes(data)
            sources = ['tests/win32/lan/' + p for p in ('build.py', 'common.h', name[5:-4] + '.c')]
            rows[name] = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data), 'imports': {},
                          'sources': {p: hashlib.sha256((self.root / p).read_bytes()).hexdigest() for p in sources}}
        (directory / 'build.json').write_text(json.dumps(rows))
        return rows


class ValidationTests(Base):
    def test_suite_declares_required_cases_and_strict_recipes(self):
        suite = cases()
        self.assertEqual(len(suite), 6)
        for c in suite:
            self.assertEqual(live.validate(c, ROOT), acceptance.KIND)
        self.assertEqual([g['lan_fixture']['address'] for g in suite[1]['guests']], ['192.0.2.2', '192.0.2.1'])
        self.assertEqual(len(suite[4]['guests']), 3)
        self.assertEqual(suite[5]['guests'][1]['lan_fixture']['frame'], '802.3')
        for mutation in ('four', 'duplicate', 'role', 'recipe', 'address', 'backend', 'profile', 'peers', 'nested'):
            c = copy.deepcopy(suite[0])
            if mutation == 'four': c['guests'] *= 2
            elif mutation == 'duplicate': c['guests'][1]['id'] = c['guests'][0]['id']
            elif mutation == 'role': c['guests'][0]['lan_fixture']['role'] = 'join'
            elif mutation == 'recipe': c['guests'][0]['steps'][-1]['wait'] = '.*'
            elif mutation == 'address': c['guests'][0]['lan_fixture']['address'] = '10.0.2.15'
            elif mutation == 'backend': c['lan_live'] = 'user'
            elif mutation == 'profile': c['profile'] = 'qemu-e500'
            elif mutation == 'peers': c['guests'][0]['lan_fixture']['peers'] = True
            else: c['guests'][0]['guests'] = []
            with self.subTest(mutation=mutation), self.assertRaises(res.Refusal): live.validate(c, ROOT)
        c = copy.deepcopy(suite[4]); c['lan_live'] = 'stream'
        with self.assertRaisesRegex(res.Refusal, 'multicast'): live.validate(c, ROOT)

    def test_netdev_arguments_and_unique_runtime_identities(self):
        for count, backend in ((2, 'stream'), (3, 'mcast')):
            nets = lan_runner.pair_networks(count, backend)
            self.assertEqual(len({n['mac'] for n in nets}), count)
            self.assertEqual(len({n['identity'] for n in nets}), count)
            self.assertEqual(len({n['port'] for n in nets}), 1)
            for i, n in enumerate(nets):
                g = {**cases()[4 if count == 3 else 0]['guests'][i], '_lan': n}
                args = acceptance.qemu_args(g)
                self.assertIn('i82559er,netdev=ciuknet0,mac=' + n['mac'] + ',romfile=', args)
                dev = args[args.index('-netdev') + 1]
                if backend == 'stream': self.assertIn(('listen' if i == 0 else 'connect') + '=127.0.0.1:', dev)
                else: self.assertRegex(dev, r'^socket,id=ciuknet0,mcast=239\.255\.\d+\.\d+:\d+,localaddr=127\.0\.0\.1$')
                self.assertNotIn('user,', dev)
        for count in (0, 1, 4, True):
            with self.assertRaises(res.Refusal): lan_runner.pair_networks(count)
        with self.assertRaises(res.Refusal): lan_runner.pair_networks(3, 'stream')

    def test_manifest_refuses_absence_mutation_symlinks_and_size(self):
        with self.assertRaises(live.EvidenceNotRun): live.precheck(self.root)
        expected = self.fixtures()
        payload, hashes = live.precheck(self.root)
        self.assertEqual(set(payload), {'/TESTS/LAN/' + n for n in live.PE_NAMES})
        self.assertEqual(hashes[live.PE_NAMES[0]]['sha256'], expected[live.PE_NAMES[0]]['sha256'])
        p = self.root / live.PE_DIR / live.PE_NAMES[0]; original = p.read_bytes()
        p.write_bytes(original[:-1] + b'x')
        with self.assertRaisesRegex(res.Refusal, 'hash'): live.precheck(self.root)
        p.unlink(); p.symlink_to(self.root / live.PE_DIR / live.PE_NAMES[1])
        with self.assertRaisesRegex(res.Refusal, 'path'): live.precheck(self.root)

    def test_stale_build_cannot_qualify_the_three_peer_fixture(self):
        self.fixtures()
        source = self.root / 'tests/win32/lan/directplay.c'
        source.write_bytes(source.read_bytes() + b'\n')
        with self.assertRaisesRegex(live.EvidenceNotRun, 'stale'): live.precheck(self.root)

    def test_slot_reservations_enforce_capacity_and_hold_each_guest(self):
        base = self.root / '.qemu.lock'
        with patch.dict(os.environ, {'CIUKI_RUNNER_SLOTS': '2'}), res.SlotLock(base):
            with self.assertRaisesRegex(res.Refusal, '3 slots'): live.reserve_slots(3)
        with patch.dict(os.environ, {'CIUKI_RUNNER_SLOTS': '4'}), res.SlotLock(base):
            stack, slots = live.reserve_slots(3)
            with stack:
                self.assertEqual(len(set(slots)), 3)
                for p in res.slot_paths(base):
                    with self.assertRaises(res.Refusal), res.ExclusiveLock(p): pass
            with res.ExclusiveLock(res.slot_paths(base)[1]): pass
        with self.assertRaisesRegex(res.Refusal, 'parent'): live.reserve_slots(2)
        with patch.dict(os.environ, {'CIUKI_RUNNER_SLOTS': '2'}), res.SlotLock(base):
            with res.ExclusiveLock(res.slot_paths(base, 4)[3]):
                with self.assertRaises(res.Refusal): live.reserve_slots(2)

    def test_host_readiness_and_all_desktops_gate_launch(self):
        sync = live.Sync(2)
        sessions = [live.Session(g, '12345678', self.root, sync=sync, guest_index=i) for i, g in enumerate(cases()[0]['guests'])]
        for s in sessions:
            s.index = next(i for i, step in enumerate(s.steps) if step['name'] == 'lan-launch')
            s.launched = 0
        qmp = FakeQMP()
        self.assertFalse(sessions[0]._advance(0, qmp))
        self.assertFalse(sessions[1]._advance(0, qmp))
        self.assertTrue(sessions[0]._advance(0, qmp))
        sessions[0].parser.feed(b'[user 12] LAN2 IPX host ready\n')
        sessions[0].tick(0, qmp)
        self.assertTrue(sync.host_ready.is_set())
        self.assertTrue(sessions[1]._advance(0, qmp))

    def test_standalone_guest_refuses_before_creating_artifacts(self):
        with self.assertRaisesRegex(res.Refusal, 'group runner'):
            runner.run_case(self.root, 'fixture', cases()[0]['guests'][0], {}, self.root / 'absent',
                            'unused', self.root / 'absent')
        self.assertFalse((self.root / 'build/test-runs').exists())

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
    def test_first_boot_is_accepted_by_production_codec(self):
        # Exercise the real C parser, including checksum and PCI/MAC binding.
        lib = self.root / 'codec.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Isrc/kernel/include', 'src/kernel/net/config.c', '-o', str(lib)], cwd=ROOT, check=True)
        codec = ctypes.CDLL(str(lib)); codec.net_config_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
        net = lan_runner.pair_networks()[0]
        for g in (cases()[0]['guests'][0], cases()[5]['guests'][1]):
            data = live.first_boot(g['lan_fixture'], net)
            text = data['/SYSTEM/NETWORK.INI']; out = ctypes.create_string_buffer(2048); ids = ctypes.create_string_buffer(32)
            self.assertEqual(codec.net_config_parse(text, len(text), out, ids), 0)
            self.assertEqual(struct.unpack_from('<II', ids.raw), (0x8086, 0x1209))
            self.assertEqual(ids.raw[8:14], bytes.fromhex(net['mac'].replace(':', '')))
            self.assertEqual(data['/SYSTEM/SETTINGS/ipx.cfg'], b'frame_type=4\n' if g['lan_fixture']['frame'] == 'auto' else b'frame_type=0\n')
            self.assertEqual(codec.net_config_parse(text[:-4] + b'999\n', len(text), out, ids), -22)


class ThreadHost:
    """Separate FakeHost processes/transports per worker, as real scopes are."""
    def __init__(self, root):
        self.root = root; self.local = threading.local(); self.hosts = []

    def preflight(self, root):
        res.check_budget(root)
        return 8 * res.GIB

    @contextmanager
    def export_writable(self, overlay, directory, offset, size):
        volume = directory / 'synthetic-partition.fat'
        with volume.open('wb') as f: f.truncate(4 * 1024**2)
        subprocess.run(['mkfs.fat', '--invariant', str(volume)], check=True, stdout=subprocess.DEVNULL)
        try: yield volume
        finally: volume.unlink(missing_ok=True)

    def launch(self, args, cwd):
        group = cwd / 'cgroup'; group.mkdir(); (group / 'memory.events').write_text('oom 0\noom_kill 0\n')
        host = FakeHost(group); self.local.host = host; self.hosts.append(host)
        return host.launch(args, cwd)

    def __getattr__(self, name):
        return getattr(self.local.host, name)


@unittest.skipUnless(TOOLS, 'mtools/dosfstools required')
class GroupTests(Base):
    def setUp(self):
        super().setUp(); self.fixtures()
        self.image = self.root / 'canonical.raw'
        mbr = bytearray(512); mbr[450] = 12; mbr[510:] = b'\x55\xaa'; struct.pack_into('<II', mbr, 454, 1, 8191)
        with self.image.open('wb') as f: f.write(mbr); f.truncate(4 * 1024**2)
        self.firmware = self.root / 'bios.bin'; self.firmware.write_bytes(b'fixture firmware')
        self.img = self.root / 'fake-img'
        self.img.write_text('#!/usr/bin/env python3\nimport pathlib,sys\nassert sys.argv[1:4]==["create","-f","qcow2"]\npathlib.Path(sys.argv[-1]).write_bytes(b"fixture overlay")\n'); self.img.chmod(0o755)
        self.scenario = self.root / 'scenario.json'
        self.host = ThreadHost(self.root)

    def tearDown(self):
        for host in self.host.hosts:
            self.assertEqual(host.scope_pids(host.cgroup), [])
            if getattr(host, 'tx', None) is not None:
                os.close(host.tx); os.close(host.rx)
        super().tearDown()

    def run_group(self, case_index=0, failure=None):
        c = copy.deepcopy(cases()[case_index]); nets = lan_runner.pair_networks(len(c['guests']), c['lan_live'])
        scenarios = {}
        for i, (g, n) in enumerate(zip(c['guests'], nets)):
            f = g['lan_fixture']; name = live.PE_NAMES[0] if f['api'] == 'ipx' else live.PE_NAMES[1]
            pid = 12 + i
            ready = 'LAN2 IPX host ready' if f['api'] == 'ipx' else 'LAN2 DirectPlay host ready'
            marker = live.IPX_PASS if f['api'] == 'ipx' else live.DP_PASS
            records = ['[win32] start pid=%d exe=C:\\TESTS\\LAN\\%s pe=00000000:00001000 shims=' % (pid, name)]
            if i == 0: records += ['[user %d] %s' % (pid, ready)]
            if failure == 'missing-pass' and i == 1: pass
            else: records += ['[user %d] %s' % (pid, marker)]
            records += ['[win32] exit pid=%d code=%s reason=normal peak_commit=4096' % (pid, '00000001' if failure == 'exit' and i == 1 else '00000000')]
            scenarios[n['mac']] = {'normal_boot': True,
                'records': ['SELECT_READY'],
                'on_key': {'n': ['[net] config boot=saved fallback=none', *boot_lines()], 'ret': records}}
            if failure == 'startup' and i == 0:
                scenarios[n['mac']].update(exit_on='startup', exit_code=7)
        self.scenario.write_text(json.dumps({'lan_guests': scenarios}))
        # Real input timing is covered by the existing acceptance driver tests.
        # Keep generated recipe validation intact while reducing wall time here.
        original_expand = acceptance.Session._expand
        def quick(session, actions):
            queue = original_expand(session, actions)
            for item in queue: item['pause'] = 0; item['hold'] = 0
            return queue
        original_init = live.Session.__init__
        def quick_init(session, *args, **kwargs):
            original_init(session, *args, **kwargs)
            for step in session.steps:
                step['settle'] = 0
                if step.get('name') == 'lan-pass': step['timeout'] = 1
        with patch.dict(os.environ, {'CIUKI_FAKE_SCRIPT': str(self.scenario), 'CIUKI_RUNNER_SLOTS': '3'}), \
                patch.object(runner, 'git_identity', return_value={'revision': 'fixture', 'dirty': False}), \
                patch.object(lan_runner, 'pair_networks', return_value=nets), \
                patch.object(acceptance.Session, '_expand', quick), patch.object(live.Session, '__init__', quick_init), \
                res.SlotLock(self.root / '.qemu.lock'):
            result, directory = runner.run_case(self.root, 'lan-fixture', c,
                runner.load(ROOT / 'tests/profiles/qemu-t23.json'), self.image,
                str(ROOT / 'tests/host/fake_qemu.py'), self.firmware, host=self.host, qemu_img=str(self.img))
        self.assertEqual(json.loads((directory / 'result.json').read_text())['outcome'], result['outcome'])
        return result, directory

    def test_pair_and_triple_pass_stage_hashes_and_remove_overlays(self):
        for index in (0, 4):
            with self.subTest(case=index):
                result, directory = self.run_group(index)
                self.assertEqual(result['outcome'], 'pass', result['reason'])
                self.assertEqual(len(result['guests']), 3 if index == 4 else 2)
                self.assertTrue(result['cleanup']['clean'])
                self.assertEqual(result['image']['sha256'], result['image']['sha256_after'])
                serials = set(); qmps = set()
                for child in result['guests']:
                    path = Path(child['directory']); e = child['evidence']
                    self.assertFalse((path / 'run.qcow2').exists())
                    serials.add(str(path / 'serial.log')); qmps.add(str(path / 'q'))
                    self.assertEqual(e['lan_fixture']['pes'], result['pes'])
                    self.assertEqual(len(e['lan_fixture']['files']), 5)
                    self.assertEqual(e['cleanup']['wrapper_returncode'], 0)
                self.assertEqual(len(serials), len(result['guests'])); self.assertEqual(len(qmps), len(serials))

    def test_nonzero_guest_exit_fails_whole_case(self):
        result, _ = self.run_group(failure='exit')
        self.assertEqual(result['outcome'], 'fail'); self.assertTrue(result['cleanup']['clean'])
        self.assertTrue(any(c['evidence']['outcome'] == 'fail' for c in result['guests']))

    def test_missing_pass_is_failure_even_with_zero_exit(self):
        result, _ = self.run_group(failure='missing-pass')
        self.assertEqual(result['outcome'], 'fail'); self.assertTrue(result['cleanup']['clean'])

    def test_listener_startup_failure_cancels_join_without_launching_it(self):
        result, _ = self.run_group(failure='startup')
        self.assertEqual(result['outcome'], 'fail'); self.assertTrue(result['cleanup']['clean'])
        self.assertEqual(len(self.host.hosts), 1)
        self.assertIn('listener did not initialize', result['reason'])

    def test_insufficient_memory_refuses_before_any_guest(self):
        with patch.object(self.host, 'preflight', return_value=5 * res.GIB):
            result, _ = self.run_group(4)
        self.assertEqual(result['outcome'], 'fail'); self.assertFalse(self.host.hosts)
        self.assertTrue(result['cleanup']['clean']); self.assertIn('6 GiB', result['reason'])

    def test_absent_fixtures_are_not_run_without_launch(self):
        (self.root / live.PE_DIR / 'build.json').unlink()
        result, _ = self.run_group()
        self.assertEqual(result['outcome'], 'not_run'); self.assertFalse(self.host.hosts)


if __name__ == '__main__': unittest.main()
