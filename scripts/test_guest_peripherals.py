#!/usr/bin/env python3
"""Build and verify the isolated per-session input/audio device layer.

This is model/backend evidence.  It deliberately does not claim Jemm/HDPMI
port interception, original-binary execution, QEMU acceptance, or hardware.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile
import wave

ROOT = Path(__file__).resolve().parents[1]
PINNED_SHA256 = "92fbd00814dc3ad2e448b40edb8eb2b0e6f7572b4accdfa6dc5cf17aa431820f"
DEPENDENCY_FILES = ("src/DBOPL.CPP", "src/DBOPL.H", "src/CONFIG.HPP")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    print("+", " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def extract_dependency(archive, destination):
    if sha(archive) != PINNED_SHA256:
        raise SystemExit("Pinned VSBHDA archive hash mismatch")
    destination.mkdir(parents=True, exist_ok=True)
    found = {}
    with tarfile.open(archive, "r:gz") as source:
        for member in source.getmembers():
            for relative in DEPENDENCY_FILES:
                if member.name.endswith("/" + relative):
                    if not member.isfile() or member.size > 1024 * 1024:
                        raise SystemExit("Invalid dependency member: " + member.name)
                    stream = source.extractfile(member)
                    if stream is None:
                        raise SystemExit("Cannot read dependency member: " + member.name)
                    payload = stream.read()
                    target = destination / Path(relative).name
                    target.write_bytes(payload)
                    found[relative] = {
                        "bytes": len(payload),
                        "sha256": hashlib.sha256(payload).hexdigest(),
                    }
    missing = set(DEPENDENCY_FILES) - set(found)
    if missing:
        raise SystemExit("Pinned archive is missing: " + ", ".join(sorted(missing)))
    return found


def pcm_metrics(path, rate):
    payload = path.read_bytes()
    if not payload or len(payload) % 4:
        raise AssertionError(f"invalid stereo PCM payload: {path}")
    samples = [value[0] for value in struct.iter_unpack("<h", payload)]
    mean = sum(samples) / len(samples)
    ac_rms = math.sqrt(sum((sample - mean) ** 2 for sample in samples) / len(samples))
    metrics = {
        "sample_rate": rate,
        "channels": 2,
        "bits": 16,
        "frames": len(samples) // 2,
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "peak": max(abs(sample) for sample in samples),
        "ac_rms": ac_rms,
        "unique_samples": len(set(samples)),
        "sample_changes": sum(a != b for a, b in zip(samples, samples[1:])),
    }
    wav = path.with_suffix(".wav")
    with wave.open(str(wav), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(payload)
    metrics["wav"] = str(wav)
    metrics["wav_sha256"] = sha(wav)
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/tests/guest-peripherals")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        raise SystemExit("Output directory is not empty; choose a new path to preserve evidence")
    output.mkdir(parents=True, exist_ok=True)
    dependency = extract_dependency(
        ROOT / "third_party/vsbhda/source-75fa4bb.tar.gz", output / "vsbhda-src")

    cc = os.environ.get("CC", "cc")
    cxx = os.environ.get("CXX", "c++")
    common = ["-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
    cflags = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", *common]
    cxxflags = ["-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", *common]
    includes = ["-I" + str(ROOT / "src/vm"), "-I" + str(output / "vsbhda-src")]
    objects = {
        "model": output / "guest_peripherals.o",
        "adapter": output / "guest_opl_dbopl.o",
        "test": output / "test_guest_peripherals.o",
        "dbopl": output / "DBOPL.o",
    }
    commands = []
    command = [cc, *cflags, *includes, "-c", ROOT / "src/vm/guest_peripherals.c",
               "-o", objects["model"]]
    run(command); commands.append(list(map(str, command)))
    command = [cxx, *cxxflags, *includes, "-c", ROOT / "src/vm/guest_opl_dbopl.cpp",
               "-o", objects["adapter"]]
    run(command); commands.append(list(map(str, command)))
    command = [cxx, *cxxflags, *includes, "-c", ROOT / "src/vm/test_guest_peripherals.cpp",
               "-o", objects["test"]]
    run(command); commands.append(list(map(str, command)))
    # Upstream has two sentinel enum values intentionally absent from a switch.
    # Keep warnings visible but do not edit or apply CiukiOS -Werror to upstream.
    command = [cxx, "-std=c++11", "-Wall", "-Wextra", "-Wno-switch", *common,
               *includes, "-c", output / "vsbhda-src/DBOPL.CPP", "-o", objects["dbopl"]]
    run(command); commands.append(list(map(str, command)))
    binary = output / "test-guest-peripherals"
    command = [cxx, "-fsanitize=address,undefined", *objects.values(), "-lm", "-o", binary]
    run(command); commands.append(list(map(str, command)))
    sb_raw, opl_raw = output / "sb-pcm.raw", output / "opl-pcm.raw"
    result = run([binary, sb_raw, opl_raw], text=True, capture_output=True,
                 env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                      "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    match = re.search(r"guest peripherals: (\d+) assertions", result.stdout)
    if not match:
        raise AssertionError("test harness did not report its assertion count")

    sb_metrics = pcm_metrics(sb_raw, 44100)
    opl_metrics = pcm_metrics(opl_raw, 44100)
    if sb_metrics["peak"] < 32000 or sb_metrics["unique_samples"] < 4:
        raise AssertionError("virtual SB PCM was silent or constant")
    if opl_metrics["peak"] < 100 or opl_metrics["unique_samples"] < 64 \
            or opl_metrics["sample_changes"] < 1000:
        raise AssertionError("DBOPL PCM was silent or insufficiently variable")

    watcom = Path(os.environ.get("WATCOM", "/opt/watcom"))
    compiler = watcom / "binl64/wcc386"
    if not compiler.exists():
        compiler = watcom / "binl/wcc386"
    if not compiler.exists():
        raise SystemExit("OpenWatcom wcc386 is required to verify the target object")
    target_object = output / "guest_peripherals-watcom.obj"
    target_binary = output / "guest_peripherals-watcom.bin"
    target_command = [compiler, "-zq", "-bt=dos", "-mf", "-3r", "-ecc", "-zl",
                      "-s", "-w4", "-we", "-i=" + str(watcom / "h"),
                      "-fo=" + str(target_object), ROOT / "src/vm/guest_peripherals.c"]
    run(target_command)
    link_command = [compiler.with_name("wlink"), "option",
                    "quiet,nodefaultlibs,start=_cvgp_init", "format", "raw", "bin",
                    "disable", "1014", "name", target_binary, "file", target_object]
    run(link_command)

    sources = [
        ROOT / "src/vm/guest_peripherals.c",
        ROOT / "src/vm/guest_peripherals.h",
        ROOT / "src/vm/guest_opl_dbopl.cpp",
        ROOT / "src/vm/guest_opl_dbopl.h",
        ROOT / "src/vm/guest_peripheral_scheduler.h",
        ROOT / "src/vm/test_guest_peripherals.cpp",
        ROOT / "scripts/test_guest_peripherals.py",
        ROOT / "third_party/vsbhda/UPSTREAM.json",
        ROOT / "third_party/vsbhda/README.md",
        ROOT / "third_party/vsbhda/LICENSE",
    ]
    report = {
        "result": "PASS",
        "scope": "isolated per-session legacy input and sound device model",
        "assertions": int(match.group(1)),
        "capabilities_tested": [
            "exclusive generation ownership and stale-owner rejection",
            "set-1 make/break including left/right Ctrl and Space",
            "focus-loss release and 64 repeated focus cycles",
            "8042 keyboard and PS/2 mouse queues with IRQ1/IRQ12",
            "8259 PIC and 8254 PIT modes 0/2/3",
            "8237 memory-to-device DMA and terminal count",
            "SB16 reset/version/rate/single-cycle PCM/IRQ/acknowledgement",
            "OPL2 register/timer behavior and pinned DBOPL synthesis",
            "normal cleanup, stale-owner cleanup rejection and DMA error cleanup",
            "explicit word-I/O, string-I/O, PIT, DMA and SB rejection paths",
        ],
        "pcm": {"sound_blaster": sb_metrics, "opl": opl_metrics},
        "dependency": {
            "archive": "third_party/vsbhda/source-75fa4bb.tar.gz",
            "archive_sha256": PINNED_SHA256,
            "commit": "75fa4bbfea70cbcc0c40d1212f04952ff8abbf16",
            "license_sha256": sha(ROOT / "third_party/vsbhda/LICENSE"),
            "extracted_files": dependency,
        },
        "sources": {str(path.relative_to(ROOT)): sha(path) for path in sources},
        "host_commands": commands,
        "target_compiler_command": list(map(str, target_command)),
        "freestanding_link_command": list(map(str, link_command)),
        "target_object_has_no_external_imports": True,
        "integration": {
            "jemm_v86_adapter": False,
            "hdpmi_adapter": False,
            "real_cpu_io_executed": False,
            "original_dos_binary_executed": False,
            "simultaneous_native_ui_executed": False,
            "qemu_accepted": False,
            "physical_hardware_accepted": False,
            "fps_claim": None,
        },
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"guest-peripherals PASS: {report['assertions']} assertions; "
          f"SB peak {sb_metrics['peak']}, OPL RMS {opl_metrics['ac_rms']:.2f}")


if __name__ == "__main__":
    main()
