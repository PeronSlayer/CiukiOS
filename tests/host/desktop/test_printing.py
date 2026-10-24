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

    def test_formatted_documents(self):
        """G28b: cp_submit_doc with PostScript, PCL and text-only printers."""
        out = ROOT / "build/host/printing-doc"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / "doc"
        sources = ["apps/libprint/print.c", "apps/libprint/render.c", "tests/host/desktop/printing_doc_test.c"]
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread",
                        "-I", str(ROOT / "src/kernel/include"), *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True)
        with tempfile.TemporaryDirectory(dir=out) as scratch:
            result = subprocess.run([str(binary), str(Path(scratch) / "spool")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS printing doc", result.stdout)

if __name__ == "__main__":
    unittest.main()
