"""G14 ScanDisk engine on real FAT12/16/32 images (no QEMU).

Volumes come from mkfs.fat and mtools; damage is written with the FAT
layout of Microsoft's fatgen103; the engine (src/kernel/fs/scandisk.c)
runs through tests/host/fs/scandisk_tool.c; every repaired volume must
then pass `fsck.fat -n` (dosfstools, an independent checker), a second
engine run must find nothing, and file contents are read back with mtools.
"""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/scandisk'
TOOL = ROOT / 'build/host/scandisk_tool'
ENV = dict(os.environ, MTOOLS_SKIP_CHECK='1', MTOOLS_NO_VFAT='0', LC_ALL='C.UTF-8')
TOOLS = all(shutil.which(t) for t in ('mkfs.fat', 'fsck.fat', 'mcopy', 'mtype', 'mdir', 'mmd', 'clang'))


def build_tool():
    OUT.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(ROOT / 'build/host'))
    subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-macro-redefined',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-DFS_HOST',
                    '-D_POSIX_C_SOURCE=200809L', '-pthread', '-I', str(ROOT / 'src/kernel/include'),
                    '-I', str(ROOT / 'src/kernel/fs'), str(ROOT / 'tests/host/fs/scandisk_tool.c'),
                    str(ROOT / 'src/kernel/fs/scandisk.c'), str(ROOT / 'src/kernel/fs/fs_port.c'),
                    '-o', str(TOOL)], check=True, env=env)


def pattern(seed, n):
    return bytes((i * 31 + seed * 7 + (i >> 9)) & 255 for i in range(n))


