"""Unified-only build-plan tests; builders and files are synthetic, no image build."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[3]

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

image = load("f363_image", ROOT / "scripts/build_image.py")
dos = load("f363_dos", ROOT / "scripts/build_dos_image.py")

class UnifiedOnlyBuildTests(unittest.TestCase):
    def payloads(self, manifest=None, error=None):
        identity = {"profile": {"kernel": "unified", "ifs": True}, "kernel": {"personality": "ciukidos-fork"}}
        record = manifest or {"kernel": "unified", "identity": identity,
                              "licenses": "synthetic/licenses", "unified_dos": {"status": "synthetic"}}
        builder = SimpleNamespace(build=Mock(return_value=(Path("synthetic/UNIFIED.IMG"), record), side_effect=error))
        games = SimpleNamespace(PACKAGES=("fixture",), GAMES=Path("synthetic/packages"),
                                package_files=Mock(return_value=({"archive": "fixture.zip"}, [], [("COPYING.TXT", b"notice")])),
                                free_hashes=Mock(return_value={"pinned-shareware-hash"}))
        reference = SimpleNamespace(module=lambda name, path: games if name=="build_games_disk" else SimpleNamespace())
        modules = {"build_dos_image": builder, "dos_conformance": reference}
        spec = lambda name, path: SimpleNamespace(name=name, loader=SimpleNamespace(exec_module=lambda module: None))
        with patch.object(importlib.util, "spec_from_file_location", side_effect=spec), \
             patch.object(importlib.util, "module_from_spec", side_effect=lambda spec: modules[spec.name]):
            result = image.dos_payloads(Path("synthetic/ciukios.img"))
        builder.build.assert_called_once_with()
        return result

    def test_canonical_payload_plan_has_one_unified_base_and_all_notices(self):
        sources, directories, metadata, free = self.payloads()
        self.assertEqual({p for p in sources if p.lower().endswith(".img")}, {"/system/dos/UNIFIED.IMG"})
        self.assertIn("/system/dos/UNIFIED.SHA", sources)
        self.assertIn("/system/dos/D48PLAN.TXT", sources)
        self.assertEqual(metadata["dos_conformance"]["reference"], "host-harness-only")
        self.assertEqual(metadata["games"]["base"], "/system/dos/UNIFIED.IMG")
        self.assertEqual(free, {"pinned-shareware-hash"})
        for notice in ("CIUKIDOS.TXT", "FDK-GPL2.TXT", "FREECOM.TXT", "SUPPL.TXT", "SHARE.TXT", "CIUKIDOS.JSN"):
            self.assertIn("/system/licenses/"+notice, sources)
        self.assertIn("/system/licenses/fixture-COPYING.TXT", sources)
        self.assertIn("/system/licenses", directories)

    def test_missing_qualification_and_legacy_guest_never_fall_back(self):
        with self.assertRaisesRegex(FileNotFoundError, "qualification"):
            self.payloads(error=FileNotFoundError("qualification"))
        for identity in ({}, {"profile": {"ifs": False}, "kernel": {"personality": "reference"}}):
            with self.assertRaises((ValueError, SystemExit)):
                self.payloads({"kernel": "unified", "identity": identity})

    def test_system_payload_generator_never_assembles_a_retired_kernel(self):
        integration = SimpleNamespace(probes=lambda out: ({"/D48/PLAN.TXT": b"synthetic"}, {"cases": [{"tag": "D48001"}]}))
        with patch.object(dos, "script_module", return_value=integration), \
             patch.object(dos, "assemble", return_value=b"synthetic") as assemble, \
             patch.object(dos, "watcom_build", return_value=(b"client", b"extender")):
            files, directories = dos.system_payloads(Path("synthetic"), Path("synthetic/UNIFIED.IMG"))
        self.assertNotIn("/SYSTEM/CIUKIDOS.SYS", files)
        self.assertNotIn("/COMMAND.COM", files)
        self.assertIn("/DOSREADY.COM", files)
        self.assertIn("D48TEST/D48001", directories)
        self.assertGreater(len(assemble.call_args_list), 10)
        for call in assemble.call_args_list:
            self.assertNotIn("ciukidos.asm", call.args[0])
            self.assertNotIn("floppy_stage1", call.args[0])

if __name__ == "__main__":
    unittest.main()
