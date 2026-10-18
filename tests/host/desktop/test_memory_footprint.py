"""F2-55/F2-60 production desktop and periodic-service ledger; serial builds."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from .test_desktop import ROOT, SHELL_SOURCES
import hashlib

# F2-81: intentional two-bar shell/theme changes; docs/design/desktop-shell.md.

# F2-82: boot shell frames include the larger orb and clipped recess.
BOOT_GOLDENS = {
    "absent-ro": "1650920b8ad2b4cf74af2ebde916682481ef6fae6aa4e75613315dbc002cfcd9",
    "absent-rw": "1650920b8ad2b4cf74af2ebde916682481ef6fae6aa4e75613315dbc002cfcd9",
    "identical-unpresented": "4e18a6ff002b75259ff4273fb9611f701c5deffa0fccff151ced69b66b1327df",
    "present-ro": "1650920b8ad2b4cf74af2ebde916682481ef6fae6aa4e75613315dbc002cfcd9",
    "present-ro-seeded": "1650920b8ad2b4cf74af2ebde916682481ef6fae6aa4e75613315dbc002cfcd9",
    "present-rw": "1650920b8ad2b4cf74af2ebde916682481ef6fae6aa4e75613315dbc002cfcd9",
    "present-rw-seeded": "1650920b8ad2b4cf74af2ebde916682481ef6fae6aa4e75613315dbc002cfcd9",
    "theme": "dfcd509aaa2b96c57c0a892aec0be1f0de0ce1d28c7006b353d03553cd37b2b1",
    "wallpaper-change": "4e18a6ff002b75259ff4273fb9611f701c5deffa0fccff151ced69b66b1327df"
}

class MemoryFootprintTests(unittest.TestCase):
    def test_sound_storage_prepared_before_worker(self):
        from .test_system_sounds import module
        module("footprint_sounds", ROOT / "scripts/build_sounds.py").build()
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="sound-footprint-", dir=parent) as temp:
            folder = Path(temp)
            inc = folder / "include/ciuki"
            inc.mkdir(parents=True)
            for name in ("audio.h", "registry.h", "devices.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / name, inc / name)
            host = ROOT / "tests/host/desktop"
            sources = [host / "sound_runtime_test.c", host / "footprint_alloc.c"]
            sources += [ROOT / "apps/desktop" / n for n in ("sounds.c", "protocol.c", "sound_storage.c", "master_storage.c")]
            sources += [ROOT / "sdk/libreg" / n for n in ("registry.c", "storage.c", "hardware.c")]
            common = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                      "-D_POSIX_C_SOURCE=200809L", "-DDESK_FOOTPRINT_TEST", "-Wno-macro-redefined",
                      "-include", str(host / "replay_base.h"), "-fsanitize=address,undefined",
                      "-fno-sanitize-recover=undefined", "-pthread", "-I", str(folder / "include"),
                      "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include")]
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            objects = []
            for i, source in enumerate(sources):
                target = folder / f"{i}.o"
                flags = [] if source.name == "footprint_alloc.c" else ["-include", str(host / "footprint_alloc.h")]
                subprocess.run([*common, *flags, "-c", str(source), "-o", str(target)], check=True, env=env)
                objects.append(str(target))
            binary = folder / "sound-footprint"
            subprocess.run(["clang", "-fsanitize=address,undefined", *objects, "-pthread", "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
            if "CIUKI_FOOTPRINT_AUDIT" not in env:
                self.assertIn("ALLOC_TOTAL phase=sound_after_init count=0", result.stdout)

    def test_no_allocation_after_first_frame(self):
        from . import icon_assets
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-footprint-", dir=parent) as temp:
            folder = Path(temp)
            inc = folder / "include/ciuki"
            inc.mkdir(parents=True)
            for name in ("channel.h", "surface.h", "spawn.h", "proc.h", "audio.h", "net.h", "media.h", "power.h", "display.h", "registry.h", "audio_preference.h", "devices.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / name, inc / name)
            for name in ("runtime.h", "raw.h"):
                (inc / name).write_text("/* supplied by host harness */\n")
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            common = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                      "-D_POSIX_C_SOURCE=200809L", "-DDESK_FOOTPRINT_TEST", "-Wno-macro-redefined",
                      "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                      "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                      "-ffunction-sections", "-fdata-sections", "-I", str(folder / "include"),
                      "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                      *icon_assets.defines()]
            sources = [ROOT / "apps/desktop" / n for n in ("client.c", "protocol.c", "input.c", "compositor.c", "theme.c")]
            sources += SHELL_SOURCES + [ROOT / "apps/desktop" / n for n in ("image.c", "wallpaper_io.c", "sounds.c", "sound_storage.c", "master_storage.c", "sound_runtime.c")]
            sources += [ROOT / name for name in icon_assets.SOURCES]
            sources += [ROOT / "sdk/libreg" / name for name in ("registry.c", "storage.c", "hardware.c")]
            host = ROOT / "tests/host/desktop"
            sources += [host / n for n in ("fake.c", "shell_launch_test.c", "footprint_alloc.c")]
            objects = []
            for i, source in enumerate(sources):
                target = folder / f"{i}.o"
                # The desktop.c included by the launcher is production code;
                # setup in memory_footprint_test.h explicitly undefines hooks.
                trace = source.is_relative_to(ROOT / "apps") or source.is_relative_to(ROOT / "sdk/libreg") or source.name == "shell_launch_test.c"
                flags = ["-include", str(host / "footprint_alloc.h")] if trace else []
                subprocess.run([*common, *flags, "-c", str(source), "-o", str(target)], check=True, env=env)
                objects.append(str(target))
            binary = folder / "footprint"
            subprocess.run(["clang", "-fsanitize=address,undefined", "-Wl,--gc-sections",
                            "-Wl,--wrap=desk_wallpaper_render,--wrap=open,--wrap=mkdir,--wrap=write,--wrap=fsync",
                            *objects, "-pthread", "-o", str(binary)], check=True, env=env)
            boot_review = ROOT / "build/host/f2-73"
            boot_review.mkdir(parents=True, exist_ok=True)
            boot = subprocess.run([str(binary), "--wallpaper-boot", str(folder)], capture_output=True, text=True,
                                  env={**env, "CIUKI_F273_FRAMES": str(boot_review)}, timeout=40)
            print(boot.stdout, end="", flush=True)
            self.assertEqual(boot.returncode, 0, boot.stdout + boot.stderr)
            from .test_desktop import capture
            with patch.dict(os.environ, CIUKI_FONT_REVIEW=str(boot_review)):
                self.assertEqual({f.stem for f in boot_review.glob("*.xrgb")}, set(BOOT_GOLDENS))
                for frame in boot_review.glob("*.xrgb"):
                    self.assertEqual(hashlib.sha256(frame.read_bytes()).hexdigest(), BOOT_GOLDENS[frame.stem], frame.stem)
                    capture(frame.stem, frame.read_bytes(), 1024, 768)
                    frame.unlink()
            review = ROOT / "build/host/f2-70"
            review.mkdir(parents=True, exist_ok=True)
            performance = subprocess.run([str(binary), "--theme-wallpaper-performance", str(folder)],
                                         capture_output=True, text=True,
                                         env={**env, "CIUKI_F270_FRAMES": str(review)}, timeout=25)
            print(performance.stdout, end="", flush=True)
            self.assertEqual(performance.returncode, 0, performance.stdout + performance.stderr)
            from .test_desktop import capture
            with patch.dict(os.environ, CIUKI_FONT_REVIEW=str(review)):
                for frame in review.glob("*.xrgb"):
                    capture(frame.stem, frame.read_bytes(), 1024, 768)
                    frame.unlink()
            sequence = subprocess.run([str(binary), "--theme-wallpaper", str(folder)], capture_output=True, text=True, env=env, timeout=25)
            self.assertEqual(sequence.returncode, 0, sequence.stdout + sequence.stderr)
            print(sequence.stdout, end="")
            result = subprocess.run([str(binary), "--memory-footprint", str(folder)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
            if "CIUKI_FOOTPRINT_AUDIT" not in env:
                self.assertIn("PASS desktop footprint: allocations_after_first_frame=0", result.stdout)
                for phase in ("survivor_after_first_frame", "crash_cycle", "periodic_services",
                              "all_slots_resize_recover", "menu_volume_media", "pool_failure_no_retry"):
                    self.assertIn(f"ALLOC_TOTAL phase={phase} count=0", result.stdout)


if __name__ == "__main__":
    unittest.main()
