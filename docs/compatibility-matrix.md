# Compatibility matrix

**Baseline: "Candidate inventory v0".** No reference product or version has been named, so this is a candidate list and not a parity claim. Any row could be wrong about a specific product until the discovery procedure below runs.

Status values: **Fixture** (implemented; tested with the fixture and a driver manager), **Live-unrun** (backend code written and tested against scripted responses; never run against a server), **None** (not implemented), **Decision** (needs a compatibility decision first).

Environment tested for every Fixture row: macOS, unixODBC 2.3.14. **No row has been tested on Windows, in Power BI or against PI.**

**Coverage against this baseline: 59 rows.** Fixture 19, Live-unrun 6, None 27, Decision 7. **Verified against a real PI server: 0.** A release supporting recorded values only is an early release, and this does not close full coverage.

## PI Data Archive

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| Point lookup and search | `dataservers/{id}/points` | `Points`, `TagName =` | Live-unrun | L2 | Wildcards not exposed |
| Point metadata | `points/{id}` | `Points` columns | Live-unrun | L2 | Only the listed attributes |
| Recorded values | `streams/{id}/recorded` | `RecordedValues` | Live-unrun | L4 | See data contract |
| Empty window, repeats, bad state, zero | same | `RecordedValues` | Fixture | `odbc_smoke`, `core_tests` | None known |
| Denied point | HTTP 403 or 404 | `42000` or `42S02` | Fixture | `odbc_smoke`, L8 | A hidden point and a missing point can look alike |
| Stable identity through rename | `IDOnly` WebId | `PointId` | Live-unrun | L10 | Unverified |
| Servers and collectives | `dataservers` | None | None | | |
| Point sources, digital state sets | `dataservers/...` | None; `DigitalSetName` only | None | | |
| Aliases | Unknown | None | None | | |
| Snapshot | `streams/{id}/value` | None | None | | |
| Value at time, retrieval modes | `streams/{id}/recordedattime` | None | None | | |
| Interpolated | `streams/{id}/interpolated` | None | None | | |
| Filtered | `filterExpression` | None | None | | |
| Plot | `streams/{id}/plot` | None | None | | |
| Multi-stream bulk | `streamsets` | `PointId IN` runs sequentially | None | | Slower; semantics same |

## Time-series calculations

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| Count, min, max, range, average, total, std dev, percent good | `streams/{id}/summary` | None | None | | Refused rather than computed from raw rows |
| Event- and time-weighting, interval alignment | same | None | None | | |
| PI relative time | Not exposed on purpose: bounds are absolute UTC | None | None | | |
| UTC and boundary rules | Local re-filter | Predicates | Fixture | `core_tests` | |
| Deterministic pagination | Timestamp resume with skip count | Transparent | Fixture | `core_tests`, L6 | Identical timestamps above `PageSize` raise an error |
| DST transitions | UTC only in driver | None | Fixture | Model-level test pending | |
| Annotations | Flag only | `IsAnnotated` | Live-unrun | L4 | Annotation text not read |
| Questionable, substituted | Flags | `IsQuestionable`, `IsSubstituted` | Live-unrun | L4 | |

## Asset Framework

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| AF servers, databases | `assetservers`, `assetdatabases` | None | None | | |
| Elements, templates, categories | `elements`, `elementtemplates` | None | None | | |
| Attributes, child attributes, values | `attributes` | None | None | | |
| Unit conversion, relationships | `desiredUnits` and others | None | None | | |
| Multiple attributes on one point | Bridge design | None | None | | See data contract |

## Event Frames

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| Search by time overlap, template, element, category | `eventframes` | None | None | | |
| Hierarchy, referenced elements, attributes | `eventframes` | None | None | | |
| Open-ended events; event spanning two windows | Design pending | None | None | | |

