"""Focused v3 fixtures, sanitizer leak checking enabled."""
import os
from pathlib import Path
import subprocess
import shutil
import unittest

ROOT=Path(__file__).resolve().parents[3]
# Reuse an existing prepared source/prefix read-only; tests never build the browser.
SOURCE=Path(os.environ.get("CIUKI_NETSURF_SOURCE",ROOT / "build/external/netsurf-host/source"))

class SurfaceTests(unittest.TestCase):
    def test_chrome_cursor(self):
        out=ROOT / "build/host/netsurf";out.mkdir(parents=True,exist_ok=True)
        prefix=Path(os.environ.get("CIUKI_NETSURF_PREFIX",ROOT / "build/external/netsurf-host/prefix"))
        source=SOURCE / "netsurf"
        flags=["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-DCIUKI_HOST","-DCIUKI_NETSURF","-Dnsframebuffer",
               "-O1","-g","-ffunction-sections","-fdata-sections","-fsanitize=address,undefined",
               "-fno-sanitize-recover=undefined","-Wl,--gc-sections","-I"+str(ROOT),"-I"+str(ROOT / "src/kernel/include"),
               "-I"+str(prefix / "include"),"-I"+str(source),"-I"+str(source / "include"),
               "-I"+str(source / "frontends"),"-I"+str(source / "frontends/framebuffer/ciuki"),
               "-include",str(ROOT / "tests/host/desktop/replay_base.h")]
        subprocess.run([*flags,str(ROOT / "tests/host/netsurf/chrome_cursor_test.c"),
                        str(ROOT / "apps/libdeskui/widgets.c"),str(ROOT / "apps/libdeskui/draw.c"),
                        str(ROOT / "apps/desktop/theme.c"),
                        "-o",str(out / "chrome-cursor")],check=True)
        subprocess.run([str(out / "chrome-cursor")],check=True,
                       env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1"),timeout=30)

    def test_registered_surface(self):
        out=ROOT / "build/host/netsurf";out.mkdir(parents=True,exist_ok=True)
        host=ROOT / "build/external/netsurf-host"
        overlay=out / "surface-include";overlay.mkdir(exist_ok=True)
        if not (overlay / "ciuki_port").exists():
            (overlay / "ciuki_port").symlink_to(ROOT / "sdk/netsurf/port",target_is_directory=True)
        common=["clang","-std=c17","-D_POSIX_C_SOURCE=200809L","-O1","-g","-Wall","-Wextra","-Werror",
                "-include",str(ROOT / "tests/host/desktop/replay_base.h"),"-fsanitize=address,undefined","-fno-sanitize-recover=undefined",
                "-I"+str(overlay),"-I"+str(ROOT),"-I"+str(host / "native-headers"),"-I"+str(host / "prefix/include"),
                "-I"+str(host / "source/libnsfb/src"),"-I"+str(host / "source/libnsfb/src/surface")]
        common += ["-I"+str(host / "source/netsurf/include"),"-I"+str(host / "source/netsurf")]
        names={"ciuki_surface_create":"fixture_create","ciuki_surface_map":"fixture_map",
               "ciuki_channel_send":"fixture_send","ciuki_channel_recv":"fixture_recv","close":"fixture_close","munmap":"fixture_unmap"}
        # Production installs this file beside libnsfb's surface.h; compile
        # a staged copy so the port's own surface.h cannot shadow it.
        source=out / "nsfb-surface.c"
        source.write_text((ROOT / "sdk/netsurf/port/nsfb_surface.c").read_text()
                          .replace("#include <ciuki/surface.h>", "#include <ciuki/surface.h>\nint fixture_info(int, struct ciuki_surface_info *);")
                          .replace("ciuki_surface_info,close", "fixture_info,close"))
        subprocess.run([*common,*["-D"+k+"="+v for k,v in names.items()],"-c",str(source),"-o",str(out / "nsfb-glue.o")],check=True)
        # The cached archive supplies upstream libnsfb only. Compile the port
        # from this source state, including its private record layout.
        archive=out / "libnsfb-fixture.a"
        shutil.copyfile(host / "prefix/lib/libnsfb.a",archive)
        subprocess.run(["llvm-ar","d",str(archive),"src_surface_ciuki_port_surface.o"],check=True)
        subprocess.run([*common,"-c",str(ROOT / "sdk/netsurf/port/surface.c"),"-o",str(out / "nsfb-port.o")],check=True)
        subprocess.run([*common,str(ROOT / "tests/host/netsurf/nsfb_test.c"),str(out / "nsfb-glue.o"),str(out / "nsfb-port.o"),
             "-L"+str(host / "prefix/lib"),"-Wl,--whole-archive",str(archive),"-Wl,--no-whole-archive","-ldeskui","-lm","-o",str(out / "nsfb")],check=True)
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
               "-I"+str(headers),
               "-I"+str(SOURCE / "libnsfb/include"),"-I"+str(SOURCE / "netsurf/include")]
        sources=["sdk/netsurf/port/surface.c","apps/libdeskui/v3.c","apps/desktop/protocol.c",
                 "apps/libdeskui/keymap.c","tests/host/netsurf/surface_test.c"]
        subprocess.run([*flags,*[str(ROOT / s) for s in sources],"-o",str(out / "surface")],check=True)
        env=dict(os.environ,ASAN_OPTIONS="detect_leaks=1",UBSAN_OPTIONS="halt_on_error=1")
        subprocess.run([str(out / "surface")],check=True,env=env,timeout=30)
