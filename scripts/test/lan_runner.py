"""Two-guest DOS IPX acceptance: isolated Ethernet, cancellation and evidence.

Caller holds the canonical runner slot. All guests retain the ordinary capped
boot runner and teardown. Only the explicitly declared pair runs concurrently.
"""
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack
import copy
import json
from pathlib import Path
import re
import secrets
import threading

import resources as res
from evidence import EvidenceError, EvidenceNotRun


def validate_network(spec):
    if (not isinstance(spec, dict) or set(spec) - {'group', 'port', 'mac', 'identity', 'mode'} or
            not {'group', 'port', 'mac', 'identity'} <= set(spec) or
            spec.get('mode', 'mcast') not in ('mcast', 'listen', 'connect') or
            not isinstance(spec['group'], str) or not re.fullmatch(r'239\.255\.(?:[0-9]{1,3})\.(?:[0-9]{1,3})', spec['group']) or
            any(int(p) > 255 for p in spec['group'].split('.')) or
            type(spec['port']) is not int or not 1024 <= spec['port'] <= 65535 or
            not isinstance(spec['mac'], str) or not re.fullmatch(r'52:54:00:[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}', spec['mac']) or
            not isinstance(spec['identity'], str) or not re.fullmatch('[0-9a-f]{32}', spec['identity']) or not int(spec['identity'], 16)):
        raise res.Refusal('invalid isolated LAN network')


def validate(case, root):
    if 'lan_live' in case:
        import lan_live
        return lan_live.validate(case, root)
    import acceptance
    allowed = {'id', 'kind', 'probe', 'profile', 'timeout', 'expected', 'item', 'title', 'guests', 'blocking', '_prune_keep'}
    if set(case) - allowed or not isinstance(case.get('guests'), list) or len(case['guests']) != 2:
        raise res.Refusal('LAN acceptance requires exactly two guests')
    # Reuse parent identity, time and item validation, without recursing.
    parent = {k: v for k, v in case.items() if k != 'guests'}
    parent['steps'] = [{'name': 'pair', 'check': 'lan'}]
    acceptance.validate_case(parent, root)
    roles, ids = set(), set()
    for guest in case['guests']:
        if 'guests' in guest or '_lan' in guest:
            raise res.Refusal('nested or externally assigned LAN network')
        acceptance.validate_case(guest, root)
        spec = guest.get('dos_game', {})
        roles.add(spec.get('role')); ids.add(spec.get('title'))
        if guest.get('profile') != case['profile'] or guest['timeout'] > case['timeout'] or guest.get('devices', {}).get('network') != 'i82559er':
            raise res.Refusal('LAN guests share profile/timeout and the T23 Ethernet NIC')
    if roles != {'host', 'join'} or len(ids) != 1 or None in ids or len({g['id'] for g in case['guests']}) != 2:
        raise res.Refusal('LAN pair is host and join of one title, with unique case IDs')
    return acceptance.KIND


def pair_networks(count=2, backend='mcast'):
    if type(count) is not int or count not in (2, 3) or backend not in ('mcast', 'stream') or count == 3 and backend != 'mcast':
        raise res.Refusal('isolated LAN requires two or three guests; three use multicast')
    entropy = secrets.token_bytes(4)
    group = '239.255.%d.%d' % (entropy[0], entropy[1])
    port = 10000 + int.from_bytes(entropy[2:], 'big') % 50000
    return [{'group': group, 'port': port, 'mac': '52:54:00:%02x:%02x:%02x' % (entropy[0], entropy[1], i + 1),
             'identity': secrets.token_hex(16),
             **({'mode': 'listen' if i == 0 else 'connect'} if backend == 'stream' else {})} for i in range(count)]


def netdev(spec):
    validate_network(spec)
    mode = spec.get('mode', 'mcast')
    if mode == 'mcast':
        return 'socket,id=ciuknet0,mcast=%s:%d,localaddr=127.0.0.1' % (spec['group'], spec['port'])
    return 'socket,id=ciuknet0,%s=127.0.0.1:%d' % (mode, spec['port'])


def reserve_other_slots():
    """Keep another runner from starting guests beside this authorized pair."""
    stack = ExitStack()
    slot = res.SlotLock.current
    try:
        if slot is not None:
            for path in res.slot_paths(slot.base):
                if path != slot.path:
                    stack.enter_context(res.ExclusiveLock(path))
        return stack
    except BaseException:
        stack.close(); raise


