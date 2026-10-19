"""Native production desktop tests with fake surfaces/channels; never boots QEMU."""
# SPDX-License-Identifier: MIT
import concurrent.futures
import hashlib
import json
import importlib.util
import os
import re
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture
from unittest.mock import patch
import zlib

sys.dont_write_bytecode = True
# F2-81: intentional two-bar shell/theme changes; docs/design/desktop-shell.md.

ROOT = Path(__file__).resolve().parents[3]
# The desktop shell's units (apps/desktop/build_desktop.py SHELL, without the stb decoder) and the toolkit
# primitives it draws with: every host build of the production desktop links them.
SHELL_SOURCES = [ROOT / "apps/desktop" / n for n in ("shell.c", "autoplay.c", "autoplay_content.c", "settings.c", "settings_io.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")] + \
                [ROOT / "apps/libdeskui" / n for n in ("draw.c","widgets.c","list.c","menu.c","dialog.c","textarea.c")]

# F2-62: production shell, menus and modal frames; lead reviews the paired PNGs.
# F2-63: re-recorded for the pinned FreeType UI atlases (build/host/f2-63 before/after).
# 2026-10-15 (main after f2-63 + f2-64): the three dialogs frames re-recorded on the combined tree (f2-64 message boxes in FreeType), reviewed against the 9a05ab9f renders.
# F2-67: recorded caption controls and owner-centred shell messages; paired PNGs await lead review.
# F2-67 + F2-68: caption changes and selection rendering combined; shared frames re-recorded.
# F4-30 (2026-10-18): reviewed Places in the shared Open/Save dialog;
# only the dialog frames change.
# 2026-10-18 (main after f4-30 + f2-81): the dialogs frames re-recorded on the combined tree (Places pane in the two-bar shell), reviewed by the lead.
# F2-82: intentional orb geometry, well and sphere changes in shell frames.
# F2-83: shaded bars, coverage rims and task buttons; desktop-shell.md.
LOOK_GOLDENS = {
    "ciuki-blue-desktop.ppm": "7be82fba7a1fbc2c76749e2d5b82e2c0f0446f66804d39f55f9a74f5ed15f236",
    "ciuki-blue-dialogs.ppm": "06611ac97f8a704e637f76f18b35fd2602ef84c3030e6bf85861e070e5c10f4a",
    "ciuki-blue-tray-disabled.ppm": "ff074a348a95ca8ba515e85f6d83440cea13e5a7d70f15235f127a43884b2e09",
    "ciuki-blue-tray-media.ppm": "9f80d80f02a8d3e0b966957100a6c857056fd2db75508e884d8a5ef5d4dcf00e",
    "platinum-desktop.ppm": "a6618af64babd303c822f2c249a2fff00e0a80b4d674383a543d576a4842884b",
    "platinum-dialogs.ppm": "92da071be98f2a25bdc3c8a6f38d41acd9c53d3db283b6a1ed0ecda3d3629f35",
    "platinum-tray-disabled.ppm": "0de6d2ccfbbbbb9f707f070ff4f975809ac6116e52db8be2bfd67bea59415618",
    "platinum-tray-media.ppm": "3382fcae2977be32494b2f8330d54178cb2b82b73e8356f24f57fb3b92be42f6"
}

# Public set-1 codes the kernel reports for the QMP qcodes of the desktop cases (E0 keys | 0x100).
QCODES = {**{c: s for c, s in zip("qwertyuiop", range(0x10, 0x1a))}, **{c: s for c, s in zip("asdfghjkl", range(0x1e, 0x27))},
          **{c: s for c, s in zip("zxcvbnm", range(0x2c, 0x33))}, **{c: s for c, s in zip("1234567890", range(0x02, 0x0c))},
          "spc": 0x39, "ret": 0x1c, "esc": 0x01, "tab": 0x0f, "backspace": 0x0e, "shift": 0x2a, "ctrl": 0x1d, "alt": 0x38,
          "dot": 0x34, "slash": 0x35, "comma": 0x33, "minus": 0x0c, "f1": 0x3b, "f2": 0x3c, "f4": 0x3e, "f5": 0x3f,
          "f9": 0x43, "f10": 0x44, "f11": 0x57, "f12": 0x58, "up": 0x148, "down": 0x150, "left": 0x14b, "right": 0x14d,
          "home": 0x147, "end": 0x14f, "delete": 0x153}
# Pacing and guest-speed models of tests/host/desktop/app_replay.c.
REPLAY_MODELS = {
    # The declared pauses; a launched program's first turn 0.8 s after its spawn.
    "declared": ["--start-delay-ms", "800"],
    # The first QEMU run of files-qemu-t23 (2026-10-11): 0.19 s per 60 ms batch and 0.33 s per 100 ms batch
    # (runner loop plus QMP under TCG); programs ready 2.5 s after their spawn; slower desktop and program turns.
    "measured": ["--overhead-ms", "90", "--pause-scale-pct", "240", "--start-delay-ms", "2500",
                 "--desktop-cost-ms", "60", "--app-cost-ms", "20"],
}
# Programs ready only 8 s after their spawn, longer than every script's wait after a launch, at the runner
# pace of images 639bcca5/2fdd515a (about 110 ms per 100 ms batch): on 2fdd515a files-qemu-t23 clicked
# into a Files window that was not ready (keys=0, survivor_exit:0). The desktop holds the scripted input
# until the launched program's first frame (desktop.c scenario_hold) and replays it with its spacing.
REPLAY_MODELS["late-start"] = ["--overhead-ms", "10", "--start-delay-ms", "8000", "--desktop-cost-ms", "60",
                               "--app-cost-ms", "20"]
# The F12 shell case also runs with a slow desktop turn (300 ms per loop): its final stage (the
# demo's reap) then lands after the last input event, as on QEMU image d964369f (keys=0 there).
SHELL_MODELS = {**REPLAY_MODELS, "slow-desktop": ["--start-delay-ms", "800", "--desktop-cost-ms", "300"],
                # f2-65, image eb774bb9: the desktop repainted the idle screen on every loop at interactive
                # priority and its normal-priority clients got under 1 % of the CPU. The Start-menu demo then
                # answered its close request only after DESK_CLOSE_NS, the desktop terminated it (status 15),
                # the case stayed at stage 5 and reported keys=0 motion=47 buttons=12. One second per program
                # turn reproduces that failure without the demo's per-turn queue drain.
                "slow-client": ["--start-delay-ms", "800", "--app-cost-ms", "1000"],
                # A desktop loop of 1 ms: a settled interaction stage must not present frames on every loop.
                "fast-desktop": ["--start-delay-ms", "800", "--desktop-cost-ms", "1"]}