## SQL compatibility

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| Quoting, catalog and schema names | n/a | `"`, `[ ]`, `` ` ``; 1 to 3 part names | Fixture | `core_tests`, `odbc_smoke` | |
| Predicates, parameters, `TOP`, `LIMIT`, `ORDER BY` | Local after bounded read | SQL contract | Fixture | `core_tests` | Sort is buffered within `MaxRows` |
| Nulls | n/a | `IS [NOT] NULL` | Fixture | `core_tests` | |
| Casts, aggregates, grouping, joins, table-valued functions | n/a | `0A000` | None | Negative tests pass | Refused |
| Provider-specific query migration | n/a | None | Decision | Needs real queries and the reference product | |

## ODBC interoperability

Target: ODBC 3.52, forward-only, read-only, Unicode entry points.

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| Handles, attributes, connect, driver-connect | n/a | Core functions | Fixture | `odbc_smoke` | No connect dialog, by design |
| Prepare, execute, parameters, describe | n/a | Core functions | Fixture | `odbc_smoke` | Input parameters only |
| Bound and unbound fetch, `SQLGetData`, piecewise, truncation | n/a | Core functions | Fixture | `odbc_smoke` | |
| Rowsets: column- and row-wise | n/a | `SQLFetchScroll(NEXT)` | Fixture | `odbc_smoke` | `SQLGetData` needs rowset size 1 |
| Catalog functions | n/a | `SQLTables`, `SQLColumns`, and others | Fixture | `odbc_smoke` | Search patterns only |
| Capability answers match behavior | n/a | `SQLGetInfo`, `SQLGetFunctions` | Fixture | `odbc_smoke` | Two overstated by ODBC's floor: see SQL contract |
| Cancel, timeout | Token; WinHTTP handle close | `SQLCancel`, query timeout | Fixture | `odbc_smoke`; WinHTTP path Not run | |
| Concurrent connections | Per-process gate | Thread safety | Fixture | `odbc_smoke` + TSan | |
| Descriptors | n/a | Implicit, reduced | Fixture | Only through the driver manager | Explicit descriptors unavailable |
| Scrollable cursors, positioned ops, async | n/a | Refused | None | Negative tests pass | |
| Windows driver manager | n/a | | None | Gate A | |

## Power BI

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| Navigator discovery and preview | n/a | Catalog, preview policy | Fixture | Gate A manual | Driver behavior only; Power BI Not run |
| Import through bounded native query | n/a | `Odbc.Query` | Fixture | Gate A manual | Driver behavior only; Power BI Not run |
| Gateway refresh and failover | n/a | | None | Gates C and E | |
| Incremental refresh | n/a | `RangeStart`/`RangeEnd` SQL | None | Gate E | |
| Query folding | n/a | Minimal claims | None | Gate A trace | |
| DirectQuery | Dedicated `.mez` | Off | None | Separate result | |

## Other reference features

| Feature | Backend route (candidate) | SQL/ODBC surface | Status | Acceptance test | Loss of semantics |
|---|---|---|---|---|---|
| Subscriptions, change notification | Channels or streamsets | None | Decision | | |
| Historical revisions, deletes | Overlap re-reads | Curated layer | Decision | Rules model passes | |
| Analytics metadata | Unknown | None | Decision | | |
| Writes, annotations | Blocked by design | `0A000` | Decision | Separate write-enabled profile if ever approved | |
| Schema administration | Blocked by design | `0A000` | Decision | | |
| OLE DB / COM compatibility | Separate decision; not this driver | None | Decision | | |

Status is per row, and a missing server component, a missing permission, an unimplemented method and a precision loss must be logged as four different outcomes when Gate D runs.

## Discovery procedure for the real baseline

Run this with whichever reference product the site names (PI OLEDB Provider, PI OLEDB Enterprise or RTQP). Do not reuse the numbers above after that.

1. Record product, version, server versions and backend.
2. From the vendor documentation for that exact version, list every catalog, view, table-valued function, retrieval mode, SQL operation and metadata item.
3. Using the reference's own catalog queries, dump what it exposes on the test server and diff it against step 2.
4. Add one row per item here, with the reference behavior stated.
5. For each, capture golden output from an independent reader plus one negative case.
6. On every reference upgrade, rerun steps 2 to 5 and diff to track drift.

Backlog order after Gate B, chosen so each stage uses the recorded path already proven: snapshot → interpolated → summary → multi-stream → digital sets → AF → event frames.
