"""Reference model of the curation rules, in plain Python so it runs without Spark.

The PySpark notebook applies the same rules to Delta. tests/test_curation_model.py proves replay,
crash recovery, out-of-order completion, amendment, deletion and incomplete-read behavior here.
"""
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

Window = Tuple[str, str]  # (start inclusive, end exclusive) in canonical UTC text; sorts chronologically


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
