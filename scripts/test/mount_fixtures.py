"""Mount/crash fixture support, without a writable host mount or image copy."""
import hashlib
import re
import struct

from evidence import EvidenceError
from resources import Refusal
from fat_fixtures import geometry


def classify_crash_checker(checked, files, patterns):
    """Declare only empty files or their nonowning orphan LFN slots.

    Microsoft fatgen103 pp. 26-28: LFN slots precede their short owner and
    have no first cluster. dosfstools 4.2 lfn_check_orphaned() proposes slot
    deletion; fsck.fat -n records that proposal without changing the volume.
    See the f1-24 amendment in docs/design/vfs-storage-contract.md.
    """
    if not isinstance(files, list) or not files or any(
            not isinstance(path, str) or not re.fullmatch(r'::/[A-Za-z0-9._-]+', path) for path in files):
        raise Refusal('interrupted files must be declared root file paths')
    if len({path.casefold() for path in files}) != len(files):
        raise Refusal('duplicate interrupted file declaration')
    lines = [line for line in checked['output'].splitlines() if line.strip()]
    dirty = 'Dirty bit is set. Fs was not properly unmounted and some data may be corrupt.'
    if checked['returncode'] != 1 or lines.count(dirty) != 1 or \
            lines.count(' Automatically removing dirty bit.') != 1 or \
            lines.count('Leaving filesystem unchanged.') != 1:
        raise EvidenceError('undeclared crash dirty-bit outcome')
    names = {path[3:]: path for path in files}; orphans = set(); remaining = []
    i = 0
    while i < len(lines):
        match = re.fullmatch(r'Orphaned long file name part "([^"]+)"', lines[i])
        if match:
            name = match[1]
            if name not in names or name in orphans or i+1 == len(lines) or lines[i+1] != '  Auto-deleting.':
                raise EvidenceError('undeclared orphaned long-name outcome')
            orphans.add(name); i += 2
        else:
            remaining.append(lines[i]); i += 1
    if not patterns or any(not any(re.fullmatch(p, line) for p in patterns) for line in remaining):
        raise EvidenceError('unclassified interrupted filesystem discrepancy')
    return {'dirty': True, 'files': [{'path': path, 'state': 'orphan-lfn' if path[3:] in orphans else 'empty-file'}
                                    for path in files]}


def check_crash_files(host, volume, directory, classification, reports):
    """Confirm absence with a successful directory listing, never a tool error."""
    listing = host.checker(['mdir', '-b', '-i', str(volume), '::/'], directory)
    reports.append({'kind': 'mtools-interrupted-listing', **listing})
    names = [line.strip() for line in listing['output'].splitlines() if line.strip()]
    if listing['returncode'] or any(not name.startswith('::/') for name in names):
        raise EvidenceError('interrupted file listing failed')
    empty = {'size': 0, 'sha256': hashlib.sha256(b'').hexdigest()}
    for item in classification['files']:
        path = item['path']; count = sum(name.rstrip('/').casefold() == path.casefold() for name in names)
        if count != (0 if item['state'] == 'orphan-lfn' else 1):
            raise EvidenceError('undeclared interrupted file outcome: '+path)
        if item['state'] == 'orphan-lfn':
            reports.append({'kind': 'mtools-interrupted-file', **item, 'absent': True})
        else:
            measured = host.file_digest(volume, directory, path.removeprefix('::'))
            reports.append({'kind': 'mtools-interrupted-file', **item, **measured})
            if measured != empty:raise EvidenceError('undeclared interrupted file outcome: '+path)


def place_marker(host, overlay, directory, image_size, name):
    """Create a zero-byte short entry through qemu-io in the overlay only.

    No allocation is needed. Refuse a full root instead of growing it, so the
    only write is one directory sector; FATs/FSInfo/boot fingerprints survive.
    The Microsoft FAT specification's pp. 22-23 define this directory format.
    """
    if name != 'F109CUT.ARM':raise Refusal('unknown mount/crash marker')
    mbr = host.overlay_read(overlay, 0, 512)
    start, count = struct.unpack_from('<II', mbr, 454)
    if mbr[510:512] != b'\x55\xaa' or not start or not count or (start+count)*512 > image_size:
        raise Refusal('invalid marker boot partition')
    boot = host.overlay_read(overlay, start*512, 512); g = geometry(boot)
    if g['kind'] != 32 or struct.unpack_from('<I', boot, 32)[0] > count:
        raise Refusal('marker requires the canonical FAT32 boot partition')
    cluster = g['root_cluster']; seen = set(); free = None; alias = b'F109CUT ARM'
    ended = False
    while not ended:
        if cluster in seen or len(seen) >= 128 or not 2 <= cluster <= g['clusters']+1:
            raise Refusal('invalid/budget-exceeding marker root chain')
        seen.add(cluster)
        for sector in range(g['data']+(cluster-2)*g['spc'], g['data']+(cluster-1)*g['spc']):
            offset = (start+sector)*512
            data = host.overlay_read(overlay, offset, 512)
            for slot in range(0, 512, 32):
                if data[slot] == 0:
                    if free is None:free = offset, slot, data
                    ended = True; break
                if data[slot] == 0xe5:
                    if free is None:free = offset, slot, data
                elif data[slot:slot+11] == alias:
                    raise Refusal('crash marker already exists')
            if ended:break
        if not ended:
            entry = host.overlay_read(overlay, (start+g['reserved'])*512+cluster*4, 4)
            cluster = int.from_bytes(entry, 'little')&0x0fffffff
            if cluster >= 0x0ffffff8:break
    if free is None:raise Refusal('marker root has no free slot')
    offset, slot, before = free
    after = bytearray(before); after[slot:slot+32] = bytes(32)
    after[slot:slot+11] = alias; after[slot+11] = 32
    for field in (16, 18, 24):struct.pack_into('<H', after, slot+field, 0x2821)
    payload = directory/'crash-marker-sector.bin'; payload.write_bytes(after)
    host.overlay_write(overlay, offset, payload, 512)
    if host.overlay_read(overlay, offset, 512) != after:raise Refusal('crash marker readback mismatch')
    return dict(file=name, sector=offset//512, offset=slot, bytes=32, size=0,
                before_hex=before[slot:slot+32].hex(), after_hex=after[slot:slot+32].hex(),
                sector_sha256_before=hashlib.sha256(before).hexdigest(),
                sector_sha256_after=hashlib.sha256(after).hexdigest())


