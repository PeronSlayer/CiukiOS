#!/usr/bin/env python3
"""Compile actual wallp.c with host DOS/XMS/compositor stubs and verify pixels.

This CPU-only test covers cache preparation and band rendering. It is not a
replacement for a DOS or graphics-device runtime test.
Run: python3 scripts/test_wallp_cache.py --output DIR
"""
import argparse
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    args.output.mkdir(parents=True, exist_ok=True)
    binary = args.output / 'wallp_cache_host'
    subprocess.run([
        'cc', '-std=c11', '-O2', '-Wno-pointer-to-int-cast',
        '-Wno-misleading-indentation', str(root / 'scripts/fixtures/wallp_cache_host.c'),
        '-o', str(binary),
    ], cwd=root, check=True)
    subprocess.run([str(binary)], cwd=root, check=True)


if __name__ == '__main__':
    main()
