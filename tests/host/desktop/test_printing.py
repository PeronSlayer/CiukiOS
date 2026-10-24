"""G31 native modal controller with the production spool and renderers."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class PrintingDialogs(unittest.TestCase):
    def test_controller(self):
        out = ROOT / "build/host/printing-dialog"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / "dialog"
        sources = ["apps/libprint/dialog.c", "apps/libprint/print.c", "apps/libprint/render.c",
                   "tests/host/desktop/printing_dialog_test.c"]
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread",
                        "-I", str(ROOT / "src/kernel/include"),
                        *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True)
        with tempfile.TemporaryDirectory(dir=out) as scratch:
            subprocess.run([str(binary), str(Path(scratch) / "spool")], check=True)

if __name__ == "__main__":
    unittest.main()
