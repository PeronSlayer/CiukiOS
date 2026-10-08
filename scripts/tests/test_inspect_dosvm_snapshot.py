import contextlib
import importlib.util
import io
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'inspect_dosvm_snapshot.py'
spec = importlib.util.spec_from_file_location('inspect_dosvm_snapshot', SCRIPT)
inspect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspect)


def sample(mask=3, reason=2):
    data = bytearray(inspect.TOTAL_SIZE)
    data[:4] = b'CDVS'
    struct.pack_into('<14H', data, 4, inspect.VERSION, inspect.TOTAL_SIZE,
                     1, 3, 500, mask, reason, 0, 0,
                     0 if mask & 1 else 0xFFFF,
                     0 if mask & 2 else 0xFFFF, 495, 490, 494)
    data[32:41] = b'DOOM.COM\0'
    if mask & 1:
        video = 64
        data[video:video + 4] = b'CVVS'
        struct.pack_into('<HH', data, video + 4, inspect.VERSION, 256)
        for offset, value in {8: 1, 12: 7, 60: 3, 64: 320, 68: 200,
                              72: 2, 76: 0, 112: 31, 124: 4,
                              204: 19200, 216: 1}.items():
            struct.pack_into('<I', data, video + offset, value)
        struct.pack_into('<Q', data, video + 140, 0x100000002)
    if mask & 2:
        devices = 320
        data[devices:devices + 4] = b'CVDV'
        struct.pack_into('<HH', data, devices + 4, inspect.VERSION, 128)
        for offset, value in {8: 1, 12: 27, 20: 1, 44: 2, 60: 1,
                              64: 12, 72: 1000, 76: 8, 88: 3389,
                              108: 44100, 124: 0x80820453}.items():
            struct.pack_into('<I', data, devices + offset, value)
    return data


