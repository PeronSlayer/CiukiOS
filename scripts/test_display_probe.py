#!/usr/bin/env python3
"""Exercise DISPLAY.APP's production VBE probe with controlled BIOS replies."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build/full/display-probe-test")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "display_probe_host"
    subprocess.run([
        "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
        str(ROOT / "scripts/fixtures/display_probe_host.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
