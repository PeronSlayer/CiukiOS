"""Small sequential host suite for the production Savage and desktop presenters."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
POINTER_GOLDENS = ROOT / "tests/host/desktop/pointer-goldens.json"
CURSOR_SET_GOLDENS = ROOT / "tests/host/desktop/cursor-set-goldens.json"

class AccelerationTests(unittest.TestCase):
    def test_cursor_svg_reproduction(self):
        subprocess.run(["python3", str(ROOT / "scripts/cursors/build_ciuki.py"), "--check"], check=True)

    def test_cursor_set_goldens(self):
        sources = [ROOT / "apps/desktop" / name for name in
                   ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c",
                    "autoplay_content.c", "settings.c", "power_policy.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
        sources += [*(ROOT / "apps/libdeskui" / n for n in ("draw.c", "widgets.c", "list.c", "menu.c", "dialog.c", "controls.c", "commdlg.c", "textarea.c", "icons.c", "icon_map.c", "iconx.c", "fileicon.c")),
                    ROOT / "tests/host/desktop/fake.c", ROOT / "tests/host/desktop/cursor_set_test.c"]
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="cursor-set-", dir=parent) as temp:
            folder = Path(temp)
            binary = folder / "cursor_set"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                            "-include", str(ROOT / "tests/host/desktop/paint_probe.h"),
                            "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sources), "-o", str(binary)], check=True)
            # Same sanitizer policy as the existing presenter tests below.
            result = subprocess.run([str(binary), str(folder)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
            actual = {p.stem: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.glob("*.ppm"))}
            self.assertEqual(len(actual), 17)
            if os.environ.get("CIUKI_CURSOR_FRAMES"):
                from PIL import Image
                dest = Path(os.environ["CIUKI_CURSOR_FRAMES"])
                dest.mkdir(parents=True, exist_ok=True)
                for frame in folder.glob("*.ppm"):
                    Image.open(frame).save(dest / (frame.stem + ".png"))
            if os.environ.get("CIUKI_RECORD_CURSORS"):
                CURSOR_SET_GOLDENS.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n")
            self.assertEqual(actual, json.loads(CURSOR_SET_GOLDENS.read_text()))

    def test_registers(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        flags = ["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
        subprocess.run([*flags, "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/savage2d_test.c"),
                        str(ROOT / "src/kernel/lib/fmt.c"),
                        "-o", str(out / "savage2d_test")], check=True)
        subprocess.run([str(out / "savage2d_test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_visible_frames(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        sources = [ROOT / "apps/desktop" / name for name in
                   ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c", "autoplay_content.c", "settings.c", "power_policy.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
        sources += [*(ROOT / "apps/libdeskui" / n for n in ("draw.c","widgets.c","list.c","menu.c","dialog.c", "controls.c", "commdlg.c","textarea.c","icons.c","icon_map.c","iconx.c","fileicon.c")), ROOT / "tests/host/desktop/fake.c", ROOT / "tests/host/desktop/accel_test.c"]
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                        "-include", str(ROOT / "tests/host/desktop/paint_probe.h"),
                        "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                        *map(str, sources), "-o", str(out / "desktop_accel_test")], check=True)
        subprocess.run([str(out / "desktop_accel_test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_pointer_goldens(self):
        # f2-96: hardware and software pointer never both visible; golden
        # frames of each state and switch. CIUKI_RECORD_POINTER=1 rewrites
        # the goldens, CIUKI_POINTER_FRAMES=<dir> keeps PNG copies.
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        sources = [ROOT / "apps/desktop" / name for name in
                   ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c", "autoplay_content.c", "settings.c", "power_policy.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
        sources += [*(ROOT / "apps/libdeskui" / n for n in ("draw.c","widgets.c","list.c","menu.c","dialog.c", "controls.c", "commdlg.c","textarea.c","icons.c","icon_map.c","iconx.c","fileicon.c")), ROOT / "tests/host/desktop/fake.c", ROOT / "tests/host/desktop/pointer_test.c"]
        with tempfile.TemporaryDirectory(prefix="pointer-", dir=out) as directory:
            folder = Path(directory)
            binary = folder / "pointer_test"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                            "-include", str(ROOT / "tests/host/desktop/paint_probe.h"),
                            "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sources), "-o", str(binary)], check=True)
            frames = folder / "frames"
            frames.mkdir()
            result = subprocess.run([str(binary), str(frames)], env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                                    text=True, capture_output=True)
            print(result.stdout, end="")
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            actual = {}
            for frame in sorted(frames.glob("*.ppm")):
                raw = frame.read_bytes()
                actual[frame.stem] = hashlib.sha256(raw).hexdigest()
                if os.environ.get("CIUKI_POINTER_FRAMES"):
                    from PIL import Image
                    import io
                    dest = Path(os.environ["CIUKI_POINTER_FRAMES"])
                    dest.mkdir(parents=True, exist_ok=True)
                    Image.open(io.BytesIO(raw)).save(dest / (frame.stem + ".png"))
        self.assertEqual(len(actual), 20)
        if os.environ.get("CIUKI_RECORD_POINTER"):
            POINTER_GOLDENS.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n")
        self.assertEqual(actual, json.loads(POINTER_GOLDENS.read_text()))

if __name__ == "__main__":
    unittest.main()
