#!/usr/bin/env python3
"""Run with:  python3 -m unittest test_ingest.py   (from the listener folder)"""
import unittest
from datetime import datetime, timezone
from unittest import mock

import ingest
from ingest import IGNORE, Combiner, filter_ignored, format_presses

NOON = datetime(2026, 1, 1, 12, 0, tzinfo=timezone.utc)


class FormatPresses(unittest.TestCase):
    def test_mapped_button(self):
        self.assertEqual(format_presses([1], {"1": "pee"}, now=NOON),
                         "pad press 12:00: pee")

    def test_combo_is_one_line(self):
        text = format_presses([1, 5, 6],
                              {"1": "pee", "5": "poo", "6": "cloth"}, now=NOON)
        self.assertEqual(text, "pad press 12:00: pee + poo + cloth")

    def test_unmapped_button_still_records(self):
        text = format_presses([7], {"7": None}, now=NOON)
        self.assertIn("button 7", text)
        self.assertIn("unmapped", text)


class Ignore(unittest.TestCase):
    def test_ignored_buttons_are_dropped(self):
        buttons = {"1": "pee", "7": IGNORE}
        self.assertEqual(filter_ignored([1, 7], buttons), [1])

    def test_all_ignored_posts_nothing(self):
        posted = []
        c = Combiner(posted.append)
        c.pending = [7]
        with mock.patch.object(ingest, "load_buttons",
                               return_value={"7": IGNORE}):
            c.flush()
        self.assertEqual(posted, [])

    def test_mixed_event_keeps_real_buttons(self):
        posted = []
        c = Combiner(posted.append)
        c.pending, c.first_at = [1, 7], NOON
        with mock.patch.object(ingest, "load_buttons",
                               return_value={"1": "pee", "7": IGNORE}):
            c.flush()
        self.assertEqual(posted, ["pad press 12:00: pee"])


class Timing(unittest.TestCase):
    def test_event_is_stamped_with_first_press_not_flush(self):
        posted = []
        c = Combiner(posted.append)
        c.pending, c.first_at = [1], NOON
        with mock.patch.object(ingest, "load_buttons",
                               return_value={"1": "pee"}):
            c.flush()
        self.assertIn("12:00", posted[0])


class Immediate(unittest.TestCase):
    def test_immediate_buttons_read_from_config(self):
        with mock.patch.object(ingest, "load_config",
                               return_value={"immediate_buttons": [3, 7]}):
            self.assertEqual(ingest.immediate_buttons(), {3, 7})

    def test_no_config_means_none_immediate(self):
        with mock.patch.object(ingest, "load_config", return_value={}):
            self.assertEqual(ingest.immediate_buttons(), set())

    def test_handle_press_posts_immediate_now(self):
        posted = []
        with mock.patch.object(ingest, "load_config",
                               return_value={"immediate_buttons": [3]}), \
             mock.patch.object(ingest, "load_buttons",
                               return_value={"3": "nursing left"}):
            result = ingest.handle_press(3, deliver=posted.append,
                                         combiner=None, now=NOON)
        self.assertEqual(result, "posted")
        self.assertEqual(posted, ["pad press 12:00: nursing left"])

    def test_handle_press_queues_normal_button(self):
        combiner = mock.Mock()
        with mock.patch.object(ingest, "load_config", return_value={}):
            result = ingest.handle_press(1, deliver=None, combiner=combiner)
        self.assertEqual(result, "queued")
        combiner.add.assert_called_once_with(1)

    def test_ignored_immediate_button_posts_nothing(self):
        posted = []
        with mock.patch.object(ingest, "load_config",
                               return_value={"immediate_buttons": [7]}), \
             mock.patch.object(ingest, "load_buttons",
                               return_value={"7": IGNORE}):
            result = ingest.handle_press(7, deliver=posted.append,
                                         combiner=None, now=NOON)
        self.assertEqual(result, "ignored")
        self.assertEqual(posted, [])


class ExampleFiles(unittest.TestCase):
    def test_examples_parse(self):
        import json
        for name in ("buttons.example.json", "config.example.json"):
            json.loads((ingest.HERE / name).read_text())


if __name__ == "__main__":
    unittest.main()