# The probe's interaction window (src/kernel/probes/f2_probes_desktop.c, interaction stage): 60 s, 180 s
# once an app case (files=1/note=1) reports. The Task Manager scenario sends no app flag, so it keeps 60 s.
TASKS_WINDOW_S = 60
APP_WINDOW_S = 180


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def xrgb_to_rgb(data):
    rgb = bytearray(len(data) // 4 * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = data[2::4], data[1::4], data[0::4]
    return bytes(rgb)


def write_png(path, width, height, rgb):
    """An 8-bit RGB PNG of review renders (CIUKI_WALL_FRAMES); never evidence."""
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def control_frame(test, name, pixels, width, height):
    """Pinned production applet frames; PNGs are review copies."""
    path=ROOT / "tests/host/desktop/control-goldens.json"
    expected=json.loads(path.read_text()) if path.exists() else {}
    digest=hashlib.sha256(pixels).hexdigest()
    if os.environ.get("CIUKI_RECORD_CONTROL_GOLDENS") == "1":
        expected[name]=digest
        path.write_text(json.dumps(expected,indent=2,sort_keys=True)+"\n")
    else:
        test.assertEqual(digest,expected.get(name),name)
    capture(name,pixels,width,height)


class DesktopTests(unittest.TestCase):
    def test_shell_suite_uses_existing_runner(self):
        path = str(ROOT / "scripts/test")
        sys.path.insert(0, path)
        try:
            runner = load("desktop_shell_runner_test", ROOT / "scripts/test/run.py")
            suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
            shell = [c for c in suite["cases"] if c["id"].startswith("desktop-shell-")]
            self.assertEqual({c["profile"] for c in shell}, {"qemu-t23", "qemu-e500"})
            self.assertEqual(len(shell), 2)
            for case in shell:
                self.assertTrue(case["desktop_screen"])
                runner.Actions(case["actions"])
                batches = case["actions"][0]["batches"]
                keys = [e["data"] for b in batches for e in b["events"] if e["type"] == "key"]
                self.assertEqual([(k["key"]["data"], k["down"]) for k in keys],
                                 [("f12", True), ("f12", False), ("a", True), ("a", False)])
                predicate = next(p for p in case["expected"]["predicates"] if p.get("where", {}).get("stage") == "after")
                self.assertEqual(predicate["fields"]["keys"], {"eq": 4})
        finally:
            sys.path.remove(path)

    def test_plain_screen_digest_and_shell_portrait(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-screen-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "capture"
            sources = [ROOT / "apps/desktop" / n for n in ("client.c", "protocol.c", "input.c", "compositor.c", "theme.c")] + SHELL_SOURCES
            sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "compositor_capture.c")]
            sources += [ROOT / "apps/libdeskui" / n for n in ("icons.c", "icon_map.c")]
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-O1", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *(str(p) for p in sources), "-o", str(binary)], check=True, env=env)
            c = load("capture_portrait", ROOT / "apps/desktop/convert_portrait.py")
            portrait = c.convert(ROOT / "assets/brand/ciuki-logo.png")
            source = folder / "portrait.xrgb"
            source.write_bytes(portrait)
            for mode in ("plain", "shell", "about"):
                target = folder / (mode + ".xrgb")
                subprocess.run([str(binary), str(source), str(target), mode], check=True, env=env)
                pixels = target.read_bytes()
                self.assertEqual(len(pixels), 640*480*4)
                capture("desktop-" + mode, pixels, 640, 480)
                if mode == "plain":
                    # F2-62: the missing-settings desktop now uses Ciuki Blue.
                    # F2-63: re-recorded for the pinned FreeType UI atlases.
                    pinned = "ab5eef0dfb084cd58fd8a272f1bffe5cff4ad296fc6a2679715c7b9d137e5a0e"
                    actual = hashlib.sha256(pixels).hexdigest()
                    self.assertEqual(actual, pinned)
                else:
                    # The Ciuki button at the left end of the top bar (desktop.h DESK_START_*) shows the
                    # approved portrait area-averaged to 24 px (shell.c desk_shell_icon "ciuki"), centred
                    # in the orb and pressed in by 1 px while the menu is open
                    # ("shell" mode). Every cell the portrait covers fully is the exact average of its
                    # source block.
                    geometry=json.loads((ROOT / "apps/desktop/start-orb.json").read_text())
                    size=geometry["portrait"]["size"]
                    shift=geometry["portrait"]["pressed_shift"] if mode == "shell" else 0
                    x0=geometry["orb_box"][0]+geometry["portrait"]["offset"][0]+shift
                    y0=geometry["orb_box"][1]+geometry["portrait"]["offset"][1]+shift
                    full = 0
                    for y in range(size):
                        for x in range(size):
                            block = [portrait[(yy*256+xx)*4:(yy*256+xx)*4+3]
                                     for yy in range(y*256//size, (y+1)*256//size) for xx in range(x*256//size, (x+1)*256//size)]
                            ink = [b for b in block if b != bytes((0x64, 0x55, 0x37))]
                            if len(ink) != len(block):
                                continue
                            full += 1
                            expected = bytes(sum(b[k] for b in ink) // len(ink) for k in range(3))
                            offset = ((y0+y)*640+x0+x)*4
                            self.assertEqual(pixels[offset:offset+3], expected, (mode, x, y))
                    self.assertGreater(full, 100)

    def test_look_reference_frames(self):
        """ui-look: every theme renders the reference desktop and dialogs frames, and the themes differ."""
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-look-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            flags = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                     "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "src/kernel/include")]
            plain = [ROOT / "tests/host/desktop/fake.c", *(ROOT / "apps/desktop" / f"{n}.c" for n in
                                                           ("client", "protocol", "input", "compositor", "theme",
                                                            "shell", "autoplay", "autoplay_content", "settings", "wallpaper", "keymap", "image", "folder"))]
            plain += [ROOT / "apps/files" / n for n in ("model.c","transfer.c")]
            posix = [ROOT / "tests/host/desktop/look_capture.c", *(ROOT / "apps/libdeskui" / f"{n}.c" for n in
                                                                   ("draw", "widgets", "list", "menu", "dialog", "textarea",
                                                                    "icons", "icon_map"))]
            icons = load("look_icon_assets", ROOT / "tests/host/desktop/icon_assets.py").defines()
            objects = []
            for k, source in enumerate([*plain, *posix]):
                target = folder / f"{k}-{source.stem}.o"
                subprocess.run([*flags, *(["-D_POSIX_C_SOURCE=200809L", *icons] if source in posix else []), "-c", str(source),
                                "-o", str(target)], check=True, env=env)
                objects.append(str(target))
            binary = folder / "look"
            subprocess.run(["clang", "-fsanitize=address,undefined", *objects, "-o", str(binary)], check=True, env=env)
            c = load("look_portrait", ROOT / "apps/desktop/convert_portrait.py")
            (folder / "portrait.xrgb").write_bytes(c.convert(ROOT / "assets/brand/ciuki-logo.png"))
            frames = folder / "frames"; frames.mkdir()
            subprocess.run([str(binary), str(folder / "portrait.xrgb"), str(frames)], check=True, env=env, capture_output=True)
            names = sorted(p.name for p in frames.glob("*.ppm"))
            self.assertEqual(names, sorted(f"{t}-{w}.ppm" for t in ("platinum", "ciuki-blue")
                                           for w in ("desktop", "dialogs", "tray-disabled", "tray-media")))
            digests = {n: hashlib.sha256((frames / n).read_bytes()).hexdigest() for n in names}
            for n in names:
                capture("look-" + Path(n).stem, (frames / n).read_bytes())
            self.assertEqual(len(set(digests.values())), 8)
            # Desktop icon labels over the default wallpaper, the owner's Ciuk2 scene (owner feedback 2026-10-12:
            # labels must stay readable on any wallpaper): look_capture's wallpaper mode renders the six desktop
            # icons with Documents selected, and the same desktop with the icons off.
            wall = folder / "wall"; wall.mkdir()
            subprocess.run([str(binary), str(folder / "portrait.xrgb"), str(wall), str(ROOT / "assets/wallpapers/ciuki/Ciuk2.png")],
                           check=True, env=env, capture_output=True)
            for p in wall.glob("*.ppm"):
                capture("look-" + p.stem, p.read_bytes())
            for theme in ("platinum", "ciuki-blue"):
                with self.subTest(theme=theme):
                    self.check_icon_labels(wall / f"{theme}-wallpaper.ppm", wall / f"{theme}-wallpaper-plain.ppm")
            target = os.environ.get("CIUKI_LOOK_FRAMES")
            if target:
                Path(target).mkdir(parents=True, exist_ok=True)
                for n in [*names, *(p.name for p in wall.glob("*.ppm"))]:
                    shutil.copy2(frames / n if (frames / n).exists() else wall / n, Path(target) / n)
            print("LOOK_FRAMES", json.dumps(digests, sort_keys=True))
            self.assertEqual(digests, LOOK_GOLDENS)

    def check_icon_labels(self, icons_ppm, plain_ppm):
        """shell.c draw_icons: 48 px icons in 96 x 94 cells from (8, 42) down the left edge; each label block (the
        cell's last 40 rows) holds white Inter 13 text over a soft dark halo blended into the wallpaper; the selected
        label (Documents) sits in a translucent rounded box; nothing is drawn outside the cells."""
        def pixels(path):
            data = path.read_bytes()
            magic, w, h, depth, raster = data.split(maxsplit=4)
            self.assertEqual((magic, int(w), int(h), depth), (b"P6", 1024, 768, b"255"))
            return raster
        icons, plain = pixels(icons_ppm), pixels(plain_ppm)
        row = 1024 * 3
        x0, y0, cw, ch, label_top, cells = 8, 42, 96, 94, 54, 3
        for y in range(768):
            a, b = icons[y * row:(y + 1) * row], plain[y * row:(y + 1) * row]
            if y0 <= y < y0 + cells * ch:
                self.assertEqual((a[:x0 * 3], a[(x0 + cw) * 3:]), (b[:x0 * 3], b[(x0 + cw) * 3:]), y)
            else:
                self.assertEqual(a, b, y)
        luma = lambda p: 299 * p[0] + 587 * p[1] + 114 * p[2]
        for k in range(cells):
            white = darker = lighter = 0
            levels, colours = set(), set()
            for y in range(y0 + k * ch + label_top, y0 + (k + 1) * ch):
                for x in range(x0, x0 + cw):
                    at = y * row + x * 3
                    p, q = icons[at:at + 3], plain[at:at + 3]
                    colours.add(p)
                    if p != q and min(p) >= 230:                # white glyph cores
                        white += 1
                    elif luma(p) < luma(q):
                        darker += 1
                        levels.add((luma(q) - luma(p)) // 2000)
                    elif p != q:
                        lighter += 1                                # anti-aliased glyph edges (and the selection box)
            self.assertGreater(white, 15, k)                        # the text
            self.assertGreater(darker, 300, k)                      # the halo around it
            self.assertGreater(len(levels), 12, k)                  # soft (many levels), not a hard outline
            if k != 1:
                self.assertGreater(darker, 2 * lighter, k)          # a halo darkens; only the glyph edges lighten
            else:
                self.assertGreater(len(colours), 200)               # translucent box: the wallpaper shows through

    def test_wallpaper_fill(self):
        """wallpaper_mode=fill: the uniform cover scale and centred crop against the stretch it crops (4:3, wide,
        tall, up and down), the desktop.cfg mode names and values, the default (Ciuk2, fill) and its round trip,
        and the owner's Ciuk2 scene decoded as the desktop decodes it (wallpaper_test.c)."""
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-wallpaper-", dir=parent) as temp:
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = Path(temp) / "wallpaper-test"
            sources = [ROOT / "tests/host/desktop/wallpaper_test.c",
                       *(ROOT / "apps/desktop" / f"{n}.c" for n in ("settings", "wallpaper", "theme", "image"))]
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=undefined", "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *map(str, sources), "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), str(ROOT / "assets/wallpapers/ciuki/Ciuk2.png")], check=True, env=env,
                                    capture_output=True, text=True)
        # 1448x1086 is 4:3: in the 1024x704 work area of 1024x768 it scales by 1024/1448 to 1024x768 and loses
        # 32 rows (45 source rows) at the top and at the bottom, nothing at the sides.
        self.assertIn("fill Ciuk2: 1448x1086 into 1024x704: scaled 1024x768, crop 0 columns and 32 rows per side", result.stdout)
        self.assertIn("PASS wallpaper fill", result.stdout)
        print(result.stdout.strip())

    def test_default_desktop_wallpaper(self):
        """The default desktop at 1024x768 as the production shell draws it: the desktop.cfg defaults
        (/system/wallpapers/Ciuk2.png, fill) with the owner's scene decoded. The scene reaches both sides of the
        work area (no desktop-colour bands) and run.py's normal-boot screen heuristic passes on the frame.
        CIUKI_WALL_FRAMES=<dir> keeps default-desktop.png for review."""
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-default-wall-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "capture"
            sources = [ROOT / "apps/desktop" / n for n in ("client.c", "protocol.c", "input.c", "compositor.c", "theme.c", "image.c")]
            sources += SHELL_SOURCES + [ROOT / "tests/host/desktop" / n for n in ("fake.c", "compositor_capture.c")]
            sources += [ROOT / "apps/libdeskui" / n for n in ("icons.c","icon_map.c")]
            icons=load("wall_icons",ROOT / "tests/host/desktop/icon_assets.py")
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-O1", "-Wall", "-Wextra", "-Werror", "-DCAPTURE_DECODE",
                            "-DCAPTURE_ICONS",*icons.defines(),
                            "-fsanitize=address,undefined", "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *map(str, sources), "-o", str(binary)], check=True, env=env)
            c = load("wall_portrait", ROOT / "apps/desktop/convert_portrait.py")
            (folder / "portrait.xrgb").write_bytes(c.convert(ROOT / "assets/brand/ciuki-logo.png"))
            path = str(ROOT / "scripts/test")
            sys.path.insert(0,path)
            try:
                runner = load("desktop_wall_runner",ROOT / "scripts/test/run.py")
            finally:
                sys.path.remove(path)
            for width,height in ((1024,768),(800,600)):
                geometry=f"{width}x{height}"
                target=folder / "default.xrgb"
                result=subprocess.run([str(binary),str(folder / "portrait.xrgb"),str(target),
                    "wallfile="+str(ROOT / "assets/wallpapers/ciuki/Ciuk2.png"),geometry],
                    check=True,env=env,capture_output=True,text=True)
                self.assertIn("WALLPAPER /system/wallpapers/Ciuk2.png 1448x1086 mode=fill",result.stdout)
                data=target.read_bytes(); self.assertEqual(len(data),width*height*4)
                rgb=xrgb_to_rgb(data)
                ppm=folder / "default.ppm"
                ppm.write_bytes(f"P6\n{width} {height}\n255\n".encode()+rgb)
                observed=runner.observe_normal_boot_screen(ppm,{"geometry":[width,height]})
                self.assertEqual(observed["failures"],[],geometry)
                self.assertLessEqual(observed["regions"]["background"]["fraction"],0.02)
                # This independent observer verifies the exact approved portrait's new position.
                self.assertTrue(runner.desktop_mark_visible(ppm),geometry)
                frames=os.environ.get("CIUKI_WALL_FRAMES")
                if frames:
                    Path(frames).mkdir(parents=True,exist_ok=True)
                    write_png(Path(frames) / f"default-desktop-{geometry}.png",width,height,rgb)
                desktop=bytes(((runner.DESK_BACKGROUND>>16)&255,(runner.DESK_BACKGROUND>>8)&255,runner.DESK_BACKGROUND&255))
                for x in (0,width-1):
                    self.assertFalse(any(rgb[(y*width+x)*3:(y*width+x)*3+3]==desktop
                                         for y in range(32,height-32)),(geometry,x))
                print("DEFAULT_DESKTOP",geometry,json.dumps({k:v["fraction"] for k,v in observed["regions"].items()},sort_keys=True))

    def test_control_display_positions(self):
        """Control Panel Display page: Center/Tile/Stretch/Fit/Fill in two rows under the monitor, clear of the
        other widgets; each saves its desktop.cfg mode and the preview follows it (control_test.c).
        CIUKI_WALL_FRAMES=<dir> keeps the rendered pages as PNG."""
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="control-display-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "control-test"
            icons = load("control_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")   # the guest's /system/icons store
            sources = ["tests/host/desktop/control_test.c", "apps/control/app.c", "apps/control/display.c", "apps/control/fonts.c", "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c", "apps/desktop/protocol.c",
                       *(f"apps/desktop/{n}.c" for n in ("settings", "sounds", "wallpaper", "image", "compositor", "theme")),
                       *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map"))]
            ft = load("control_freetype", ROOT / "sdk/freetype/build.py"); library = ft.host()
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", *icons.defines(),
                            "-I", str(ROOT / "apps/control"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "src/kernel/include"),
                            "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                            *ft.includes(), *(str(ROOT / s) for s in sources), str(library), "-pthread", "-lm", "-o", str(binary)], check=True, env=env)
            c = load("control_portrait", ROOT / "apps/desktop/convert_portrait.py")
            (folder / "portrait.xrgb").write_bytes(c.convert(ROOT / "assets/brand/ciuki-logo.png"))
            renders = folder / "renders"
            renders.mkdir()
            result = subprocess.run([str(binary), str(folder / "portrait.xrgb"), str(ROOT / "assets/wallpapers/ciuki"), str(renders)],
                                    check=False, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS control display", result.stdout)
            names = sorted(p.stem for p in renders.glob("*.xrgb"))
            self.assertEqual(names, ["control-display-fill", "control-display-fit", "control-display-migrated-fill"])
            for name in names:
                width, height = map(int, (renders / f"{name}.size").read_text().split())
                control_frame(self,name,(renders / f"{name}.xrgb").read_bytes(),width,height)
            frames = os.environ.get("CIUKI_WALL_FRAMES")
            if frames:
                Path(frames).mkdir(parents=True, exist_ok=True)
                for name in names:
                    width, height = map(int, (renders / f"{name}.size").read_text().split())
                    write_png(Path(frames) / f"{name}.png", width, height, xrgb_to_rgb((renders / f"{name}.xrgb").read_bytes()))
        print(result.stdout.strip())

    def test_control_display_modes(self):
        """f2-37: mode transactions, query ABI, adapter page and review renders."""
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="control-modes-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "control-modes-test"
            icons = load("control_modes_icons", ROOT / "tests/host/desktop/icon_assets.py")
            sources = ["tests/host/desktop/control_modes_test.c", "apps/control/app.c", "apps/control/display.c", "apps/control/fonts.c", "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c",
                       *(f"apps/desktop/{n}.c" for n in ("protocol", "settings", "sounds", "wallpaper", "image", "compositor", "theme")),
                       *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map"))]
            ft = load("control_modes_freetype", ROOT / "sdk/freetype/build.py"); library = ft.host()
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", *icons.defines(),
                            "-I", str(ROOT / "apps/control"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "src/kernel/include"),
                            "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                            *ft.includes(), *(str(ROOT / s) for s in sources), str(library), "-pthread", "-lm", "-o", str(binary)], check=True, env=env)
            c = load("control_modes_portrait", ROOT / "apps/desktop/convert_portrait.py")
            (folder / "portrait.xrgb").write_bytes(c.convert(ROOT / "assets/brand/ciuki-logo.png"))
            renders = folder / "renders"; renders.mkdir()
            result = subprocess.run([str(binary), str(renders), str(folder / "portrait.xrgb")],
                                    check=False, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS control display modes", result.stdout)
            for raw in renders.glob("*.xrgb"):
                width,height=map(int,raw.with_suffix(".size").read_text().split())
                control_frame(self,raw.stem,raw.read_bytes(),width,height)
            frames = os.environ.get("CIUKI_WALL_FRAMES")
            if frames:
                Path(frames).mkdir(parents=True, exist_ok=True)
                for raw in renders.glob("*.xrgb"):
                    width, height = map(int, raw.with_suffix(".size").read_text().split())
                    write_png(Path(frames) / f"{raw.stem}.png", width, height, xrgb_to_rgb(raw.read_bytes()))
        print(result.stdout.strip())

    def test_shell_launcher(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-launch-", dir=parent) as temp:
            folder = Path(temp)
            inc = folder / "include/ciuki"
            inc.mkdir(parents=True)
            for name in ("channel.h", "surface.h", "spawn.h", "proc.h", "audio.h", "net.h", "media.h", "power.h", "display.h", "registry.h", "audio_preference.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / name, inc / name)
            for name in ("runtime.h", "raw.h"):
                (inc / name).write_text("/* supplied by host harness */\n")
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "launch-test"
            sources = [ROOT / "apps/desktop" / n for n in ("client.c", "protocol.c", "input.c", "compositor.c", "theme.c")] + SHELL_SOURCES
            sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "shell_launch_test.c")]
            sources += [ROOT / "apps/desktop/image.c", ROOT / "apps/desktop/wallpaper_io.c"]  # shell_service's workers
            sources += [ROOT / "apps/libdeskui" / n for n in ("icons.c", "icon_map.c")]
            bridge = folder / "kernel-poll.o"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                            "-DCIUKI_DESKTOP_SPAWN_HOST", "-I", str(ROOT / "src/kernel/include"),
                            "-c", str(ROOT / "tests/host/proc/desktop_test.c"), "-o", str(bridge)], check=True, env=env)
            sources += [bridge, ROOT / "tests/host/proc/signal_legacy.c", ROOT / "src/kernel/lib/fmt.c"]
            sources += [ROOT / "src/kernel/probes/selector.c", ROOT / "src/kernel/lib/sha256.c"]
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-DDESK_POLL_LEDGER_TEST",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I", str(folder / "include"), "-I", str(ROOT / "apps/desktop"),
                            "-I", str(ROOT / "src/kernel/include"), *(str(p) for p in sources), "-o", str(binary)],
                           check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)
            subprocess.run([str(binary), "--launch-input"], check=True, env=env)
            subprocess.run([str(binary), "--autoplay-startup"], check=True, env=env, timeout=20)
            subprocess.run([str(binary), "--slow-stick-folder"], check=True, env=env, timeout=20)
            subprocess.run([str(binary), "--slow-media-panel"], check=True, env=env, timeout=15)
            result = subprocess.run([str(binary), "--poll-handles"], check=True, env=env, capture_output=True, text=True)
            print(result.stdout, end="")
            for kind, fd in (("net", 20), ("mixer", 21), ("media", 22)):
                self.assertRegex(result.stdout, rf"case=handle server=desktop cycle=1 pid=2 fd={fd} stage=restored kind={kind}")

    def test_dosbox_autorun(self):
        """/Programs/dosbox COMMAND (Programs > Games): the typed key events (apps/dosbox/autorun.h)."""
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="dosbox-autorun-", dir=parent) as temp:
            binary = Path(temp) / "autorun-test"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/desktop/dosbox_autorun_test.c"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                                    env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
            self.assertIn("PASS dosbox autorun", result.stdout)

    def test_dosbox_loading(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="dosbox-loading-", dir=parent) as temp:
            folder = Path(temp)
            inc = folder / "include/ciuki"
            inc.mkdir(parents=True)
            for name in ("dosvm.h", "channel.h", "surface.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / name, inc / name)
            shutil.copy2(ROOT / "apps/desktop/protocol.h", inc / "desktop.h")
            binary = folder / "loading-test"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(folder / "include"), "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/desktop/dosbox_loading_test.c"), str(ROOT / "apps/desktop/protocol.c"),
                            "-o", str(binary)], check=True)
            for scenario in range(9):
                result = subprocess.run([str(binary), str(scenario)], capture_output=True, text=True,
                                        timeout=30, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
                self.assertEqual(result.returncode, 0, f"scenario {scenario}: {result.stderr}")
                self.assertIn("PASS dosbox loading replay", result.stdout)

    def test_tango_icon_provenance(self):
        c = load("shell_icon_test", ROOT / "apps/desktop/convert_icons.py")
        record = c.check()
        self.assertEqual(set(record["icons"]), set(c.MEMBERS))
        self.assertEqual(record["archive_sha256"], c.ARCHIVE_SHA)

    def test_native_production_model(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-", dir=parent) as temp:
            # This sandbox runs under ptrace, which LeakSanitizer cannot inspect.
            # ASan/UBSan stay enabled; fake live-fd/map counters check lifetimes.
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            out = Path(temp) / "desktop-test"
            sources = [ROOT / "apps/desktop" / n for n in
                       ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c")] + SHELL_SOURCES
            sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "desktop_test.c")]
            sources += [ROOT / "apps/libdeskui" / n for n in ("icons.c", "icon_map.c")]
            subprocess.run(["cc", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *(str(s) for s in sources), "-o", str(out)], check=True, env=env)
            subprocess.run([str(out)], check=True, env=env)

    def test_approved_portrait_determinism(self):
        c = load("desktop_conversion_test", ROOT / "apps/desktop/convert_portrait.py")
        source = ROOT / "assets/brand/ciuki-logo.png"
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        a, b = c.convert(source), c.convert(source)
        self.assertEqual(a, b)
        self.assertEqual(len(a), 256*256*4)
        self.assertEqual(hashlib.sha256(a).hexdigest(), c.ARRAY_SHA256)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)
        self.assertEqual(a[:4], bytes((0x64,0x55,0x37,0)))
        self.assertTrue(all(a[i] == 0 for i in range(3, len(a), 4)))
        damaged = bytearray(source.read_bytes()); damaged[16] ^= 1
        with patch.object(Path, "read_bytes", return_value=bytes(damaged)):
            with self.assertRaisesRegex(ValueError, "source SHA-256"):
                c.convert(source)
        with self.assertRaisesRegex(ValueError, "PNG CRC"):
            c.decode(bytes(damaged))
        print("PASS portrait: pinned source/array SHA-256, repeat determinism, XRGB bytes, corrupt rejection")

    def test_build_provenance_and_payloads(self):
        build = load("desktop_provenance_test", ROOT / "apps/desktop/build_desktop.py")
        if not (build.OUT / "manifest.json").is_file():
            self.skipTest("run python3 apps/desktop/build_desktop.py first")
        m = build.validate_payloads()
        self.assertEqual(set(m["files"]), {"desktop", "demo", "gfxdemo", "dosbox", "files", "tasks", "ciuknote", "ciukide", "control", "terminal", "regedit", "libdeskui.a",
                                           "ciuki-portrait.xrgb",  # f3-02 /Programs/dosbox; desktop shell /Programs/control
                                           "tango.cis", "TANGO.TXT", "AUTHORS.TXT", "CREDITS.TXT",  # Tango store
                                           "calc", "charmap", "clock", "help", "mixer", "music", "paint", "viewer"})  # legacy-recovery accessories
        self.assertEqual(m["icons"]["store_sha256"], m["files"]["tango.cis"])
        expected = build.inputs()
        self.assertTrue(build.current_build(expected))
        expected["cflags"] = []
        self.assertFalse(build.current_build(expected))
        image = load("desktop_image_test", ROOT / "scripts/build_image.py")
        sources, directories, metadata = image.desktop_payloads()
        accessories = {f"/Programs/{n}" for n in ("calc", "charmap", "clock", "help", "mixer", "viewer")}
        self.assertEqual(set(sources), {"/Programs/desktop", "/Programs/demo", "/Programs/gfxdemo", "/Programs/dosbox", "/Programs/files", "/Programs/tasks",
                                        "/Programs/terminal", "/Programs/ciuknote", "/Programs/ciukide", "/Programs/control", "/Programs/regedit", "/Programs/paint", "/Programs/calc", "/Programs/charmap", "/Programs/clock", "/Programs/help", "/Programs/mixer", "/Programs/viewer", "/Programs/music",
                                        "/system/assets/ciuki-portrait.xrgb",
                                        "/system/licenses/stb_image.txt", "/system/licenses/stb_image_write.txt", "/system/licenses/stb_vorbis.txt",
                                        "/system/licenses/minimp3.txt", "/system/licenses/FreeType-FTL.txt", "/system/licenses/FreeType-LICENSE.txt", "/system/licenses/FreeType-NOTICE.txt", "/system/licenses/TinyGL-LICENSE.txt", "/system/licenses/TinyGL-NOTICE.txt", "/system/fonts/LICENSES.TXT",
                                        *image.font_payloads()[0],
                                        *(f"/system/fonts/{p.name}" for p in (ROOT / "assets/fonts/native").glob("*.CFN")),
                                        "/system/icons/tango.cis",
                                        "/system/icons/TANGO.TXT", "/system/icons/AUTHORS.TXT", "/system/icons/CREDITS.TXT",
                                        "/system/wallpapers/Ciuk1.png", "/system/wallpapers/Ciuk2.png", "/system/wallpapers/Ciuk3.png",
                                        "/System/hwids/pci-ids.json", "/System/hwids/usb-ids.json",
                                        "/system/sounds/scheme.cfg",
                                        *(f"/System/cache/wallpaper/{n}" for n in metadata["wallpaper_cache"]["files"]),
                                        *(f"/system/sounds/{n}" for n in metadata["sounds"]["files"] if n != "scheme.cfg")})
        self.assertTrue({"/Programs", "/System", "/System/cache", "/System/cache/wallpaper", "/system/assets", "/system/icons", "/system/settings", "/system/wallpapers", "/Documents/Music", "/Documents/Pictures"} <= directories)
        self.assertEqual(metadata["portrait"]["array_sha256"], m["files"]["ciuki-portrait.xrgb"])
        self.assertEqual(metadata["icons"]["store_sha256"], m["files"]["tango.cis"])
        records = image.payload_records(sources)
        self.assertEqual(len(records), 91)  # Includes TinyGL licence and product acknowledgment.
        for r in records:
            if r["path"] in ("/system/licenses/stb_image.txt", "/system/licenses/stb_image_write.txt", "/system/licenses/stb_vorbis.txt"):
                self.assertEqual(r["sha256"], hashlib.sha256((ROOT / "third_party/stb/LICENSE").read_bytes()).hexdigest())
            elif r["path"].startswith(("/system/licenses/FreeType-", "/system/licenses/TinyGL-")):
                self.assertEqual(r["sha256"], hashlib.sha256((ROOT / "build/tools/ciuki-sdk/licenses" / Path(r["path"]).name).read_bytes()).hexdigest())
            elif r["path"] == "/system/licenses/minimp3.txt":
                self.assertEqual(r["sha256"], hashlib.sha256((ROOT / "third_party/minimp3/LICENSE").read_bytes()).hexdigest())
            elif r["path"].startswith("/System/fonts/"):
                self.assertEqual(r["sha256"], metadata["fonts"]["files"][r["path"]])
            elif r["path"].startswith("/system/fonts/"):
                source = str(sources[r["path"]].relative_to(ROOT))
                self.assertEqual(r["sha256"], m["inputs"]["sources"][source])
            elif r["path"].startswith("/system/sounds/"):
                self.assertEqual(r["sha256"], metadata["sounds"]["files"][Path(r["path"]).name])
            elif r["path"].startswith("/System/hwids/"):
                # f2-43: hardware id tables generated into assets/hwids, byte-identical.
                self.assertEqual(r["sha256"], hashlib.sha256((ROOT / "assets/hwids" / Path(r["path"]).name).read_bytes()).hexdigest())
            elif r["path"].startswith("/system/wallpapers/"):
                # The owner's adaptive Ciuki scenes, byte-identical to the assets (misc/ciukios_bg/adaptive hashes).
                self.assertEqual(r["sha256"], hashlib.sha256((ROOT / "assets/wallpapers/ciuki" / Path(r["path"]).name).read_bytes()).hexdigest())
            elif r["path"].startswith("/System/cache/wallpaper/"):
                self.assertEqual(r["sha256"], metadata["wallpaper_cache"]["files"][Path(r["path"]).name]["sha256"])
            else:
                self.assertEqual(r["sha256"], m["files"][Path(r["path"]).name])
        # Private wallpapers (third_party/win_bg, local only) never enter the image.
        self.assertFalse(any("win_bg" in str(v) or Path(k).suffix.lower() == ".bmp" for k, v in sources.items()))
        print("PASS desktop/demo/Files ELF, SDK/source/output provenance, payload size/SHA-256 inventory")

    @unittest.skipUnless(all(shutil.which(t) for t in ("mkfs.fat", "mcopy", "mmd", "mdir")),
                         "requires mtools and mkfs.fat")
    def test_actual_desktop_payload_fat_readback(self):
        image = load("desktop_fat_test", ROOT / "scripts/build_image.py")
        if not (image.DESKTOP / "manifest.json").is_file():
            self.skipTest("build desktop first")
        sources, directories, _ = image.desktop_payloads()
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-fat-", dir=parent) as temp:
            env = dict(os.environ, TMPDIR=temp)
            volume = Path(temp) / "fixture.fat"
            with volume.open("wb") as f:
                f.truncate(64*1024*1024)   # includes five raw wallpaper frames (~19 MiB)
            subprocess.run(["mkfs.fat", str(volume)], check=True, env=env, stdout=subprocess.DEVNULL)
            image.populate_payloads(volume, sources, directories)
            with patch.object(image, "PART_LBA", 0):
                image.check_payloads(volume, image.payload_records(sources), directories)
        print("PASS real FAT read-back: desktop, demo, Files, portrait sizes/SHA-256")

    def test_stale_output_rejected(self):
        build = load("desktop_stale_test", ROOT / "apps/desktop/build_desktop.py")
        if not (build.OUT / "manifest.json").is_file():
            self.skipTest("build desktop first")
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-stale-", dir=parent) as temp:
            dest = Path(temp)
            for name in ("manifest.json", *sorted(build.OUTPUTS)):
                shutil.copy2(build.OUT / name, dest / name)
            with patch.object(build, "OUT", dest):
                self.assertTrue(build.current_build(build.inputs()))
                (dest / "ciuki-portrait.xrgb").write_bytes(b"wrong portrait")
                with self.assertRaisesRegex(ValueError, "stale desktop"):
                    build.validate_payloads()
        print("PASS stale/tampered portrait rejected before image payload acceptance")


