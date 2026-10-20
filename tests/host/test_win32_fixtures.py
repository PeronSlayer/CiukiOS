"""Deterministic, local-only fixture media; all input bytes are authored here."""
import copy
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
import win32_fixtures as wf
import fat_fixtures

TOOLS = all(shutil.which(t) for t in ('mkfs.fat', 'fsck.fat', 'mmd', 'mcopy', 'mdir'))


class FixtureTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build/host/win32-fixtures'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / 'source'
        (self.source / 'Long Folder').mkdir(parents=True)
        (self.source / 'Long Folder' / 'Authored.bin').write_bytes(bytes(range(251)) * 37)
        (self.source / 'SETUP.TXT').write_bytes(b'CiukiOS authored fixture, no game payload\n')

    def test_scan_identity_is_sorted_and_sensitive_to_names_and_bytes(self):
        scanned = wf.scan(self.source)
        expected = hashlib.sha256(b''.join(
            f"{r['path']}\0{r['size']}\0{r['sha256']}\n".encode() for r in scanned['files'])).hexdigest()
        self.assertEqual(wf.payload_identity(scanned['files'][::-1]), expected)
        (self.source / 'SETUP.TXT').rename(self.source / 'OTHER.TXT')
        self.assertNotEqual(wf.payload_identity(wf.scan(self.source)['files']), expected)

    def test_absent_reserved_ambiguous_symlink_and_budget(self):
        with self.assertRaises(wf.WorkloadAbsent): wf.scan(self.base / 'absent')
        for name in ('ciuki-id.txt', 'setup.txt', 'bad?.txt'):
            p = self.source / name; p.write_bytes(b'x')
            with self.assertRaises(ValueError): wf.scan(self.source)
            p.unlink()
        p = self.source / 'link'; p.symlink_to(self.source / 'SETUP.TXT')
        with self.assertRaises(ValueError): wf.scan(self.source)
        p.unlink()
        with patch.object(wf, 'MAX_BYTES', 2), self.assertRaises(ValueError): wf.scan(self.source)

    @unittest.skipUnless(TOOLS, 'FAT fixture host tools absent')
    def test_fat16_fat32_hashes_ignore_source_and_wall_clock_timestamps(self):
        for media in ('fat16', 'fat32'):
            with self.subTest(media=media):
                scanned = wf.scan(self.source)
                identity = wf.identity_text('authored-fixture', media, wf.payload_identity(scanned['files']))
                images = []
                for n, stamp in enumerate((946684800, 1262304000)):
                    for p in self.source.rglob('*'): os.utime(p, (stamp, stamp))
                    image = self.base / f'{media}-{n}.img'
                    wf.fat_image(scanned, self.source, image, media, identity)
                    images.append(image)
                self.assertEqual(wf.sha(images[0]), wf.sha(images[1]))
                with images[0].open('rb') as stream:
                    self.assertEqual(fat_fixtures.geometry(stream.read(512))['kind'], int(media[3:]))
                result = subprocess.run(['mtype', '-i', str(images[0]), '::/Long Folder/Authored.bin'],
                                        check=True, capture_output=True)
                self.assertEqual(result.stdout, (self.source / 'Long Folder/Authored.bin').read_bytes())
                for image in images: image.unlink()

    def test_iso_and_mixed_data_track_are_reproducible(self):
        scanned = wf.scan(self.source)
        for media in ('iso', 'mixed'):
            outputs = []
            for n in (0, 1):
                folder = self.base / f'{media}-{n}'; folder.mkdir()
                outputs.append(wf.iso_image(scanned, self.source, folder, 0, media, b'authored identity', 4))
            self.assertEqual(wf.sha(outputs[0]['path']), wf.sha(outputs[1]['path']))
            if media == 'mixed':
                mixed = outputs[0]['master']['mixed_mode']
                self.assertEqual(Path(mixed['data']['path']).read_bytes(), Path(outputs[0]['path']).read_bytes())
                self.assertEqual(mixed['tracks'][1]['type'], 'AUDIO')
                self.assertIn('not attached', outputs[0]['audio_track_transport'])
            self.assertFalse(any(self.base.rglob('win32-iso-source-*')))

    def test_validation_rejects_unknown_fields_media_and_audio(self):
        fixture = {'generator': wf.GENERATOR, 'workload': 'diablo-shareware', 'media': 'iso', 'redistribution': 'local-only'}
        wf.validate(fixture)
        for changes in ({'media': 'raw'}, {'redistribution': 'public'}, {'audio_seconds': 4},
                        {'media': 'mixed', 'audio_seconds': True}, {'path': '/elsewhere'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError): wf.validate({**fixture, **changes})

    def test_generation_checks_archive_pin_and_keeps_payload_out_of_image(self):
        fixture = {'generator': wf.GENERATOR, 'workload': 'diablo-shareware', 'media': 'iso', 'redistribution': 'local-only'}
        row = copy.deepcopy(wf.workload_row('diablo-shareware'))
        output = self.base / 'build/host/media'
        with patch.object(wf, 'workload_row', return_value=row), patch.object(wf, 'source_directory', return_value=self.source):
            with self.assertRaises(wf.WorkloadUnqualified): wf.generate(fixture, output, root=self.base)
            archive = self.base / wf.DOWNLOADS / row['id'] / row['source']['file']
            archive.parent.mkdir(parents=True); archive.write_bytes(b'CiukiOS authored archive fixture')
            from scripts.win32 import workloads
            row['source'].update(workloads.hash_file(archive))
            manifest = wf.generate(fixture, output, root=self.base)
            self.assertEqual(manifest['archive_verification']['status'], 'pinned')
            self.assertEqual(manifest['payload_sha256'], wf.payload_identity(wf.scan(self.source)['files']))
            self.assertEqual(manifest['sha256'], wf.sha(manifest['path']))
            with self.assertRaisesRegex(ValueError, 'already exists'): wf.generate(fixture, output, root=self.base)
            with self.assertRaises(ValueError): wf.generate(fixture, self.base / 'build/f0', root=self.base)
            archive.write_bytes(b'corrupt')
            with self.assertRaisesRegex(ValueError, 'mismatch'): wf.generate(fixture, self.base / 'build/host/other', root=self.base)


if __name__ == '__main__':
    unittest.main()
