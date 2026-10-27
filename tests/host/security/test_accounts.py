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
        sources = ["tests/host/security/accounts_test.c", "tests/host/security/fake.c", "tests/host/security/round2_test.c",
                   *[f"apps/accounts/{name}.c" for name in
                     ("crypto", "codec", "owner", "bootstrap", "protocol", "store", "service", "migration",
                      "persist", "results", "auth", "enrollment", "manifest", "checkpoint", "logon")],
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

    def test_shared_manifest(self):
        self.run_case("round2-manifest")

    def test_auth_wire_independent_oracle(self):
        path = Path(self.tmp.name) / "auth-wire.bin"
        result = self.run_case("round2-wire", path, check=False)
        begin = bytearray(64)
        struct.pack_into("<IIIIII", begin, 0, 1, 2, 0, 0, 1, 1000)
        def challenge(attempt):
            p = bytearray(64)
            struct.pack_into("<IIQIIIIIIIIQ", p, 0, 1, 2, attempt, 1000, 0, 1, 1, 1, 1, 1, 0, 1)
            return p
        success = bytearray(304)
        success[:64] = challenge(1)
        struct.pack_into("<iII", success, 64, 0, 1, 0)
        struct.pack_into("<Q", success, 80, 1)
        struct.pack_into("<IIIIIII", success, 96, 1, 64, 1000, 545, 2, 544, 545)
        struct.pack_into("<II", success, 96 + 52, 0, 1)
        root = b"/Windows/Profiles/u000003e8"
        struct.pack_into("<IIIIQQQII", success, 160, 1, 112, 1000, 0, 1234, 7, 103, len(root) + 1, 0)
        success[208:208 + len(root)] = root
        success[272:281] = b"CiukiUser"
        failure = bytearray(304)
        failure[:64] = challenge(2)
        struct.pack_into("<iII", failure, 64, -13, 4, 1000)
        struct.pack_into("<Q", failure, 80, 1)
        management = bytearray(312)
        struct.pack_into("<I", management, 0, 1)
        struct.pack_into("<IIQ", management, 32, 9, 1000, 1)
        management[144:153] = b"password1"
        digest = hashlib.sha256(struct.pack("<I", 2) + management).digest()
        self.assertEqual(path.read_bytes(), begin + challenge(1) + success + failure + digest)
        print("PASS Python independent full authentication and management-digest oracle")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_checkpoint_cuts(self):
        self.run_case("round2-checkpoint-faults")

    def test_faulted_queued_authentication(self):
        self.run_case("round3-faulted-queue")

    def test_imported_numeric_root_allocation(self):
        self.run_case("round3-root-collision")

    def test_committed_enrollment_outcome(self):
        self.run_case("round3-enrollment-outcome")

    def test_logon_respawn_attempts(self):
        self.run_case("round3-logon-restart")

    def test_distinct_retained_handles(self):
        self.run_case("round3-distinct-handles")

    def test_local_intent_payload_bound(self):
        self.run_case("round3-intent-bounds")

    def test_durable_cancellation(self):
        self.run_case("round2-cancellation")

    def test_logon_relay(self):
        self.run_case("round2-logon")

    def test_durable_results_and_metadata_window(self):
        self.run_case("round2-results")

    def test_owner_compaction_and_reservations(self):
        self.run_case("round2-owner")

    def test_profile_mapping(self):
        self.run_case("round2-profile")

    def test_autologon_route(self):
        self.run_case("round2-autologon")

    def test_enrollment_exchange(self):
        self.run_case("round2-enrollment")

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
        # actual destination manifest, DB hash, zero request locators and OWNER keys.
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
        manifest = bytearray(64 + 256 * 3)
        manifest[:8] = b"CIUKISTG"
        struct.pack_into("<IIII", manifest, 8, 1, 64, 3, 0)
        manifest[24:36] = bytes(range(1, 13))
        struct.pack_into("<QQ", manifest, 40, 1234, 1)
        tree = struct.pack("<I", 1) + b"\0" + struct.pack("<IQ", 1, 0) + bytes(32)
        for i, (uid, root) in enumerate(roots):
            off = 160 + 128 * i
            struct.pack_into("<IIQQQQII", request, off, uid, 1, 0, 0, 1234, 0, len(root) + 1, 1)
            request[off + 48:off + 48 + len(root)] = root
            off = 64 + 256 * i
            struct.pack_into("<IIII", manifest, off, uid, 0, 1, 1)
            manifest[off + 16:off + 16 + len(root)] = root
            staging = f"/System/Security/Staging/g{1:016x}/p{uid:08x}".encode()
            manifest[off + 144:off + 144 + len(staging)] = staging
            manifest[off + 208:off + 240] = hashlib.sha256(tree).digest()
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
