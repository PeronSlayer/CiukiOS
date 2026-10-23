"""Compile modified kernel units with canonical flags; no link or image build."""
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("kernel_build", root / "scripts/build_kernel.py")
recipe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipe)
out = root / "build/host/floppy/target"
out.mkdir(parents=True, exist_ok=True)
flags = [*recipe.CFLAGS, "-I" + str(root / "src/kernel/include")]
units = ["drivers/floppy.c", "drivers/floppy_media.c", "drivers/floppy_platform.c",
         "vm/floppy_model.c", "vm/dosvm_floppy.c", "vm/dosvm.c", "vm/dosvm_bios.c",
         "vm/dosvm_disk.c", "core/init.c", "core/devices.c", "fs/mount.c", "fs/fs_port.c"]
objects = []
for name in units:
    obj = out / (name.replace("/", "_") + ".o")
    subprocess.run(["clang", *flags, "-c", str(root / "src/kernel" / name), "-o", str(obj)], check=True)
    objects.append(obj)
symbols = {}
for obj in objects:
    for line in subprocess.check_output(["nm", "-g", "--defined-only", str(obj)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[1] not in ("W", "V"):
            assert fields[2] not in symbols, (fields[2], symbols.get(fields[2]), obj)
            symbols[fields[2]] = obj
source = out / "sizes.c"
source.write_text('#include "dosvm_internal.h"\n'
                  'const unsigned fdc_bytes = sizeof(struct fdc);\n'
                  'const unsigned floppy_vm_bytes = sizeof(struct dosvm_floppy);\n'
                  'const unsigned dosvm_bytes = sizeof(struct dosvm);\n')
assembly = out / "sizes.s"
subprocess.run(["clang", *flags, "-I" + str(root / "src/kernel/vm"), "-S", str(source), "-o", str(assembly)], check=True)
text = assembly.read_text()
for name in ("fdc_bytes", "floppy_vm_bytes", "dosvm_bytes"):
    match = re.search(r"^" + name + r":.*\n\s*\.long\s+(\d+)", text, re.MULTILINE)
    assert match, name
    print(name + "=" + match[1])
print("PASS: 12 changed i686 units, production frame warnings, no duplicate strong symbols")
