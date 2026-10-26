"""H03: link production account service with fault-injecting boundary adapters.

ASan/UBSan and default leak detection are retained, just as in the G32 harness.
"""
from pathlib import Path
import hashlib
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class AccountTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix="accounts-", dir=parent)
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "accounts-test"
        sources = ["tests/host/security/accounts_test.c", "tests/host/security/fake.c",
                   *[f"apps/accounts/{name}.c" for name in
                     ("crypto", "codec", "owner", "bootstrap", "protocol", "store", "service", "migration")],
                   "apps/desktop/users.c", "src/kernel/lib/sha256.c"]
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=undefined", "-I", str(ROOT / "apps/accounts"),
                        "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                        *[str(ROOT / name) for name in sources], "-pthread", "-o", str(cls.binary)], check=True)

    def run_case(self, name, *args, check=True):
        result = subprocess.run([str(self.binary), name, *map(str, args)],
                                text=True, capture_output=True)
        print(result.stdout, end="")
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_crypto(self):
        expected = hashlib.pbkdf2_hmac("sha256", b"password", bytes(range(16)), 10000)
        self.run_case("crypto", expected.hex())

    def test_codec_independent_fixture(self):
        path = Path(self.tmp.name) / "seed.bin"
        result = self.run_case("codec", path, check=False)
        expected = bytearray(96 + 3 * 256)
        expected[:8] = b"CIUKISEE"
        struct.pack_into("<IIQIIII", expected, 8, 2, 96, 0, 1001, 3, 0, 256)
        struct.pack_into("<II", expected, 52, 1, 1000)
        for i, (uid, flags, groups, name) in enumerate([
                (500, 2, 3, b"CiukiAdministrator"), (501, 2, 4, b"Guest"),
                (1000, 9, 3, b"CiukiUser")]):
            offset = 96 + 256 * i
            struct.pack_into("<III", expected, offset, uid, flags, groups)
            expected[offset + 24:offset + 24 + len(name)] = name
        expected[64:96] = hashlib.sha256(expected).digest()
        self.assertEqual(path.read_bytes(), expected)
        print("PASS Python independent full-file seed oracle")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_batch_codecs_and_receipts(self):
        path = Path(self.tmp.name) / "owner.bin"
        result = self.run_case("batch", path, check=False)
        # Independent complete bootstrap request/reply oracle, including the
        # actual destination manifest, DB hash, resolver locators and keys.
        db = bytearray(96 + 3 * 256)
        db[:8] = b"CIUKISEC"
        struct.pack_into("<IIQIIII", db, 8, 2, 96, 1, 1001, 3, 0, 256)
        db[40:52] = bytes(range(1, 13))
        struct.pack_into("<II", db, 52, 1, 1000)
        roots = []
        for i, (uid, flags, groups, name) in enumerate([
                (500, 2, 3, b"CiukiAdministrator"), (501, 2, 4, b"Guest"),
                (1000, 13, 3, b"CiukiUser")]):
            off = 96 + 256 * i
            struct.pack_into("<III", db, off, uid, flags, groups)
            db[off + 24:off + 24 + len(name)] = name
            if uid == 1000:
                struct.pack_into("<II", db, off + 12, 1, 10000)
                salt = bytes(range(13, 29))
                db[off + 56:off + 72] = salt
                db[off + 72:off + 104] = hashlib.pbkdf2_hmac("sha256", b"", salt, 10000)
            roots.append((uid, f"/Windows/Profiles/u{uid:08x}".encode()))
        db[64:96] = hashlib.sha256(db).digest()
        request = bytearray(544)
        struct.pack_into("<IIII", request, 0, 1, 160, 0, 3)
        request[16:28] = bytes(range(1, 13))
        struct.pack_into("<QQQQ", request, 32, 1234, 1, 0, 0)
        request[64:96] = hashlib.sha256(db).digest()
        manifest = bytearray()
        for i, (uid, root) in enumerate(roots):
            manifest += root.ljust(64, b"\0") + struct.pack("<I", uid)
            off = 160 + 128 * i
            locator = int.from_bytes(hashlib.sha256(root).digest()[:8], "little")
            struct.pack_into("<IIQQQQII", request, off, uid, 1, 0, 0, 1234, locator, len(root) + 1, 1)
            request[off + 48:off + 48 + len(root)] = root
        request[96:128] = hashlib.sha256(manifest).digest()
        digest = hashlib.sha256(request).digest()
        request[128:160] = digest
        struct.pack_into("<I", request, 8, 1)
        reply = bytearray(192)
        struct.pack_into("<IIII", reply, 0, 1, 96, 2, 3)
        reply[16:28] = bytes(range(1, 13))
        struct.pack_into("<QQQ", reply, 32, 1234, 1, 7)
        reply[64:96] = digest
        for i, (uid, _) in enumerate(roots):
            struct.pack_into("<IIQQII", reply, 96 + 32 * i, uid, i, 101 + i, 7, 1, 0)
        self.assertEqual(path.read_bytes(), request + reply)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_bootstrap_state_oracle(self):
        path = Path(self.tmp.name) / "bootstrap.bin"
        result = self.run_case("bootstrap-codec", path, check=False)
        expected = bytearray(160)
        expected[:8] = b"CIUKIBST"
        struct.pack_into("<IIIIQ", expected, 8, 1, 160, 1, 1, 1234)
        expected[32:64] = bytes(range(1, 33))
        struct.pack_into("<IIQ", expected, 104, 1001, 0, 1)
        expected[120:152] = hashlib.sha256(expected).digest()
        self.assertEqual(path.read_bytes(), expected)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_retirement(self):
        self.run_case("retirement")

    def test_endpoint_codecs_and_attempts(self):
        self.run_case("protocol")

    def test_service(self):
        self.run_case("service")

    def test_setup_seed(self):
        self.run_case("setup-seed")

    def test_failures(self):
        self.run_case("failures")

    def test_capacity_and_exhaustion(self):
        self.run_case("capacity")

    def test_disk_oom_and_corruption(self):
        self.run_case("disk-oom")

    def test_concurrent_clients_and_cancellation(self):
        self.run_case("concurrency")

    def test_every_power_cut(self):
        self.run_case("faults")

    def test_sixteen_profile_batch(self):
        self.run_case("migration-batch")

    def test_g32_read_only_import(self):
        self.run_case("migration")


if __name__ == "__main__":
    unittest.main()
