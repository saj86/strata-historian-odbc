"""Run: python3 -m unittest discover -s fabric/tests. Uses synthetic inputs only."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "notebooks"))
from curation_model import Event, Extract, Store, ValidationError, advance_watermark, commit_window, run_window  # noqa: E402

W1 = ("2026-01-15T08:00:00.0000000Z", "2026-01-15T09:00:00.0000000Z")
W2 = ("2026-01-15T09:00:00.0000000Z", "2026-01-15T10:00:00.0000000Z")
W3 = ("2026-01-15T10:00:00.0000000Z", "2026-01-15T11:00:00.0000000Z")
P = "PT-PRESSURE-001"


def ev(t, seq=0, v=1.0, **k):
    return Event(P, f"2026-01-15T{t}Z", seq, v, **k)


FIRST = [ev("08:00:00.0000000", v=101.5), ev("08:00:10.0000000", v=0.0), ev("08:00:10.0000153", 0, 102.25), ev("08:00:10.0000153", 1, 102.5),
         ev("08:05:00.0000000", v=None, value_kind="SYSTEM", is_good=False)]


def extract(window=W1, events=None, run="r1", attempt=1, complete=True, expected=None):
    return Extract(P, window, run, attempt, FIRST if events is None else events, complete, expected)


class Replay(unittest.TestCase):
    def test_replay_gives_identical_output(self):
        s = Store()
        run_window(s, extract(), [W1])
        once = s.rows()
        run_window(s, extract(run="r2"), [W1])
        self.assertEqual(once, s.rows())
        self.assertEqual(len(s.rows()), 5)

    def test_repeated_timestamps_survive(self):
        s = Store()
        run_window(s, extract(), [W1])
        same = [e for e in s.rows() if e.event_utc_raw.endswith("08:00:10.0000153Z")]
        self.assertEqual([e.numeric_value for e in same], [102.25, 102.5])

    def test_zero_bad_and_missing_stay_distinct(self):
        s = Store()
        run_window(s, extract(), [W1])
        values = {e.event_utc_raw: e for e in s.rows() if e.event_seq == 0}
        self.assertEqual(values["2026-01-15T08:00:10.0000000Z"].numeric_value, 0.0)
        bad = values["2026-01-15T08:05:00.0000000Z"]
        self.assertIsNone(bad.numeric_value)
        self.assertEqual(bad.value_kind, "SYSTEM")


class Failures(unittest.TestCase):
    def test_crash_between_commit_and_checkpoint(self):
        s = Store()
        with self.assertRaises(RuntimeError):
            run_window(s, extract(), [W1], crash_before_checkpoint=True)
        self.assertEqual(len(s.rows()), 5)              # the sink committed
        self.assertIsNone(s.watermark.get(P))           # the checkpoint did not
        run_window(s, extract(run="r2"), [W1])          # the retry repeats the window
        self.assertEqual(len(s.rows()), 5)              # no duplicates
        self.assertEqual(s.watermark[P], W1[1])

    def test_failed_read_changes_nothing_and_infers_no_delete(self):
        s = Store()
        run_window(s, extract(), [W1])
        with self.assertRaises(ValidationError):
            commit_window(s, extract(events=[], run="r2", complete=False))
        self.assertEqual(len(s.rows()), 5)
        self.assertEqual(s.watermark[P], W1[1])

    def test_count_mismatch_rejected(self):
        with self.assertRaises(ValidationError):
            commit_window(Store(), extract(expected=99))

    def test_event_outside_window_rejected(self):
        with self.assertRaises(ValidationError):
            commit_window(Store(), extract(events=[ev("09:00:00.0000000")]))

    def test_end_bound_is_exclusive_and_start_inclusive(self):
        s = Store()
        commit_window(s, extract(events=[ev("08:00:00.0000000")]))
        with self.assertRaises(ValidationError):
            commit_window(s, extract(events=[ev("09:00:00.0000000")]))

    def test_duplicate_key_rejected(self):
        with self.assertRaises(ValidationError):
            commit_window(Store(), extract(events=[ev("08:00:00.0000000"), ev("08:00:00.0000000")]))


class Watermark(unittest.TestCase):
    def test_out_of_order_completion_holds_watermark(self):
        s = Store()
        schedule = [W1, W2, W3]
        run_window(s, extract(window=W2, events=[]), schedule)
        self.assertIsNone(s.watermark[P])               # W1 is not done
        run_window(s, extract(window=W3, events=[]), schedule)
        self.assertIsNone(s.watermark[P])
        run_window(s, extract(window=W1), schedule)
        self.assertEqual(s.watermark[P], W3[1])         # now contiguous

    def test_failed_window_blocks_watermark_after_it(self):
        s = Store()
        schedule = [W1, W2, W3]
        run_window(s, extract(window=W1), schedule)
        run_window(s, extract(window=W3, events=[]), schedule)
        self.assertEqual(s.watermark[P], W1[1])

    def test_backfill_does_not_move_watermark_backwards(self):
        s = Store()
        run_window(s, extract(window=W1), [W1, W2])
        run_window(s, extract(window=W2, events=[]), [W1, W2])
        run_window(s, extract(window=W1, run="backfill"), [W1, W2])
        self.assertEqual(s.watermark[P], W2[1])


class Corrections(unittest.TestCase):
    def test_amended_event_replaces_old_value_with_stable_count(self):
        s = Store()
        run_window(s, extract(), [W1])
        amended = [FIRST[0].__class__(**{**FIRST[0].__dict__, "numeric_value": 999.0})] + FIRST[1:]
        run_window(s, extract(events=amended, run="reconcile"), [W1])
        self.assertEqual(len(s.rows()), 5)
        self.assertEqual(s.rows()[0].numeric_value, 999.0)

    def test_deleted_event_disappears_only_after_a_complete_reread(self):
        s = Store()
        run_window(s, extract(), [W1])
        run_window(s, extract(events=FIRST[:4], run="reconcile", expected=4), [W1])
        self.assertEqual(len(s.rows()), 4)

    def test_late_event_shifts_seq_without_duplicates(self):
        s = Store()
        run_window(s, extract(), [W1])
        late = FIRST[:1] + [ev("08:00:05.0000000", v=50.0)] + FIRST[1:]
        run_window(s, extract(events=late, run="overlap"), [W1])
        self.assertEqual(len(s.rows()), 6)


if __name__ == "__main__":
    unittest.main()
