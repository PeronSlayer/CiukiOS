"""f5-01: compiler/SDK ABI, production loader, IDE model/UI and guide coverage."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
import resource
import signal
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "build/tools/ciuki-sdk"
TCC = ROOT / "build/apps/tcc"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class NativeDevelopmentTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="native-dev-", dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.env = dict(os.environ, TMPDIR=str(self.work), PYTHONDONTWRITEBYTECODE="1", ASAN_OPTIONS="detect_leaks=0")

    def run_command(self, args):
        p = subprocess.run([str(x) for x in args], capture_output=True, text=True,
                           env=self.env, cwd=ROOT, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return p

    def compiler(self, *args):
        if not (TCC / "manifest.json").is_file():
            self.skipTest("build the offline SDK/desktop/TCC first")
        return self.run_command([TCC / "tcc-host", *([] if args == ("-v",) else ["-B" + str(TCC / "sdk")]), *args])

    def test_ide_model_job_editor_and_navigation(self):
        toolkit = ["draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map"]
        sources = [*(ROOT / f"apps/libdeskui/{n}.c" for n in toolkit),
                   ROOT / "apps/desktop/compositor.c", ROOT / "apps/desktop/theme.c",
                   *(ROOT / f"apps/ciukide/{n}.c" for n in ("app", "model", "job")),
                   ROOT / "apps/ciuknote/doc.c", ROOT / "tests/host/desktop/ciukide_test.c"]
        binary = self.work / "ide-test"
        self.run_command(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                          "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                          "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                          "-I", ROOT / "apps/ciukide", "-I", ROOT / "src/kernel/include", *sources, "-o", binary])
        self.assertIn("PASS", self.run_command([binary]).stdout)

    def test_generated_guide_is_reproducible(self):
        generator = load("native_api_generator", ROOT / "scripts/gen_api_reference.py")
        first, second = generator.generate(), generator.generate()
        self.assertEqual(first, second)
        self.assertEqual(first, (ROOT / "docs/developer-api.md").read_text())
        self.assertNotIn(str(ROOT), first)
        self.assertIn("ciuki_spawn", first)
        self.assertIn("du_window_open", first)
        guide = (ROOT / "docs/developer-guide.md").read_text()
        self.assertLess(len(guide.encode()), 61440)
        for text in ("F7", "F5", "libdeskui", "project.cfg", "/System/sdk", "-ldeskui"):
            self.assertIn(text, guide)

    def test_native_channel_spawn_descriptor_cleanup(self):
        binary = self.work / "process-test"
        inc = self.work / "include/ciuki"
        inc.mkdir(parents=True)
        shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki/spawn.h", inc / "spawn.h")
        shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki/channel.h", inc / "channel.h")
        self.run_command(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                          "-fsanitize=address,undefined", "-I", inc.parent,
                          "-I", ROOT / "src/kernel/include", ROOT / "tests/host/ciukide_process_test.c", "-o", binary])
        self.assertIn("PASS", self.run_command([binary]).stdout)

    def test_target_compiler_stdio_channel_adapter(self):
        inc = self.work / "include/ciuki"
        inc.mkdir(parents=True)
        shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki/channel.h", inc / "channel.h")
        (inc.parent / "reent.h").write_text('#ifndef HOST_CAPTURE_REENT\n#define HOST_CAPTURE_REENT\nstruct _reent { int _errno; };\n#endif\n')
        binary = self.work / "capture-test"
        self.run_command(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                          "-fsanitize=address,undefined", "-I", inc.parent, "-I", ROOT / "src/kernel/include",
                          ROOT / "tests/host/tcc_capture_test.c", "-o", binary])
        self.assertIn("PASS", self.run_command([binary]).stdout)

    def test_upstream_release_bytes_and_pins(self):
        vendor = ROOT / "third_party/tcc"
        pin = json.loads((vendor / "UPSTREAM.json").read_text())
        self.assertEqual(pin["release"], "0.9.27")
        rows = json.loads((vendor / "upstream-files.json").read_text())
        for row in rows:
            b = (vendor / "upstream" / row["path"]).read_bytes()
            self.assertEqual(hashlib.sha1(b"blob " + str(len(b)).encode() + b"\0" + b).hexdigest(), row["sha"])
            self.assertEqual(hashlib.sha256(b).hexdigest(), pin["files"][row["path"]])
        self.assertIn("LESSER GENERAL PUBLIC LICENSE", (vendor / "COPYING").read_text())

    def test_samples_compile_link_and_pass_production_elf_admission(self):
        self.compiler("-v")
        checker = load("native_dev_elf_checker", ROOT / "sdk/tests/check_sdk.py")
        admission = self.work / "elf-check"
        self.run_command(["clang", "-std=c17", "-O1", "-g", "-Wno-macro-redefined", "-fsanitize=address,undefined",
                          "-ffunction-sections", "-Wl,--gc-sections", "-I", ROOT / "src/kernel/include",
                          ROOT / "tests/host/native_elf_check.c", ROOT / "src/kernel/proc/elf.c", "-o", admission])
        for name in ("HelloConsole", "HelloWindow"):
            output = self.work / (name + ".elf")
            self.compiler("-o", output, ROOT / "sdk/samples" / name / "main.c", *(["-ldeskui"] if name == "HelloWindow" else []))
            record = checker.inspect(output)
            self.assertGreater(record["load_memory_bytes"], 0)
            self.assertIn("PASS", self.run_command([admission, output]).stdout)
        for source in (ROOT / "sdk/tests/layout.c",):
            self.compiler("-c", source, "-o", self.work / "layout.o")

    def test_tcc_rejects_unsupported_output_and_failed_layout_assertions(self):
        self.compiler("-v")
        source = self.work / "main.c"
        source.write_text("int main(void) { return 0; }\n")
        for option in ("-shared", "-run", "-m64", "-Wl,-Ttext=0x800000", "-Wl,-oformat=binary"):
            p = subprocess.run([str(TCC / "tcc-host"), "-B" + str(TCC / "sdk"), option,
                                str(source), "-o", str(self.work / "bad")], capture_output=True, text=True, env=self.env)
            self.assertNotEqual(p.returncode, 0, option)
        source.write_text('#include <ciuki/abi.h>\n_Static_assert(sizeof(void *) == 8, "wrong ABI");\n')
        p = subprocess.run([str(TCC / "tcc-host"), "-B" + str(TCC / "sdk"), "-c", str(source),
                            "-o", str(self.work / "bad.o")], capture_output=True, text=True, env=self.env)
        self.assertNotEqual(p.returncode, 0, p.stderr)

    def test_c99_integer_float_runtime_and_public_headers(self):
        self.compiler("-v")
        source = self.work / "runtime.c"
        source.write_text('''#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <ciuki/spawn.h>
#include <ciuki/audio.h>
#include <ciuki/proc.h>
#include <ciuki/channel.h>
#include <ciuki/runtime.h>
volatile uint64_t x = UINT64_C(4294967297);
int main(void) { double f = (double)x; uint64_t y = (uint64_t)f;
  printf("%" PRIu64 " %.2f\\n", y / 3, f); return (int)(y != x); }
''')
        self.compiler("-o", self.work / "runtime.elf", source)
        checker = load("native_dev_runtime_check", ROOT / "sdk/tests/check_sdk.py")
        checker.inspect(self.work / "runtime.elf")

    def test_compiler_output_write_failure_is_not_success(self):
        self.compiler("-v")
        output = self.work / "limited.elf"
        def limited_file():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            resource.setrlimit(resource.RLIMIT_FSIZE, (256, 256))
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        p = subprocess.run([str(TCC / "tcc-host"), "-B" + str(TCC / "sdk"), "-o", str(output),
                            str(ROOT / "sdk/samples/HelloConsole/main.c")], capture_output=True, text=True,
                           env=self.env, timeout=30, preexec_fn=limited_file)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("could not finish output", p.stderr)
        self.assertFalse(output.exists())

    def test_tcc_sample_objects_execute_with_native_newlib_host_adapter(self):
        self.compiler("-v")
        sdk = json.loads((SDK / "manifest.json").read_text())
        cc = SDK / "bin/ciuki-cc"
        lib = SDK / "sysroot/lib"
        start, host, window = (self.work / (name + ".o") for name in ("start", "host", "window"))
        self.run_command([cc, "-c", ROOT / "tests/host/native_samples_start.S", "-o", start])
        self.run_command([cc, "-c", ROOT / "tests/host/native_samples_host.c", "-o", host])
        self.run_command([cc, "-I", TCC / "sdk/include", "-c", ROOT / "tests/host/native_samples_window.c", "-o", window])
        programs = []
        for name in ("HelloConsole", "HelloWindow"):
            obj, program = self.work / (name + ".o"), self.work / (name + "-host")
            self.compiler("-c", ROOT / "sdk/samples" / name / "main.c", "-o", obj)
            self.run_command([sdk["tools"]["ld.lld"]["path"], "-m", "elf_i386", "-static", "--no-undefined",
                              "--gc-sections", "--wrap=ciuki_syscall", "-L", lib, "-T", lib / "ciuki.ld",
                              "-o", program, start, host, obj, *([window] if name == "HelloWindow" else []),
                              "--start-group", "-lpthread", "-lciuki", "-lc", "-lm", "-lcompiler",
                              TCC / "sdk/lib/libtcc1.a", "--end-group"])
            programs.append((name, program))
        blocked = []
        for name, program in programs:
            p = subprocess.run([str(program)], capture_output=True, text=True, env=self.env, timeout=30,
                               preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            if p.returncode == -31:  # SIGSYS: this sandbox forbids Linux's i386 int 80h ABI.
                blocked.append(name)
                continue
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertEqual(p.stderr, "")
            self.assertIn("Hello from CiukiOS! 4294967297" if name == "HelloConsole" else "sample callback: PASS", p.stdout)
        if blocked:
            self.skipTest("Linux i386 execution rejected with SIGSYS by sandbox: " + ", ".join(blocked))

    def test_installed_sdk_samples_and_licence_inventory(self):
        self.compiler("-v")
        build = load("native_dev_tcc_build", ROOT / "apps/tcc/build_tcc.py")
        paths, dirs, metadata = build.image_payloads()
        for name in ("/Programs/tcc", "/System/sdk/lib/libtcc1.a", "/System/sdk/lib/libdeskui.a",
                     "/System/sdk/lib/crt0.o", "/System/sdk/include/ciuki/abi.h", "/System/sdk/include/deskui.h",
                     "/System/sdk/TCC.TXT", "/Documents/Developer/Guide.txt",
                     "/Documents/Developer/Samples/HelloWindow/main.c", "/Documents/Developer/API/deskui.h.txt"):
            self.assertIn(name, paths)
            self.assertTrue(paths[name].is_file())
        self.assertIn("/Documents/Developer/Samples/HelloConsole", dirs)
        self.assertEqual(paths["/Documents/Developer/Guide.txt"].read_bytes(), (ROOT / "docs/developer-guide.md").read_bytes())
        self.assertEqual(metadata["release"], "0.9.27")

    @unittest.skipUnless(all(shutil.which(t) for t in ("mkfs.fat", "mmd", "mcopy", "mdir", "mtype")), "requires mtools")
    def test_sdk_and_developer_payloads_roundtrip_on_fat32(self):
        self.compiler("-v")
        build = load("native_dev_fat_tcc", ROOT / "apps/tcc/build_tcc.py")
        image = load("native_dev_fat_image", ROOT / "scripts/build_image.py")
        paths, dirs, _ = build.image_payloads()
        volume = self.work / "sdk-fixture.fat"
        with volume.open("wb") as f:
            f.truncate(64 * 1024 * 1024)
        self.run_command(["mkfs.fat", "-F", "32", volume])
        image.populate_payloads(volume, paths, dirs)
        offset = image.PART_LBA
        try:
            image.PART_LBA = 0
            image.check_payloads(volume, image.payload_records(paths), dirs)
        finally:
            image.PART_LBA = offset
