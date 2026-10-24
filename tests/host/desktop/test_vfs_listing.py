"""Production VFS cost/metadata contract using the target dirent extension."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class VfsListingTests(unittest.TestCase):
    def test_directory_metadata_without_stat_or_sniff(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="vfs-listing-", dir=parent) as tmp:
            folder = Path(tmp)
            # The target overlay exposes these fields. Keep libc's host DIR
            # outside this seam so its implementation cannot hide a stat call.
            (folder / "dirent.h").write_text("""
#ifndef TEST_DIRENT_H
#define TEST_DIRENT_H
#include <stdint.h>
#define CIUKI_DIRENT_METADATA 1
#define DT_REG 8
#define DT_DIR 4
typedef struct { unsigned at; } DIR;
struct dirent { uint64_t d_ino, d_size; int64_t d_mtime; uint32_t d_attributes; unsigned char d_type; char d_name[768]; };
#define opendir test_opendir
#define readdir test_readdir
#define closedir test_closedir
DIR *test_opendir(const char *);
struct dirent *test_readdir(DIR *);
int test_closedir(DIR *);
#endif
""")
            binary = folder / "listing"
            env = dict(os.environ, TMPDIR=tmp, ASAN_OPTIONS="detect_leaks=0")
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(folder),
                            "-I", str(ROOT / "src/kernel/include"),
                            str(ROOT / "tests/host/desktop/vfs_listing_test.c"),
                            str(ROOT / "apps/files/model.c"), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)
