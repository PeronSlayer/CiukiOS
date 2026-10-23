"""Production LPT engine, native boundary and DOS INT17 register fixture."""
from pathlib import Path
import os
import runpy
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LptTests(unittest.TestCase):
    def test_register_model(self):
        out = ROOT / "build/host/lpt"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / "lpt_test"
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-I", str(ROOT / "src/kernel/include"),
                        str(ROOT / "tests/host/lpt_test.c"), "-o", str(binary)], check=True)
        subprocess.run([binary], check=True)

    def test_target_compile(self):
        out = ROOT / "build/host/lpt"
        out.mkdir(parents=True, exist_ok=True)
        flags = runpy.run_path(str(ROOT / "scripts/build_kernel.py"))["CFLAGS"]
        sources = ("drivers/lpt.c", "drivers/lpt_native.c", "vm/dosvm_lpt.c",
                   "vm/dosvm_bios.c", "vm/dosvm.c", "vm/dosvm_vga.c", "core/init.c", "core/devices.c",
                   "proc/posixpath.c", "proc/fdtable.c", "proc/syscalls_file.c")
        for rel in sources:
            source = ROOT / "src/kernel" / rel
            subprocess.run(["clang", *flags, "-I", str(ROOT / "src/kernel/include"),
                            "-fstack-usage", "-c", str(source), "-o", str(out / (source.stem + ".o"))],
                           check=True, env={**os.environ, "TMPDIR": str(out)})
        subprocess.run(["ld.lld", "-m", "elf_i386", "-r",
                        *(str(out / (name + ".o")) for name in ("lpt", "lpt_native", "dosvm_lpt")),
                        "-o", str(out / "lpt-service.o")], check=True)
        client = out / "sdk-client.c"
        client.write_text('#include <ciuki/lpt.h>\n'
                          'int lpt_client(int fd) { struct ciuki_lpt_status s = {.version=1,.size=sizeof(s)}; '
                          'return ciuki_devctl(fd,LPT_GET_STATUS,&s,sizeof(s)); }\n')
        subprocess.run(["clang", *flags, "-I", str(ROOT / "sdk/sysroot-overlay/include"),
                        "-I", str(ROOT / "src/kernel/include"), "-c", str(client),
                        "-o", str(out / "sdk-client.o")], check=True)
        print("PASS LPT i686 canonical flags: eleven integration units and relocatable service link")


if __name__ == "__main__":
    unittest.main()
