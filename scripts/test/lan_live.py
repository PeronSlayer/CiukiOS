"""Authored Win32 LAN acceptance; runtime overlay fixtures and 2--3 guests."""
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack
import copy
import hashlib
import json
from pathlib import Path
import re
import secrets
import threading
import time

import acceptance
import dos_fixtures
import dos_games
import lan_runner
import resources as res
from evidence import EvidenceError, EvidenceNotRun

PE_NAMES = ('lan2-ipx.exe', 'lan2-directplay.exe')
PE_DIR = 'build/host/lan2-pe'
IPX_PASS = 'PASS LAN2 Win32 AF_IPX broadcast/directed echo'
DP_PASS = 'PASS LAN2 DirectPlay COM enumerate/open/player/guaranteed chat/close'
CONSOLE = r'^\[user \d+\] '


def guest_case(identity, fixture, timeout=1200):
    """One fixed recipe; callers cannot weaken PASS, exit or failure checks."""
    name = 'lan2-ipx.exe' if fixture['api'] == 'ipx' else 'lan2-directplay.exe'
    command = 'C:\\TESTS\\LAN\\' + name + ' ' + fixture['role']
    if fixture['api'] == 'directplay':
        command += ' ' + fixture['transport']
        if fixture['peers'] == 3:
            command += ' --peers=3'
    command += ' --ciuki-serial'
    marker = IPX_PASS if fixture['api'] == 'ipx' else DP_PASS
    return {'id': identity, 'kind': 'acceptance', 'probe': 'acceptance', 'profile': 'qemu-t23',
            'timeout': timeout, 'item': 18, 'title': 'Authored LAN ' + identity,
            'expected': {'evidence_kind': 'acceptance'}, 'devices': {'network': 'i82559er'},
            'lan_fixture': fixture,
            'forbid': [CONSOLE + 'FAIL ', r'^\[win32\] (?:crash|refused) ',
                       r'^\[win32\] exit pid=\d+ code=(?!00000000 )',
                       r'^\[win32\] exit .* reason=(?!normal )'],
            'steps': [
                {'template': 'boot', 'prefix': 'b1', 'check': 'boot', 'settle': 2},
                {'name': 'settings', 'check': 'settings', 'since': 'boot',
                 'wait': r'^\[net\] config boot=saved fallback=none$'},
                {'name': 'lan-launch', 'check': 'lan', 'actions': [{'keys': 'meta_l+r', 'pause': 1}]},
                {'name': 'lan-command', 'check': 'lan',
                 'actions': [{'text': command}, {'keys': 'ret'}]},
                {'name': 'lan-start', 'check': 'lan', 'timeout': 120,
                 'wait': r'^\[win32\] start pid=(?P<lan_pid>[1-9]\d*) exe=C:\\TESTS\\LAN\\' + re.escape(name) + r' pe='},
                {'name': 'lan-pass', 'check': 'lan', 'timeout': 90,
                 'wait': CONSOLE + re.escape(marker) + '$'},
                {'name': 'lan-exit', 'check': 'lan', 'timeout': 30,
                 'wait': r'^\[win32\] exit pid=<<lan_pid>> code=00000000 reason=normal '},
            ]}


def validate_guest(case):
    f = case.get('lan_fixture')
    acceptance.refuse(isinstance(f, dict) and set(f) == {'api', 'transport', 'role', 'address', 'frame', 'peers'}, 'LAN fixture fields')
    acceptance.refuse(f['api'] in ('ipx', 'directplay') and f['transport'] in ('ipx', 'tcp') and
                      (f['api'] != 'ipx' or f['transport'] == 'ipx'), 'LAN fixture API/transport')
    acceptance.refuse(f['role'] in ('host', 'join') and type(f['peers']) is int and f['peers'] in (2, 3), 'LAN role/peers')
    acceptance.refuse(f['address'] in ('192.0.2.1', '192.0.2.2', '192.0.2.3') and f['frame'] in ('auto', '802.3'), 'LAN first-boot address/frame')
    expected = guest_case(case['id'], f, case['timeout'])
    acceptance.refuse({k: v for k, v in case.items() if k not in ('_lan', '_run_id', '_prune_keep')} == expected,
                      'authored LAN fixture requires the complete launch/PASS/zero-exit recipe')