def cross_peer(children):
    """Both serials must contain the other VM's source, never self-reflection."""
    seen = {}
    for child in children:
        evidence = child['evidence']
        variables = evidence['acceptance']['variables']
        vm, generation = variables['game_vm'], variables['game_gen']
        pattern = re.compile(r'^\[dos-ipx\] vm=' + re.escape(vm) + r' gen=' + re.escape(generation) +
                             r' event=(tx|rx) count=[1-4] (?:bytes=\d+ )?src=([0-9a-f]{12}) dst=([0-9a-f]{12})$')
        path = Path(child['directory']) / 'serial.log'
        if path.stat().st_size > res.LOG_CAP:
            raise EvidenceError('LAN serial exceeds cap')
        rows = [pattern.fullmatch(line) for line in path.read_text().splitlines()]
        seen[child['role']] = {kind: {m[2] for m in rows if m and m[1] == kind} for kind in ('tx', 'rx')}
    host, join = seen['host'], seen['join']
    if (len(host['tx']) != 1 or len(join['tx']) != 1 or host['tx'] == join['tx'] or
            not host['tx'] <= join['rx'] or not join['tx'] <= host['rx']):
        raise EvidenceError('LAN serials do not prove traffic from distinct peers in both directions')
    return {role: {kind: sorted(values) for kind, values in kinds.items()} for role, kinds in seen.items()}


def run_pair(root, suite, case, profile, image, executable, firmware, host, keep, qemu_img, run_boot):
    validate(case, root)
    root = Path(root)
    import dos_games
    from run import sha, utc, atomic_write
    baseline = sha(image)
    run_id = secrets.token_hex(4)
    directory = root / 'build/test-runs' / suite / run_id
    directory.mkdir(parents=True, exist_ok=False)
    cancelled = threading.Event(); children = []; errors = []
    networks = pair_networks()
    result = {'schema_version': 1, 'run_id': run_id, 'suite': suite, 'case': case['id'], 'probe': case['probe'],
              'evidence_kind': 'acceptance', 'utc_start': utc(), 'image': {'path': str(image), 'sha256': baseline},
              'outcome': 'fail', 'reason': 'incomplete LAN pair', 'network': networks, 'guests': children}
    try:
        # All package, tree and checkpoint checks precede every guest launch.
        for guest in case['guests']:
            dos_games.precheck(guest['dos_game'], root, baseline)
        with reserve_other_slots():
            available = host.preflight(root / 'build/test-runs')
            if available < 4 * res.GIB:
                raise res.Refusal('LAN pair needs MemAvailable >= 4 GiB (two 1500M scopes)')
            with ThreadPoolExecutor(max_workers=2, thread_name_prefix='ciuki-ipx') as pool:
                futures = {}
                try:
                    for guest, network in zip(case['guests'], networks):
                        selected = copy.deepcopy(guest); selected['_lan'] = network
                        child_suite = suite + '-lan-' + run_id + '-' + guest['dos_game']['role']
                        futures[pool.submit(run_boot, root, child_suite, selected, profile, image, executable, firmware,
                                            host, keep, qemu_img, cancel_event=cancelled)] = guest
                    for future in as_completed(futures):
                        guest = futures[future]
                        try:
                            evidence, path = future.result()
                            children.append({'role': guest['dos_game']['role'], 'case': guest['id'], 'directory': str(path), 'evidence': evidence})
                            if evidence['outcome'] != 'pass' or not evidence.get('cleanup', {}).get('clean'):
                                cancelled.set()
                        except BaseException as error:
                            cancelled.set(); errors.append(guest['id'] + ': ' + str(error))
                except BaseException:
                    cancelled.set(); raise
            if sha(image) != baseline:
                errors.append('canonical image changed during LAN pair')
            if len(children) == 2 and not errors and all(c['evidence']['outcome'] == 'pass' and c['evidence'].get('cleanup', {}).get('clean') for c in children):
                result['cross_peer'] = cross_peer(children)
                result.update(outcome='pass', reason='both guests completed IPX gameplay and cleanup')
            else:
                result['reason'] = '; '.join(errors) or 'LAN guest failed or did not run; both guests stopped'
    except EvidenceNotRun as error:
        result.update(outcome='not_run', reason=str(error))
    except (Exception, KeyboardInterrupt) as error:
        cancelled.set(); result['reason'] = str(error) or type(error).__name__
    finally:
        result['utc_end'] = utc()
        result['cleanup'] = {'clean': not children and result['outcome'] == 'not_run' or
                             len(children) == 2 and all(c['evidence'].get('cleanup', {}).get('clean') for c in children)}
        status = 'pass' if result['outcome'] == 'pass' else 'not_run' if result['outcome'] == 'not_run' else 'fail'
        result['acceptance'] = {'item': case['item'], 'title': case['title'], 'blocking': True,
                                'checks': {'lan': {'status': status, 'items': 2}}, 'steps': [], 'figures': {}, 'assertions': []}
        atomic_write(directory / 'result.json', json.dumps(result, indent=2) + '\n')
    return result, directory
