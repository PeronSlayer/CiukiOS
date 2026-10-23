# Shared generated host fixtures; no kernel/image build.
# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import subprocess
import sys
root, out = map(Path, sys.argv[1:])
source = (root / "src/kernel/core/vmm.c").read_text()
aspace = source.split("/* ---- user address spaces ---- */", 1)[1]
# F0-44: charge actual production teardown PTE/PDE visits, including holes.
aspace = aspace.replace("uint32_t old = pt[j];", "AS_AUDIT_CHARGE(1000); uint32_t old = pt[j];")
aspace = aspace.replace("if (pd[i] & PTE_P) {", "AS_AUDIT_CHARGE(1000); if (pd[i] & PTE_P) {")
(out / "dosvm_aspace.inc").write_text(
    "#ifndef AS_AUDIT_CHARGE\n#define AS_AUDIT_CHARGE(ns) ((void)0)\n#endif\n" + aspace + "\n#undef AS_AUDIT_CHARGE\n")
# F4-13: replay the exact production cursor functions beside VM sources,
# replacing only the register transport, as for the paging functions above.
source = (root / "src/kernel/drivers/ac97.c").read_text()
# Include channel helpers and ALi's raw read used by the cursor, excluding
# codec/global-write helpers that this standalone cursor fixture never calls.
helpers = source[source.index("static unsigned ac97_offset("):source.index("static void raw_wr(")]
cursor = source[source.index("int ac97_observe("):source.index("int ac97_publish(")]
(out / "dosvm_audio_cursor.inc").write_text(helpers + cursor)
# f3-02: the exact bytes src/kernel/arch/font.asm includes (offset 64,
# 95 widths + 95 glyphs x 16 u16 rows), for the VM's synthetic ROM fonts.
font = (root / "assets/fonts/native/INCONSOL.CFN").read_bytes()[64:64 + 95 + 95 * 32]
assert len(font) == 95 + 95 * 32
rows = ",\n".join(", ".join(str(b) for b in font[i:i + 24]) for i in range(0, len(font), 24))
(out / "dosvm_font.inc").write_text("const uint8_t font_cfn_regular[%d] = {\n%s\n};\n" % (len(font), rows))
# F3-25: execute the exact DOS MZ relocation validator, without assembling
# a kernel or an image. The harness supplies ES:0 through the private disk.
source = (root / "src/boot/floppy_stage1.asm").read_text()
start = source.index("int21_exec_validate_mz_reloc_table:\n")
end = source.index("int21_exec_validate_mz_reloc_target:\n", start)
validator = out / "dosvm_mz_reloc.asm"
validator.write_text("bits 16\norg 0\ncall int21_exec_validate_mz_reloc_table\nint3\n" + source[start:end])
binary = out / "dosvm_mz_reloc.bin"
subprocess.run(["nasm", "-f", "bin", str(validator), "-o", str(binary)], check=True)
data = binary.read_bytes()
(out / "dosvm_mz_reloc.inc").write_text("static const uint8_t mz_reloc_code[] = {" +
    ",".join(str(b) for b in data) + "};\n")
# F3-27: execute the production boot sector through the active directory,
# after the real worker's startup diagnostic has yielded. No image needed.
boot = out / "dosvm_boot.bin"
subprocess.run(["nasm", "-f", "bin", str(root / "src/boot/full_boot.asm"),
                "-DFAT_TOTAL_SECTORS=262144", "-o", str(boot)], check=True)
data = boot.read_bytes()
assert len(data) == 512 and data[510:] == b"\x55\xaa"
(out / "dosvm_boot.inc").write_text("static const uint8_t startup_boot_sector[] = {" +
    ",".join(str(b) for b in data) + "};\n")