class Fat:
    """Byte-level view of a volume: BPB, both FAT copies, root entries."""

    def __init__(self, path):
        self.path = path
        b = path.read_bytes()[:512]
        u16 = lambda o: struct.unpack_from('<H', b, o)[0]
        u32 = lambda o: struct.unpack_from('<I', b, o)[0]
        self.spc, self.reserved, self.fats = b[13], u16(14), b[16]
        self.root_entries = u16(17)
        total = u16(19) or u32(32)
        self.fat_sectors = u16(22) or u32(36)
        self.root_sectors = (self.root_entries * 32 + 511) // 512
        self.data = self.reserved + self.fats * self.fat_sectors + self.root_sectors
        self.clusters = (total - self.data) // self.spc
        self.type = 12 if self.clusters < 4085 else 16 if self.clusters < 65525 else 32
        self.root_cluster = u32(44) if self.type == 32 else 0
        self.cb = self.spc * 512
        self.mask = {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[self.type]

    def raw(self):
        return bytearray(self.path.read_bytes())

    def entry_offset(self, c):
        return c + c // 2 if self.type == 12 else c * self.type // 8

    def get(self, c, copy=0, image=None):
        image = image or self.raw()
        base = (self.reserved + copy * self.fat_sectors) * 512 + self.entry_offset(c)
        if self.type == 12:
            x = image[base] | image[base + 1] << 8
            return x >> 4 if c & 1 else x & 0xfff
        if self.type == 16:
            return struct.unpack_from('<H', image, base)[0]
        return struct.unpack_from('<I', image, base)[0] & 0x0fffffff

    def set(self, image, c, value, copies=None):
        for copy in (range(self.fats) if copies is None else copies):
            base = (self.reserved + copy * self.fat_sectors) * 512 + self.entry_offset(c)
            if self.type == 12:
                x = image[base] | image[base + 1] << 8
                x = (x & 0x000f) | (value & 0xfff) << 4 if c & 1 else (x & 0xf000) | (value & 0xfff)
                image[base], image[base + 1] = x & 255, x >> 8
            elif self.type == 16:
                struct.pack_into('<H', image, base, value)
            else:
                old = struct.unpack_from('<I', image, base)[0]
                struct.pack_into('<I', image, base, (old & 0xf0000000) | value)

    def cluster(self, c):
        return (self.data + (c - 2) * self.spc) * 512

    def root(self, image):
        """(offset, entry) of every 32-byte root slot."""
        if self.type != 32:
            start = (self.reserved + self.fats * self.fat_sectors) * 512
            return [(start + i * 32, image[start + i * 32:start + i * 32 + 32]) for i in range(self.root_entries)]
        slots, c = [], self.root_cluster
        while 2 <= c < self.clusters + 2:
            base = self.cluster(c)
            slots += [(base + i * 32, image[base + i * 32:base + i * 32 + 32]) for i in range(self.cb // 32)]
            c = self.get(c, image=image)
        return slots

    def find(self, image, name83):
        for offset, entry in self.root(image):
            if entry[:11] == name83:
                return offset
        raise KeyError(name83)

    def first(self, image, offset):
        lo = struct.unpack_from('<H', image, offset + 26)[0]
        hi = struct.unpack_from('<H', image, offset + 20)[0] if self.type == 32 else 0
        return lo | hi << 16

    def chain(self, image, first):
        out, c = [], first
        while 2 <= c < self.clusters + 2 and len(out) <= self.clusters:
            out.append(c)
            c = self.get(c, image=image)
        return out

    def free_clusters(self, image, count, start=None):
        found, c = [], start or self.clusters // 2
        while len(found) < count:
            if self.get(c, image=image) == 0:
                found.append(c)
            c += 1
        return found


@unittest.skipUnless(TOOLS, 'requires clang, dosfstools and mtools')
class ScanDiskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_tool()

    def volume(self, bits, name=None):
        path = OUT / f'{name or self._testMethodName}-fat{bits}.img'
        mib, spc, reserved = {12: (4, 4, 1), 16: (16, 2, 4), 32: (40, 1, 160)}[bits]
        path.unlink(missing_ok=True)
        with path.open('wb') as f:
            f.truncate(mib * 1024 * 1024)
        subprocess.run(['mkfs.fat', '--invariant', '-F', str(bits), '-S', '512', '-s', str(spc), '-f', '2',
                        '-R', str(reserved), '-i', '00C1A001', str(path)], check=True, capture_output=True)
        for i, (fname, size) in enumerate((('A.BIN', 9000), ('B.BIN', 5000), ('C.TXT', 700))):
            data = OUT / f'{self._testMethodName}-{fname}'
            data.write_bytes(pattern(i + 1, size))
            subprocess.run(['mcopy', '-i', str(path), str(data), '::/' + fname], check=True, env=ENV)
        subprocess.run(['mmd', '-i', str(path), '::/SUB'], check=True, env=ENV)
        data = OUT / f'{self._testMethodName}-N.BIN'
        data.write_bytes(pattern(9, 3000))
        subprocess.run(['mcopy', '-i', str(path), str(data), '::/SUB/N.BIN'], check=True, env=ENV)
        self.addCleanup(path.unlink, missing_ok=True)
        return path, Fat(path)

    def scan(self, path, *args):
        result = subprocess.run([str(TOOL), str(path), *args], capture_output=True, text=True, check=True)
        report = {}
        for line in result.stdout.splitlines():
            key, _, value = line.partition('=')
            report[key] = int(value) if value.lstrip('-').isdigit() else value
        self.assertEqual(report['leak'], 0, 'contiguous buffers released')
        return report

    def fsck(self, path):
        result = subprocess.run(['fsck.fat', '-n', str(path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('Reclaimed', result.stdout)
        return result.stdout

    def read(self, path, name):
        return subprocess.run(['mtype', '-i', str(path), '::/' + name], check=True, capture_output=True, env=ENV).stdout

    def clean_after(self, path):
        self.fsck(path)
        again = self.scan(path)
        for key in ('lost_chains', 'crosslinked_files', 'invalid_chains', 'size_mismatches',
                    'invalid_entries', 'fat_mismatches'):
            self.assertEqual(again[key], 0, key)
        self.assertFalse(again['flags'] & 0x400)

    def test_clean_volumes_report_nothing_and_fix_writes_nothing(self):
        for bits in (12, 16, 32):
            with self.subTest(fat=bits):
                path, fat = self.volume(bits)
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                for args in ((), ('fix',), ('fix', 'thorough')):
                    r = self.scan(path, *args)
                    self.assertEqual((r['result'], r['fat_type'], r['writes']), (0, bits, 0))
                    self.assertEqual((r['files'], r['directories']), (4, 1))
                    self.assertFalse(r['flags'] & 0x400)
                    self.assertEqual(r['clusters'], fat.clusters)
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)
                self.assertGreater(r['surface_sectors'], 0)

    def lost_chain(self, fat, image, length=3):
        free = fat.free_clusters(image, length)
        for i, c in enumerate(free):
            fat.set(image, c, free[i + 1] if i + 1 < length else fat.mask)
            image[fat.cluster(c):fat.cluster(c) + fat.cb] = pattern(40 + i, fat.cb)
        return free

    def test_lost_chains_become_chk_files_or_are_freed(self):
        for bits in (12, 16, 32):
            with self.subTest(fat=bits):
                path, fat = self.volume(bits)
                image = fat.raw()
                lost = self.lost_chain(fat, image)
                path.write_bytes(image)
                preview = self.scan(path)
                self.assertEqual((preview['lost_chains'], preview['lost_clusters'], preview['writes']), (1, 3, 0))
                r = self.scan(path, 'fix')
                self.assertEqual((r['lost_chains'], r['salvaged_files'], r['found']), (1, 1, 'FOUND.000'))
                self.assertTrue(r['flags'] & 0x800)
                self.clean_after(path)
                listing = subprocess.run(['mdir', '-i', str(path), '::/FOUND.000'], capture_output=True, text=True, env=ENV).stdout
                self.assertIn('FILE0000 CHK', listing)
                data = self.read(path, 'FOUND.000/FILE0000.CHK')
                self.assertEqual(data, b''.join(pattern(40 + i, fat.cb) for i in range(3)))
                self.assertEqual(self.read(path, 'A.BIN'), pattern(1, 9000))
                # FREE_LOST: a second lost chain is released instead.
                image = fat.raw()
                lost = self.lost_chain(fat, image, 2)
                path.write_bytes(image)
                r = self.scan(path, 'fix', 'free')
                self.assertEqual((r['lost_chains'], r['salvaged_files']), (1, 0))
                image = fat.raw()
                self.assertEqual([fat.get(c, image=image) for c in lost], [0, 0])
                self.clean_after(path)

    def test_cycle_in_a_cluster_chain_cycles_of_lost_clusters(self):
        path, fat = self.volume(32)
        image = fat.raw()
        a = fat.chain(image, fat.first(image, fat.find(image, b'A       BIN')))
        fat.set(image, a[-1], a[0])                       # the file's chain loops
        free = fat.free_clusters(image, 2)                # a lost two-cluster cycle
        fat.set(image, free[0], free[1]); fat.set(image, free[1], free[0])
        path.write_bytes(image)
        r = self.scan(path, 'fix')
        self.assertEqual((r['invalid_chains'], r['lost_chains'], r['salvaged_files']), (1, 1, 1))
        self.assertEqual(r['first_problem'], 'A.BIN')
        self.clean_after(path)
        self.assertEqual(self.read(path, 'A.BIN'), pattern(1, 9000))

    def test_invalid_links_and_sizes_beyond_the_chain(self):
        for bits in (16, 32):
            with self.subTest(fat=bits):
                path, fat = self.volume(bits)
                image = fat.raw()
                a = fat.chain(image, fat.first(image, fat.find(image, b'A       BIN')))
                fat.set(image, a[1], fat.clusters + 50)       # beyond the volume
                c_off = fat.find(image, b'C       TXT')
                struct.pack_into('<I', image, c_off + 28, 700 + 3 * fat.cb)   # size beyond its chain
                path.write_bytes(image)
                r = self.scan(path, 'fix')
                self.assertEqual(r['invalid_chains'], 1)
                self.assertEqual(r['size_mismatches'], 2)     # A after the cut, and C
                self.clean_after(path)
                self.assertEqual(self.read(path, 'A.BIN'), pattern(1, 9000)[:2 * fat.cb])
                self.assertEqual(self.read(path, 'C.TXT')[:700], pattern(3, 700))

    def test_cross_linked_files_are_copied(self):
        for bits in (12, 16, 32):
            with self.subTest(fat=bits):
                path, fat = self.volume(bits)
                image = fat.raw()
                a = fat.chain(image, fat.first(image, fat.find(image, b'A       BIN')))
                b = fat.chain(image, fat.first(image, fat.find(image, b'B       BIN')))
                fat.set(image, b[0], a[1])                     # B's second cluster is now A's
                path.write_bytes(image)
                r = self.scan(path, 'fix')
                self.assertEqual((r['crosslinked_files'], r['crosslinks_fixed']), (1, 1))
                self.assertEqual(r['lost_chains'], 1)          # B's own former tail
                self.assertTrue(r['flags'] & 0x800)
                self.clean_after(path)
                self.assertEqual(self.read(path, 'A.BIN'), pattern(1, 9000))
                expected = pattern(2, 5000)[:fat.cb] + pattern(1, 9000)[fat.cb:]
                self.assertEqual(self.read(path, 'B.BIN'), expected[:5000])
                image = fat.raw()
                b_after = fat.chain(image, fat.first(image, fat.find(image, b'B       BIN')))
                self.assertFalse(set(b_after) & set(fat.chain(image, a[0])))

    def test_no_copy_and_directory_cross_links_stay_reported(self):
        path, fat = self.volume(32)
        image = fat.raw()
        a = fat.chain(image, fat.first(image, fat.find(image, b'A       BIN')))
        b_off = fat.find(image, b'B       BIN')
        struct.pack_into('<H', image, b_off + 26, a[0] & 0xffff)
        struct.pack_into('<H', image, b_off + 20, a[0] >> 16)
        path.write_bytes(image)
        r = self.scan(path, 'fix', 'nocopy')
        self.assertEqual((r['crosslinked_files'], r['crosslinks_fixed']), (1, 0))
        self.assertFalse(r['flags'] & 0x800)                 # errors left
        self.assertTrue(r['flags'] & 0x400)

    def test_reserved_attribute_bits_and_fat_mirror(self):
        path, fat = self.volume(16)
        image = fat.raw()
        image[fat.find(image, b'C       TXT') + 11] |= 0x40
        a = fat.chain(image, fat.first(image, fat.find(image, b'A       BIN')))
        fat.set(image, a[0], 0x1234, copies=[1])            # second copy disagrees
        path.write_bytes(image)
        preview = self.scan(path)
        self.assertEqual((preview['invalid_entries'], preview['fat_mismatches'], preview['writes']), (1, 1, 0))
        r = self.scan(path, 'fix')
        self.assertEqual((r['invalid_entries'], r['fat_mismatches']), (1, 1))
        self.clean_after(path)
        image = fat.raw()
        self.assertEqual(fat.get(a[0], copy=1, image=image), fat.get(a[0], copy=0, image=image))

    def test_dirty_and_hard_error_bits_are_reported_and_kept(self):
        path, fat = self.volume(32)
        image = fat.raw()
        fat.set(image, 1, fat.get(1, image=image) & ~0x0c000000)
        path.write_bytes(image)
        r = self.scan(path, 'fix')
        self.assertEqual(r['flags'] & 0x6000, 0x6000)
        self.assertEqual(fat.get(1) & 0x0c000000, 0)       # the FAT layer owns them
        self.assertEqual(r['writes'], 0)

    def test_thorough_marks_bad_free_clusters_and_moves_used_data(self):
        for bits in (16, 32):
            with self.subTest(fat=bits):
                path, fat = self.volume(bits)
                image = fat.raw()
                a = fat.chain(image, fat.first(image, fat.find(image, b'A       BIN')))
                free = fat.free_clusters(image, 1)[0]
                bad_used = fat.data + (a[1] - 2) * fat.spc      # first sector of A's second cluster
                bad_head = fat.data + (fat.chain(image, fat.first(image, fat.find(image, b'C       TXT')))[0] - 2) * fat.spc
                bad_free = fat.data + (free - 2) * fat.spc
                bad = [f'bad={bad_used}', f'bad={bad_free}', f'bad={bad_head}']
                standard = self.scan(path, 'fix', *bad)
                self.assertEqual(standard['bad_sectors'], 0)
                r = self.scan(path, 'fix', 'thorough', *bad)
                self.assertEqual((r['bad_sectors'], r['bad_marked'], r['relocated']), (3, 1, 2))
                self.assertTrue(r['flags'] & 0x800)
                self.fsck(path)
                image = fat.raw()
                self.assertEqual(fat.get(free, image=image), fat.mask - 8)
                self.assertEqual(fat.get(a[1], image=image), fat.mask - 8)
                data = self.read(path, 'A.BIN')
                expected = bytearray(pattern(1, 9000))
                expected[fat.cb:fat.cb + 512] = bytes(512)     # the unreadable sector
                self.assertEqual(data, bytes(expected))
                c = self.read(path, 'C.TXT')
                self.assertEqual(c[512:700] if fat.spc > 1 else c, (pattern(3, 700)[512:700] if fat.spc > 1 else bytes(512)[:700] + pattern(3, 700)[512:]))
                self.assertEqual(r['bad_clusters'], 2 + 1)
                # The relocated clusters are now ordinary file data on a later scan.
                again = self.scan(path, 'thorough', *bad)
                self.assertEqual((again['bad_sectors'], again['relocated'], again['bad_marked']), (3, 0, 0))

    def test_subdirectory_damage_and_unreadable_system_area(self):
        path, fat = self.volume(32)
        image = fat.raw()
        sub = fat.first(image, fat.find(image, b'SUB        '))
        base = fat.cluster(sub)
        for i in range(fat.cb // 32):
            if image[base + i * 32:base + i * 32 + 11] == b'N       BIN':
                struct.pack_into('<I', image, base + i * 32 + 28, 3000 + 9 * fat.cb)
        path.write_bytes(image)
        r = self.scan(path, 'fix', 'thorough', f'bad={fat.reserved + 1}')
        self.assertEqual((r['size_mismatches'], r['unreadable_system']), (1, 1))
        self.assertFalse(r['flags'] & 0x800)                 # a bad FAT sector cannot be repaired
        self.fsck(path)

    def test_refuses_non_fat_and_reports_geometry(self):
        path = OUT / 'not-fat.img'
        path.write_bytes(bytes(64 * 512))
        self.addCleanup(path.unlink, missing_ok=True)
        r = self.scan(path, 'fix')
        self.assertEqual((r['result'], r['writes']), (-22, 0))


if __name__ == '__main__':
    unittest.main()
