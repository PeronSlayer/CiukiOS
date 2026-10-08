#!/usr/bin/env python3
"""Exercise bounded intro waits and prohibit duplicate or premature keys."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from wolf_intro import wait_for_menu


class Timeline:
    def __init__(self, frames):
        self.frames = frames
        self.now = 0.0
        self.keys = []

    def capture(self):
        return next(state for when, state in reversed(self.frames) if self.now >= when)

    def acknowledge(self):
        self.keys.append((self.now, self.capture()))

    def sleep(self, seconds):
        self.now += seconds

    def run(self, **options):
        return wait_for_menu(self.capture,
                             lambda frame: dict(matched=frame == 'menu'),
                             lambda frame: dict(matched=frame.startswith('intro-'), screen=frame),
                             self.acknowledge, clock=lambda: self.now,
                             sleep=self.sleep, **options)


class WolfIntroStatesTests(unittest.TestCase):
    def test_long_fades_wait_beyond_old_ten_second_limit(self):
        timeline = Timeline([(0, 'intro-signon'), (1, 'fading-title'),
                             (22, 'intro-title'), (23, 'fading-menu'), (38, 'menu')])
        frame, evidence = timeline.run(timeout=90)
        self.assertEqual(frame, 'menu')
        self.assertEqual(timeline.keys, [(0, 'intro-signon'), (22, 'intro-title')])
        self.assertEqual(evidence['seconds'], 38)

    def test_same_complete_page_receives_only_one_acknowledgement(self):
        timeline = Timeline([(0, 'intro-title'), (2, 'fade'),
                             (4, 'intro-title'), (12, 'menu')])
        timeline.run(timeout=20)
        self.assertEqual(timeline.keys, [(0, 'intro-title')])

    def test_unknown_colourful_frame_never_receives_input(self):
        timeline = Timeline([(0, 'colourful-unrecognized')])
        with self.assertRaisesRegex(AssertionError, 'state timed out'):
            timeline.run(timeout=3)
        self.assertEqual(timeline.keys, [])
        self.assertEqual(timeline.now, 3)

    def test_no_menu_after_ack_fails_bounded_without_repeating_key(self):
        timeline = Timeline([(0, 'intro-title')])
        with self.assertRaisesRegex(AssertionError, 'state timed out'):
            timeline.run(timeout=3)
        self.assertEqual(timeline.keys, [(0, 'intro-title')])
        self.assertEqual(timeline.now, 3)

    def test_next_recognized_page_gets_its_own_configured_timeout(self):
        timeline = Timeline([(0, 'intro-signon'), (1, 'fade'),
                             (2.5, 'intro-title'), (3, 'fade'), (5, 'menu')])
        timeline.run(timeout=3)
        self.assertEqual(len(timeline.keys), 2)
        self.assertEqual(timeline.now, 5)

    def test_acknowledgement_limit_bounds_page_cycle(self):
        timeline = Timeline([(0, 'intro-signon'), (1, 'intro-rating'),
                             (2, 'intro-title'), (3, 'intro-credits'), (4, 'intro-title')])
        with self.assertRaisesRegex(AssertionError, 'acknowledgement limit'):
            timeline.run(timeout=90, max_acknowledgements=4)
        self.assertEqual(len(timeline.keys), 4)

    def test_already_ready_menu_needs_no_acknowledgement(self):
        timeline = Timeline([(0, 'menu')])
        self.assertEqual(timeline.run()[0], 'menu')
        self.assertEqual(timeline.keys, [])


if __name__ == '__main__':
    unittest.main()
