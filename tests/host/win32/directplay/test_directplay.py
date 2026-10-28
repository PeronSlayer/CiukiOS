"""Patched Wine callbacks on deterministic host Winsock/registry fakes.

No host sockets, QEMU, image build or proprietary title payload is used.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[4]
SOURCE = ROOT / 'build/external/wine-10.0'


class DirectPlayProvider(unittest.TestCase):
    def setUp(self):
        if not (SOURCE / 'dlls/dpwsockx/dpwsockx_main.c').is_file():
            self.skipTest('pinned Wine source not staged')
        parent = ROOT / 'build/host/win32/directplay'
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        for module, names in {'dpwsockx': ('dpwsockx_main.c', 'dpwsockx_dll.h', 'Makefile.in'),
                              'dplayx': ('dplobby.c', 'Makefile.in')}.items():
            target = self.directory / 'dlls' / module
            target.mkdir(parents=True)
            for name in names:
                shutil.copyfile(SOURCE / 'dlls' / module / name, target / name)
        for patch in ('0011-directplay-lan.patch', '0012-directplay-lobby.patch'):
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i',
                            str(ROOT / 'third_party/wine/patches' / patch)],
                           cwd=self.directory, check=True, capture_output=True, text=True)
        # Replace only Wine's logging backend. ABI, list, rbtree and COM
        # headers remain the pinned upstream headers.
        debug = self.directory / 'wine/debug.h'
        debug.parent.mkdir()
        debug.write_text('#define WINE_DEFAULT_DEBUG_CHANNEL(x)\n'
                         '#define TRACE(...) ((void)0)\n#define ERR(...) ((void)0)\n'
                         '#define FIXME(...) ((void)0)\n#define WARN(...) ((void)0)\n')
        # These units call neither OLE nor an IDL-generated COM method.
        # Reuse Wine's exact interface declaration macros and supply only
        # IUnknown's three standard slots, avoiding an upper build as a
        # prerequisite just to generate objidlbase.h and its dependencies.
        macros = (SOURCE / 'include/objbase.h').read_text()
        macros = macros[macros.index('#undef STDMETHOD'):macros.index('#include <combaseapi.h>')]
        (self.directory / 'objbase.h').write_text(
            '#ifndef CIUKI_HOST_OLE\n#define CIUKI_HOST_OLE\n'
            '#include "windef.h"\n#include "winbase.h"\n#include "winerror.h"\n'
            + macros + '\n#define INTERFACE IUnknown\n'
            'DECLARE_INTERFACE(IUnknown) {\n'
            ' STDMETHOD(QueryInterface)(THIS_ REFIID, void **) PURE;\n'
            ' STDMETHOD_(ULONG,AddRef)(THIS) PURE;\n'
            ' STDMETHOD_(ULONG,Release)(THIS) PURE;\n};\n#undef INTERFACE\n'
            'typedef IUnknown *LPUNKNOWN;\n'
            'DEFINE_GUID(IID_IUnknown,0,0,0,0xc0,0,0,0,0,0,0,0x46);\n'
            'DEFINE_GUID(GUID_NULL,0,0,0,0,0,0,0,0,0,0,0);\n'
            '#define IUnknown_AddRef(p) (p)->lpVtbl->AddRef(p)\n'
            '#define IUnknown_QueryInterface(p,a,b) (p)->lpVtbl->QueryInterface(p,a,b)\n'
            'HRESULT WINAPI CLSIDFromString(const WCHAR *, CLSID *);\n'
            'int WINAPI StringFromGUID2(REFGUID, WCHAR *, int);\n#endif\n')
        (self.directory / 'ole2.h').write_text('#include "objbase.h"\n')

    def compile_and_run(self, kind, include, expected):
        if kind == 'lobby':
            # Select the verbatim helper definitions under test. ASan keeps
            # the full file's COM vtables alive, which otherwise drags in the
            # unrelated lobby message/session engine and its OS services.
            source = (self.directory / include).read_text()
            prefix = source[:source.index('static void dplobby_destroy')]
            enumeration = source[source.index('static HRESULT DPL_EnumApplications('):
                                 source.index('static HRESULT WINAPI IDirectPlayLobby3Impl_EnumLocalApplications(')]
            registration = source[source.index('static WCHAR *DPL_Wide('):
                                  source.index('static HRESULT WINAPI IDirectPlayLobby3Impl_RegisterApplication(')]
            body = prefix + enumeration + registration
        else:
            body = '#include "' + include + '"\n'
        (self.directory / (kind + '-under-test.c')).write_text(body)
        program = self.directory / (kind + '-test')
        command = ['clang', '-std=gnu11', '-O1', '-g', '-fshort-wchar',
                   '-fms-extensions', '-D__WINESRC__', '-D_DEFAULT_SOURCE',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                   '-Wno-pragma-pack', '-Wno-format', '-Wno-incompatible-pointer-types-discards-qualifiers',
                   '-I' + str(self.directory), '-I' + str(SOURCE / 'include'),
                   '-I' + str(SOURCE / 'dlls/dplayx'),
                   str(Path(__file__).with_name(kind + '_test.c')), '-o', str(program)]
        build = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        # Upstream .c is included only in each standalone test executable.
        # Verify definitions instead of assuming no symbol can be duplicated.
        symbols = subprocess.check_output(['nm', '-g', '--defined-only', program], text=True)
        names = [row.split()[-1] for row in symbols.splitlines() if row.split()]
        self.assertEqual(len(names), len(set(names)))
        run = subprocess.run([program], capture_output=True, text=True, timeout=60)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(expected, run.stdout)
        print(run.stdout.strip())

    def test_patched_wine_provider_on_winsock_model(self):
        self.compile_and_run('provider', 'dlls/dpwsockx/dpwsockx_main.c', 'PASS patched Wine TCP provider')

    def test_patched_wine_lobby_on_registry_model(self):
        self.compile_and_run('lobby', 'dlls/dplayx/dplobby.c', 'PASS patched Wine lobby')
