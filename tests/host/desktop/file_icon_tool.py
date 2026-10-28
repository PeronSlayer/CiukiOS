"""fx-exeicons host tool: the iconx core and libdeskui's file icon service
(tests/host/desktop/file_icons_test.c) built for the host, and helpers that
run its commands. Used by test_file_icons.py and the installer tests, which
check the shortcuts an original installer created (read in place)."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import icon_assets  # noqa: E402

FLAGS = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_POSIX_C_SOURCE=200809L',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined']


def build(out):
    """The tool binary in out (sources: the driver, icons, icon_map, iconx, fileicon)."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'file_icons_test'
    sources = ['tests/host/desktop/file_icons_test.c', *icon_assets.SOURCES]
    subprocess.run([*FLAGS, '-I', str(ROOT / 'apps/libdeskui'), '-I', str(ROOT / 'src/kernel/include'),
                    *icon_assets.defines(), *(str(ROOT / s) for s in sources), '-lpthread', '-o', str(binary)],
                   check=True, cwd=ROOT)
    return binary


def environment(root=None):
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1', ICONS_STORE=str(icon_assets.store()),
               ICONS_PORTRAIT=str(icon_assets.portrait()))
    if root is not None:
        env['FILE_ICONS_ROOT'] = str(root)
    return env


def parse(line):
    out = {}
    for part in line.split():
        if '=' in part:
            k, v = part.split('=', 1)
            out[k] = v
    return out


def run(binary, *args, root=None):
    result = subprocess.run([str(binary), *map(str, args)], env=environment(root), capture_output=True, text=True, timeout=120)
    if result.returncode or 'runtime error' in result.stderr:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


def resolve(binary, root, path, size=32):
    """own/file/index/fallback/loaded/fnv of what the shell shows for path (POSIX, under root)."""
    line = run(binary, 'resolve', path, -1, size, root=root)
    # file= may contain blanks: take it between 'file=' and ' index='.
    out = parse(line)
    if ' file=' in line and ' index=' in line:
        out['file'] = line.split(' file=', 1)[1].split(' index=', 1)[0]
    return out


def extract_fnv(binary, path, index=0, size=32):
    """The fnv of an icon extracted straight from a file (host path), or None."""
    out = parse(run(binary, 'extract', path, index, size))
    return out.get('fnv') if out.get('status') == '0' else None
