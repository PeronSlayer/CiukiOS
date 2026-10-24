"""G32 account/profile model and filesystem adapter, ASan/UBSan including leaks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class UsersTests(unittest.TestCase):
    def test_model_and_persistence(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="users-", dir=parent) as tmp:
            binary = Path(tmp) / "users-test"
            store = Path(tmp) / "store"
            store.mkdir()
            sources = ["tests/host/desktop/users_test.c", "apps/desktop/users.c",
                       "apps/desktop/users_io.c", "apps/desktop/settings_io.c", "apps/desktop/settings.c",
                       "src/kernel/lib/sha256.c", "apps/desktop/theme.c"]
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-I", str(ROOT / "apps/desktop"),
                            "-I", str(ROOT / "src/kernel/include"),
                            *[str(ROOT / name) for name in sources], "-pthread", "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(store)], text=True, capture_output=True)
            print(result.stdout, end="")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
