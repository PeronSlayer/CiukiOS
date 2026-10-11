"""f2-33 Task Manager: model and widget host tests, pinned renders, QEMU case shape."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
INCLUDES = ["-I", str(ROOT / "apps/tasks"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
# Production renders at 440x470 from fixed model data (tasks_view_test.c). Any
# toolkit or Task Manager visual change must update these deliberately.
RENDERS = {
    "tasks-applications.xrgb": "70ad7f1b01d08c85bed236ca13e555040ea033056ac57c988000808b93747254",
    "tasks-newtask.xrgb": "04207d196e85dc05996f1606ea5e91ee855054e51438e78ff2fd859e9282d365",
    "tasks-performance.xrgb": "30311a4854914f26d56c54195e923927633c30cbc2be75089ef96187a358bb4c",
    "tasks-processes.xrgb": "61655ebff0963b04004f2c36ae5dddf9367ac8dc739b801ca70379c514138e15",
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TasksTests(unittest.TestCase):
    def run_native(self, name, sources, capture=False):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=name + "-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / name
            subprocess.run([*FLAGS, *INCLUDES, *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary), *([str(folder / "tasks")] if capture else [])], check=True, env=env)
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.glob("*.xrgb"))}

    def test_model(self):
        self.run_native("tasks-model", ["apps/tasks/model.c", "tests/host/desktop/tasks_model_test.c"])

    def test_view_and_renders(self):
        actual = self.run_native("tasks-view", ["apps/tasks/model.c", "apps/tasks/view.c", "apps/libdeskui/draw.c",
                                                "apps/libdeskui/widgets.c", "apps/desktop/compositor.c",
                                                "tests/host/desktop/tasks_view_test.c"], True)
        self.assertEqual(actual, RENDERS)

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
            self.assertEqual(keys[:2], [("f11", True), ("f11", False)])
            self.assertEqual(keys[-2:], [("a", True), ("a", False)])
            self.assertEqual([k for k, _ in keys].count("a"), 2)         # only the counted A edges
            self.assertNotIn("f12", [k for k, _ in keys])
            typed = "".join(k for k, down in keys[2:-2] if down and len(k) == 1)
            self.assertEqual(typed, "ndemoe")                            # Alt+N, demo, Alt+E
            self.assertLessEqual(sum(b["pause_ms"] for b in batches), 12000)  # inside the 15 s window
            after = next(p for p in case["expected"]["predicates"] if p.get("where", {}).get("stage") == "after")
            self.assertEqual(after["fields"]["keys"], {"eq": 4})
        finally:
            sys.path.remove(str(ROOT / "scripts/test"))

    def test_image_inventory(self):
        build = load("tasks_build", ROOT / "apps/desktop/build_desktop.py")
        if not (build.OUT / "manifest.json").is_file():
            self.skipTest("run python3 apps/desktop/build_desktop.py first")
        manifest = build.validate_payloads()
        self.assertIn("tasks", manifest["files"])
        self.assertIn("tasks", manifest["elf"])
        image = load("tasks_image", ROOT / "scripts/build_image.py")
        sources, _, _ = image.desktop_payloads()
        self.assertEqual(sources["/bin/tasks"], build.OUT / "tasks")


if __name__ == "__main__":
    unittest.main()
