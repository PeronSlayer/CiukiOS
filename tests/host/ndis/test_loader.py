"""f6-04 synthetic loader, containment prerequisite, and x86 ABI evidence."""
import os
import signal
from pathlib import Path
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[3]

class LoaderTests(unittest.TestCase):
    def test_synthetic_loader_under_address_undefined_and_leak_sanitizers(self):
        out = ROOT/'build/host/ndis'; out.mkdir(parents=True, exist_ok=True)
        sources = ['sdk/pe/pe.c', 'sdk/win32/ldr/pe.c', 'tests/host/ndis/user_policy.c',
                   'src/kernel/ndis/ntimage.c', 'src/kernel/ndis/exports.c',
                   'src/kernel/ndis/callback.c', 'src/kernel/ndis/scan.c',
                   'tests/host/ndis/loader_test.c']
        command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-DW32_HOST_MODEL', '-DCIUKI_NDIS_HOST_MODEL', '-ffunction-sections',
                   '-fdata-sections', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-I.', '-Isrc/kernel/include', '-Isdk/win32/include',
                   *sources, '-Wl,--gc-sections', '-o', str(out/'loader')]
        subprocess.run(command, cwd=ROOT, check=True)
        # Leave leak detection enabled; do not inherit another suite's override.
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1')
        result = subprocess.run([out/'loader'], cwd=ROOT, env=env, text=True,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS shared PE/kernel policy', result.stdout)

    def test_resident_allocator_under_address_undefined_and_leak_sanitizers(self):
        out = ROOT/'build/host/ndis'; out.mkdir(parents=True, exist_ok=True)
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I.', '-Isrc/kernel/include', 'src/kernel/ndis/resident.c',
                        'tests/host/ndis/resident_test.c', '-o', str(out/'resident')],
                       cwd=ROOT, check=True)
        result = subprocess.run([out/'resident'], cwd=ROOT, text=True, capture_output=True,
                                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1'), timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS production resident allocator', result.stdout)

    def test_production_i686_objects_compile_without_model_execution_hook(self):
        out = ROOT/'build/host/ndis/kernel'; out.mkdir(parents=True, exist_ok=True)
        sources = [*sorted((ROOT/'src/kernel/ndis').glob('*.c')), ROOT/'src/kernel/arch/trap.c']
        for source in sources:
            subprocess.run(['clang', '--target=i686-unknown-elf', '-march=pentiumpro',
                            '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-pie',
                            '-mno-sse', '-mno-sse2', '-mno-mmx', '-mno-80387',
                            '-mstack-alignment=4', '-std=c17', '-O2', '-Wall', '-Wextra',
                            '-Werror', '-Wframe-larger-than=2048', '-Isrc/kernel/include',
                            '-c', str(source), '-o', str(out/(source.stem+'.o'))],
                           cwd=ROOT, check=True)
        symbols = subprocess.check_output(['llvm-nm', str(out/'callback.o')], text=True)
        self.assertNotIn('ndis_callback_model_call2', symbols)
        subprocess.run(['nasm', '-f', 'elf32', 'src/kernel/arch/ndis_context.asm',
                        '-o', str(out/'ndis_context.o')], cwd=ROOT, check=True)
        asm = subprocess.check_output(['llvm-objdump', '-dr', str(out/'execution.o')], text=True)
        gate = asm.split('<ndis_execute2>:', 1)[1].split('<ndis_shim_enter>:', 1)[0]
        self.assertNotIn('ndis_context_save', gate)
        self.assertNotRegex(gate, r'call[l]?\s+\*')

    def build_abi(self):
        out = ROOT/'build/host/ndis/abi'; out.mkdir(parents=True, exist_ok=True)
        subprocess.run(['clang', '--target=i386-unknown-linux-gnu', '-march=pentiumpro',
                        '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-pie',
                        '-fno-stack-protector', '-mno-sse', '-mno-sse2', '-mno-mmx',
                        '-O1', '-Wall', '-Wextra', '-Werror', '-I.', '-Isrc/kernel/include',
                        '-DCIUKI_NDIS_HOST_MODEL', '-ffunction-sections', '-fdata-sections',
                        '-c', 'tests/host/ndis/abi.c', '-o', str(out/'abi.o')], cwd=ROOT, check=True)
        subprocess.run(['clang', '--target=i386-unknown-linux-gnu', '-c',
                        'tests/host/ndis/abi.S', '-o', str(out/'probe.o')], cwd=ROOT, check=True)
        extra = []
        for name in ('callback', 'ntimage'):
            obj = out/(name+'.o'); extra.append(str(obj))
            subprocess.run(['clang', '--target=i386-unknown-linux-gnu', '-march=pentiumpro',
                            '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-pie',
                            '-fno-stack-protector', '-mno-sse', '-mno-sse2', '-mno-mmx',
                            '-O1', '-Wall', '-Wextra', '-Werror', '-Isrc/kernel/include',
                            '-DCIUKI_NDIS_HOST_MODEL', '-ffunction-sections', '-fdata-sections',
                            '-c', 'src/kernel/ndis/'+name+'.c', '-o', str(obj)], cwd=ROOT, check=True)
        subprocess.run(['ld.lld', '-m', 'elf_i386', '-static', '--entry=_start', '--gc-sections',
                        str(out/'abi.o'), str(out/'probe.o'), *extra, '-o', str(out/'abi')], check=True)
        return out

    def test_i386_stdcall_fastcall_cdecl_and_data_disassembly(self):
        out = self.build_abi()
        asm = subprocess.check_output(['llvm-objdump', '-dr', str(out/'abi.o')], text=True)
        std = asm.split('<abi_std>:', 1)[1].split('<abi_kf>:', 1)[0]
        fast = asm.split('<abi_kf>:', 1)[1].split('<abi_cdecl>:', 1)[0]
        cdecl = asm.split('<abi_cdecl>:', 1)[1].split('<abi_main>:', 1)[0]
        self.assertRegex(std, r'ret[l]?\s+\$(?:0x)?8')
        self.assertRegex(fast, r'ret[l]?\s+\$(?:0x)?4')
        self.assertIn('%ecx', fast); self.assertIn('%edx', fast)
        self.assertRegex(cdecl, r'ret[l]?\s*\n')
        symbols = subprocess.check_output(['llvm-nm', str(out/'abi.o')], text=True)
        self.assertRegex(symbols, r' D abi_tick_count')
        thunk = subprocess.check_output(['llvm-objdump', '-d', str(out/'callback.o')], text=True)
        thunk = thunk.split('<ndis_callback_model_call2>:', 1)[1]
        self.assertRegex(thunk, r'call[l]?\s+\*')

    def test_i386_assembly_callers_check_stack_registers_and_data(self):
        out = self.build_abi()
        result = subprocess.run([out/'abi'], capture_output=True, timeout=10)
        if result.returncode == -signal.SIGSYS:
            self.skipTest('sandbox blocks i386 syscalls (SIGSYS); disassembly is separate evidence')
        self.assertEqual(result.returncode, 0, result.stderr)

if __name__ == '__main__':
    unittest.main()
