"""f2-33 Task Manager: model and widget host tests, pinned renders, QEMU case shape."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture_frame = _review.capture

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c",
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c", "apps/desktop/theme.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
INCLUDES = ["-I", str(ROOT / "apps/tasks"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
# Production renders at 400x420 on the f2-31 toolkit (9239aca) from fixed
# model data (tasks_view_test.c), reviewed 2026-10-11. Any toolkit or Task
# Manager visual change must update these deliberately.
# wt/ui-integ (2026-10-12): Tango store icons (wt/icons) and the ui-look theme, fonts and controls together;
# every frame is PIN-FROM-FIRST-RUN until the lead's visual review.
RENDERS = {
    "tasks-applications.xrgb": "PIN-FROM-FIRST-RUN",
    "tasks-newtask.xrgb": "PIN-FROM-FIRST-RUN",
    "tasks-performance.xrgb": "PIN-FROM-FIRST-RUN",
    "tasks-processes.xrgb": "PIN-FROM-FIRST-RUN",
}

# ui-look (2026-10-12): the new theme engine, fonts and controls changed every
# rendered frame. Entries set to PIN-FROM-FIRST-RUN print their new digest and
# are re-pinned by the lead after a visual review; pinned entries still compare.
PIN = "PIN-FROM-FIRST-RUN"


def assert_pins(test, actual, pinned, label):
    test.assertEqual(sorted(actual), sorted(pinned))
    fresh = {k: v for k, v in actual.items() if pinned[k] == PIN}
    if fresh:
        print(label, PIN, json.dumps(fresh, indent=1, sort_keys=True))
    test.assertEqual({k: v for k, v in actual.items() if pinned[k] != PIN}, {k: v for k, v in pinned.items() if v != PIN})


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ICONS = load("tasks_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")


class TasksTests(unittest.TestCase):
    def run_native(self, name, sources, capture=False):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=name + "-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / name
            subprocess.run([*FLAGS, *INCLUDES, *ICONS.defines(), *(str(ROOT / s) for s in sources), "-o", str(binary)],
                           check=True, env=env)
            subprocess.run([str(binary), *([str(folder)] if capture else [])], check=True, env=env)
            for p in folder.glob("*.xrgb"):
                capture_frame(p.stem, p.read_bytes(), 400, 420)
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.glob("*.xrgb"))}

    def test_model(self):
        self.run_native("tasks-model", ["apps/tasks/model.c", "tests/host/desktop/tasks_model_test.c"])

    def test_view_and_renders(self):
        actual = self.run_native("tasks-view", ["apps/tasks/model.c", "apps/tasks/view.c", *TOOLKIT,
                                                "tests/host/desktop/tasks_view_test.c"], True)
        assert_pins(self, actual, RENDERS, "RENDERS")

    def test_qemu_case(self):
        sys.path.insert(0, str(ROOT / "scripts/test"))
        try:
            runner = load("tasks_case_runner", ROOT / "scripts/test/run.py")
            suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
            cases = [c for c in suite["cases"] if c["id"].startswith("tasks-")]
            self.assertEqual([c["id"] for c in cases], ["tasks-qemu-t23"])
            case = cases[0]
            shell = next(c for c in suite["cases"] if c["id"] == "desktop-shell-qemu-t23")
            # Same probe, selector, deadline and evidence as the shell case.
            for field in ("probe", "profile", "selector", "timeout", "expected", "desktop_screen"):
                self.assertEqual(case[field], shell[field], field)
            runner.Actions(case["actions"])
            batches = case["actions"][0]["batches"]
            keys = [(e["data"]["key"]["data"], e["data"]["down"]) for b in batches for e in b["events"] if e["type"] == "key"]
            self.assertEqual(keys[:2], [("f9", True), ("f9", False)])   # F11 is the Files case
            self.assertEqual(keys[-2:], [("a", True), ("a", False)])
            self.assertEqual([k for k, _ in keys].count("a"), 2)         # only the counted A edges
            self.assertFalse({"f11", "f12"} & {k for k, _ in keys})
            typed = "".join(k for k, down in keys[2:-2] if down and len(k) == 1)
            self.assertEqual(typed, "ndemoe")                            # Alt+N, demo, Alt+E
            self.assertLessEqual(sum(b["pause_ms"] for b in batches), 12000)  # inside the 15 s window
            after = next(p for p in case["expected"]["predicates"] if p.get("where", {}).get("stage") == "after")
            self.assertEqual(after["fields"]["keys"], {"eq": 4})
        finally:
            sys.path.remove(str(ROOT / "scripts/test"))

    def test_menu_clicks_follow_the_image_inventory(self):
        """Every scripted desktop case reaches its program through the Ciuki button, Programs and the
        program's row of the Programs submenu (tests/host/desktop/shell_menu.py), with the image's /Programs."""
        programs = sorted(set(re.findall(r'"/Programs/([A-Za-z0-9_.-]+)"', (ROOT / "scripts/build_image.py").read_text())) - {"desktop"})
        menu = load("tasks_shell_menu", ROOT / "tests/host/desktop/shell_menu.py")
        suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
        targets = {"desktop-shell-qemu-t23": "demo", "desktop-shell-qemu-e500": "demo", "files-qemu-t23": "files",
                   "ciuknote-qemu-t23": "ciuknote", "tasks-qemu-t23": "tasks"}
        for case in suite["cases"]:
            if case["id"] not in targets:
                continue
            x, y, clicks = 512, 384, []   # 1024x768; the cursor starts at the centre
            for batch in case["actions"][0]["batches"]:
                for event in batch["events"]:
                    if event["type"] == "rel":
                        if event["data"]["axis"] == "x": x = min(max(x + event["data"]["value"], 0), 1023)
                        else: y = min(max(y + event["data"]["value"], 0), 767)
                    elif event["type"] == "btn" and event["data"]["down"]: clicks.append((x, y))
            menu.check_launch(self, clicks, programs, targets[case["id"]], case["id"])
        self.assertEqual(set(targets) - {c["id"] for c in suite["cases"]}, set())

    def test_image_inventory(self):
        build = load("tasks_build", ROOT / "apps/desktop/build_desktop.py")
        if not (build.OUT / "manifest.json").is_file():
            self.skipTest("run python3 apps/desktop/build_desktop.py first")
        manifest = build.validate_payloads()
        self.assertIn("tasks", manifest["files"])
        self.assertIn("tasks", manifest["elf"])
        image = load("tasks_image", ROOT / "scripts/build_image.py")
        sources, _, _ = image.desktop_payloads()
        self.assertEqual(sources["/Programs/tasks"], build.OUT / "tasks")


if __name__ == "__main__":
    unittest.main()
