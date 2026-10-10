"""f2-20 stack watermark and guarded production fd/FAT/spawn regression."""
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 3]


class KernelStackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / "build/host"
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="kernel-stack-", dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.env = dict(os.environ, TMPDIR=str(cls.work), PYTHONDONTWRITEBYTECODE="1")
        task = (ROOT / "src/kernel/core/task.c").read_text()
        cls.watermark = ('#include <ciuki/kernel.h>\n#include <ciuki/task.h>\n'
                         + function(task, "void task_stack_init(")
                         + function(task, "unsigned task_stack_high_water("))
        cls.size = int(re.search(r"#define KSTACK_SIZE (\d+)u",
                                (ROOT / "src/kernel/include/ciuki/mm.h").read_text())[1])
        cls.binary = cls.work / "fd-stack"
        source = cls.work / "fd-stack.c"
        source.write_text(cls.watermark + f'\n#include "{ROOT / "tests/host/proc/files_stack_test.c"}"\n')
        sources = ["tests/host/proc/signal_legacy.c", "tests/host/proc/desktop_legacy.c",
                   "src/kernel/fs/fs_port.c", "src/kernel/fs/cache.c", "src/kernel/fs/path.c",
                   "src/kernel/fs/fat.c", "src/kernel/fs/vfs.c", "tests/host/fs/fake.c",
                   "src/kernel/lib/sha256.c", "src/kernel/lib/fmt.c"]
        subprocess.run(["clang", "-std=c17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections,-z,now", "-DFS_HOST", "-D_GNU_SOURCE", "-pthread",
                        f'-DCIUKI_FILES_PAYLOAD_BIN="{ROOT / "build/f0/files-payload.elf"}"',
                        "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "src/kernel/fs"),
                        str(source), *[str(ROOT / s) for s in sources], "-o", str(cls.binary)],
                       check=True, env=cls.env, capture_output=True, text=True)

    def test_high_water(self):
        source = self.work / "watermark.c"
        source.write_text(self.watermark + f'\n#include "{ROOT / "tests/host/proc/stack_water_test.c"}"\n')
        binary = self.work / "watermark"
        subprocess.run(["clang", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE",
                        "-I", str(ROOT / "src/kernel/include"), str(source), "-o", str(binary)],
                       check=True, env=self.env, capture_output=True, text=True)
        result = subprocess.run([str(binary)], check=True, env=self.env, capture_output=True, text=True)
        self.assertIn("stack watermark: PASS", result.stdout)

    def test_allocator(self):
        source = (ROOT / "src/kernel/core/vmm.c").read_text()
        first = source.index("void *kstack_alloc(")
        end = source.index("/* ---- user address spaces ---- */", first)
        (self.work / "stack_allocator.inc").write_text(source[first:end])
        binary = self.work / "allocator"
        subprocess.run(["clang", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "src/kernel/include"), "-I", str(self.work),
                        str(ROOT / "tests/host/proc/stack_alloc_test.c"), "-o", str(binary)],
                       check=True, env=self.env, capture_output=True, text=True)
        result = subprocess.run([str(binary)], check=True, env=self.env, capture_output=True, text=True)
        self.assertIn("stack allocator: PASS", result.stdout)

    def run_stack(self, size, mode):
        # Independent mkfs/mtools fixtures, reset by the C harness's sector undo.
        subprocess.run(["python3", str(ROOT / "tests/host/fs/fixtures.py")], check=True,
                       env=self.env, capture_output=True, text=True)
        return subprocess.run([str(self.binary), str(ROOT / "build/host/fs/fat32.img"),
                               str(ROOT / "build/host/fs/fat16.img"), str(ROOT / "build/f0/files-payload.elf"),
                               str(size), mode], env=self.env, capture_output=True, text=True, timeout=30)

    def test_fd_cases_with_interrupt(self):
        result = self.run_stack(self.size, "irq")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        record = re.search(r"case=stack task=fd-host size=(\d+) high_water=(\d+) io_depth=(\d+) irq_depth=(\d+) irq_count=(\d+) irq_water=(\d+)", result.stdout)
        self.assertIsNotNone(record, result.stdout)
        size, water, io_depth, irq_depth, interrupts, irq_water = map(int, record.groups())
        self.assertEqual(size, self.size)
        self.assertGreater(water, 0)
        self.assertLess(water, size)
        self.assertGreater(irq_depth, io_depth)
        self.assertGreater(interrupts, 0)
        self.assertGreaterEqual(water, irq_water)
        self.assertIn("operation=inherit-cloexec expected=-9 observed=-9", result.stdout)
        self.assertIn("operation=share-denied expected=-13 observed=-13", result.stdout)
        print(record[0])

    def test_overflow_hits_guard(self):
        result = self.run_stack(self.size, "overflow")
        self.assertEqual(result.returncode, -signal.SIGSEGV, result.stderr)


if __name__ == "__main__":
    unittest.main()
