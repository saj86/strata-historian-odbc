"""Run: python3 -m unittest discover -s fabric/tests. Uses synthetic inputs only."""
import ast
import json
import os
import sys
import unittest
from datetime import datetime

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "notebooks"))
from curation_model import Event, Extract, Store, ValidationError, advance_watermark, commit_window, event_checksum, raw_matches_timestamp, run_window, validate_window_params  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
NOTEBOOKS = os.path.join(ROOT, "fabric", "notebooks")


def function_source(path, name):
    """The source of one function, without importing the file (notebooks need Spark to import)."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    for node in ast.parse(text).body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return ast.get_source_segment(text, node)
    raise AssertionError(f"{name} not found in {path}")


def fixture_rows():
    """The hand-written golden file, as checksum inputs."""
    rows, seen = [], {}
    with open(os.path.join(ROOT, "fixtures", "expected_pressure_window.csv"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("EventUtc,"):
                continue
            when, number, state, good = line.split(",")
            raw = when.replace(" ", "T") + "Z"
            seq = seen.get(raw, 0)
            seen[raw] = seq + 1
            value = None if number == "<NULL>" else float(number)
            rows.append((raw, seq, "NUMERIC" if value is not None else "SYSTEM", value, None if state == "<NULL>" else state, good == "1"))
    return rows

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


class ChecksumAndPrecision(unittest.TestCase):
    def test_checksum_ignores_order_and_detects_changes(self):
        rows = fixture_rows()
        base = event_checksum(rows)
        self.assertEqual(base, event_checksum(list(reversed(rows))))
        changed = [rows[0][:3] + (999.0,) + rows[0][4:]] + rows[1:]
        self.assertNotEqual(base, event_checksum(changed))
        self.assertNotEqual(base, event_checksum(rows[1:]))

    def test_checksum_keeps_zero_and_null_apart(self):
        zero = event_checksum([("t", 0, "NUMERIC", 0.0, None, True)])
        null = event_checksum([("t", 0, "NUMERIC", None, None, True)])
        self.assertNotEqual(zero, null)

    def test_sample_control_record_holds_the_fixture_checksum(self):
        with open(os.path.join(ROOT, "fabric", "schemas", "sample_control_records.json"), encoding="utf-8") as f:
            sample = json.load(f)[0]
        rows = fixture_rows()
        self.assertEqual(sample["checksum"], event_checksum(rows))
        self.assertEqual(sample["observed_count"], len(rows))

    def test_validate_rejects_a_wrong_checksum(self):
        with self.assertRaises(ValidationError):
            commit_window(Store(), Extract(P, W1, "r1", 1, FIRST, True, None, "sha256:wrong"))

    def test_precision_compare(self):
        ts = datetime(2026, 1, 15, 8, 0, 10, 15)  # 15 microseconds
        self.assertTrue(raw_matches_timestamp("2026-01-15T08:00:10.0000153Z", ts))  # the 100 ns digit is beyond Spark
        self.assertTrue(raw_matches_timestamp("2026-01-15 08:00:10.000015", ts))
        self.assertFalse(raw_matches_timestamp("2026-01-15T08:00:10.0000153Z", ts.replace(microsecond=0)))  # rounded away
        self.assertFalse(raw_matches_timestamp("2026-01-15T08:00:11.0000153Z", ts))
        self.assertTrue(raw_matches_timestamp("2026-01-15T08:00:10Z", ts.replace(microsecond=0)))

    def test_notebook_copies_match_the_model(self):
        model = os.path.join(NOTEBOOKS, "curation_model.py")
        notebook = os.path.join(NOTEBOOKS, "curate_recorded_events.py")
        for name in ("event_checksum", "raw_matches_timestamp"):
            self.assertEqual(function_source(model, name), function_source(notebook, name))


class WindowParameters(unittest.TestCase):
    KNOWN = {P, "it's"}

    def good(self, **k):
        args = {"point_id": P, "start": "2026-01-15T08:00:00Z", "end": "2026-01-15T09:00:00Z", "known_points": self.KNOWN, "max_days": 31}
        args.update(k)
        return validate_window_params(**args)

    def test_returns_canonical_literals(self):
        self.assertEqual(self.good(), {"point_id": P, "start_literal": "2026-01-15 08:00:00", "end_literal": "2026-01-15 09:00:00"})

    def test_quote_in_a_known_point_is_escaped(self):
        self.assertEqual(self.good(point_id="it's")["point_id"], "it''s")

    def test_rejects_unsafe_or_invalid_input(self):
        for bad in (
            {"point_id": "x' OR '1'='1"},
            {"start": "2026-01-15T08:00:00Z' OR 1=1"},
            {"start": "2026-01-15 08:00:00"},
            {"end": "2026-02-30T00:00:00Z"},
            {"end": "2026-01-15T08:00:00Z"},
            {"end": "2026-01-15T07:00:00Z"},
            {"end": "2026-03-15T08:00:00Z"},
        ):
            with self.subTest(bad), self.assertRaises(ValueError):
                self.good(**bad)

    def test_notebook_copy_matches_the_model(self):
        self.assertEqual(function_source(os.path.join(NOTEBOOKS, "curation_model.py"), "validate_window_params"),
                         function_source(os.path.join(NOTEBOOKS, "validate_window.py"), "validate_window_params"))


if __name__ == "__main__":
    unittest.main()