def validate(case, root):
    allowed = {'id', 'kind', 'probe', 'profile', 'timeout', 'expected', 'item', 'title', 'guests',
               'blocking', '_prune_keep', 'lan_live'}
    acceptance.refuse(not set(case) - allowed and case.get('lan_live') in ('stream', 'mcast'), 'LAN live parent fields/backend')
    guests = case.get('guests')
    acceptance.refuse(isinstance(guests, list) and len(guests) in (2, 3), 'LAN live requires two or three guests')
    acceptance.refuse(len(guests) == 2 or case['lan_live'] == 'mcast', 'three guests require multicast')
    parent = {k: v for k, v in case.items() if k not in ('guests', 'lan_live')}
    parent['steps'] = [{'name': 'lan', 'check': 'lan'}]
    acceptance.validate_case(parent, root)
    for g in guests:
        acceptance.refuse(isinstance(g, dict) and 'guests' not in g and '_lan' not in g, 'nested/externally networked LAN guest')
        acceptance.refuse('lan_fixture' in g, 'authored LAN guest needs lan_fixture')
        acceptance.validate_case(g, root)
        acceptance.refuse(g['profile'] == case['profile'] == 'qemu-t23' and g['timeout'] <= case['timeout'] and
                          g['lan_fixture']['peers'] == len(guests), 'LAN profile, timeout and peer count')
    fixtures = [g['lan_fixture'] for g in guests]
    acceptance.refuse(fixtures[0]['role'] == 'host' and all(f['role'] == 'join' for f in fixtures[1:]), 'one host followed by joiners')
    acceptance.refuse(len({g['id'] for g in guests}) == len(guests) and
                      len({f['address'] for f in fixtures}) == len(guests), 'unique guest IDs and static addresses')
    acceptance.refuse(len({(f['api'], f['transport']) for f in fixtures}) == 1 and
                      (len(guests) == 2 or fixtures[0]['api'] == 'directplay' and fixtures[0]['transport'] == 'tcp'), 'matching protocol; three peers use DirectPlay TCP/IP')
    return acceptance.KIND


def precheck(root):
    """No implicit build. Pin both authored PEs to the explicit build manifest."""
    root = Path(root).resolve()
    directory = root / PE_DIR
    for p in (directory, *directory.parents):
        if p == root:
            break
        if p.is_symlink():
            raise res.Refusal('LAN fixture directory symlink')
    manifest = directory / 'build.json'
    if not manifest.is_file():
        raise EvidenceNotRun('authored LAN fixtures absent; run tests/win32/lan/build.py explicitly')
    if manifest.is_symlink() or manifest.stat().st_size > 16384:
        raise res.Refusal('invalid LAN build manifest')
    rows = json.loads(manifest.read_text())
    if set(rows) != set(PE_NAMES):
        raise res.Refusal('LAN build manifest PE census')
    payload = {}; hashes = {}
    for name in PE_NAMES:
        path = directory / name
        if not path.is_file():
            raise EvidenceNotRun('authored LAN fixture absent: ' + name)
        row = rows[name]
        if path.is_symlink() or not isinstance(row, dict) or type(row.get('bytes')) is not int or not 64 <= row['bytes'] <= 1024**2 or path.stat().st_size != row['bytes']:
            raise res.Refusal('LAN fixture size/path: ' + name)
        data = path.read_bytes(); digest = hashlib.sha256(data).hexdigest()
        if not data.startswith(b'MZ') or digest != row.get('sha256'):
            raise res.Refusal('LAN fixture PE/hash mismatch: ' + name)
        sources = ['tests/win32/lan/' + p for p in ('build.py', 'common.h', name[5:-4] + '.c')]
        expected = {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources}
        if row.get('sources') != expected:
            raise EvidenceNotRun('authored LAN fixture build is stale; rebuild ' + name)
        payload['/TESTS/LAN/' + name] = data
        hashes[name] = {'sha256': digest, 'bytes': len(data), 'sources': expected}
    return payload, hashes