class DosvmSnapshotTests(unittest.TestCase):
    def test_decodes_actual_scanout_and_audio_without_assuming_bios_mode(self):
        parsed = inspect.parse_snapshot(sample())
        self.assertEqual(parsed['reason'], 'observed_exit')
        self.assertEqual(parsed['state'], 'free')
        self.assertEqual(parsed['exit_code'], 0)
        self.assertEqual(parsed['video']['bios_mode'], 3)
        self.assertEqual(parsed['video']['scanout_kind'], 'graphics')
        self.assertEqual(parsed['video']['pm_faults'], 19200)
        self.assertEqual(parsed['video']['total_present_cycles'], 0x100000002)
        self.assertEqual(parsed['devices']['audio_state'], 'running')
        self.assertEqual(parsed['devices']['opl_writes'], 3389)
        self.assertEqual(parsed['devices']['protected_irq'],
                         {'claimed': True, 'held_mask': 0x82, 'delivered_low_word': 1107})

    def test_slot_and_session_generations_are_distinct_contracts(self):
        parsed = inspect.parse_snapshot(sample())
        self.assertEqual(parsed['generation'], 3)
        self.assertEqual(parsed['video']['session_generation'], 7)

    def test_missing_packets_are_explicit(self):
        parsed = inspect.parse_snapshot(sample(mask=0, reason=1))
        self.assertIsNone(parsed['video'])
        self.assertIsNone(parsed['devices'])
        self.assertEqual(parsed['video_query']['result'], 'unavailable')

    def test_retained_valid_packet_reports_failed_latest_query_and_age(self):
        data = sample()
        struct.pack_into('<H', data, 22, 0xFFFE)
        parsed = inspect.parse_snapshot(data)
        self.assertEqual(parsed['video_query']['result'], 'invalid_packet')
        self.assertFalse(parsed['video']['latest_query_succeeded'])
        self.assertEqual(parsed['video']['sample']['age_ticks'], 10)

    def test_packet_age_handles_tick_word_wrap(self):
        data = sample()
        struct.pack_into('<H', data, 12, 4)
        struct.pack_into('<H', data, 28, 65530)
        self.assertEqual(inspect.parse_snapshot(data)['video']['sample']['age_ticks'], 10)

    def test_user_close_keeps_pre_kill_state_and_unknown_exit(self):
        data = sample(reason=3)
        struct.pack_into('<HH', data, 18, 1, 0xFFFF)
        parsed = inspect.parse_snapshot(data)
        self.assertEqual((parsed['reason'], parsed['state'], parsed['exit_code']),
                         ('user_close', 'ready', 0xFFFF))

    def test_oem_window_title(self):
        data = sample(mask=0)
        data[32:64] = bytes(32)
        data[32:37] = b'Caf\x82\0'
        self.assertEqual(inspect.parse_snapshot(data)['title'], 'Café')

    def test_rejects_truncated_or_appended_records(self):
        for data in (sample()[:-1], sample() + b'X'):
            with self.subTest(size=len(data)), self.assertRaisesRegex(ValueError, 'exactly 448'):
                inspect.parse_snapshot(data)

    def test_rejects_invalid_record_identity(self):
        for offset, replacement, expected in (
            (0, b'NOPE', 'magic'), (4, b'\x01\x01', 'version'),
            (6, b'\x00\x02', 'size'), (8, b'\x00\x00', 'VM slot'),
            (14, b'\x04\x00', 'valid-mask'), (16, b'\x04\x00', 'reason'),
            (18, b'\x03\x00', 'VM state'),
        ):
            data = sample()
            data[offset:offset + len(replacement)] = replacement
            with self.subTest(offset=offset), self.assertRaisesRegex(ValueError, expected):
                inspect.parse_snapshot(data)

    def test_rejects_wrong_abi_for_each_claimed_packet(self):
        for packet in (64, 320):
            for offset, replacement in ((0, b'BAD!'), (4, b'\x01\x01'), (6, b'\x00\x00')):
                data = sample()
                data[packet + offset:packet + offset + len(replacement)] = replacement
                with self.subTest(packet=packet, offset=offset), self.assertRaises(ValueError):
                    inspect.parse_snapshot(data)

    def test_rejects_payload_without_validity_and_false_success(self):
        data = sample(mask=0)
        data[64] = 1
        with self.assertRaisesRegex(ValueError, 'nonzero video'):
            inspect.parse_snapshot(data)
        data = sample(mask=0)
        struct.pack_into('<H', data, 24, 0)
        with self.assertRaisesRegex(ValueError, 'success without'):
            inspect.parse_snapshot(data)

    def test_rejects_unterminated_title(self):
        data = sample()
        data[32:64] = b'A' * 32
        with self.assertRaisesRegex(ValueError, 'NUL-terminated'):
            inspect.parse_snapshot(data)

    def test_cli_emits_json_and_returns_error_on_bad_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'DOSVM1.BIN'
            path.write_bytes(sample())
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(inspect.main([str(path)]), 0)
            self.assertEqual(json.loads(stdout.getvalue())['vm'], 1)
            path.write_bytes(b'CDVS')
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(inspect.main([str(path)]), 1)
            self.assertIn('exactly 448', stderr.getvalue())

    @unittest.skipUnless(shutil.which('cc'), 'host C compiler unavailable')
    def test_actual_sampler_handles_failure_slot_reuse_budget_and_file_layout(self):
        # Compile the actual production helpers with a small mocked VM/DOS ABI.
        # Only the host's pointer transport differs from the 16-bit DOS target.
        source = (SCRIPT.parents[1] / 'src/apps/dosvm.c').read_text()
        defines = '\n'.join(re.findall(r'^#define (?:SNAP_\w+|VM_COUNT|VM_READY) .*$',
                                       source, re.M))
        structs = '\n'.join(re.search(r'struct ' + name + r' \{.*?\n\};', source,
                                      re.S).group(0) for name in
                            ('dosvm_snapshot', 'guest_window'))
        helpers = source[source.index('static u16 snapshot_query('):
                         source.index('static struct guest_window *by_window(')]
        preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
struct regs { u16 ax, bx, cx, dx, di; };
static struct { u16 ticks; } HOST;
static u8 snapshot_video[256], snapshot_devices[128], saved[448];
static unsigned mode, queries, statuses, writes, closes, errors;
static char saved_path[32];
static void mem_set(void *p, int c, int n) { memset(p, c, (unsigned)n); }
static void mem_copy(void *d, const void *s, int n) { memcpy(d, s, (unsigned)n); }
static void str_ncopy(char *d, const char *s, int n) {
    strncpy(d, s, (unsigned)n); d[n-1] = 0;
}
static int vm_status(int vm, u16 *code, u16 *generation) {
    assert(vm >= 1 && vm <= 3);
    ++statuses;
    *code = 0xffff; *generation = mode == 3 && statuses % 2 == 0 ? 4 : 3;
    return mode == 7 && statuses % 2 == 0 ? 0 : 1;
}
static int vm_target(int vm) { assert(vm >= 1 && vm <= 3); return mode == 4; }
static int vm_call(struct regs *r) {
    u8 *p = r->ax == 0x21 ? snapshot_video : snapshot_devices;
    unsigned bytes = r->ax == 0x21 ? 256 : 128;
    ++queries;
    assert(r->di == (u16)(uintptr_t)p && r->cx == bytes);
    if (mode == 1) { r->ax = 5; return 1; }
    memset(p, 0, bytes);
    memcpy(p, bytes == 256 ? "CVVS" : "CVDV", 4);
    p[5] = 1; p[6] = bytes & 255; p[7] = bytes >> 8;
    if (bytes == 256) {
        if (mode == 2) p[0] = '!';
        *(u32 *)(p + 8) = 1; *(u32 *)(p + 12) = 7;
        *(u32 *)(p + 64) = 320; *(u32 *)(p + 68) = 200;
    } else { *(u32 *)(p + 8) = 1; *(u32 *)(p + 20) = 1; }
    return 0;
}
static int dos_create(const char *p) { strcpy(saved_path, p); return 4; }
static int dos_write(int h, const void *p, int n) {
    assert(h == 4 && n == 448); ++writes; memcpy(saved, p, (unsigned)n);
    return mode == 5 ? n-1 : n;
}
static void dos_close(int h) { assert(h == 4); ++closes; }
static void disk_log(const char *e, const char *d, u16 c) {
    assert(strcmp(e, "Snapshot failed") == 0 && d && c == 0xffff); ++errors;
}
'''
        main = r'''
