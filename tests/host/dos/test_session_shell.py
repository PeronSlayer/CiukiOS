"""Decision 2026-10-24: EXIT at the DOS Box prompt ends the session (Windows 98
MS-DOS Prompt, "Close on exit"). The primary FreeCOM started with /P ignores
EXIT, so the unified guest boots src/com/dossh.asm (CIUKISH.COM) through SHELL=;
it runs COMMAND.COM non-permanently (/K FDAUTO.BAT) and, when that returns,
writes the VM control port. Host-only: no build/ links, QEMU or image."""
import importlib.util, re, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("unified_dos_s", ROOT / "scripts/unified_dos.py")
unified = importlib.util.module_from_spec(spec); sys.modules["unified_dos_s"] = unified; spec.loader.exec_module(unified)


class SessionShellTests(unittest.TestCase):
    def config(self):
        return unified.fork_config(unified.reference_module())

    def test_config_boots_the_wrapper_not_a_permanent_shell(self):
        config = self.config()
        shells = re.findall(rb"^SHELL=(.*)\r$", config, re.M)
        self.assertEqual(shells, [b"C:\\CIUKISH.COM"])
        self.assertNotIn(b"/P", config)                      # no permanent primary FreeCOM
        self.assertIn(b"DEVICE=C:\\CIUKIDF.SYS\r\nSHELL=", config)
        # the reference profile (conformance battery) keeps its permanent shell and autoexec
        self.assertIn(b"/P=C:\\FDAUTO.BAT", unified.reference_module().CONFIG)

    def test_wrapper_runs_command_non_permanent_and_stops_the_vm(self):
        with tempfile.TemporaryDirectory() as t:
            out = Path(t) / "S.COM"
            subprocess.run(["nasm", "-f", "bin", "-l", str(Path(t) / "S.lst"), str(ROOT / "src/com/dossh.asm"), "-o", str(out)], check=True)
            image = out.read_bytes()
        self.assertEqual(image, unified.session_shell())
        self.assertLess(len(image), 512)
        self.assertIn(b"\\COMMAND.COM\0", image)
        self.assertIn(b" C:\\ /E:1024 /K C:\\FDAUTO.BAT\r", image)
        self.assertNotIn(b"/P", image)
        # EXEC (INT 21h AX=4B00h), then OUT 0EAh,AL(0), then terminate
        self.assertIn(bytes.fromhex("b8004bcd21"), image)
        self.assertIn(bytes.fromhex("baea0030c0ee"), image)
        tail_len = image[image.index(b" C:\\ /E:1024"):].index(b"\r")
        self.assertEqual(image[image.index(b" C:\\ /E:1024") - 1], tail_len)

    def test_session_shell_is_installed_and_read_back(self):
        source = (ROOT / "scripts/unified_dos.py").read_text()
        self.assertIn('files["/CIUKISH.COM"] = session_shell()', source)
        self.assertIn('read("/CIUKISH.COM") != session_shell()', source)


if __name__ == "__main__":
    unittest.main()