class WriteGate:
    """After crash ARM, suspend guest writes and cut a completed trace prefix.

    QEMU v11 block/io.c emits pwritev on the addressed blkdebug node; its
    one-shot breakpoint yields BEFORE forwarding the write. Re-arm it before
    resuming the previous request. Writethrough bypasses volatile device cache
    and makes each returned write durable. Stdout must be line-buffered so
    blkdebug's suspension notice is observable. No qcow2 metadata I/O is gated.
    https://www.qemu.org/docs/master/devel/testing/blkdebug.html
    https://github.com/qemu/qemu/blob/v11.0.0/block/io.c
    https://github.com/qemu/qemu/blob/v11.0.0/block/blkdebug.c
    Do not issue QMP stop: do_vm_stop() drains/flushes block requests and
    cannot finish with a suspended breakpoint (system/cpus.c in that tag).
    HMP must address the persistent -drive backend, not the blkdebug node:
    hmp_qemu_io() creates/drains a temporary backend for node names, which
    deadlocks when re-arming with a request suspended on that node.
    """
    def __init__(self, index):
        if type(index) is not int or not 1 <= index <= 100000:raise Refusal('invalid declared cut index')
        self.index = index; self.pending = b''; self.suspended = False
        self.observations = []; self.arm_record = None; self.started = False

    def command(self, qmp, text):
        reply = qmp.command('human-monitor-command', {'command-line':f'qemu-io ciuki-cut-drive "{text}"'})
        if not isinstance(reply,str) or reply.strip():raise EvidenceError('blkdebug gate command failed: '+str(reply))

    def arm(self, qmp):self.command(qmp, 'break pwritev ciuki-write')

    def synchronize(self, record, qmp):
        if not record or record.get('event') != 'ARM' or record.get('action') != 'crash_cut':return
        if self.arm_record is not None:
            if record != self.arm_record:raise EvidenceError('duplicate crash ARM')
            return
        # storage_enable_write() performs untraced setup writes before ARM.
        # Install immediately on receipt, before blockstats/status round trips.
        self.arm_record = record; self.arm(qmp); self.started = True

    def feed(self, chunk, now):
        self.pending += chunk
        while b'\n' in self.pending:
            line, self.pending = self.pending.split(b'\n', 1)
            if line.strip() == b"blkdebug: Suspended request 'ciuki-write'":
                if self.suspended:raise EvidenceError('multiple suspended cut requests')
                self.suspended = True
        if len(self.pending) > 4096:raise EvidenceError('unterminated blkdebug notice')

    def step(self, parser, qmp, now):
        arms = [r for r in parser.records if r.get('event') == 'ARM' and r.get('action') == 'crash_cut']
        cuts = [r for r in parser.records if r.get('case') == 'cut']
        if cuts and (not arms or parser.records.index(cuts[0]) < parser.records.index(arms[0])):
            raise EvidenceError('cut trace before crash ARM')
        if len(arms) > 1:raise EvidenceError('duplicate crash ARM')
        if arms:self.synchronize(arms[0], qmp)
        if [r.get('index') for r in cuts] != [str(i) for i in range(1, len(cuts)+1)]:
            raise EvidenceError('noncontiguous cut trace')
        if any(r.get('action') != 'write' or r.get('result') != '0' or r.get('durable') != '1' for r in cuts):
            raise EvidenceError('writethrough cut lacks durable successful sector writes')
        if len(cuts) > self.index:raise EvidenceError('declared cut index already passed')
        if not self.suspended:return False
        if not self.started:raise EvidenceError('write suspended before crash ARM')
        self.observations.append(dict(completed_index=len(cuts), armed=bool(arms), suspended=True))
        if len(cuts) == self.index:return True
        self.arm(qmp); self.command(qmp, 'resume ciuki-write'); self.suspended = False
        return False

    def pending_stimulus(self, records):
        completed = len([r for r in records if r.get('case') == 'cut'])
        phase = ('waiting_for_arm' if self.arm_record is None else
                 'arming_gate' if not self.started else
                 'waiting_for_cut_index' if completed < self.index else 'waiting_for_write_suspension')
        return dict(declared_cut_index=self.index, completed_index=completed,
                    gate_armed=self.started, next_write_suspended=self.suspended, phase=phase)

    def evidence(self, records, partition_start):
        cuts = [r for r in records if r.get('case') == 'cut']
        if len(cuts) != self.index:raise EvidenceError('cut does not match declared trace prefix')
        return dict(index=self.index, record=cuts[-1], mode='guest-termination', gate='blkdebug-pwritev',
                    next_write_suspended=True, durable_sectors=sorted({partition_start+int(r['lba']) for r in cuts}),
                    sector_domain='disk-lba', trace=cuts, gate_observations=self.observations)
