"""Offline authored NSIS/CAB/INF/self-extractor PE fixtures; no image build."""
# SPDX-License-Identifier: MIT
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts import build_nsis
from scripts.win32 import cabinet, gdi32

SOURCE = ROOT / 'tests/win32/fixtures/nsis'
CHAIN = ROOT / 'tests/win32/fixtures/installers'
NAME = 'ciuki-installer-fixture.exe'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def authored_pe(out, name, source, dlls):
    """Compile the authored stubs, deriving checked import libraries from nm."""
    import re
    obj = out / (name + '.obj')
    subprocess.run(['clang', *gdi32.FLAGS, '-I' + str(ROOT / 'sdk/win32/include'),
                    '-I' + str(ROOT / 'src/kernel/include'), '-c', str(source), '-o', str(obj)], check=True)
    symbols = subprocess.run(['llvm-nm', '-u', obj], check=True, capture_output=True, text=True).stdout
    libraries = []
    by_dll = {}
    for symbol in re.findall(r'\bU (\S+)', symbols):
        match = re.fullmatch(r'_([A-Za-z]\w*)@(\d+)', symbol)
        if not match:
            raise ValueError('unexpected fixture import: ' + symbol)
        function, size = match.groups()
        by_dll.setdefault(dlls.get(function, 'kernel32'), []).append(function + '@' + size)
    for dll, names in by_dll.items():
        definition = out / (name + '-' + dll + '.def')
        definition.write_text(f'LIBRARY {dll}.dll\nEXPORTS\n' + ''.join(' ' + n + '\n' for n in sorted(names)))
        library = definition.with_suffix('.lib')
        subprocess.run(['llvm-dlltool', '-m', 'i386', '-k', '-d', definition, '-l', library], check=True)
        libraries.append(library)
    image = out / name
    subprocess.run(['lld-link', '/machine:x86', '/entry:entry@0', '/nodefaultlib', '/safeseh:no',
                    '/timestamp:0', '/incremental:no', '/base:0x00400000', '/subsystem:windows,4.0',
                    '/osversion:4.0', '/out:' + str(image), obj, *libraries], check=True)
    return image


def build(out, compiler=None):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    compiler = compiler or build_nsis.build()
    record = json.loads((Path(compiler).parent / 'build.json').read_text())
    child = authored_pe(out, 'child.exe', CHAIN / 'child.c', {})
    for name in ('fixture.nsi', 'payload.txt'):
        shutil.copyfile(SOURCE / name, out / name)
    for name in ('fixture.nsi', 'payload.txt', 'child.exe'):
        os.utime(out / name, (946684800, 946684800))
    env = dict(os.environ, NSISDIR=record['data'], LC_ALL='C', TZ='UTC', SOURCE_DATE_EPOCH='946684800')
    # Twice in the same canonical output directory: no copied installer/image.
    hashes = []
    for _ in range(2):
        compiled = subprocess.run([compiler, '-NOCONFIG', '-V2', out / 'fixture.nsi'], env=env,
                                  check=True, capture_output=True, text=True)
        if re.search(r'\bwarning\b', compiled.stdout + compiled.stderr, re.I):
            raise ValueError('NSIS fixture compiler warning: ' + compiled.stdout + compiled.stderr)
        hashes.append(sha(out / NAME))
    if hashes[0] != hashes[1]:
        raise ValueError('NSIS fixture is not reproducible')
    shutil.copyfile(ROOT / 'tests/host/win32/installers/chain.inf', out / 'chain.inf')
    (out / 'fixture.cab').write_bytes(cabinet.mszip({'payload.txt': (SOURCE / 'payload.txt').read_bytes(),
                                                 'chain.inf': (out / 'chain.inf').read_bytes(),
                                                 'child.exe': child.read_bytes()}))
    stub = authored_pe(out, 'selfextract.exe', CHAIN / 'selfextract.c', {})
    inf = authored_pe(out, 'infchain.exe', CHAIN / 'infchain.c', {})
    shutil.copyfile(ROOT / 'third_party/nsis/COPYING', out / 'NSIS-COPYING.TXT')
    result = {'schema': 1, 'engine': 'NSIS 2.51', 'pin': json.loads(build_nsis.PIN.read_text()),
              'compiler_sha256': sha(compiler), 'identical_runs': 2,
              'files': {n: sha(out / n) for n in (NAME, 'child.exe', 'fixture.cab', 'chain.inf', stub.name, inf.name, 'NSIS-COPYING.TXT')},
              'sources': {p.name: sha(p) for p in (*sorted(SOURCE.iterdir()), *sorted(CHAIN.iterdir()))
                          if p.is_file()}, 'execution': 'not_run'}
    (out / 'installer-fixtures.json').write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return result


if __name__ == '__main__':
    print(json.dumps(build(ROOT / 'build/host/win32/installers/fixtures'), indent=2))
