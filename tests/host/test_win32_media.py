"""Independent sector/path-table checks for Python-only optical mastering."""
import hashlib
import os
from pathlib import Path
import struct
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'scripts/test'))
import iso_fixtures


class MediaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base/'source'
        self.source.mkdir()
        (self.source/'README.TXT').write_bytes(b'Ciuki authored optical fixture\n')
        (self.source/'Caf\u00e9').mkdir()
        (self.source/'Caf\u00e9'/'Long title.txt').write_bytes(bytes(range(256))*17)
        (self.source/'EMPTY.BIN').write_bytes(b'')

    def test_reproducible_independent_of_timestamps_permissions_creation_order(self):
        a = iso_fixtures.generate_directory(self.source, self.base/'a.iso')
        for p in self.source.rglob('*'):
            os.utime(p, (1, 1))
        b = iso_fixtures.generate_directory(self.source, self.base/'b.iso')
        self.assertEqual(a['sha256'], b['sha256'])
        self.assertEqual(a['entries'], b['entries'])
        self.assertEqual(a['size'], 300*2048)
        self.assertEqual({e['path'] for e in a['entries']}, {'/README.TXT','/Caf\u00e9','/Caf\u00e9/Long title.txt','/EMPTY.BIN'})
        reverse = self.base/'reverse'
        reverse.mkdir()
        for p in sorted(self.source.rglob('*'), reverse=True):
            target = reverse/p.relative_to(self.source)
            if p.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(p.read_bytes())
                target.chmod(0o600)
        c = iso_fixtures.generate_directory(reverse, self.base/'c.iso')
        self.assertEqual(c['sha256'], a['sha256'])

    @unittest.skipUnless(shutil.which('xorriso'), 'independent optical extractor absent')
    def test_independent_xorriso_extracts_python_mastered_joliet(self):
        m = iso_fixtures.generate_directory(self.source, self.base/'disc.iso')
        extracted = self.base/'extracted'
        result = subprocess.run(['xorriso','-osirrox','on','-indev',m['path'],
                                 '-extract','/',str(extracted)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        for entry in m['entries']:
            path = extracted/entry['path'].lstrip('/')
            if entry['kind'] == 'directory':
                self.assertTrue(path.is_dir())
            else:
                self.assertEqual(path.stat().st_size, entry['size'])
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), entry['sha256'])
        # Extracted ISO directories can be read-only.
        for path in [extracted, *extracted.rglob('*')]:
            if path.is_dir(): path.chmod(0o700)

    def test_descriptors_both_endian_path_tables_and_pinned_dates(self):
        m = iso_fixtures.generate_directory(self.source, self.base/'disc.iso')
        data = Path(m['path']).read_bytes()
        self.assertEqual(data[16*2048:16*2048+7], b'\1CD001\1')
        self.assertEqual(data[17*2048:17*2048+7], b'\2CD001\1')
        self.assertEqual(data[18*2048:18*2048+7], b'\xffCD001\1')
        self.assertEqual(data[17*2048+88:17*2048+91], b'%/E')
        for offset in (16*2048, 17*2048):
            for at, width, value in ((80,4,300),(120,2,1),(124,2,1),(128,2,2048)):
                self.assertEqual(int.from_bytes(data[offset+at:offset+at+width], 'little'), value)
                self.assertEqual(int.from_bytes(data[offset+at+width:offset+at+2*width], 'big'), value)
            self.assertEqual(data[offset+813:offset+830], b'2020010100000000\0')
            size = int.from_bytes(data[offset+132:offset+136], 'little')
            le = int.from_bytes(data[offset+140:offset+144], 'little')*2048
            be = int.from_bytes(data[offset+148:offset+152], 'big')*2048
            tables = []
            for base, endian in ((le,'little'),(be,'big')):
                rows = []; pos = 0
                while pos < size:
                    n = data[base+pos]
                    rows.append((int.from_bytes(data[base+pos+2:base+pos+6], endian),
                                 int.from_bytes(data[base+pos+6:base+pos+8], endian),
                                 data[base+pos+8:base+pos+8+n]))
                    pos += 8+n+(n%2)
                self.assertEqual(pos, size)
                tables.append(rows)
            self.assertEqual(tables[0], tables[1])
            self.assertEqual(tables[0][0][1:], (1,b'\0'))
            self.assertEqual(tables[0][1][1], 1)

    def test_primary_hierarchy_and_many_records_cross_blocks(self):
        for n in range(100):
            (self.source/f'F{n:03}.TXT').write_bytes(str(n).encode())
        m = iso_fixtures.generate_directory(self.source, self.base/'primary.iso', False)
        self.assertFalse(m['joliet'])
        self.assertTrue(any(e['path'].endswith('.BIN') and e['size'] == 4352 for e in m['entries'] if e['kind'] == 'file'))
        data = Path(m['path']).read_bytes()
        root = 16*2048+156
        extent = int.from_bytes(data[root+2:root+6], 'little')*2048
        size = int.from_bytes(data[root+10:root+14], 'little')
        self.assertGreater(size, 2048)
        count = 0
        for block in range(extent, extent+size, 2048):
            pos = block
            while data[pos]:
                length = data[pos]
                self.assertLessEqual(pos+length, block+2048)
                self.assertEqual(data[pos+18:pos+25], bytes((120,1,1,0,0,0,0)))
                pos += length
                count += 1
        self.assertEqual(count, 105)

    def test_source_boundaries_and_symlinks(self):
        with self.assertRaises(ValueError):
            iso_fixtures.generate_directory(self.source, self.source/'bad.iso')
        bad = self.source/'bad-link'
        bad.symlink_to(self.source/'README.TXT')
        with self.assertRaises(ValueError):
            iso_fixtures.generate_directory(self.source, self.base/'bad.iso')
        bad.unlink()
        for name in ('bad;version', '\U0001f415.txt', 'x'*65, 'bad.'):
            path = self.source/name
            path.write_bytes(b'bad')
            with self.assertRaises(ValueError):
                iso_fixtures.generate_directory(self.source, self.base/'bad.iso')
            path.unlink()
        (self.source/'readme.txt').write_bytes(b'collision')
        with self.assertRaises(ValueError):
            iso_fixtures.generate_directory(self.source, self.base/'bad.iso')

    def test_depth_name_limits_and_versioned_extensionless_primary(self):
        deep = self.source
        for n in range(7):
            deep = deep/f'D{n}'
            deep.mkdir()
        (deep/'FILE').write_bytes(b'extensionless')
        good = self.source/('N'*62)
        good.write_bytes(b'maximum file identifier')
        m = iso_fixtures.generate_directory(self.source, self.base/'limits.iso')
        data = Path(m['path']).read_bytes()
        self.assertIn(b'FILE.;1', data)
        self.assertIn(('N'*62+';1').encode('utf-16-be'), data)
        (deep/'D8').mkdir()
        with self.assertRaises(ValueError):
            iso_fixtures.generate_directory(self.source, self.base/'deep.iso')
        (deep/'D8').rmdir()
        (self.source/('N'*63)).write_bytes(b'too long')
        with self.assertRaises(ValueError):
            iso_fixtures.generate_directory(self.source, self.base/'long.iso')

    def test_track_aware_cue_and_authored_pcm(self):
        a = iso_fixtures.generate_mixed_mode(self.source, self.base/'a')
        b = iso_fixtures.generate_mixed_mode(self.source, self.base/'b')
        self.assertEqual(a['data']['sha256'], b['data']['sha256'])
        self.assertEqual(a['audio']['sha256'], b['audio']['sha256'])
        self.assertEqual(Path(a['cue']).read_bytes(), Path(b['cue']).read_bytes())
        self.assertEqual(a['tracks'], [{'number':1,'type':'MODE1/2048','index01_lba':0,'sectors':300},
                                     {'number':2,'type':'AUDIO','index00_lba':300,'index01_lba':450,'sectors':300}])
        self.assertEqual(a['leadout_lba'], 750)
        audio = Path(a['audio']['path']).read_bytes()
        self.assertEqual(len(audio), 450*2352)
        self.assertEqual(audio[:150*2352], bytes(150*2352))
        frames = list(struct.iter_unpack('<hh', audio[150*2352:]))
        self.assertEqual(len(frames), 4*44100)
        self.assertTrue(all(left == right and abs(left) == 6000 for left, right in frames))
        self.assertEqual(hashlib.sha256(audio).hexdigest(), a['audio']['sha256'])
        self.assertIn('TRACK 02 AUDIO', Path(a['cue']).read_text())
        for seconds in (0,3,61,4.5,True):
            with self.assertRaises(ValueError):
                iso_fixtures.generate_mixed_mode(self.source, self.base/'bad', seconds)


if __name__ == '__main__':
    unittest.main()