def first_boot(fixture, network):
    """Existing NETWORK.INI codec; en0 identity is the T23 i82559ER (8086:1209)."""
    lan_runner.validate_network(network)
    text = 'version=1\n'
    for index in range(2):
        text += '[en%d]\nenabled=1\ndhcp=%d\nspeed=0\nduplex=0\n' % (index, index)
        text += 'address=%s\nmask=%s\ngateway=0.0.0.0\n' % (fixture['address'] if index == 0 else '0.0.0.0', '255.255.255.0' if index == 0 else '0.0.0.0')
        text += 'dns1=0.0.0.0\ndns2=0.0.0.0\ndns3=0.0.0.0\nhostname=ciukios\ndomain=\n'
        text += 'vendor=%d\ndevice=%d\nmac=%s\n' % (0x8086 if index == 0 else 0, 0x1209 if index == 0 else 0, network['mac'] if index == 0 else '00:00:00:00:00:00')
    digest = 2166136261
    for ch in text.encode('ascii'):
        digest = ((digest ^ ch) * 16777619) & 0xffffffff
    text += 'checksum=%d\n' % digest
    return {'/SYSTEM/NETWORK.INI': text.encode('ascii'),
            '/SYSTEM/SETTINGS/ipx.cfg': ('frame_type=%d\n' % (4 if fixture['frame'] == 'auto' else 0)).encode('ascii'),
            '/SYSTEM/NETID.BIN': bytes.fromhex(network['identity'])}


def stage(host, case, image, overlay, directory, root, cancel_event=None):
    directory = dos_fixtures.check_output(directory, root)
    if Path(overlay).resolve() != (directory / 'run.qcow2').resolve() or Path(overlay).is_symlink():
        raise res.Refusal('authored LAN staging requires the private run overlay')
    files, hashes = precheck(root)
    settings = first_boot(case['lan_fixture'], case['_lan'])
    offset, size = dos_games.partition(image)
    with host.export_writable(overlay, directory, offset, size) as volume:
        records = dos_games.copy_files(volume, {**files, **settings}, directory, cancel_event)
        dos_fixtures.run(['fsck.fat', '-n', volume])
    return {'pes': hashes, 'files': records, 'command': case['steps'][3]['actions'][0]['text'],
            'first_boot': {**case['lan_fixture'], 'mask': '255.255.255.0',
            'gateway': '0.0.0.0', 'dns': [], 'mac': case['_lan']['mac'], 'mechanism': 'existing-settings-loader'}}


class Sync:
    def __init__(self, count):
        self.desktops = [threading.Event() for _ in range(count)]
        self.host_ready = threading.Event()
        self.listener = threading.Event()


class Session(acceptance.Session):
    def __init__(self, *args, sync, guest_index, **kwargs):
        super().__init__(*args, **kwargs)
        self.sync, self.guest_index, self.scan = sync, guest_index, 0

    def _advance(self, now, qmp):
        if self.steps[self.index]['name'] == 'lan-launch' and self.phase == 'start':
            self.sync.desktops[self.guest_index].set()
            if not all(e.is_set() for e in self.sync.desktops):
                return False
            if self.guest_index and not self.sync.host_ready.is_set():
                return False
        return super()._advance(now, qmp)

    def tick(self, now, qmp):
        if self.guest_index == 0:
            ready = 'LAN2 IPX host ready' if self.case['lan_fixture']['api'] == 'ipx' else 'LAN2 DirectPlay host ready'
            for row in self.parser.lines[self.scan:]:
                if re.fullmatch(CONSOLE + re.escape(ready), row['text']):
                    self.sync.host_ready.set()
            self.scan = len(self.parser.lines)
        super().tick(now, qmp)


def reserve_slots(count):
    """Each guest owns a lock; reserve spare slots too to exclude other runners."""
    slot = res.SlotLock.current
    if slot is None:
        raise res.Refusal('LAN live requires the parent runner slot')
    if len(res.slot_paths(slot.base)) < count:
        raise res.Refusal('LAN needs %d slots; set CIUKI_RUNNER_SLOTS=%d' % (count, count))
    # Another runner may use a larger CIUKI_RUNNER_SLOTS value. Reserve the
    # entire supported namespace, not just this process's configured prefix.
    paths = res.slot_paths(slot.base, res.SLOT_MAX)
    stack = ExitStack()
    try:
        for path in paths:
            if path != slot.path:
                stack.enter_context(res.ExclusiveLock(path))
        return stack, [str(slot.path)] + [str(p) for p in paths if p != slot.path][:count-1]
    except BaseException:
        stack.close()
        raise


