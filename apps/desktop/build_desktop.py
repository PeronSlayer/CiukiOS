#!/usr/bin/env python3
"""Build and inspect desktop, demo, libdeskui, Files, Tasks, CiukNote, the Control Panel, Terminal and the accessories using the static Ciuki SDK."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/apps/desktop"
SDK = ROOT / "build/tools/ciuki-sdk"
CFLAGS = ["-O2", "-Wall", "-Wextra", "-Werror"]
# Tango store and its notices: /system/icons/<key> in the image (scripts/build_image.py).
ICON_NOTICES = {"TANGO.TXT": "upstream/COPYING", "AUTHORS.TXT": "upstream/AUTHORS", "CREDITS.TXT": "CREDITS.TXT"}
ICON_FILES = ("tango.cis", *ICON_NOTICES)
# Accessories recovered from the 0.8 line (apps/accessories; launcher names in apps/desktop/catalog.h).
ACCESSORIES = {
    "calc": ["calc_main.c", "calc_view.c", "calc_model.c"],
    "charmap": ["charmap_main.c", "charmap_view.c"],
    "clock": ["clock_main.c", "clock_view.c", "clock_model.c"],
    "help": ["help_main.c", "help_view.c", "help_topics.c"],
    "mixer": ["mixer_main.c", "mixer_view.c"],
    "music": ["../music/main.c", "../music/view.c", "../music/player.c", "../music/runtime.c", "../music/decoder.c", "../music/playlist.c", "../music/vorbis.c"],
    "paint": ["../paint/main.c", "../paint/paint.c", "../paint/io.c", "../paint/stb.c"],
    "viewer": ["viewer_main.c", "viewer_view.c", "viewer_model.c", "viewer_stb.c"],
}
PROGRAMS = ("desktop", "demo", "gfxdemo", "dosbox", "files", "tasks", "ciuknote", "ciukide", "control", "terminal", "regedit", *ACCESSORIES)
# Desktop shell (shell.c and friends): Start menu, tray, icons, wallpapers, settings, keyboard layouts.
SHELL = ["sounds.c", "sound_storage.c", "sound_runtime.c", "shell.c", "autoplay.c", "autoplay_content.c", "autoplay_registry.c", "settings.c", "settings_io.c", "wallpaper.c", "wallpaper_io.c", "image.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c", "../files/vfs.c"]
OUTPUTS = {*PROGRAMS, "libdeskui.a", "ciuki-portrait.xrgb", *ICON_FILES}


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inputs():
    files = [p for directory in (ROOT / "apps/desktop", ROOT / "apps/demo", ROOT / "apps/gfxdemo", ROOT / "apps/dosbox", ROOT / "apps/libdeskui",
                                 ROOT / "apps/files", ROOT / "apps/tasks", ROOT / "apps/ciuknote", ROOT / "apps/control",
                                 ROOT / "apps/ciukide", ROOT / "apps/regedit", ROOT / "apps/accessories", ROOT / "apps/paint",
                                 ROOT / "apps/music", ROOT / "apps/terminal", ROOT / "third_party/minimp3", ROOT / "third_party/stb", ROOT / "assets/fonts/native")
             for p in directory.rglob("*") if p.is_file() and "__pycache__" not in p.parts]
    files.extend(ROOT / name for name in ("scripts/build_sounds.py", "assets/sounds/LICENSE.txt",
                 "assets/sounds/CREDITS.TXT", "assets/sounds/kenney-interface/UPSTREAM.json",
                 "assets/sounds/kenney-interface/upstream.zip"))
    files.append(ROOT / "assets/brand/ciuki-logo.png")
    files.append(ROOT / "assets/fonts/LICENSES.TXT")
    files.append(ROOT / "assets/icons/upstream/tango-icon-theme-0.8.90.tar.gz")
    files.extend(ROOT / "assets/icons" / name for name in ICON_NOTICES.values())
    # UI fonts (apps/desktop/ui_fonts.h is generated from these by convert_fonts.py).
    for face in ("inter", "inconsolata"):
        files.extend(p for p in (ROOT / "assets/fonts/upstream" / face).iterdir() if p.is_file())
    if (SDK / "sysroot/include/ciuki/desktop.h").read_bytes() != (ROOT / "apps/desktop/protocol.h").read_bytes():
        raise ValueError("SDK desktop header is stale; rebuild SDK")
    return {"sdk_manifest_sha256": digest(SDK / "manifest.json"),
            "sources": {str(p.relative_to(ROOT)): digest(p) for p in sorted(files)},
            "cflags": CFLAGS,
            "lua_manifest_sha256": digest(ROOT / "build/apps/lua/manifest.json"),
            "lua_library_sha256": digest(ROOT / "build/apps/lua/liblua.a")}


def current_build(expected):
    try:
        m = json.loads((OUT / "manifest.json").read_text())
        return (m["inputs"] == expected and set(m["files"]) == OUTPUTS and
                all(digest(OUT / n) == h for n, h in m["files"].items()))
    except (OSError, ValueError, KeyError):
        return False


def validate_payloads():
    lua = module("desktop_sdk_validator", ROOT / "apps/lua/build_lua.py")
    lua.validate_sdk()
    icons = module("desktop_icon_check", ROOT / "apps/desktop/convert_icons.py")
    icons.check()
    module("desktop_font_check", ROOT / "apps/desktop/convert_fonts.py").check()
    if not current_build(inputs()):
        raise ValueError("missing/stale desktop payloads; run make desktop")
    m = json.loads((OUT / "manifest.json").read_text())
    if m["files"]["tango.cis"] != icons.store_record()["store_sha256"] or m["icons"] != icon_record(icons):
        raise ValueError("Tango icon store provenance mismatch")
    converter = module("desktop_portrait", ROOT / "apps/desktop/convert_portrait.py")
    if (m["files"]["ciuki-portrait.xrgb"] != converter.ARRAY_SHA256 or
            digest(ROOT / "assets/brand/ciuki-logo.png") != converter.SOURCE_SHA256 or
            m["portrait"] != portrait_record(converter)):
        raise ValueError("approved Ciuki portrait provenance mismatch")
    inspect = lua.checker().inspect
    for name in PROGRAMS:
        if inspect(OUT / name) != m["elf"][name]:
            raise ValueError(f"desktop ELF evidence: {name}")
    return m


def portrait_record(c):
    return {"source": "assets/brand/ciuki-logo.png", "source_sha256": c.SOURCE_SHA256,
            "array_sha256": c.ARRAY_SHA256, "width": c.SIZE, "height": c.SIZE,
            "format": "XRGB8888 little-endian B,G,R,0", "bytes": c.SIZE*c.SIZE*4,
            "conversion": "floor(x*1254/256), floor(y*1254/256); integer alpha over #375564"}


def icon_record(icons):
    record = icons.store_record()
    return {"path": "/system/icons/tango.cis", "record": "apps/desktop/tango-store.json",
            "store_sha256": record["store_sha256"], "store_bytes": record["store_bytes"],
            "decoded_sha256": record["decoded_sha256"], "archive_sha256": record["archive_sha256"],
            "names": record["names"], "images": record["images"], "members": len(record["members"]),
            "sizes": record["sizes"], "notices": {name: "assets/icons/" + source for name, source in ICON_NOTICES.items()}}


def build():
    lua = module("desktop_sdk_build", ROOT / "apps/lua/build_lua.py")
    lua.validate_sdk()
    lua.build(ROOT / "build/downloads/newlib/lua-5.4.8.tar.gz", ROOT / "build/downloads/newlib/lua-5.4.8-tests.tar.gz", 1)
    icons = module("desktop_icon_build", ROOT / "apps/desktop/convert_icons.py")
    icons.check()
    icons.check_store_record()
    module("desktop_font_build", ROOT / "apps/desktop/convert_fonts.py").check()
    c = module("desktop_portrait", ROOT / "apps/desktop/convert_portrait.py")
    data = c.convert(ROOT / "assets/brand/ciuki-logo.png")
    if hashlib.sha256(data).hexdigest() != c.ARRAY_SHA256:
        raise ValueError("converted portrait differs from recorded SHA-256")
    module("desktop_sounds", ROOT / "scripts/build_sounds.py").build()
    expected = inputs()
    if current_build(expected):
        validate_payloads()
        print("[desktop] inputs and output hashes unchanged")
        return
    OUT.mkdir(parents=True, exist_ok=True)
    temp = OUT / "temp"
    temp.mkdir(exist_ok=True)
    env = os.environ.copy()
    env["TMPDIR"] = str(temp)
    start = time.monotonic()
    sources = {"desktop": ["desktop.c", "client.c", "protocol.c", "input.c", "compositor.c", "theme.c", "assets.c", *SHELL,
                           "../libdeskui/icons.c", "../libdeskui/icon_map.c"],
               "demo": ["../demo/demo.c", "protocol.c", "compositor.c", "theme.c"],
               "dosbox": ["../dosbox/dosbox.c", "protocol.c"]}
    evidence, commands = {}, {}
    sdk = json.loads((SDK / "manifest.json").read_text())
    objects = []
    for source in [*sorted((ROOT / "apps/libdeskui").glob("*.c")), ROOT / "apps/desktop/protocol.c", ROOT / "apps/desktop/compositor.c", ROOT / "apps/desktop/theme.c"]:
        obj = temp / (source.stem + ".o")
        command = [str(SDK / "bin/ciuki-cc"), *CFLAGS, "-ffunction-sections", "-fdata-sections", "-c", str(source), "-o", str(obj)]
        subprocess.run(command, check=True, env=env)
        # The whole toolkit draws with integers only, like the compositor.
        audit_integer(obj, sdk)
        objects.append(obj)
    library = OUT / "libdeskui.a"
    library.unlink(missing_ok=True)
    archive = [sdk["tools"]["llvm-ar"]["path"], "rcsD", str(library), *(str(p) for p in objects)]
    subprocess.run(archive, check=True, env=env)
    commands["libdeskui"] = archive
    sources["files"] = ["autoplay_content.c", "../files/files.c", "../files/app.c", "../files/model.c", "../files/transfer.c", "../files/vfs.c"]
    sources["tasks"] = ["../tasks/tasks.c", "../tasks/model.c", "../tasks/view.c"]
    sources["ciuknote"] = ["../ciuknote/main.c", "../ciuknote/app.c", "../ciuknote/doc.c", "../ciuknote/vfs.c"]
    sources["ciukide"] = ["../ciukide/main.c", "../ciukide/app.c", "../ciukide/model.c", "../ciukide/job.c", "../ciukide/process.c",
                          "../ciuknote/doc.c", "../ciuknote/vfs.c"]
    sources["terminal"] = ["../terminal/main.c", "../terminal/app.c", "../terminal/commands.c", "../terminal/lua.c", "../files/model.c", "../files/transfer.c", "../files/vfs.c", "../tasks/model.c"]
    sources["control"] = ["autoplay_registry.c", "../control/main.c", "../control/app.c", "../control/display.c", "../control/fonts.c", "sounds.c", "sound_storage.c", "settings.c", "settings_io.c", "wallpaper.c", "image.c"]
    sources["regedit"] = ["../regedit/main.c", "../regedit/app.c", "../accessories/acc.c"]
    sources["gfxdemo"] = ["../gfxdemo/main.c", "../gfxdemo/app.c", "../gfxdemo/scene.c", "../accessories/acc.c"]
    for name, files in ACCESSORIES.items():
        sources[name] = [f"../accessories/{p}" for p in (*files, "acc.c")]
    for name, files in sources.items():
        commands[name] = [str(SDK / "bin/ciuki-cc"), *CFLAGS, "-I", str(ROOT / "apps/desktop"),
                          *(str(ROOT / "apps/desktop" / p) for p in files),
                          *([str(library)] if name in ("desktop", "dosbox", "files", "tasks", "ciuknote", "ciukide", "control", "terminal", "regedit", "gfxdemo", *ACCESSORIES) else []),
                          *(["-I", str(ROOT / "build/apps/lua/lua-5.4.8/src"), str(ROOT / "build/apps/lua/liblua.a")] if name == "terminal" else []),
                          *(["-lm"] if name in ACCESSORIES else []), *(["-lfont"] if name == "control" else []), *(["-lreg"] if name in ("regedit", "desktop", "control") else []), "-o", str(OUT / name)]
        subprocess.run(commands[name], check=True, cwd=ROOT, env=env)
        evidence[name] = lua.checker().inspect(OUT / name)
    # Audit integer drawing separately, including the fixed-point demo rasterizer.
    for source in ("apps/desktop/compositor.c", "apps/desktop/theme.c", "apps/gfxdemo/app.c", "apps/gfxdemo/scene.c"):
        obj = temp / f"{Path(source).stem}.audit.o"
        subprocess.run([str(SDK / "bin/ciuki-cc"), *CFLAGS, "-c", str(ROOT / source),
                        "-o", str(obj)], check=True, env=env)
        audit_integer(obj, sdk)
        obj.unlink()
    (OUT / "ciuki-portrait.xrgb").write_bytes(data)
    store, _ = icons.build_store()
    icons.check_store(store)
    (OUT / "tango.cis").write_bytes(store)
    for name, source in ICON_NOTICES.items():
        (OUT / name).write_bytes((ROOT / "assets/icons" / source).read_bytes())
    m = {"schema_version": 1, "application": "Ciuki desktop", "protocol_version": 1,
         "protocol_extensions": [2],
         "inputs": expected, "commands": commands, "elf": evidence,
         "files": {name: digest(OUT / name) for name in (*sources, "libdeskui.a", "ciuki-portrait.xrgb", *ICON_FILES)},
         "portrait": portrait_record(c), "icons": icon_record(icons), "integer_compositor_audit": "PASS",
         "integer_graphics_demo_audit": "PASS",
         "build_seconds": round(time.monotonic()-start, 3), "guest_qualification": "not_run"}
    (OUT / "manifest.json").write_text(json.dumps(m, indent=2, sort_keys=True)+"\n")
    for obj in objects:
        obj.unlink(missing_ok=True)
    print(f"[desktop] built/inspected desktop, demo, dosbox, libdeskui, Files, Tasks, CiukNote, Control Panel and Terminal in {m['build_seconds']} seconds; "
          f"Tango store {m['icons']['store_bytes']} bytes, {m['icons']['names']} names; guest not_run")


def audit_integer(obj, sdk):
    import re
    audit = module("desktop_opcode_audit", ROOT / "scripts/build_kernel.py")
    dis = subprocess.check_output([sdk["tools"]["llvm-objdump"]["path"], "-d", "--no-show-raw-insn", str(obj)], text=True)
    for line in dis.splitlines():
        ins = re.match(r"\s*[0-9a-f]+:\s+([a-z][a-z0-9]*)\s*(.*)", line)
        if ins and (audit.is_fpu_or_simd(ins[1]) or re.search(r"%(?:xmm|ymm|zmm|mm)[0-9]+", ins[2])):
            raise ValueError(f"noninteger compositor instruction: {line}")


if __name__ == "__main__":
    try:
        build()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
