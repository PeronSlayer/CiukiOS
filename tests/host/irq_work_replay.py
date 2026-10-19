"""F0-46: executed IRQ/deferred paths on production register-model fixtures.

The cost model is explicit in irq_work_model.h; this is not a measurement of
firmware, port stalls, cache misses or physical callback latency.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HOST = ROOT / 'tests/host'
OUT = ROOT / 'build/host'
FAILURES = []
ENV = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0', 'TMPDIR': str(OUT)}


def replay(folder, name, unit, targets, ports, extra=(), flags=(), args=(), argc=False, text=None):
    wrapper = folder / (name + '.c')
    body = text if text is not None else f'#include "{HOST / unit}"\n'
    setup = '\n'.join(f'model_target((void *){fn},"{label}",{limit});' for fn, label, limit in targets)
    setup += '\n' + '\n'.join(f'model_port((void *){fn});' for fn in ports)
    call = 'irq_fixture_main(argc,argv)' if argc else 'irq_fixture_main()'
    wrapper.write_text(f'''#include "{HOST / 'irq_work_model.h'}"
#define main irq_fixture_main
{body}
#undef main
int main(int argc,char **argv) {{
 (void)argc;(void)argv;{setup}
 int result={call};return result ? result : model_report();
}}
''')
    binary = folder / name
    command = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-finstrument-functions',
               '-fsanitize=address,undefined',
               '-fsanitize-coverage=trace-pc-guard,trace-loads,trace-stores,trace-div',
               '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-I', str(ROOT / 'src/kernel/include'), '-I', str(HOST), '-I', str(OUT), '-I', str(folder),
               *flags, str(wrapper), *(str(ROOT / p) for p in extra), '-o', str(binary)]
    subprocess.run(command, check=True, env=ENV, stdout=subprocess.DEVNULL)
    result = subprocess.run([str(binary), *map(str, args)], check=False, env=ENV,
                            stdout=subprocess.PIPE, text=True)
    if result.returncode:
        print("\n".join(result.stdout.splitlines()[-25:]), flush=True)
        FAILURES.append(name)
    print('\n'.join(line for line in result.stdout.splitlines() if line.startswith(('f0-46 IRQ model', 'f0-46 timer cost', 'f0-46 VM catchup'))), flush=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='irq-work-', dir=OUT) as tmp:
        folder = Path(tmp)
        replay(folder, 'timers', 'kernel_sync_test.c',
               [('ktimer_tick', 'timer-expiry-transfer', 100),
                ('timer_work', 'timer-deferred-turn', 1000),
                ('wait_irq_deferred', 'device-wake-turn', 1000),
                ('kwait_wake_irq', 'device-notification-capture', 100),
                ('kwork_queue', 'coalescing-work-ring', 100),
                ('chain_irq', 'shared-IRQ-dispatch', 100)], [])
        from vm_creation_model import write_model
        task_source = (ROOT / 'src/kernel/core/task.c').read_text()
        scheduler = task_source[task_source.index('static struct task *rq_head'):task_source.index('void task_stack_init')]
        for fn in ('schedule', 'sched_vm_tick_work', 'sched_tick', 'task_sleep_ms', 'task_start', 'sched_tasks', 'kpreempt_point'):
            match = re.search(r'^(?:static )?[^\n]+\b' + fn + r'\([^\n]*\)\s*\{', task_source, re.M)
            depth, end = 1, match.end()
            while depth:
                depth += (task_source[end] == '{') - (task_source[end] == '}')
                end += 1
            scheduler += task_source[match.start():end] + '\n'
        (folder / 'latency_scheduler.inc').write_text(scheduler)
        timers = (ROOT / 'src/kernel/core/sync.c').read_text()
        (folder / 'deadline_timers.inc').write_text(timers[timers.index('/* F0-46 timers:'):timers.index('/* End F0-46 timers. */')])
        write_model(folder / 'vm_creation_model.inc')
        replay(folder, 'pit', 'kernel_latency_test.c', [('sched_tick', 'PIT-scheduler-accounting', 100)], [], ['src/kernel/lib/fmt.c'])
        pit = (ROOT / 'src/kernel/arch/pic_pit.c').read_text()
        (folder / 'pit_timer.inc').write_text(pit[pit.index('static void shared_tick_work('):pit.index('void pit_init(')])
        composed=(HOST / 'timer_audio_load_test.c').read_text().replace('int main(void)', 'int irq_fixture_main(void)')
        composed=composed.replace('#include "kernel_latency_test.c"', f'#include "{HOST / "kernel_latency_test.c"}"')
        composed=composed.replace('#include "../../src/', f'#include "{ROOT}/src/')
        replay(folder, 'pit-full', None, [('timer_irq', 'PIT-full-ring-1000-timers', 100)],
               ['serial_tick', 'timing_timer_irq'], ['src/kernel/lib/fmt.c'], text='#undef main\n'+composed)
        vm_source=(HOST / 'dosvm_test.c').read_text()
        vm_source=vm_source[:vm_source.index('int main(void)')] + 'int main(void) { ram=calloc(HOST_PAGES,PAGE_SIZE); CHECK(ram); test_audio(); test_timer(); test_game_audio(); printf("VM clock coverage checks=%u guest_enters=%u deletes=%u\\n",checks,guest_enters,deletes); free(ram); return 0; }\n'
        vm_source=re.sub(r'(#include ")([^"]+)(")', lambda m:m[1]+str(((HOST / m[2]) if (HOST / m[2]).is_file() else (OUT / m[2])).resolve())+m[3],vm_source)
        replay(folder, 'vm-clock', None,
               [('dosvm_tick', 'DOS-virtual-clock-deferred', 1000),
                ('dosvm_clock_work', 'DOS-clock-continuation', 1000),
                ('kwork_run_one', 'DOS-clock-worker-turn', 1000),
                ('dosvm_audio_advance', 'DOS-audio-clock-per-VM', 250)], [],
               ['src/kernel/lib/fmt.c', 'src/kernel/lib/sha256.c',
                'src/kernel/vm/virtual_vga.c', 'src/kernel/vm/virtual_vga_bios.c',
                'src/kernel/vm/vga_x86.c', 'src/kernel/vm/vga_presenter.c',
                *(OUT / f'dosvm_audio_{n}.o' for n in ('vm_source', 'opl3', 'mixer'))],
               ['-D_POSIX_C_SOURCE=200809L', '-Wno-pointer-to-int-cast'], text=vm_source)
        replay(folder, 'firmware-input', 'v86_test.c', [('irq_input', 'BIOS-input-capture', 100)], [])
        source = (HOST / 'i8042_test.c').read_text()
        source = source.replace('!arg && prio == P_DEVICE', '!arg && prio == P_INTERACTIVE')
        source = re.sub(r'(#include ")([^"]+)(")',
                        lambda m: m[1] + str((HOST / m[2]).resolve()) + m[3], source)
        source += '''
bool fwinput_pending(void) { return fw_count || fake_fw_state==BIOSVM_DISABLED_BACKEND; }
uint64_t biosvm_input_reflections(void) { return 0; }
void biosvm_set_input_wait(struct kwait *q) { CHECK(q); }
void biosvm_input_snapshot(struct biosvm_input_diag *d) { memset(d,0,sizeof(*d)); }
'''
        replay(folder, 'i8042', None,
               [('input_irq', 'i8042-capture', 93),  # reserve existing 7us data pacing
                ('input_work', 'i8042-deferred-byte', 1000)],
               ['fake_read', 'fake_write'], argc=True, text=source)
        replay(folder, 'net', 'net/net_test.c',
               [('netif_irq', 'NIC-common-wrapper', 50),
                ('e100_interrupt', 'e100', 50), ('rtl_interrupt', 'rtl8139', 50),
                ('ne_interrupt', 'ne2000', 50), ('pc_interrupt', 'pcnet', 50),
                ('e1_interrupt', 'e1000', 50), ('rh_interrupt', 'rhine', 50),
                ('xl_interrupt', '3c90x', 50), ('tu_interrupt', 'tulip', 50)], ['model_read', 'model_write'],
               ['src/kernel/lib/fmt.c'], text=f'''#include "{HOST / 'net/net_test.c'}"
void serial_write(const char *s,size_t n) {{ (void)s;(void)n;abort(); }}
void bootlog_capture(const char *s,size_t n) {{ (void)s;(void)n;abort(); }}
void console_write(const char *s,size_t n) {{ (void)s;(void)n;abort(); }}
''')
        ata_fs = ['partition', 'fs_port', 'blkreq', 'cache', 'fat', 'path', 'vfs', 'fs_ro', 'iso9660', 'ntfs_fs', 'ntfs_ro', 'mount']
        replay(folder, 'ata', 'ata_test.c', [('ata_channel_irq', 'ATA-status-capture', 100), ('ata_dma_irq', 'ATA-DMA-stop-ack', 100)],
               ['fake_in8', 'fake_out8'],
               [*(f'src/kernel/fs/{n}.c' for n in ata_fs), 'src/kernel/core/bootlog.c', 'src/kernel/probes/fat_probes.c', 'src/kernel/lib/sha256.c', 'src/kernel/lib/fmt.c'],
               ['-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread'])
        usb_sources = ['usb_core', 'uhci', 'usb_interrupt', 'ohci', 'usb_hub', 'usb_hid', 'usb_storage']
        replay(folder, 'usb', 'usb/usb_test.c', [('usb_host_irq', 'UHCI-OHCI', 100)],
               ['read_reg', 'write_reg', 'config_read', 'config_write'],
               [*(f'src/kernel/usb/{n}.c' for n in usb_sources), 'src/kernel/fs/blkreq.c', 'src/kernel/lib/fmt.c'],
               ['-DCIUKI_USB_HOST', '-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread'])
        subprocess.run(['python3', str(HOST / 'fs/fixtures.py')], check=True, env=ENV, stdout=subprocess.DEVNULL)
        fs_sources = ['fs_port', 'blkreq', 'cache', 'path', 'fat', 'vfs', 'fs_ro', 'iso9660']
        audio_source=(HOST / 'audio/audio_test.c').read_text().replace('int main(int argc,char **argv)', 'int irq_fixture_main(int argc,char **argv)').replace('test_service_budget();', '')
        audio_source=re.sub(r'(#include \")([^\"]+)(\")', lambda m:m[1]+str((HOST / 'audio' / m[2]).resolve())+m[3],audio_source)
        replay(folder, 'audio', None,
               [('ac97_irq', 'AC97-period', 100), ('backend_irq', 'ES137x-SB16-period', 100)],
               ['mock_read', 'mock_write'],
               ['tests/host/proc/signal_legacy.c', 'tests/host/proc/desktop_legacy.c',
                'src/kernel/audio/registry_master.c', *(f'src/kernel/fs/{n}.c' for n in fs_sources),
                'tests/host/fs/fake.c', 'src/kernel/probes/f2_probes_files.c'],
               ['-DFS_HOST', '-D_POSIX_C_SOURCE=200809L', '-pthread', '-I', str(ROOT / 'src/kernel/fs')],
               [OUT / 'fs/fat32.img', OUT / 'fs/fat16.img'], argc=True, text='#undef main\n'+audio_source)


if __name__ == '__main__':
    main()
    if FAILURES:
        raise SystemExit('IRQ model failures: ' + ', '.join(FAILURES))
