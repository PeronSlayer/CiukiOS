"""F2-98a native update regions and T23 operation budgets, production views.

Run sequentially with nice -n 19. LeakSanitizer remains enabled; persistent
process caches are reachable globals. No image/SDK/guest builds.
"""
# SPDX-License-Identifier: MIT
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

class NativeUpdateModelTests(unittest.TestCase):
    def test_budgets(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        icons = module("native_model_icons", ROOT / "tests/host/desktop/icon_assets.py")
        flags = ["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                 "-Wno-macro-redefined", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                 "-ffunction-sections", "-fdata-sections", "-fno-builtin-memcmp", *icons.defines(),
                 "-I", str(ROOT / "src/kernel/include"), "-include", str(ROOT / "tests/host/desktop/replay_base.h")]
        common = [*(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map", "recent", "client")),
                  "apps/desktop/compositor.c", "apps/desktop/theme.c", "apps/accessories/acc.c"]
        cases = {
            "terminal": ["apps/terminal/app.c", "apps/terminal/commands.c", "apps/terminal/script.c", "apps/terminal/lua.c", "apps/tasks/model.c", "apps/files/model.c", "tests/host/desktop/fake_vfs.c"],
            "gfx": ["apps/gfxdemo/app.c", "apps/gfxdemo/scene.c"],
            "dosbox": [],
            "demo": [],
            "calc": ["apps/accessories/calc_view.c", "apps/accessories/calc_model.c"],
            "clock": ["apps/accessories/clock_view.c", "apps/accessories/clock_model.c"],
            "charmap": ["apps/accessories/charmap_view.c", "apps/accessories/charmap_model.c", "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c"],
            "help": ["apps/accessories/help_view.c", "apps/accessories/help_topics.c"],
            "viewer": ["apps/accessories/viewer_view.c", "apps/accessories/viewer_model.c", "apps/accessories/viewer_stb.c", "apps/accessories/wallpaper.c", "apps/desktop/settings.c", "apps/desktop/protocol.c"],
            "mixer": ["apps/accessories/mixer_view.c"],
            "music": ["apps/music/view.c"],
            "paint": ["apps/paint/paint.c", "apps/paint/io.c", "apps/paint/stb.c", "apps/paint/xform.c"],
            "note": ["apps/ciuknote/app.c", "apps/ciuknote/doc.c"],
            "tasks": ["apps/tasks/view.c", "apps/tasks/model.c"],
            "ide": ["apps/ciukide/app.c", "apps/ciukide/model.c", "apps/ciukide/job.c", "apps/ciuknote/doc.c"],
            "regedit": ["apps/regedit/app.c", *(str(p.relative_to(ROOT)) for p in sorted((ROOT / "sdk/libreg").glob("*.c")))],
            "files": ["apps/files/app.c", "apps/files/model.c", "apps/files/transfer.c", "tests/host/desktop/fake_vfs.c", "apps/desktop/autoplay_content.c"],
            "control": ["apps/control/app.c", "apps/control/driver_pack_ui.c", "apps/control/display.c", "apps/control/fonts.c", "apps/desktop/settings.c", "apps/desktop/sounds.c",
                        "apps/desktop/wallpaper.c", "apps/desktop/image.c", "apps/desktop/protocol.c", "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c"],
        }
        with tempfile.TemporaryDirectory(prefix="native-update-", dir=parent) as tmp:
            work = Path(tmp)
            (work / "include").mkdir()
            (work / "include/ciuki").symlink_to(ROOT / "sdk/sysroot-overlay/include/ciuki", target_is_directory=True)
            # raw/runtime are compiler/ABI adapters for the included Demo source.
            # Do not write through the SDK header symlink.
            adapter = work / "adapter/ciuki"
            adapter.mkdir(parents=True)
            (adapter / "raw.h").write_text("#include <stdint.h>\nuint32_t ciuki_raw_probe_report(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);\n")
            (adapter / "runtime.h").write_text("#include <stdint.h>\n#define CU_PTR(p) ((uint32_t)(uintptr_t)(p))\nstatic inline int ciuki_error(uint32_t r) { return r >= (uint32_t)-4095 ? -(int32_t)r : 0; }\n")
            (work / "home").mkdir()
            (work / "registry").mkdir()
            env={**os.environ,"HOME":str(work / "home"),"TMPDIR":str(work),"CIUKI_MODEL_REGISTRY":str(work / "registry")}
            flags = flags[:1] + ["-I", str(work / "include")] + flags[1:]
            objects = []
            for i, source in enumerate(common):
                obj = work / f"common-{i}.o"
                subprocess.run([*flags, "-c", str(ROOT / source), "-o", str(obj)], check=True)
                objects.append(str(obj))
            for name, sources in cases.items():
                extra = []
                if name == "demo":
                    extra = ["-I", str(work / "adapter")]
                if name == "terminal":
                    lua = module("native_model_lua", ROOT / "apps/lua/build_lua.py")
                    source = lua.extract(ROOT / "build/downloads/newlib/lua-5.4.8.tar.gz", work, "lua-5.4.8") / "src"
                    sources = sources + [str(p) for p in sorted(source.glob("*.c")) if p.name not in ("lua.c", "luac.c")]
                    extra = ["-I", str(source), "-DLUA_COMPAT_5_3"]
                if name == "gfx":
                    gl = module("native_model_gl", ROOT / "sdk/libgl/build.py")
                    extra = [*gl.includes(), str(gl.host(sanitize=True)), "-Wl,--wrap=gfx_cube", "-Wl,--wrap=gfx_sprites"]
                if name in ("control", "charmap"):
                    ft = module("native_model_ft", ROOT / "sdk/freetype/build.py")
                    extra = [*ft.includes(), str(ft.host())]
                binary = work / name
                command = [*flags, "-DCASE_" + name.upper(), "-Wl,--wrap=poll", "-Wl,--wrap=clock_gettime", "-Wl,--gc-sections", "-Wl,--wrap=memcmp", "-Wl,--wrap=du_fill",
                           str(ROOT / "tests/host/desktop/native_update_model_test.c"), *objects,
                           *(str(ROOT / p) for p in sources), *extra, "-pthread", "-lm", "-o", str(binary)]
                if name=="demo": command[1:1]=["-I",str(work / "adapter"),"-I",str(ROOT / "apps/desktop")]
                with self.subTest(application=name):
                    subprocess.run(command, check=True)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120, env=env)
                    print(result.stdout, end="", flush=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("PASS F2-98a native update budgets", result.stdout)

if __name__ == "__main__":
    unittest.main()
