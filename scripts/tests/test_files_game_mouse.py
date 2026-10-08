#!/usr/bin/env python3
"""Qualify real-input acknowledgement with an asynchronous mouse poll model."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qemu_files_game_launch import observed_double_click


class PolledMouse:
    """One-shot press/release booleans can collapse blind input during paint."""
    def __init__(self, intervals=(.12,), start_tick=0, owner=9, modifier=0):
        self.now = 0.0
        self.start_tick = start_tick
        self.intervals = iter(intervals)
        self.interval = .06
        self.poll_at = self.next_interval()
        self.raw = self.buttons = self.previous = self.capture = 0
        self.pending_press = self.pending_release = False
        self.owner = owner
        self.modifier = modifier
        self.active = 8
        self.host_tick = start_tick
        self.last_down = None
        self.done = False
        self.sent = []
        self.delivered = []

    def next_interval(self):
        self.interval = next(self.intervals, self.interval)
        return self.interval

    def ticks(self):
        return (self.start_tick + int(self.now * 18.2)) & 0xffff

    def send(self, button):
        self.sent.append((self.now, button))
        if button != self.raw:
            if button:
                self.pending_press = True
            else:
                self.pending_release = True
        self.raw = button

    def poll(self):
        button = self.raw
        if self.pending_press:
            self.pending_press = False
            button = 1
        elif self.pending_release:
            self.pending_release = False
            button = 0
        self.buttons = button
        if button != self.previous:
            self.previous = button
            self.capture = self.owner if button else 0
            self.delivered.append((self.now, button))
            self.host_tick = self.ticks()
            if button:
                if self.last_down is not None and ((self.host_tick - self.last_down) & 0xffff) < 9:
                    self.done = True
                    self.active = 11  # EXEC can change focus before observation.
                self.last_down = self.host_tick

    def observe(self):
        self.now += .002  # A monitor read costs time while the guest runs.
        while self.now >= self.poll_at:
            self.poll()
            self.poll_at += self.next_interval()
        return dict(buttons=self.buttons, previous=self.previous, capture=self.capture,
                    active=self.active, ticks=self.ticks(), host_ticks=self.host_tick,
                    shift=self.modifier)

    def sleep(self, seconds):
        self.now += seconds

    def run(self, **options):
        return observed_double_click(self.send, self.observe, lambda: self.done,
                                     clock=lambda: self.now, sleep=self.sleep, **options)


class FilesMouseTests(unittest.TestCase):
    def test_old_blind_pulses_coalesce_to_one_down(self):
        mouse = PolledMouse((.3, .3))
        for button in (1, 0, 1, 0):
            mouse.send(button)
            mouse.sleep(.08)
        mouse.observe()
        mouse.sleep(.3)
        mouse.observe()
        self.assertEqual([button for _, button in mouse.delivered], [1, 0])
        self.assertFalse(mouse.done)

    def test_waits_for_each_edge_and_launches_without_sleep_assumptions(self):
        mouse = PolledMouse((.22, .07, .06, .06))
        evidence = mouse.run()
        self.assertTrue(mouse.done)
        self.assertEqual([button for _, button in mouse.delivered][:3], [1, 0, 1])
        self.assertEqual(mouse.sent[-1][1], 0)
        self.assertEqual(evidence['attempts'], 1)
        self.assertTrue(evidence['executed_during_edge'])

    def test_expired_paint_retries_after_old_click_interval(self):
        mouse = PolledMouse((.3, .3, .04, .04, .04, .04))
        evidence = mouse.run()
        self.assertTrue(mouse.done)
        self.assertEqual(evidence['attempts'], 2)
        self.assertTrue(any(event.get('expired') for event in evidence['events']))
        downs = [when for when, button in mouse.delivered if button]
        self.assertGreaterEqual(int(downs[1] * 18.2) - int(downs[0] * 18.2), 9)
        self.assertLess(int(downs[2] * 18.2) - int(downs[1] * 18.2), 9)

    def test_bios_low_word_wrap_keeps_valid_interval(self):
        mouse = PolledMouse((.12, .07, .07), start_tick=65534)
        self.assertEqual(mouse.run()['attempts'], 1)
        self.assertTrue(mouse.done)
        self.assertLess(mouse.ticks(), 8)

    def test_wrong_owner_is_never_accepted_as_files_delivery(self):
        mouse = PolledMouse((.06,), owner=10)
        with self.assertRaisesRegex(AssertionError, 'acknowledge press1'):
            mouse.run(timeout=.5)
        self.assertEqual(mouse.sent[-1][1], 0)
        self.assertFalse(mouse.done)

    def test_shift_or_control_modifier_rejects_doubleclick(self):
        for modifier in (1, 2, 4):
            with self.subTest(modifier=modifier):
                mouse = PolledMouse((.06,), modifier=modifier)
                with self.assertRaisesRegex(AssertionError, 'Modifier held'):
                    mouse.run()
                self.assertEqual(mouse.sent[-1][1], 0)

    def test_permanently_slow_paints_fail_bounded_and_release(self):
        mouse = PolledMouse((.6,) * 40)
        with self.assertRaisesRegex(AssertionError, 'expired after 3 attempts'):
            mouse.run()
        self.assertFalse(mouse.done)
        self.assertEqual(mouse.sent[-1][1], 0)
        self.assertLess(mouse.now, 10)


if __name__ == '__main__':
    unittest.main()
