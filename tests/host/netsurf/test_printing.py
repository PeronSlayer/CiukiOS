"""G31 browser printing boundary; reuses prepared upstream headers read-only."""
from pathlib import Path
import os
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[3]

class BrowserPrinting(unittest.TestCase):
    def test_shortcuts_and_viewport(self):
        base=ROOT / "build/external/netsurf-host"
        source=Path(os.environ.get("CIUKI_NETSURF_SOURCE",base / "source")) / "netsurf"
        prefix=Path(os.environ.get("CIUKI_NETSURF_PREFIX",base / "prefix"))
        if not (source / "include/netsurf/browser_window.h").is_file():
            self.skipTest("prepared NetSurf source headers are unavailable")
        out=ROOT / "build/host/netsurf-printing";out.mkdir(parents=True,exist_ok=True)
        binary=out / "printing"
        subprocess.run(["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-DCIUKI_HOST","-DCIUKI_NETSURF","-Dnsframebuffer",
                        "-O1","-g","-Wall","-Wextra","-Werror","-ffunction-sections","-fdata-sections",
                        "-fsanitize=address,undefined","-fno-sanitize-recover=undefined","-Wl,--gc-sections",
                        "-I"+str(ROOT),"-I"+str(ROOT / "src/kernel/include"),"-I"+str(prefix / "include"),
                        "-I"+str(source),"-I"+str(source / "include"),"-I"+str(source / "frontends"),
                        "-I"+str(source / "frontends/framebuffer/ciuki"),
                        "-include",str(ROOT / "tests/host/desktop/replay_base.h"),
                        str(ROOT / "tests/host/netsurf/printing_test.c"),
                        str(ROOT / "sdk/netsurf/port/download_store.c"),
                        str(ROOT / "apps/libdeskui/widgets.c"),str(ROOT / "apps/libdeskui/draw.c"),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__=="__main__":unittest.main()
