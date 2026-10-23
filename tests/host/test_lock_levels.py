#!/usr/bin/env python3
"""fx-5 follow-up (lead decision 2026-10-23): cross-object kmutex nestings.

sync.h orders mutexes by (level, address). Separate objects are placed by the
compiler and linker in build_kernel.py's object order, which a new source file
shifts, so a nesting across objects must never rest on addresses: its outer
class carries KMUTEX_LEVEL_OUTER and its inner class stays at level 0. The T23
trial of image 9a60215f panicked on such a nesting (audio service/control).

This test fails when a listed pair loses its explicit level, i.e. the outer
lock is created or re-initialised without KMUTEX_LEVEL_OUTER, or the inner
lock is raised to the outer level, or a listed nesting site disappears
without this inventory being updated. The inventory came from a static scan
of every kmutex acquisition site (build/codex/fx-5-report.md, Follow-up).
SPDX-License-Identifier: GPL-2.0-only
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'src/kernel'

# class -> (file, lock expression as written in that file)
OUTER = {
    'audio.service': ('audio/audio_dev.c', 'audio_lock.service'),
    'audio.control': ('audio/audio_dev.c', 'audio_lock.control'),
    'display.lease': ('core/display_present.c', 'lease_lock'),
    'net.control': ('net/lwip/control.c', 'control_mutex'),
}
INNER = {
    'audio_dma.register': [('audio/backend.c', r'(?:d|audio_dma)(?:->|\.)register_lock')],
    'thinkpad.ec': [('drivers/thinkpad_ec.c', 'ec_lock')],
    'fbdev.presenter': [('drivers/fbdev.c', 'presenter')],
    'net.settings': [('net/settings.c', 'lock')],
    'netif': [('drivers/net.c', r'd->mutex')],
}
# (outer, inner, file of the nesting, a call that performs it with the outer held)
PAIRS = [
    ('audio.service', 'audio_dma.register', 'audio/audio_dev.c', 'audio_backend_shutdown'),
    ('audio.control', 'audio_dma.register', 'audio/audio_dev.c', 'audio_backend_master'),
    ('audio.service', 'thinkpad.ec', 'audio/audio_dev.c', 'thinkpad_ec_stop'),
    ('audio.control', 'thinkpad.ec', 'audio/audio_dev.c', 'thinkpad_ec_master_set'),
    ('display.lease', 'fbdev.presenter', 'core/display_present.c', 'fbdev_present_native'),
    ('net.control', 'net.settings', 'net/lwip/control.c', 'net_settings_get'),
    ('net.control', 'netif', 'net/lwip/control.c', 'net_control_driver'),
]


def source(rel):
    text = (SRC / rel).read_text()
    return re.sub(r'/\*.*?\*/|//[^\n]*', ' ', text, flags=re.S)


class LockLevelTests(unittest.TestCase):
    def test_outer_classes_keep_their_level(self):
        for name, (rel, expr) in OUTER.items():
            text = source(rel)
            e = re.escape(expr)
            classed = [
                rf'\b{e}\s*=\s*KMUTEX_CLASS\(\s*"{re.escape(name)}"\s*,\s*KMUTEX_LEVEL_OUTER\s*\)',
                rf'kmutex_init_level\(\s*&{e}\s*,\s*"{re.escape(name)}"\s*,\s*KMUTEX_LEVEL_OUTER\s*\)',
                rf'ciuki_lwip_mutex_class\(\s*&{e}\s*,\s*"{re.escape(name)}"\s*,\s*KMUTEX_LEVEL_OUTER\s*\)',
            ]
            self.assertTrue(any(re.search(p, text) for p in classed), f'{name}: {rel} lost KMUTEX_LEVEL_OUTER')
            # A plain or named re-initialisation would reset the level to 0.
            self.assertIsNone(re.search(rf'kmutex_init(?:_named)?\(\s*&{e}\s*[,)]', text),
                              f'{name}: {rel} re-initialises {expr} at level 0')
        audio = source('audio/audio_dev.c')
        static = re.search(r'struct\s*\{\s*struct kmutex service;\s*struct kmutex control;\s*\}\s*audio_lock\s*=\s*\{(.*?)\};', audio, re.S)
        self.assertIsNotNone(static, 'audio locks must stay one struct, service declared before control')
        for member in ('service', 'control'):
            self.assertRegex(static.group(1), rf'\.{member}\s*=\s*KMUTEX_CLASS\(\s*"audio\.{member}"\s*,\s*KMUTEX_LEVEL_OUTER\s*\)')

    def test_inner_classes_stay_ordinary(self):
        for name, places in INNER.items():
            for rel, expr in places:
                text = source(rel)
                inits = re.findall(rf'(?:kmutex_init\w*|KMUTEX_CLASS)\(\s*&?(?:{expr})\b[^;]*', text)
                for init in inits:
                    self.assertNotIn('KMUTEX_LEVEL_OUTER', init, f'{name}: inner class raised to the outer level in {rel}')
                self.assertNotIn('KMUTEX_LEVEL_OUTER', text, f'{name}: {rel} declares an outer level; update the inventory')

    def test_listed_nestings_still_exist(self):
        for outer, inner, rel, call in PAIRS:
            self.assertRegex(source(rel), rf'\b{call}\s*\(', f'{outer} -> {inner}: {call} no longer in {rel}; update the inventory')

    def test_order_key_is_level_then_address(self):
        sync = (SRC / 'include/ciuki/sync.h').read_text()
        body = sync[sync.index('static inline bool kmutex_order_conflict'):]
        body = body[:body.index('\n}') + 2]
        self.assertRegex(body, r'if \(held->level != want->level\) return held->level > want->level;')
        self.assertIn('#define KMUTEX_LEVEL_OUTER (-1)', sync)


if __name__ == '__main__':
    unittest.main()
