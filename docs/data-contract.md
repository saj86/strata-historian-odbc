# Data contract

## Logical tables

Catalog `STRATA`, schema `HISTORIAN`. The same on every host and backend; column order is part of the contract.

**Points** (grain: one point per source server; key `SourceServer, PointId`):
`SourceBackend, SourceServer, PointId, TagName, Descriptor, PointClass, PointType, EngUnits, DigitalSetName, IsStep`

**RecordedValues** (grain: one archive event as returned by one read):

| Column | Type | Null | Meaning |
|---|---|---|---|
| SourceBackend | WVARCHAR(32) | No | `fixture` or `piwebapi`. A fixture row can never pass as PI data |
| SourceServer | WVARCHAR(128) | No | Data server identity |
| PointId | WVARCHAR(256) | No | Stable identity. Live: `IDOnly` WebId, intended to survive renames (L10 will test) |
| TagName | WVARCHAR(256) | No | Name at read time |
| EventUtc | TIMESTAMP(7) | No | UTC, 100 ns resolution |
| EventUtcRaw | WVARCHAR(40) | No | Time text exactly as the source returned it |
| EventSeq | INTEGER | No | Source-order ordinal among events sharing `EventUtcRaw` in this read |
| RetrievalMode | WVARCHAR(16) | No | `RECORDED`. Other modes will be separate tables or columns, never mixed in silently |
| ValueKind | WVARCHAR(16) | No | `NUMERIC`, `DIGITAL`, `TEXT`, `SYSTEM` |
| NumericValue | DOUBLE | Yes | NULL unless NUMERIC. A real zero is 0 |
| TextValue | WVARCHAR(1024) | Yes | NULL unless TEXT |
| DigitalCode | INTEGER | Yes | Digital or system state code |
| StateText | WVARCHAR(128) | Yes | Digital or system state label |
| EngUnits | WVARCHAR(64) | Yes | Units returned with the event; NULL when absent, never defaulted |
| IsGood, IsQuestionable, IsSubstituted, IsAnnotated | BIT | No | Source flags. A live response missing one is an error |
| ResultScope | WVARCHAR(16) | No | `BOUNDED` or `PREVIEW` |

## Worked example

Fixture event: `PT-PRESSURE-001` / `SYN.PRESSURE.PV`, `2026-01-15T08:00:10.0000153Z`, second event at that time:

```
SourceBackend=fixture  SourceServer=FIXTURE-V1  PointId=PT-PRESSURE-001  TagName=SYN.PRESSURE.PV
EventUtc=2026-01-15 08:00:10.0000153  EventUtcRaw=2026-01-15T08:00:10.0000153Z  EventSeq=1
RetrievalMode=RECORDED  ValueKind=NUMERIC  NumericValue=102.5  TextValue=NULL  DigitalCode=NULL  StateText=NULL
EngUnits=kPa  IsGood=1  IsQuestionable=0  IsSubstituted=0  IsAnnotated=0  ResultScope=BOUNDED
```

## Keeping these separate

| Situation | ValueKind | NumericValue | StateText | IsGood |
|---|---|---|---|---|
| Reading of exactly zero | NUMERIC | 0 | NULL | 1 |
| Bad state such as Bad Input | SYSTEM | NULL | Bad Input | 0 |
| Digital state | DIGITAL | NULL | Running | 1 |
| No event in the window | no row | | | |

Nothing fills missing readings, and "No Data" never becomes zero. `SQL_NULL_DATA` with no indicator buffer is error `22002`, not 0.

## Grain and keys

**`RecordedValues` declares no primary key**, and `SQLPrimaryKeys` returns no rows for it. A timestamp does not identify an event: the fixture has two events at `08:00:10.0000153`. `EventSeq` separates them within a read, but **it is relative to the read**: a late insert at the same timestamp can renumber it. So:

- In a model, use `(SourceServer, PointId, EventUtcRaw, EventSeq)` as a row identity within a refresh, not as a durable key.
- In the curated layer, replace whole windows ([fabric-design.md](fabric-design.md)) rather than merging by that key.
- Do not relate two facts on it.

## What Power BI conversion loses

| Source | Through ODBC | In Power BI | Mitigation |
|---|---|---|---|
| Time finer than 100 ns | Truncated in `EventUtc` | | `EventUtcRaw` keeps it |
| Sub-second time | Preserved to 100 ns | M datetime holds 100 ns. **Model storage and visuals may be coarser; not measured** | `EventUtcRaw`, `EventSeq`. Measure in Gate A |
| Offset | Always UTC | Datetime has no zone; label it UTC | Report-time conversion below |
| Numeric | IEEE double | Decimal number is a double. Fixed decimal would round | Keep Decimal Number |
| Digital value | Code and label, separately | Text and whole number | Both retained |
| Source order at one timestamp | `EventSeq` | Model tables have no row order | Sort by `EventUtcRaw, EventSeq` |
| Tag rename | New `TagName`, same `PointId` | History shows the name at read time | Relate on `PointId` |

The precision of the source itself must come from a Gate B observation. The fixture's `0.0000153 s` step is only an illustration of "as close as the source allows".

## Retrieval modes

Only **recorded** is implemented. These mean different things and must not be merged in one column.

| Mode | Meaning | Status |
|---|---|---|
| Snapshot/current | Latest value known | Not implemented |
| Recorded | Stored archive events; irregular | Implemented |
| Interpolated | Computed at chosen times, with stepped points held and continuous points interpolated | Not implemented |
| Summaries | Computed over intervals: alignment, boundary and weighting matter | Not implemented |

When summaries are added, use PI's own summary operation and state these: sample interval, interval alignment, boundary rule, stepped versus continuous, time- versus event-weighted, and treatment of bad or missing data. `IsStep` is exposed so a model can already tell the two signal kinds apart.

## Why averaging recorded rows is wrong

Pressure over one hour `[0 s, 3600 s)`. The value holds at 100, then a noisy minute logs three readings at 200.

| t (s) | 0 | 3540 | 3550 | 3560 |
|---|---|---|---|---|
| value | 100 | 200 | 200 | 200 |

| Calculation | Working | Result |
|---|---|---|
| Event-weighted (`AVERAGE` of rows) | (100+200+200+200) / 4 | **175.0** |
| Time-weighted, stepped | (100×3540 + 200×60) / 3600 | **101.67** |
| Time-weighted, continuous (linear) | Trapezoids; final value held to the end | **150.83** |

The pressure sat at 100 for 98% of the hour, and the row average reports 175. Compression and exception reporting make row density depend on signal behavior, not on time, so the row average measures how often data was logged. The stepped and continuous results differ too, which is why `IsStep` matters.

## AF attributes, when added

Two AF attributes can reference one PI point, possibly with different units. The fact should stay keyed on the PI point. A separate attribute bridge carries AF path and conversion, so one event is not counted twice. An attribute in other units is a labelled conversion, not a second fact. Not implemented.

## Time zones and refresh

- UTC is the comparison basis everywhere. Boundaries are UTC.
- For local-time reports, add a date dimension or calculated column converted from UTC with explicit zone rules. Fall-back repeats an hour and spring-forward skips one, so never group raw data by local hour without the UTC instant.
- Refresh time and last source-event time are separate measures ([powerbi-acceptance.md](powerbi-acceptance.md)).
