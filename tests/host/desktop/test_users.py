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
            result = subprocess.run([str(binary), str(store)], text=True, capture_output=True,timeout=30)
            print(result.stdout, end="")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_guest_lease_and_process_snapshot(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="users-guest-", dir=parent) as tmp:
            work=Path(tmp); (work/"include").mkdir()
            (work/"include/ciuki").symlink_to(ROOT/"sdk/sysroot-overlay/include/ciuki",target_is_directory=True)
            binary=work/"users-guest"; store=work/"store"; store.mkdir()
            flags=["-std=c17","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra","-Werror",
                   "-fsanitize=address,undefined","-fno-sanitize-recover=undefined",
                   "-ffunction-sections","-fdata-sections","-Wno-macro-redefined",
                   "-I"+str(work/"include"),"-I"+str(ROOT/"src/kernel/include"),
                   "-I"+str(ROOT/"apps/desktop"),"-include",str(ROOT/"tests/host/desktop/replay_base.h")]
            obj=work/"users_io.o"
            subprocess.run(["clang",*flags,"-DUSERS_NATIVE_LOCK=1","-c",str(ROOT/"apps/desktop/users_io.c"),"-o",str(obj)],check=True)
            sources=["tests/host/desktop/users_guest_lock_test.c","apps/desktop/users.c","apps/desktop/settings_io.c","src/kernel/lib/sha256.c"]
            subprocess.run(["clang",*flags,*[str(ROOT/s) for s in sources],str(obj),"-Wl,--gc-sections","-Wl,--wrap=rename","-Wl,--wrap=cfg_write_at","-Wl,--wrap=getpid","-pthread","-o",str(binary)],check=True)
            result=subprocess.run([str(binary),str(store)],text=True,capture_output=True,timeout=30)
            print(result.stdout,end="")
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)


if __name__ == "__main__":
    unittest.main()
