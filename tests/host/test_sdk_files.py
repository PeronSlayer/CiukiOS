"""f2-18: execute unchanged files.lua using pinned newlib and SDK wrappers."""
# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[2]


class LuaFilesTests(unittest.TestCase):
    def test_portable_files_against_sdk_newlib(self):
        if not (ROOT/'build/tools/ciuki-sdk/manifest.json').is_file():
            self.skipTest('SDK absent; build_sdk is a mandatory prerequisite')
        for name in ('lua-5.4.8','lua-5.4.8-tests'):
            if not (ROOT/'build/downloads/newlib'/f'{name}.tar.gz').is_file():
                self.skipTest('pinned Lua archives absent; make lua is a prerequisite')
        spec=importlib.util.spec_from_file_location('libc_files_host',ROOT/'sdk/tests/build_libc_host.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        program=module.build(lua_files=True)
        result=subprocess.run([str(program)],capture_output=True,text=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual(result.stderr,'')
        self.assertIn('testing i/o',result.stdout)
        self.assertIn('testing date/time',result.stdout)
        self.assertIn('host-reference files.lua PASS',result.stdout)
        self.assertIn('host stdout\n',result.stdout)
        self.assertIn('host stderr\n',result.stdout)
        self.assertNotIn('skipping file tests',result.stdout)
        self.assertNotIn('testing popen/pclose and execute',result.stdout)
        self.assertNotIn('testing large files',result.stdout)