int main(int argc, char **argv) {
    struct guest_window g;
    unsigned before;
    FILE *file;
    assert(argc == 2 && sizeof(struct dosvm_snapshot) == 448);
    memset(&g, 0, sizeof g); g.live = 1; g.vm = 1; g.generation = 3;
    strcpy(g.title, "DOOM.COM"); HOST.ticks = 100;
    snapshot_begin(&g); snapshot_sample(&g);
    assert(g.snapshot.valid_mask == 3 && g.snapshot.video_ticks == 100);
    assert(queries == 2);
    HOST.ticks = 118; snapshot_sample(&g); assert(queries == 2);
    HOST.ticks = 119; mode = 1; snapshot_sample(&g);
    assert(g.snapshot.video_error == 5 && g.snapshot.device_error == 5);
    assert(g.snapshot.video_ticks == 100 && g.snapshot.device_ticks == 100);
    HOST.ticks = 138; mode = 2; snapshot_sample(&g);
    assert(g.snapshot.video_error == 0xfffe && g.snapshot.video_ticks == 100);
    assert(g.snapshot.device_error == 0 && g.snapshot.device_ticks == 138);
    HOST.ticks = 157; mode = 3; statuses = 0; snapshot_sample(&g);
    assert(g.snapshot.video_error == 0xffff && g.snapshot.device_error == 0xffff);
    assert(g.snapshot.video_ticks == 100 && g.snapshot.device_ticks == 138);
    before = queries; HOST.ticks = 176; mode = 4; snapshot_sample(&g);
    assert(queries == before && g.snapshot.valid_mask == 3);
    HOST.ticks = 200; mode = 0; snapshot_save(&g, 2, 0, 0);
    assert(strcmp(saved_path, "\\SYSTEM\\DOSVM1.BIN") == 0);
    assert(writes == 1 && closes == 1);
    file = fopen(argv[1], "wb"); assert(file);
    assert(fwrite(saved, 1, sizeof saved, file) == sizeof saved); fclose(file);
    g.vm = 3; snapshot_close(&g);
    assert(strcmp(saved_path, "\\SYSTEM\\DOSVM3.BIN") == 0);
    assert(g.snapshot.reason == 3 && g.snapshot.state == 1 &&
           g.snapshot.exit_code == 0xffff);
    mode = 5; snapshot_save(&g, 1, 1, 0xffff); assert(errors == 1);
    HOST.ticks = 65530; mode = 0; snapshot_begin(&g); snapshot_sample(&g);
    before = queries; HOST.ticks = 12; snapshot_sample(&g); assert(queries == before);
    HOST.ticks = 13; snapshot_sample(&g); assert(queries == before + 2);
    HOST.ticks = 32; mode = 7; statuses = 0; snapshot_sample(&g);
    assert(g.snapshot.video_error == 0xffff && g.snapshot.device_error == 0xffff);
    assert(g.snapshot.video_ticks == 13 && g.snapshot.device_ticks == 13);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            c_file, binary, capture = path / 'probe.c', path / 'probe', path / 'DOSVM1.BIN'
            c_file.write_text(preamble + defines + '\n#pragma pack(push, 1)\n' + structs +
                              '\n#pragma pack(pop)\n' + helpers + main)
            subprocess.run(['cc', '-std=c11', '-O0', '-Wno-pointer-to-int-cast',
                            str(c_file), '-o', str(binary)], check=True,
                           capture_output=True, timeout=20)
            subprocess.run([str(binary), str(capture)], check=True,
                           capture_output=True, timeout=10)
            parsed = inspect.parse_snapshot(capture.read_bytes())
            self.assertEqual(parsed['reason'], 'observed_exit')
            self.assertEqual(parsed['video']['scanout_kind'], 'graphics')
            self.assertEqual(parsed['video']['sample']['age_ticks'], 100)
            self.assertEqual(parsed['devices']['sample']['age_ticks'], 62)
            self.assertEqual(parsed['video_query']['result'], 'unavailable')


if __name__ == '__main__':
    unittest.main()
