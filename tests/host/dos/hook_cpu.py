"""H50 production hook CPU fixture. No canonical/floppy image or QEMU build."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def prepare():
    out = ROOT / "build/host"
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/kernel/core/vmm.c").read_text()
    aspace = source.split("/* ---- user address spaces ---- */", 1)[1]
    (out / "dosvm_aspace.inc").write_text("#define AS_AUDIT_CHARGE(ns) ((void)0)\n" + aspace + "\n#undef AS_AUDIT_CHARGE\n")
    font = (ROOT / "assets/fonts/native/INCONSOL.CFN").read_bytes()[64:64 + 95 + 95 * 32]
    assert len(font) == 95 + 95 * 32
    (out / "dosvm_font.inc").write_text("const uint8_t font_cfn_regular[] = {" + ",".join(map(str, font)) + "};\n")


def main():
    spec = importlib.util.spec_from_file_location("dos_fixture", ROOT / "scripts/build_dos_image.py")
    dos = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dos)
    scratch = ROOT / "build/host/dos"
    with tempfile.TemporaryDirectory(prefix="hook-cpu-", dir=scratch) as folder:
        work = Path(folder)

        def asm(source, name, definitions=None):
            return dos.assemble(source, work / name, definitions)

        boot = asm("src/boot/full_boot.asm", "boot.bin", {"FAT_TOTAL_SECTORS": dos.SECTORS})
        loader = asm("src/boot/full_stage1_loader.asm", "loader.bin", dos.GEOMETRY)
        files = {
            "/SYSTEM/CIUKIDOS.SYS": asm("src/runtime/ciukidos.asm", "CIUKIDOS.SYS", {**dos.GEOMETRY, **dos.PERSONALITY}),
            "/SYSTEM/SHELL.COM": asm("src/com/dosvm_shell.asm", "SHELL.COM"),
            "/COMMAND.COM": asm("src/com/shell.asm", "COMMAND.COM", {"COMMAND_COMPAT": 1}),
            "/DOSREADY.COM": asm("src/probes/doscompat/dosready.asm", "DOSREADY.COM"),
            "/SHINPUT.TXT": b"Ciuki hook shell fixture\r\n",
            "/AUTOEXEC.BAT": b"\\DOSREADY.COM\r\n",
        }
        for width in (16, 32):
            for source, name in (("dpmi_client", f"DPMI{width}.COM"), ("dpmi_child", f"DPMICH{width}.COM")):
                files["/" + name] = asm(f"src/probes/dos/{source}.asm", name, {"BITS32": int(width == 32)})
        files["/RELBASE/H50TSR.COM"]=bytes.fromhex("ba1000b80031cd21")
        files["/TOOBIG.COM"]=bytes(0xff00)
        files["/H50EXEC.COM"] = asm("tests/host/dos/hook_exec.asm", "H50EXEC.COM")
        header=bytearray(34)
        struct.pack_into("<14H",header,0,0x5a4d,34,1,1,2,0,0,0,0x100,0,0,0,28,0)
        struct.pack_into("<H",header,32,0x1111)
        files["/GOODREL.EXE"]=bytes(header)
        struct.pack_into("<H",header,24,31)  # four-byte relocation escapes the header
        files["/BADREL.EXE"]=bytes(header)
        files["/DPMI4GW.EXE"], files["/SYSTEM/DOS4GW.EXE"] = dos.watcom_build("src/probes/vm/dpmi_dos4gw.c", "DPMI4GW.EXE", work)
        import conformance
        manifest_probes = conformance.build(work / "probes")
        files["/D48/D48CHILD.COM"] = (work / "probes/D48CHILD.COM").read_bytes()
        selected = []
        for row in manifest_probes["cases"]:
            if row["fixture"] not in ("basic", "share"):
                continue
            tag = row["tag"]
            files["/D48/" + row["binary"]] = (work / "probes" / row["binary"]).read_bytes()
            # A separate cwd gives each unchanged COM a fresh namespace.
            selected.append(row)
        fixture = work / "host.fat16"
        dos.write_fat16(fixture, boot, loader, files, directories=("APPS", "RELBASE", "D48", "D48TEST", *("D48TEST/"+row["tag"] for row in selected)))
        manifest = {name: hashlib.sha256(data).hexdigest() for name, data in files.items() if name.endswith((".COM", ".SYS", ".EXE"))}
        (ROOT / "build/codex/f3-50-cpu-hashes.json").write_text(json.dumps(manifest, indent=2) + "\n")
        for variant in (() if os.getenv("H50_CASE") else ("disabled", "absent", "old", "short", "swap", "overlap", "occupied", "outside", "alloc")):
            subprocess.run([str(scratch / "dosvm_hook_cpu"), str(fixture)], cwd=ROOT,
                           env=dict(os.environ, GUEST_NO_FILE_HOOK=variant), timeout=120, check=True)
        if not os.getenv("H50_CASE"):
            subprocess.run([str(scratch / "dosvm_hook_cpu"), str(fixture), "\\DPMI16.COM", "DPMI16", "\\DPMI32.COM", "DPMI32", "\\DPMI4GW.EXE", "DPMI4GW", "\\H50EXEC.COM", "H50EXEC"],
                           cwd=ROOT, env=dict(os.environ, GUEST_BUDGET="30000000"), timeout=120, check=True)
        commands=[]
        slash=chr(92)
        if not os.getenv("H50_CASE"):
            for command, expect in (
                (f"DIR C:{slash}", "DIR"),
                (f"MD C:{slash}H50DIR", f"?+C:{slash}H50DIR"),
                (f"CD C:{slash}H50DIR", "CD"),
                (f"COPY C:{slash}SHINPUT.TXT C:{slash}H50DIR{slash}ONE.TXT", f"?+C:{slash}H50DIR{slash}ONE.TXT"),
                (f"TYPE C:{slash}H50DIR{slash}ONE.TXT", "TYPE"),
                (f"REN C:{slash}H50DIR{slash}ONE.TXT C:{slash}H50DIR{slash}TWO.TXT", f"?+C:{slash}H50DIR{slash}TWO.TXT"),
                (f"DEL C:{slash}H50DIR{slash}TWO.TXT", f"?-C:{slash}H50DIR{slash}TWO.TXT"),
                (f"CD C:{slash}", "CD"),
                (f"RD C:{slash}H50DIR", f"?-C:{slash}H50DIR")):
                commands += ["shell:"+command, expect]
        for row in selected:
            if os.getenv("H50_CASE") and row["tag"] not in os.getenv("H50_CASE").split(","):
                continue
            commands += [f"shell:CD C:{slash}D48TEST{slash}{row['tag']}", "CD", f"C:{slash}D48{slash}{row['binary']}", row["tag"]]
        subprocess.run([str(scratch / "dosvm_hook_cpu"), str(fixture), *commands], cwd=ROOT,
                       env=dict(os.environ, GUEST_BUDGET="30000000"), timeout=240, check=True)


if __name__ == "__main__":
    prepare() if "--prepare" in sys.argv else main()
