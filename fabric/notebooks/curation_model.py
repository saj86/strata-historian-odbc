"""Reference model of the curation rules, in plain Python so it runs without Spark.

The PySpark notebook applies the same rules to Delta. tests/test_curation_model.py proves replay,
crash recovery, out-of-order completion, amendment, deletion and incomplete-read behavior here.
"""
import hashlib
import re
from datetime import datetime
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

Window = Tuple[str, str]  # (start inclusive, end exclusive) in canonical UTC text; sorts chronologically


# The notebook carries an exact copy of these two functions so it needs no packaged module.
# test_notebook_copies_match_the_model fails if the copies drift.
def event_checksum(rows):
    """rows: (EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, IsGood). Order does not matter."""
    def field_text(value):
        if value is None:
            return "\\N"
        if isinstance(value, bool):
            return "1" if value else "0"
        if isinstance(value, float):
            return repr(value)
        return str(value)
    lines = sorted("\x1f".join(field_text(v) for v in row) for row in rows)
    return "sha256:" + hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()


def raw_matches_timestamp(raw, ts):
    """True if the typed timestamp agrees with the raw text to microseconds, Spark's finest unit."""
    text = raw.rstrip("Zz")
    whole, _, fraction = text.partition(".")
    if whole.replace("T", " ") != ts.strftime("%Y-%m-%d %H:%M:%S"):
        return False
    return (fraction + "000000")[:6] == f"{ts.microsecond:06d}"


# validate_window.py carries an exact copy of this function.
def validate_window_params(point_id, start, end, known_points, max_days):
    """Returns the only values allowed into the Copy query text, or raises ValueError."""
    if point_id not in known_points:
        raise ValueError("point is not in the control table")
    pattern = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z"
    if not re.fullmatch(pattern, start) or not re.fullmatch(pattern, end):
        raise ValueError("bounds must look like 2026-01-15T08:00:00Z")
    first = datetime.strptime(start, "%Y-%m-%dT%H:%M:%SZ")
    last = datetime.strptime(end, "%Y-%m-%dT%H:%M:%SZ")
    if last <= first:
        raise ValueError("window end must be after its start")
    if (last - first).total_seconds() > max_days * 86400:
        raise ValueError(f"window is longer than {max_days} days")
    return {
        "point_id": point_id.replace("'", "''"),
        "start_literal": first.strftime("%Y-%m-%d %H:%M:%S"),
        "end_literal": last.strftime("%Y-%m-%d %H:%M:%S"),
    }


@dataclass(frozen=True)
class Event:
    point_id: str
    event_utc_raw: str
    event_seq: int
    numeric_value: Optional[float]
    value_kind: str = "NUMERIC"
    is_good: bool = True
    source_server: str = "FIXTURE-V1"


@dataclass
class Extract:
    """One immutable per-attempt extract of one point and window."""
    point_id: str
    window: Window
    run_id: str
    attempt: int
    events: List[Event]
    complete: bool  # False for a failed, denied or partial read
    expected_count: Optional[int] = None
    expected_checksum: Optional[str] = None


@dataclass
class Store:
    curated: Dict[Tuple[str, str, Window], List[Event]] = field(default_factory=dict)  # replaced per window
    committed: Set[Tuple[str, Window]] = field(default_factory=set)
    watermark: Dict[str, Optional[str]] = field(default_factory=dict)

    def rows(self) -> List[Event]:
        out = [e for events in self.curated.values() for e in events]
        return sorted(out, key=lambda e: (e.point_id, e.event_utc_raw, e.event_seq))


class ValidationError(Exception):
    pass


def validate(x: Extract) -> None:
    if not x.complete:
        raise ValidationError("incomplete read: nothing may be inferred from it")
    if x.expected_count is not None and x.expected_count != len(x.events):
        raise ValidationError(f"count mismatch: expected {x.expected_count}, got {len(x.events)}")
    if x.expected_checksum is not None:
        actual = event_checksum((e.event_utc_raw, e.event_seq, e.value_kind, e.numeric_value, None, e.is_good) for e in x.events)
        if actual != x.expected_checksum:
            raise ValidationError(f"checksum mismatch: expected {x.expected_checksum}, got {actual}")
    start, end = x.window
    for e in x.events:
        if e.point_id != x.point_id:
            raise ValidationError("event belongs to another point")
        if not start <= e.event_utc_raw < end:
            raise ValidationError(f"event {e.event_utc_raw} outside window")
    seen = set()
    for e in x.events:
        key = (e.event_utc_raw, e.event_seq)
        if key in seen:
            raise ValidationError(f"duplicate key {key}")
        seen.add(key)


def commit_window(store: Store, x: Extract) -> None:
    """Atomic in Delta: replaceWhere on (source, point, window) in one transaction.

    Window replacement is what makes replay idempotent. It is also how corrections and deletions
    become visible, without assuming a stable event key exists at the source.
    """
    validate(x)
    source = x.events[0].source_server if x.events else "FIXTURE-V1"
    store.curated[(source, x.point_id, x.window)] = list(x.events)
    store.committed.add((x.point_id, x.window))


def advance_watermark(store: Store, point_id: str, schedule: List[Window]) -> Optional[str]:
    """Move only across a contiguous run of committed windows. Safe to re-run after a crash."""
    mark = None
    for w in sorted(schedule):
        if (point_id, w) not in store.committed:
            break
        mark = w[1]
    store.watermark[point_id] = mark
    return mark


def run_window(store: Store, x: Extract, schedule: List[Window], crash_before_checkpoint: bool = False) -> None:
    commit_window(store, x)
    if crash_before_checkpoint:
        raise RuntimeError("simulated crash after Delta commit, before checkpoint")
    advance_watermark(store, x.point_id, schedule)
