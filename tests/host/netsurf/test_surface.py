"""Focused v3 fixtures, sanitizer leak checking enabled."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[3]

class SurfaceTests(unittest.TestCase):
    def test_registered_surface(self):
        out=ROOT / "build/host/netsurf";out.mkdir(parents=True,exist_ok=True)
        host=ROOT / "build/external/netsurf-host"
        common=["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra","-Werror",
                "-include",str(ROOT / "tests/host/desktop/replay_base.h"),"-fsanitize=address,undefined","-fno-sanitize-recover=undefined",
                "-I"+str(ROOT),"-I"+str(host / "native-headers"),"-I"+str(host / "prefix/include"),
                "-I"+str(host / "source/libnsfb/src")]
        names={"ciuki_surface_create":"fixture_create","ciuki_surface_info":"fixture_info","ciuki_surface_map":"fixture_map",
               "ciuki_channel_send":"fixture_send","ciuki_channel_recv":"fixture_recv","close":"fixture_close","munmap":"fixture_unmap"}
        source=host / "source/libnsfb/src/surface/ciuki.c"
        subprocess.run([*common,*["-D"+k+"="+v for k,v in names.items()],"-c",str(source),"-o",str(out / "nsfb-glue.o")],check=True)
        subprocess.run([*common,str(ROOT / "tests/host/netsurf/nsfb_test.c"),str(out / "nsfb-glue.o"),
             "-L"+str(host / "prefix/lib"),"-Wl,--whole-archive","-lnsfb","-Wl,--no-whole-archive","-ldeskui","-lm","-o",str(out / "nsfb")],check=True)
        subprocess.run([str(out / "nsfb")],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1"),timeout=30)

    def test_v3_surface(self):
        out=ROOT / "build/host/netsurf"
        out.mkdir(parents=True,exist_ok=True)
        headers=ROOT / "build/external/netsurf-host/prefix/include"
        if not (headers / "libnsfb.h").exists():
            # This transport-only test needs patched upstream types, no host browser
            # build, native libraries or target SDK.
            headers=ROOT / "build/external/netsurf-target/prefix/include"
        flags=["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra","-Werror",
               "-include",str(ROOT / "tests/host/desktop/replay_base.h"),"-fsanitize=address,undefined","-fno-sanitize-recover=undefined","-I"+str(ROOT),
               "-I"+str(ROOT / "src/kernel/include"),
               "-I"+str(headers)]
        sources=["sdk/netsurf/port/surface.c","apps/libdeskui/v3.c","apps/desktop/protocol.c",
                 "apps/libdeskui/keymap.c","tests/host/netsurf/surface_test.c"]
        subprocess.run([*flags,*[str(ROOT / s) for s in sources],"-o",str(out / "surface")],check=True)
        env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(out / "surface")],check=True,env=env,timeout=30)
