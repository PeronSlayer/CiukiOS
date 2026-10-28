"""H08 (sec-user-02): Setup and ready-image provisioning, first-boot promotion,
G32 migration/enrollment, the confirmed Setup CD repair and the protected
installer store.

Links the production account service core, the first-boot adapter and Setup's
seed/repair module with the fault-injecting fake store (ASan/UBSan, default
leak detection), checks the image builder's Python encoders against those C
codecs, and checks the protected installer store with the production kernel
FAT/VFS/policy resolver. Run one command at a time, at nice 19.
"""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SERVICE = ("crypto", "codec", "owner", "bootstrap", "protocol", "store", "service", "migration",
           "persist", "results", "auth", "enrollment", "manifest", "checkpoint", "logon", "firstboot")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


image = load("h08_build_image", ROOT / "scripts/build_image.py")
q3a = load("h08_q3a_demo", ROOT / "scripts/q3a_demo.py")


class ProvisioningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix="provisioning-", dir=parent)
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "provisioning-test"
        sources = ["tests/host/security/provisioning_test.c", "tests/host/security/fake.c",
                   *[f"apps/accounts/{name}.c" for name in SERVICE],
                   "apps/setup/accounts.c", "apps/desktop/users.c", "src/kernel/lib/sha256.c"]
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=undefined", "-I", str(ROOT / "apps/accounts"),
                        "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                        "-I", str(ROOT / "sdk/libsetup/include"), "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                        *[str(ROOT / name) for name in sources], "-pthread", "-o", str(cls.binary)],
                       check=True, env=dict(os.environ, TMPDIR=cls.tmp.name))

    def run_case(self, *args):
        result = subprocess.run([str(self.binary), *map(str, args)], text=True, capture_output=True,
                                env=dict(os.environ, TMPDIR=self.tmp.name), timeout=600)
        print(result.stdout, end="")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_builder_codec_against_account_codec(self):
        """scripts/build_image.py's ready seed and bootstrap.state are the exact
        section 3.6 bytes the account service decodes and re-encodes."""
        seed = image.ready_seed()
        identity = 2048 << 32 | 0x1234ABCD
        state = image.bootstrap_state(1, identity, hashlib.sha256(seed).digest())
        folder = Path(self.tmp.name)
        (folder / "ready.seed").write_bytes(seed)
        (folder / "ready.state").write_bytes(state)
        self.assertIn("PASS H08 codec", self.run_case("codec", folder / "ready.seed", folder / "ready.state", hex(identity)))
        # Independent Python reading of the layout (contract 3.3/3.6).
        self.assertEqual((seed[:8], len(seed)), (b"CIUKISEE", 864))
        self.assertEqual(struct.unpack_from("<IIQIIII", seed, 8), (2, 96, 0, 1001, 3, 0, 256))
        self.assertEqual(seed[40:52], bytes(12))
        self.assertEqual(struct.unpack_from("<III", seed, 52), (1, 1000, 0))
        zeroed = bytearray(seed); zeroed[64:96] = bytes(32)
        self.assertEqual(hashlib.sha256(zeroed).digest(), seed[64:96])
        rows = [struct.unpack_from("<IIIIII", seed, 96 + 256 * i) + (seed[96 + 256 * i + 24:96 + 256 * i + 56].rstrip(b"\0"),)
                for i in range(3)]
        self.assertEqual(rows, [(500, 2, 3, 0, 0, 0, b"CiukiAdministrator"), (501, 2, 4, 0, 0, 0, b"Guest"),
                                (1000, 9, 3, 0, 0, 0, b"CiukiUser")])
        for i in range(3):
            record = seed[96 + 256 * i:96 + 256 * (i + 1)]
            self.assertEqual(record[56:104], bytes(48))        # no salt/verifier in a ready seed
            self.assertEqual(record[104:256], bytes(152))      # empty picture, reserved zero
        self.assertEqual(struct.unpack_from("<IIIIQ", state, 8), (1, 160, 1, 1, identity))
        self.assertEqual(state[32:64], hashlib.sha256(seed).digest())
        self.assertEqual(struct.unpack_from("<IIIIQQQIIQ", state, 64), (0, 0, 0, 0, 0, 0, 0, 1001, 0, 1))
        zeroed = bytearray(state); zeroed[120:152] = bytes(32)
        self.assertEqual(hashlib.sha256(zeroed).digest(), state[120:152])
        self.assertEqual(state[152:], bytes(8))
        for bad in ((0, identity), (4, identity), (1, 0)):
            with self.assertRaises(ValueError):
                image.bootstrap_state(bad[0], bad[1], hashlib.sha256(seed).digest())

    def test_seed_promotion_every_cut(self):
        out = self.run_case("promotion")
        self.assertIn("PASS H08 ready seed promotion", out)
        self.assertIn("PASS H08 Setup seed promotion", out)

    def test_contradictory_evidence(self):
        self.assertIn("PASS H08 contradictory evidence", self.run_case("contradictory"))

    def test_g32_migration_and_enrollment(self):
        self.assertIn("PASS H08 G32 migration", self.run_case("migration"))

    def test_confirmed_repair(self):
        self.assertIn("PASS H08 Setup CD repair", self.run_case("repair"))

    @unittest.skipUnless(shutil.which("mkfs.fat"), "requires mkfs.fat")
    def test_ready_metadata_and_volume_identity(self):
        """The builder binds the seed to the formatted volume's BPB serial and
        declares the ready kind in explicit build metadata."""
        folder = Path(self.tmp.name) / "volume"
        folder.mkdir()
        volume = folder / "system.fat"
        with volume.open("wb") as stream:
            stream.truncate(40 * 1024 * 1024)
        subprocess.run(["mkfs.fat", "--invariant", "-F", "32", "-s", "1", "-n", "CIUKIOS", str(volume)],
                       check=True, stdout=subprocess.DEVNULL)
        sources, directories, metadata = image.ready_provisioning(folder / "provision", volume)
        serial = struct.unpack_from("<I", volume.read_bytes()[:512], 67)[0]
        self.assertEqual(metadata["volume_id"], f"0x{2048 << 32 | serial:016x}")
        self.assertEqual((metadata["kind"], metadata["origin"], metadata["auto_logon_uid"]), ("ready", 1, 1000))
        self.assertEqual(set(sources), {"/System/Security/provision.seed", "/System/Security/bootstrap.state"})
        self.assertIn("/WINDOWS/Profiles", directories)
        self.assertNotIn("/WINDOWS/Profiles/u000003e8", directories)   # OWNER installs private roots at first boot
        state = sources["/System/Security/bootstrap.state"].read_bytes()
        self.assertEqual(struct.unpack_from("<Q", state, 24)[0], 2048 << 32 | serial)

    def test_no_executable_in_shared_documents(self):
        """Contract 8.3/A1-5: a static payload-map check refuses PE, ELF, COM/MZ,
        DLL and scripts (also without a suffix) under shared /Documents."""
        folder = Path(self.tmp.name) / "documents"
        folder.mkdir()
        cases = {"pe.bin": b"MZ\x90\0", "elf": b"\x7fELF\x01", "script": b"#!/bin/sh\n", "plain.exe": b"text",
                 "LIB.DLL": b"text", "run.bat": b"@echo", "tool.com": b"\xb4\x4c", "start.ciuki": b"text"}
        for name, data in cases.items():
            (folder / name).write_bytes(data)
            with self.assertRaisesRegex(ValueError, "executable payload under shared /Documents"):
                image.refuse_shared_executables({"/Documents/Desktop/" + name: folder / name})
        (folder / "notes.txt").write_bytes(b"Hello\n")
        (folder / "main.c").write_bytes(b"int main(void){return 0;}\n")
        image.refuse_shared_executables({"/Documents/notes.txt": folder / "notes.txt",
                                         "/Documents/Developer/Samples/HelloWindow/main.c": folder / "main.c",
                                         "/Programs/elsewhere.exe": folder / "pe.bin"})
        sources, _, _, _ = q3a.payloads(folder / "q3a")
        image.refuse_shared_executables(sources)
        self.assertFalse([p for p in sources if p.casefold().startswith("/documents/") and not p.endswith(".lnk")])

    def test_installer_store_policy(self):
        """Protected installer store, presentation anchor and ancestors through
        the production FAT/VFS resolver and policy: replacement, adjacent and
        current-directory DLL planting, renames and 8.3 aliases, before and
        after a policy rebuild (reboot)."""
        subprocess.run(["python3", "-B", str(ROOT / "tests/host/fs/fixtures.py")], check=True)
        out = ROOT / "build/host/security"
        out.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(out))
        owner = [f"src/kernel/fs/{name}.c" for name in
                 ("owner", "owner_wire", "owner_store", "owner_checkpoint", "owner_roots", "security_policy")]
        sources = ["tests/host/security/installer_store_test.c", "tests/host/proc/signal_legacy.c",
                   "tests/host/proc/media_legacy.c", "tests/host/proc/desktop_legacy.c",
                   "src/kernel/fs/fs_port.c", "src/kernel/fs/blkreq.c", "src/kernel/fs/cache.c",
                   "src/kernel/fs/path.c", "src/kernel/fs/fat.c", "src/kernel/fs/vfs.c",
                   "src/kernel/fs/fs_ro.c", "src/kernel/fs/iso9660.c", "tests/host/fs/fake.c",
                   "src/kernel/probes/f2_probes_files.c", "src/kernel/lib/sha256.c",
                   "src/kernel/fs/owner_vfs.c", "src/kernel/fs/security_path.c", *owner]
        binary = out / "installer_store"
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-pthread",
                        "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "src/kernel/fs"),
                        *[str(ROOT / s) for s in sources], "-o", str(binary)], check=True, env=env)
        result = subprocess.run([str(binary), str(ROOT / "build/host/fs/fat32.img"), str(ROOT / "build/host/fs/fat16.img")],
                                env=env, capture_output=True, text=True, timeout=180)
        print(result.stdout, end="")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS H08 installer store", result.stdout)


if __name__ == "__main__":
    unittest.main()
