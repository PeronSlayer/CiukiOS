"""Guest provenance helper; no image build, no external payloads."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build_identity', ROOT/'scripts/build_identity.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)

class BuildIdentityTests(unittest.TestCase):
    def test_staging_and_determinism(self):
        with tempfile.TemporaryDirectory() as temp:
            d=Path(temp); kernel=d/'VMM.ELF'; kernel.write_bytes(b'kernel fixture')
            sha=hashlib.sha256(kernel.read_bytes()).hexdigest()
            (d/'build-clock.json').write_text(json.dumps({'kernel_sha256':sha,'utc_epoch':1792713600}))
            sources={'/System/VMM.ELF':kernel}; metadata={}
            first=identity.stage_identity(kernel,d/'out',sources,metadata,'a'*40,False,'fixture')
            text=sources['/System/BUILD.TXT'].read_text()
            self.assertIn('source_commit='+'a'*40,text)
            self.assertIn('kernel_elf_sha256='+sha,text)
            self.assertIn('build_manifest_id='+first,text)
            self.assertIn('build_date=2026-10-23T00:00:00Z',text)
            self.assertNotIn('image_sha256='+sha,text)
            self.assertEqual(identity.stage_identity(kernel,d/'out',sources,metadata,'a'*40,False,'fixture'),first)
            self.assertEqual(sources['/System/BUILD.TXT'].read_text(),text)
            self.assertEqual(metadata['build_manifest_id'],first)
            kernel.write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'kernel build-clock hash'):
                identity.stage_identity(kernel,d/'out',sources,metadata,'a'*40,False,'fixture')

class BuildIdentityUnknownTests(unittest.TestCase):
    def test_source_tree_without_git(self):
        with tempfile.TemporaryDirectory() as temp:
            d=Path(temp); kernel=d/'VMM.ELF'; kernel.write_bytes(b'k')
            sha=hashlib.sha256(b'k').hexdigest()
            (d/'build-clock.json').write_text(json.dumps({'kernel_sha256':sha,'utc_epoch':0}))
            sources={}; metadata={}
            identity.stage_identity(kernel,d/'o',sources,metadata,'unknown','unknown','v')
            text=sources['/System/BUILD.TXT'].read_text()
            self.assertIn('source_commit=unknown',text); self.assertIn('source_dirty=unknown',text)
            with self.assertRaises(ValueError):
                identity.stage_identity(kernel,d/'o',sources,metadata,'unknown',False,'v')