def run_group(root, suite, case, profile, image, executable, firmware, host, keep, qemu_img, run_boot):
    from run import sha, utc, atomic_write
    validate(case, root)
    if profile.get('name') != 'qemu-t23':
        raise res.Refusal('authored LAN requires qemu-t23')
    root = Path(root); count = len(case['guests']); baseline = sha(image)
    run_id = secrets.token_hex(4)
    directory = root / 'build/test-runs' / suite / run_id
    directory.mkdir(parents=True, exist_ok=False)
    networks = lan_runner.pair_networks(count, case['lan_live'])
    children, errors = [], []
    cancelled = threading.Event(); sync = Sync(count); attempted = set()
    result = {'schema_version': 1, 'run_id': run_id, 'suite': suite, 'case': case['id'], 'probe': case['probe'],
              'evidence_kind': 'acceptance', 'utc_start': utc(), 'image': {'path': str(image), 'sha256': baseline},
              'outcome': 'fail', 'reason': 'incomplete LAN group', 'network': networks, 'guests': children,
              'commands': {g['id']: g['steps'][3]['actions'][0]['text'] for g in case['guests']}}
    try:
        _, hashes = precheck(root)
        result['pes'] = hashes
        stack, slots = reserve_slots(count)
        with stack:
            result['slots'] = slots
            if host.preflight(root / 'build/test-runs') < 2 * count * res.GIB:
                raise res.Refusal('LAN needs MemAvailable >= %d GiB' % (2 * count))
            def boot(index, guest):
                # Do not probe the TCP listener with a connection: QEMU accepts
                # one Ethernet peer there. QMP readiness proves it initialized.
                if index and case['lan_live'] == 'stream':
                    deadline = time.monotonic() + case['timeout']
                    while not sync.listener.wait(.05):
                        if cancelled.is_set() or time.monotonic() >= deadline:
                            raise EvidenceError('LAN listener did not initialize')
                if cancelled.is_set():
                    raise EvidenceError('LAN peer failed before boot')
                selected = copy.deepcopy(guest); selected['_lan'] = networks[index]
                # Nest the child beneath its parent for ordinary suite pruning.
                child_suite = suite + '/' + run_id + '/guest-' + str(index)
                attempted.add(index)
                return run_boot(root, child_suite, selected, profile, image, executable, firmware, host, keep, qemu_img,
                                cancel_event=cancelled, lan_sync=sync, lan_index=index)
            with ThreadPoolExecutor(max_workers=count, thread_name_prefix='ciuki-lan') as pool:
                futures = {pool.submit(boot, i, g): (i, g) for i, g in enumerate(case['guests'])}
                try:
                    for future in as_completed(futures):
                        index, guest = futures[future]
                        try:
                            evidence, path = future.result()
                            children.append({'index': index, 'role': guest['lan_fixture']['role'], 'case': guest['id'],
                                             'slot': slots[index], 'directory': str(path), 'evidence': evidence})
                            if evidence['outcome'] != 'pass' or not evidence.get('cleanup', {}).get('clean'):
                                cancelled.set()
                        except BaseException as error:
                            cancelled.set(); errors.append(guest['id'] + ': ' + (str(error) or type(error).__name__))
                except BaseException:
                    cancelled.set()
                    raise
            result['image']['sha256_after'] = sha(image)
            if result['image']['sha256_after'] != baseline:
                errors.append('canonical image changed during LAN group')
            children.sort(key=lambda c: c['index'])
            if len(children) == count and not errors:
                for child in children:
                    e = child['evidence']
                    if e['outcome'] != 'pass' or not e.get('cleanup', {}).get('clean') or e.get('lan_fixture', {}).get('pes') != hashes:
                        errors.append(child['case'] + ': failed, unclean or fixture identity mismatch')
            if len(children) == count and not errors:
                result.update(outcome='pass', reason='all LAN fixtures passed, exited zero and stopped cleanly')
            else:
                result['reason'] = '; '.join(errors) or 'LAN guest did not complete'
    except EvidenceNotRun as error:
        result.update(outcome='not_run', reason=str(error))
    except (Exception, KeyboardInterrupt) as error:
        cancelled.set(); result['reason'] = str(error) or type(error).__name__
    finally:
        result['utc_end'] = utc()
        result['cleanup'] = {'clean': len(children) == len(attempted) and
                             all(c['evidence'].get('cleanup', {}).get('clean') for c in children)}
        result['acceptance'] = {'item': case['item'], 'title': case['title'], 'blocking': case.get('blocking', True),
                                'checks': {'lan': {'status': result['outcome'], 'items': count}},
                                'steps': [], 'figures': {}, 'assertions': []}
        atomic_write(directory / 'result.json', json.dumps(result, indent=2) + '\n')
        res.prune(directory.parent, max(5, case.get('_prune_keep', 5)))
    return result, directory