class AppCaseReplayTests(unittest.TestCase):
    """files-qemu-t23, tasks-qemu-t23 and ciuknote-qemu-t23 replayed on the host: their exact QMP batches,
    through a model of QEMU's PS/2 devices and the kernel's i8042 decoder, into the production desktop (gate
    mode, the probe's interaction stage, scenario keys and launches) and the production Files, CiukNote,
    Task Manager and demo with libdeskui at the 1024x768 geometry of the QEMU profiles. See app_replay.c
    for what the virtual kernel models and what it does not."""
    @classmethod
    def setUpClass(cls):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="desktop-replay-", dir=parent)
        folder = Path(cls.temp.name)
        cls.env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS="detect_leaks=0")
        include = folder / "include/ciuki"
        include.mkdir(parents=True)
        for name in ("channel.h", "surface.h", "spawn.h", "proc.h", "audio.h", "net.h", "media.h", "power.h", "display.h", "registry.h", "audio_preference.h"):
            shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / name, include / name)
        # Host stand-ins: CU_PTR keeps the whole pointer, the report call takes it (desktop_gate_test.c does the same).
        (include / "runtime.h").write_text("/* app replay */\n#include <ciuki/abi.h>\n#include <stdint.h>\n"
                                           "#define CU_PTR(p) ((uintptr_t)(p))\n"
                                           "static inline int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }\n")
        (include / "raw.h").write_text("/* app replay */\n#include <stdint.h>\n"
                                       "uint32_t ciuki_raw_probe_report(uintptr_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);\n")
        common = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                  "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-I", str(folder / "include"), "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "apps/libdeskui"),
                  "-I", str(ROOT / "apps/tasks"), "-I", str(ROOT / "src/kernel/include")]
        host = ROOT / "tests/host/desktop"
        icons = load("replay_icon_assets", host / "icon_assets.py")   # the guest's /system/icons store
        os_calls = ["-D_DEFAULT_SOURCE", "-include", str(host / "replay_os.h")]   # the replay's virtual kernel
        posix = ["-D_POSIX_C_SOURCE=200809L"]
        units = [("replay_desktop", os_calls, host / "replay_desktop.c"),
                 *((f"desktop_{n}", [], ROOT / f"apps/desktop/{n}.c") for n in ("client", "protocol", "input", "compositor", "theme",
                                                                                  "shell", "autoplay", "autoplay_content", "sounds", "settings", "settings_io", "wallpaper", "wallpaper_io", "keymap", "image", "folder")),
                 *((f"deskui_{n}", posix, ROOT / f"apps/libdeskui/{n}.c") for n in ("draw", "widgets", "list", "menu", "dialog", "textarea")),
                 *((f"deskui_{n}", posix + icons.defines(), ROOT / f"apps/libdeskui/{n}.c") for n in ("icons", "icon_map")),
                 ("deskui_client", os_calls, ROOT / "apps/libdeskui/client.c"),
                 ("deskui_recent", os_calls, ROOT / "apps/libdeskui/recent.c"),   # CiukNote's recent list (off in --test runs)
                 ("files_main", os_calls + ["-Dmain=files_main"], ROOT / "apps/files/files.c"),
                 ("files_app", posix + ["-include", str(host / "replay_files_worker.h")], ROOT / "apps/files/app.c"), ("files_model", posix, ROOT / "apps/files/model.c"),
                 ("files_transfer", posix + ["-include", str(host / "replay_files_worker.h")], ROOT / "apps/files/transfer.c"),
                 ("note_main", os_calls + ["-Dmain=ciuknote_main"], ROOT / "apps/ciuknote/main.c"),
                 ("note_app", posix, ROOT / "apps/ciuknote/app.c"), ("note_doc", posix, ROOT / "apps/ciuknote/doc.c"),
                 ("tasks_main", os_calls + ["-Dmain=tasks_main"], ROOT / "apps/tasks/tasks.c"),
                 ("tasks_model", posix, ROOT / "apps/tasks/model.c"), ("tasks_view", posix, ROOT / "apps/tasks/view.c"),
                 ("demo", os_calls + ["-Dmain=demo_main"], ROOT / "apps/demo/demo.c"),
                 ("survivor_demo", os_calls + ["-Dmain=survivor_demo_main"], ROOT / "apps/demo/demo.c"),
                 ("fake_vfs", posix, host / "fake_vfs.c"),
                 ("harness", ["-D_DEFAULT_SOURCE"], host / "app_replay.c")]
        def compile_unit(unit):
            name, flags, source = unit
            target = folder / f"{name}.o"
            subprocess.run([*common, *flags, "-c", str(source), "-o", str(target)], check=True, env=cls.env)
            return target
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            objects = list(pool.map(compile_unit, units))
        cls.binary = folder / "app_replay"
        subprocess.run(["clang", "-fsanitize=address,undefined", "-pthread", *map(str, objects), "-o", str(cls.binary)],
                       check=True, env=cls.env)
        cls.suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
        cls.suite["cases"] += json.loads((ROOT / "tests/suites/f1-usb.json").read_text())["cases"]
        # The image's /Programs, as the desktop scans it (the menu lists it without the desktop itself).
        cls.programs = sorted(set(re.findall(r'"/Programs/([A-Za-z0-9_.-]+)"', (ROOT / "scripts/build_image.py").read_text())))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def replay(self, case_id, kind, model, args=None):
        case = next(c for c in self.suite["cases"] if c["id"] == case_id)
        lines = []
        for batch in case["actions"][0]["batches"]:
            lines.append(f"B {batch['pause_ms']}")
            for event in batch["events"]:
                data = event["data"]
                if event["type"] == "key": lines.append(f"K {QCODES[data['key']['data']]} {int(data['down'])}")
                elif event["type"] == "rel": lines.append(f"R {0 if data['axis'] == 'x' else 1} {data['value']}")
                elif event["type"] == "btn": lines.append(f"M {('left', 'right', 'middle').index(data['button']) + 1} {int(data['down'])}")
                else: self.fail(event)
        script = Path(self.temp.name) / f"{case_id}.script"
        script.write_text("\n".join(lines) + "\n")
        result = subprocess.run([str(self.binary), "--script", str(script), "--case", kind, "--bin", ",".join(self.programs),
                                 "--limit-s", "200", *(["--trace"] if kind == "files" else []),
                                 *(args if args is not None else SHELL_MODELS[model])],
                                capture_output=True, text=True, env=self.env, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout[-4000:] + result.stderr[-4000:])
        out = {"checkpoints": [], "system": [], "text": result.stdout}
        for line in result.stdout.splitlines():
            head, _, rest = line.partition(" ")
            if head == "CHECKPOINT_AT": out["checkpoints"].append((int(rest.split()[0]), float(rest.split()[1])))
            elif head == "SYSTEM": out["system"].append(rest)
            elif head == "FILE": out["file"] = rest.split()
            elif head in ("PROBE", "GATE", "PS2", "USB", "TIMING", "RESULT", "DESKTOP", "PRESENTS"):
                out[head] = dict(kv.split("=", 1) for kv in re.findall(r'(\w+=(?:"[^"]*"|\S+))', rest))
        print(f"REPLAY {case_id} {model}: " + " | ".join(l for l in result.stdout.splitlines()
                                                         if l.startswith(("PROBE", "GATE", "PS2", "TIMING", "CHECKPOINT_AT"))))
        return case, out

    def check(self, case_id, kind, checkpoints, window, models=REPLAY_MODELS):
        for model, args in models.items():
            with self.subTest(model=model):
                case, out = self.replay(case_id, kind, model, args)
                self.assertEqual(out["RESULT"]["success"], "1", out["text"][-3000:])
                # Every checkpoint, in order, each reported once (the probe's checkpoint_order rule).
                self.assertEqual([n for n, _ in out["checkpoints"]], list(range(1, checkpoints + 1)))
                self.assertEqual(int(out["PROBE"]["checkpoint"]), checkpoints)
                after = next(p for p in case["expected"]["predicates"] if p["where"].get("stage") == "after")
                self.assertEqual(int(out["PROBE"]["keys"]), after["fields"]["keys"]["eq"])
                gate = {k: int(v) for k, v in out["GATE"].items()}
                self.assertGreaterEqual(gate["keys"], 2)
                self.assertGreaterEqual(gate["buttons"], 2)
                self.assertGreaterEqual(gate["motion"], 1)
                # Nothing the QEMU PS/2 queues would drop or carry into a later batch.
                self.assertEqual(out["PS2"]["dropped_bytes"], "0")
                self.assertEqual(out["PS2"]["leftover_batches"], "0")
                timing = out["TIMING"]
                self.assertEqual(timing["batches"].split("/")[0], timing["batches"].split("/")[1])
                # The probe ENDs 3 s after its interaction record: the runner must have sent every batch by then.
                self.assertLessEqual(float(timing["last_batch"]), float(timing["success"]) + 3.0)
                self.assertLess(float(timing["success"]), window - 1.0)
                yield model, out

    def test_files_case_replay(self):
        for _, out in self.check("files-qemu-t23", "files", 5, APP_WINDOW_S):
            self.assertEqual(out["DESKTOP"]["app_done"], "1")
            # The host checker lists ::/SYSTEM for CIUKI (the renamed folder); the copy was deleted again.
            self.assertIn("Ciuki", out["system"])
            self.assertIn('title="Files - C:\\System\\Ciuki"', out["text"])

    def test_shell_case_replay(self):
        """desktop-shell-qemu-t23 (F12): the demo launched from the Start menu, dragged by its title, closed and
        reaped (stage 6), then the final a press and release. The reap completes without input, so the desktop
        must report it on its own: otherwise the probe keeps the keys=0 of the last input's report."""
        for model, out in self.check("desktop-shell-qemu-t23", "shell", 0, TASKS_WINDOW_S, SHELL_MODELS):
            # f2-69: the guest creates/drops all 100 victims before ARM. Its
            # menu demo opens at (110,142), so the unchanged (60,80) title
            # press misses it without the scenario's live geometry targeting.
            self.assertIn("HISTORY windows=100 cascade=101 focus=0", out["text"], model)
            self.assertEqual(out["DESKTOP"]["shell_stage"], "6", model)
            self.assertEqual(out["DESKTOP"]["notice"], '"shell stage 6 programs %d"' % (len(self.programs) - 1), model)
            self.assertEqual(int(out["PROBE"]["keys"]), 4, model)
            # The demo was reaped with status 0 (not terminated by the close timeout).
            self.assertRegex(out["text"], r"(?m)^PROC pid=105 ppid=3 name=demo state=0 status=0$", model)
            self.assertRegex(out["text"], r"(?m)^PROC pid=4 ppid=3 name=demo state=1 status=0$", model)
            # f2-65: once the case has settled the desktop presents only what changed (the clock at most).
            # Before, the interaction stage presented a complete frame on every loop: 47 per second with
            # the declared desktop, about 500 per second with the 1 ms one.
            presents = {k: int(v) for k, v in out["PRESENTS"].items()}
            self.assertGreaterEqual(presents["idle_ms"], 1000, model)
            idle_frames_per_s = presents["idle_px"] * 1000 / presents["frame_px"] / presents["idle_ms"]
            self.assertLessEqual(idle_frames_per_s, 2.0, (model, presents))

    def test_usb_shell_case_replay(self):
        """The actual UHCI/OHCI scripts, translated set-1 keys and source=USB,
        through the production desktop's F12 selector, launch, drag, close/reap.
        No repeats are needed; keyboard edges remain independent of the mouse.
        """
        orb=json.loads((ROOT / "apps/desktop/start-orb.json").read_text())["orb_box"]
        centre=(orb[0]+orb[2]//2,orb[1]+orb[3]//2)
        models = {"usb-10ms": ["--usb-poll-ms", "10", "--start-delay-ms", "800"],
                  "usb-late-start": ["--usb-poll-ms", "10", *REPLAY_MODELS["late-start"]]}
        for case_id in ("usb-input-uhci", "usb-input-ohci"):
            for model, out in self.check(case_id, "shell", 0, TASKS_WINDOW_S, models):
                self.assertIn("HISTORY windows=100 cascade=101 focus=0", out["text"], model)
                self.assertEqual(out["DESKTOP"]["shell_stage"], "6", model)
                self.assertRegex(out["text"], r"(?m)^PROC pid=4 ppid=3 name=demo state=1 status=0$", model)
                self.assertEqual(out["PROBE"]["keys"], "4", model)
                self.assertEqual(out["USB"]["read_keys"], "4")
                self.assertEqual(out["USB"]["pending_mouse"], "0")
                self.assertEqual(out["USB"]["pending_keys"], "0")
                self.assertEqual((out["USB"]["first_click_x"], out["USB"]["first_click_y"]),
                                 tuple(map(str,centre)))

    def test_usb_shell_slow_poll_misses_start(self):
        """The evidence has 1.4--2.5 s between USB reports, versus 20--100 ms
        runner pauses. QEMU merges same-button motion before it is polled;
        the move onto Start cancels the tail of the corner clamp, so stage 0
        never opens the menu even though F12 and A are consumed.
        """
        _, out = self.replay("usb-input-uhci", "shell", "usb-slow",
                             ["--usb-poll-ms", "2000", "--start-delay-ms", "800", "--limit-s", "60", "--trace"])
        self.assertEqual(out["RESULT"]["success"], "0")
        self.assertEqual(out["DESKTOP"]["shell_test"], "1")
        self.assertEqual(out["DESKTOP"]["shell_stage"], "0")
        self.assertEqual(out["PROBE"]["keys"], "0")
        self.assertEqual(out["GATE"]["keys"], "2")
        self.assertEqual(out["USB"]["read_keys"], "4")
        self.assertEqual(out["GATE"]["motion"], "36")
        self.assertEqual(out["GATE"]["buttons"], "12")
        reports = re.findall(r'USB mouse report dx=(-?\d+) dy=(-?\d+) buttons=(\d+)', out["text"])
        orb=json.loads((ROOT / "apps/desktop/start-orb.json").read_text())["orb_box"]
        tail=tuple(str(orb[i]+orb[i+2]//2-127) for i in range(2))+("0",)
        self.assertEqual(reports[:7], [("-127", "-127", "0")] * 5 +
                         [tail, ("0", "0", "1")])
        self.assertGreater(int(out["USB"]["compressed"]), 0)
        self.assertEqual((out["USB"]["first_click_x"], out["USB"]["first_click_y"]), ("0", "0"))
        self.assertEqual(out["USB"]["pending_mouse"], "0")
        self.assertEqual(out["USB"]["pending_keys"], "0")

    def test_shell_case_firmware_input_order(self):
        """desktop-shell-qemu-e500 on image 639bcca5: stage 6 reached, keys=0, motion=47 buttons=12. On the
        firmware input path the guest reads the controller bytes through the BIOS VM; while it does not read
        them, the 8042 serves a pending keyboard byte before the queued aux bytes, so the A press reaches the
        desktop before the close click. The model stalls the reader from the close click's batch for 400 ms
        (the A batch follows 200 ms later): the A edges precede the click (reordered=2) and must still count."""
        case = next(c for c in self.suite["cases"] if c["id"] == "desktop-shell-qemu-e500")
        batches = case["actions"][0]["batches"]
        first_key = next(i for i, b in enumerate(batches) if i > 1 and any(e["type"] == "key" for e in b["events"]))
        close = max(i for i, b in enumerate(batches[:first_key])
                    if any(e["type"] == "btn" and e["data"]["down"] for e in b["events"]))
        stall = ["--i8042-byte-ms", "1", "--i8042-stall-batch", str(close), "--i8042-stall-ms", "400"]
        models = {"firmware-stall": ["--start-delay-ms", "800", *stall],
                  "firmware-stall-late": [*REPLAY_MODELS["late-start"], "--i8042-byte-ms", "2", *stall[2:]]}
        for model, out in self.check("desktop-shell-qemu-e500", "shell", 0, TASKS_WINDOW_S, models):
            self.assertEqual(out["DESKTOP"]["notice"], '"shell stage 6 programs %d"' % (len(self.programs) - 1), model)
            self.assertEqual(out["PS2"]["reordered"], "2", model)
            self.assertEqual((out["PROBE"]["keys"], out["PROBE"]["motion"], out["PROBE"]["buttons"]), ("4", "47", "12"), model)

    def test_tasks_case_replay(self):
        for _, out in self.check("tasks-qemu-t23", "tasks", 0, TASKS_WINDOW_S):
            self.assertEqual(out["DESKTOP"]["tasks_stage"], "7")

    def test_ciuknote_case_replay(self):
        case = next(c for c in self.suite["cases"] if c["id"] == "ciuknote-qemu-t23")
        for _, out in self.check("ciuknote-qemu-t23", "ciuknote", 3, APP_WINDOW_S):
            self.assertEqual(out["DESKTOP"]["app_done"], "1")
            path, data = out["file"]
            self.assertEqual(path, "/system/tests/note.txt")
            self.assertEqual(hashlib.sha256(bytes.fromhex(data)).hexdigest(), case["checks"]["files"][0]["sha256"])


def load_tests(loader, tests, pattern):
    # Discovery finds these sibling modules itself. Explicit desktop runs include
    # both refinement groups once, without duplicating them during discovery.
    if pattern is None:
        for name in ("test_interaction", "test_text_selection", "test_start_orb"):
            tests.addTests(loader.loadTestsFromModule(load(name, ROOT / "tests/host/desktop" / (name + ".py"))))
    return tests


if __name__ == "__main__":
    unittest.main()
