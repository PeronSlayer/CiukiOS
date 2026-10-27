"""sec-int-01: the production account-service kernel adapter on the kernel host harness.

apps/accounts/kernel_adapter.c is linked unchanged with the production account
service and driven through proc_syscall (call 101) and the kernel's own service
channels as the ACCOUNT process; LOGON and LOGON_UI are kernel-registered roles
of the fixture. Only OWNER and protected storage use the H03 host fake, which
live open refuses. ASan/UBSan and default leak detection are retained.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SERVICE = ("crypto", "codec", "owner", "bootstrap", "protocol", "store", "service", "migration",
           "persist", "results", "auth", "enrollment", "manifest", "checkpoint", "logon")
SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
WARN = ["-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections"]


class AccountAdapterTests(unittest.TestCase):
    def test_kernel_adapter_integration(self):
        out = ROOT / "build/host/security/adapter"
        out.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(out))
        include = ["-I", str(ROOT / "apps/accounts"), "-I", str(ROOT / "sdk/private"),
                   "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include")]
        objects = []
        # Account service, adapter and fake: user-space C with POSIX headers.
        user = [f"apps/accounts/{name}.c" for name in SERVICE]
        user += ["apps/accounts/kernel_adapter.c", "apps/accounts/server.c", "apps/accounts/owner_interim.c",
                 "apps/accounts/storage_posix.c", "sdk/private/accounts_link.c",
                 "tests/host/security/fake.c", "apps/desktop/users.c"]
        for source in user:
            obj = out / (source.replace("/", "_") + ".o")
            subprocess.run(["clang", *WARN, "-D_POSIX_C_SOURCE=200809L", *SANITIZE, *include,
                            "-c", str(ROOT / source), "-o", str(obj)], check=True, env=env)
            objects.append(str(obj))
        program = out / "adapter"
        subprocess.run([
            "clang", *WARN, "-Wl,--gc-sections", *SANITIZE, *include,
            str(ROOT / "tests/host/security/adapter_test.c"),
            str(ROOT / "tests/host/proc/signal_legacy.c"),
            str(ROOT / "src/kernel/lib/sha256.c"),
            str(ROOT / "src/kernel/lib/tzrule.c"),
            str(ROOT / "src/kernel/lib/fmt.c"),
            str(ROOT / "src/kernel/probes/selector.c"),
            *objects, "-pthread", "-o", str(program),
        ], check=True, env=env)
        with tempfile.TemporaryDirectory(prefix="store-", dir=out) as store:
            storage, production = Path(store) / "storage", Path(store) / "production"
            storage.mkdir()
            production.mkdir()
            result = subprocess.run([str(program), str(storage), str(production)], capture_output=True,
                                    text=True, env=env, timeout=120)
        print(result.stdout, end="")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("sec-int-01 adapter integration: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
