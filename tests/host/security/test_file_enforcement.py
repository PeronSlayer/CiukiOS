"""H04: every file, handle and mapping entry point enforces the retained subject.

Links the production native syscalls/fd table/POSIX namespace, the legacy
VFS tables, the Win32 drive service, executable snapshots and the
managed-volume policy/OWNER code on the real FAT/VFS (fake block device,
in-memory OWNER blobs, stub Win32 handle table). A source census checks
that no user frontend calls the kernel-internal (PX_KERNEL) entry points.
"""
import os
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/security'
OWNER = [f'src/kernel/fs/{name}.c' for name in (
    'owner', 'owner_wire', 'owner_store', 'owner_checkpoint', 'owner_roots', 'owner_vfs',
    'security_policy', 'security_path')]


class FileEnforcementTests(unittest.TestCase):
    def test_production_enforcement(self):
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        subprocess.run(['python3', '-B', str(ROOT / 'tests/host/fs/fixtures.py')], check=True, env=env)
        sources = ['tests/host/security/file_enforcement_test.c', 'tests/host/proc/signal_legacy.c',
                   'tests/host/proc/media_legacy.c', 'tests/host/proc/desktop_legacy.c',
                   'src/kernel/fs/fs_port.c', 'src/kernel/fs/blkreq.c', 'src/kernel/fs/cache.c',
                   'src/kernel/fs/path.c', 'src/kernel/fs/fat.c', 'src/kernel/fs/vfs.c',
                   'src/kernel/fs/fs_ro.c', 'src/kernel/fs/iso9660.c', 'tests/host/fs/fake.c',
                   'src/kernel/probes/f2_probes_files.c', 'src/kernel/lib/sha256.c', *OWNER]
        program = OUT / 'file_enforcement'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread',
                        '-I', str(ROOT / 'src/kernel/include'), '-I', str(ROOT / 'src/kernel/fs'),
                        *[str(ROOT / s) for s in sources], '-o', str(program)], check=True, env=env)
        result = subprocess.run([str(program), str(ROOT / 'build/host/fs/fat32.img'),
                                 str(ROOT / 'build/host/fs/fat16.img')],
                                env=env, capture_output=True, text=True, timeout=300)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('H04 file enforcement', result.stdout)

    def test_frontend_census(self):
        """User frontends reach files only through subject-attributed calls."""
        kernel_only = re.compile(r'\bpx_(open|mkdir|remove|rename|file_info_set)_locked\(')
        frontends = ['src/kernel/proc/syscalls_file.c', 'src/kernel/proc/win32/files.c',
                     'src/kernel/proc/win32/loader.c', 'src/kernel/proc/win32/process.c',
                     'src/kernel/proc/win32/sections.c', 'src/kernel/proc/win32/personality.c']
        for name in frontends:
            text = (ROOT / name).read_text()
            self.assertIsNone(kernel_only.search(text), name)
            self.assertNotIn('px_resolve_locked(', text, name)
        fdtable = (ROOT / 'src/kernel/proc/fdtable.c').read_text()
        self.assertIn('px_open_as_locked(cwd->space, cwd, path, flags, d, px_process_subject(p))', fdtable)
        for symbol in ('int file_description_authorize_locked(', 'int security_exec_open(',
                       'int file_image_open_as(', 'int file_image_authorize('):
            self.assertIn(symbol, fdtable)
        sections = (ROOT / 'src/kernel/proc/win32/sections.c').read_text()
        self.assertEqual(sections.count('section_authorize('), 3)
        loader = (ROOT / 'src/kernel/proc/win32/loader.c').read_text()
        self.assertIn('file_image_authorize(p->cwd, p->subject, native)', loader)
        vfs = (ROOT / 'src/kernel/fs/vfs.c').read_text()
        for function in ('int vfs_open_claim(', 'int vfs_stat(', 'static int metadata(',
                         'int vfs_find_first(', 'static int remove_path(', 'int vfs_mkdir(',
                         'int vfs_rename(', 'int vfs_chdir('):
            body = vfs[vfs.index(function):]
            body = body[:body.index('\n}\n')]
            self.assertTrue('table_authorize(' in body or 'px_table_rename_locked' in body, function)


if __name__ == '__main__':
    unittest.main()
