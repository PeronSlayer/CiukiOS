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
                     ("crypto", "codec", "store", "service", "migration")],
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
        struct.pack_into("<IIQIIII", expected, 8, 2, 96, 1, 1001, 3, 0, 256)
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

    def test_g32_read_only_import(self):
        self.run_case("migration")


if __name__ == "__main__":
    unittest.main()
