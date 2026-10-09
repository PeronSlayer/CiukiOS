import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/test'))
from physical import import_evidence, FIELDS
from evidence import EvidenceError


class PhysicalImportTests(unittest.TestCase):
    def setUp(self):
        folder=ROOT/'build/runner-host-tests';folder.mkdir(exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=folder);self.capture=Path(self.temp.name)
        self.hash=hashlib.sha256(b'canonical').hexdigest()
        self.metadata={k:'unknown' for k in FIELDS}
        self.metadata.update(operator_confirmed=True,selector='f0:boot run=12345678',build_id='12ab34cd',write_sha256=self.hash,readback_sha256=self.hash,disk_log='unavailable')
        self.expected={'terminal':'END','predicates':[{'where':{'event':'DATA'},'fields':{'tick':{'ge':10000}}}]}
        (self.capture/'f0.log').write_text('CIUKI_TEST v=1 run=12345678 seq=000001 probe=boot event=BEGIN\nCIUKI_TEST v=1 run=12345678 seq=000002 probe=boot event=DATA tick=10000 build_id=12ab34cd\nCIUKI_TEST v=1 run=12345678 seq=000003 probe=boot event=END status=PASS\n')
    def tearDown(self):self.temp.cleanup()
    def acquire(self):
        (self.capture/'acquisition.json').write_text(json.dumps(self.metadata))
        return import_evidence(self.capture,self.hash,self.expected)
    def test_selector_matching_build_and_unknown_inventory(self):
        result=self.acquire();self.assertEqual(result['outcome'],'pass');self.assertEqual(result['disk_log'],'unavailable')
        self.assertEqual(result['physical']['bios_version'],'unknown');self.assertIn('f0.log',result['artifact_hashes'])
    def test_unconfirmed_wrong_hash_wrong_build_and_override_rejected(self):
        for key,value in [('operator_confirmed',False),('readback_sha256','0'*64),('build_id','87654321'),('selector','f0:boot run=12345678 platform=e500')]:
            old=self.metadata[key];self.metadata[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):self.acquire()
            self.metadata[key]=old
    def test_bounded_allowlist(self):
        (self.capture/'f0.log').write_bytes(b'a'*(128*1024+1))
        with self.assertRaisesRegex(EvidenceError,'128 KiB'):self.acquire()
