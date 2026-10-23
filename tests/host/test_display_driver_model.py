"""Sequential host evidence for the display model. No image/guest execution.

UBSan is the default. CIUKI_DISPLAY_ASAN=1 additionally enables ASan with
its default leak detection; no sanitizer option is suppressed here.
"""
from pathlib import Path
import os
import importlib.util
import ast
import json
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DisplayDriverTests(unittest.TestCase):
    def test_integrated_nomination_tables(self):
        # Check the production build graph, not a second hand-maintained list.
        # Preprocess the real match macros to catch cross-family PCI collisions.
        builder = ast.parse((ROOT/'scripts/build_kernel.py').read_text())
        sources = []
        for node in ast.walk(builder):
            if not isinstance(node, ast.ListComp) or len(node.generators) != 1:
                continue
            part = node.elt
            if not (isinstance(part, ast.BinOp) and isinstance(part.left, ast.BinOp)
                    and isinstance(part.left.right, ast.Constant)
                    and str(part.left.right.value).startswith('drivers/video')):
                continue
            sources.extend(part.left.right.value+'/'+name
                           for name in ast.literal_eval(node.generators[0].iter))
        expected = {p.relative_to(ROOT/'src/kernel').as_posix()
                    for p in (ROOT/'src/kernel/drivers/video').rglob('*.c')}
        self.assertEqual(set(sources), expected)
        self.assertEqual(len(sources), len(set(sources)))
        objects = [Path(p).parent.name+'_'+Path(p).stem for p in sources]
        self.assertEqual(len(objects), len(set(objects)))
        registry = (ROOT/'src/kernel/drivers/video/registry.c').read_text()
        body = re.search(r'const drivers\[\]\s*=\s*\{(.*?)\};', registry, re.S)[1]
        order = re.findall(r'&(\w+)', body)
        entries, owners = {}, {}
        for path in sources:
            source = ROOT/'src/kernel'/path
            if not re.search(r'const struct display_driver\s+\w+\s*=|S3_ADAPTER\(', source.read_text()):
                continue
            text = subprocess.check_output(['clang', '--target=i686-unknown-elf',
                '-ffreestanding', '-I', str(ROOT/'src/kernel/include'),
                '-E', '-P', str(source)], text=True)
            tables = {}
            for name, body in re.findall(r'static const struct display_match\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};', text, re.S):
                rows = re.findall(r'\{([^{}]+)\}', body)
                tables[name] = [tuple(int(x.strip().rstrip('uUlL'), 0)
                                      for x in row.split(',')) for row in rows]
            for name, body in re.findall(r'const struct display_driver\s+(\w+)\s*=\s*\{(.*?)\};', text, re.S):
                table = re.search(r'\.matches\s*=\s*(\w+)', body)
                if not table or name not in order:
                    continue
                self.assertNotIn(name, entries)
                entries[name] = tables[table[1]]
                for vendor, device, klass, mask in entries[name]:
                    self.assertEqual((klass, mask), (3, 255))
                    key = (vendor, device)
                    self.assertNotIn(key, owners, f'PCI collision {key}: {name}, {owners.get(key)}')
                    owners[key] = name
        self.assertEqual(set(entries), set(order))
        vendors = [entries[name][0][0] for name in order]
        self.assertEqual(vendors, sorted(vendors))
        output = ROOT/'build/host/video-nomination-tables.json'
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(entries, indent=2)+'\n')
        print(f'Integrated nominations: {len(order)} descriptors, {len(owners)} exclusive PCI pairs, {len(sources)} sources PASS')

    def run_host(self, sources, extra=(), wrapper=False, arguments=()):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='display-model-',dir=folder) as tmp:
            tmp = Path(tmp)
            if wrapper:
                source = (ROOT/'sdk/libciuki/posix.c').read_text()
                first = source.index('int ciuki_display_engine_info(')
                last = source.index('\nint ciuki_display_info(',first)
                (tmp/'display_query_wrapper.inc').write_text(source[first:last])
            binary = tmp/'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize='+sanitizer,'-fno-sanitize-recover=undefined',
                '-I',str(ROOT/'src/kernel/include'),'-I',str(tmp),*extra,
                *(str(ROOT/s) for s in sources),'-o',str(binary)],check=True)
            subprocess.run([str(binary),*arguments],check=True)

    def test_abi_oracle(self):
        folder=ROOT/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='display-abi-',dir=folder) as tmp:
            output=Path(tmp)/'target.json'
            subprocess.run(['python3',str(ROOT/'scripts/test/abi_layout_dump.py'),'--output',str(output)],check=True)
            self.run_host(['tests/host/abi_layout_test.c'],arguments=[str(output)])

    def test_target_translation_units(self):
        spec=importlib.util.spec_from_file_location('display_kernel_flags',ROOT/'scripts/build_kernel.py')
        module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        sources=[
            'drivers/video/attempt.c',
            'drivers/video/chipshiqv/chipshiqv.c',
            'drivers/video/chipshiqv/hiqv_engine.c',
            'drivers/video/cirrus/driver.c',
            'drivers/video/cirrus/engine.c',
            'drivers/video/i740/driver.c',
            'drivers/video/i810/driver.c',
            'drivers/video/i810/memory.c',
            'drivers/video/intel2d/driver.c',
            'drivers/video/intel2d/engine.c',
            'drivers/video/mach64/driver.c',
            'drivers/video/mach64/engine.c',
            'drivers/video/matrox/driver.c',
            'drivers/video/matrox/engine.c',
            'drivers/video/native.c',
            'drivers/video/native_hw.c',
            'drivers/video/neomagic/cursor.c',
            'drivers/video/neomagic/driver.c',
            'drivers/video/neomagic/engine.c',
            'drivers/video/nv2d/adapter.c',
            'drivers/video/nv2d/dma_hw.c',
            'drivers/video/nv2d/engine.c',
            'drivers/video/nv2d/hardware.c',
            'drivers/video/radeon2d/cursor.c',
            'drivers/video/radeon2d/driver.c',
            'drivers/video/radeon2d/engine.c',
            'drivers/video/rage128/driver.c',
            'drivers/video/rage128/engine.c',
            'drivers/video/registry.c',
            'drivers/video/riva128/driver.c',
            'drivers/video/riva128/engine.c',
            'drivers/video/s3legacy/driver.c',
            'drivers/video/s3legacy/engine.c',
            'drivers/video/s3legacy/platform.c',
            'drivers/video/s3savage/s3savage_register.c',
            'drivers/video/s3savage/savage2d.c',
            'drivers/video/s3savage/supersavage_display.c',
            'drivers/video/s3savage/supersavage_mode.c',
            'drivers/video/s3trio64/engine.c',
            'drivers/video/s3virge/engine.c',
            'drivers/video/sis/cursor.c',
            'drivers/video/sis/engine.c',
            'drivers/video/sis/register.c',
            'drivers/video/swdib.c',
            'drivers/video/tdfx/driver.c',
            'drivers/video/tdfx/engine.c',
            'drivers/video/trident/driver.c',
            'drivers/video/trident/engine.c',
            'drivers/video/unichrome/driver.c',
            'drivers/video/unichrome/engine.c',
            'drivers/video/vmwaresvga/driver.c',
            'drivers/video/vmwaresvga/engine.c',
            'drivers/video/vmwaresvga/io.c',
            'drivers/fbdev.c',
            'core/devices.c',
            'core/display.c',
            'core/hardware_check.c',
            'core/hardware_check_model.c',
            'proc/syscalls_desktop.c',
        ]
        folder=ROOT/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='display-target-',dir=folder) as tmp:
            objects=[]
            for source in sources:
                obj=Path(tmp)/(source.replace('/','_')+'.o')
                subprocess.run(['clang',*module.CFLAGS,'-I',str(ROOT/'src/kernel/include'),
                    '-c',str(ROOT/'src/kernel'/source),'-o',str(obj)],check=True)
                objects.append(obj)
            symbols={}
            for obj in objects:
                output=subprocess.check_output(['nm','-g','--defined-only',str(obj)],text=True)
                for line in output.splitlines():
                    fields=line.split()
                    if len(fields)!=3 or fields[1] in ('W','V'): continue
                    name=fields[2]
                    self.assertNotIn(name,symbols,f'duplicate target symbol {name}: {obj}, {symbols.get(name)}')
                    symbols[name]=obj
        builder=(ROOT/'scripts/build_kernel.py').read_text()
        for name in ('registry.c','attempt.c','s3savage_register.c','swdib.c',
                     'savage2d.c','supersavage_display.c','supersavage_mode.c'):
            self.assertIn('"'+name+'"',builder)

    def test_keyed_attempt_markers(self):
        self.run_host(['tests/host/display_attempt_test.c','src/kernel/drivers/video/attempt.c',
            'src/kernel/lib/fmt.c'])

    def test_kernel_syscall(self):
        self.run_host(['tests/host/proc/desktop_test.c','tests/host/proc/signal_legacy.c',
            'src/kernel/lib/sha256.c','src/kernel/lib/fmt.c','src/kernel/probes/selector.c'],
            ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections'])

    def test_software_goldens(self):
        self.run_host(['tests/host/swdib_test.c','src/kernel/drivers/video/swdib.c'])

    def test_registry_integration(self):
        self.run_host(['tests/host/display_driver_test.c'])

    def test_adapter_cursor_evidence(self):
        self.run_host(['tests/host/display_adapter_test.c'],
            ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections'])

    def test_sdk_protocol(self):
        self.run_host(['tests/host/display_query_test.c','apps/desktop/protocol.c'],wrapper=True)

if __name__=='__main__': unittest.main()
