# Fabric design

**Readiness only. No tenant exists for this work and nothing here has run in Fabric.** The Power BI and gateway release doesn't depend on any of it. The driver catalog is the same for both. An ordinary Windows gateway doesn't put this DLL in a cloud Spark notebook, so notebooks work only on landed data.

```
PI Web API → driver on gateway → Pipeline Copy or Dataflow Gen2 → Lakehouse staging → notebook → curated Delta → Power BI
```

## Route choice

From the [Fabric connector table](https://learn.microsoft.com/en-us/fabric/data-factory/connector-odbc-overview) (updated 2026-03):

| Workload | Gateway | Authentication |
|---|---|---|
| Dataflow Gen2 | On-premises | Anonymous, Basic, Windows |
| Pipeline Copy, Lookup | On-premises | Anonymous, Basic |
| Copy job (full load, append) | On-premises | Anonymous, Basic |

| Site | Primary route |
|---|---|
| PI Web API accepts Basic | **Pipeline Copy** with a dedicated least-privilege identity |
| Windows-integrated only | **Dataflow Gen2.** Do not reinterpret Anonymous as Windows, store secrets in the connection string, or weaken PI security |
| Neither is acceptable | An approved staging step: a scheduled extract on an internal host using the same driver, writing files that are then uploaded to OneLake. Needs its own design and approval |

Prerequisites to confirm: gateway ≥ 3000.214.2; outbound `*.frontend.clouddatahub.net`; Fabric capacity and any licensing cost; workspace and OneLake permissions; identity per hop. Microsoft notes that local access to the gateway machine isn't allowed in pipelines; PI Web API must therefore be reached by a real hostname. The [Lakehouse Copy destination page](https://learn.microsoft.com/en-us/fabric/data-factory/connector-lakehouse-copy-activity) was not read during this work: read it before the pilot.

Things only a pilot can answer:

- Which connection string form works: Microsoft's example is `Driver={...};...`, others use `dsn=...`. With `Driver=`, the settings must be in the string; with `dsn=`, attributes after it aren't included.
- Whether Copy can bind `?`. Assume not: ODBC binding in native code doesn't establish it, so the design uses validated literals.
- How Copy behaves with `Table` mode. Do not use it: it reads unbounded, and the driver refuses (or, on a preview DSN, returns a sample, which the notebook then rejects).

## Pipeline

Parameters, activity sequence and the source query: [`fabric/pipeline/pipeline_design.json`](../fabric/pipeline/pipeline_design.json). It is a design and **not an importable export**, as no export schema could be verified. Build it in the UI and commit the real export afterwards. Create the tables first with [`curated_tables.sql`](../fabric/schemas/curated_tables.sql), and add a `PENDING` control row for each point. [`validate_window.py`](../fabric/notebooks/validate_window.py) parses the parameters as UTC and checks the point against the control table. Only those canonical values reach the query string, which is what makes literal construction safe.

Settings: bounded source SQL; explicit column mapping from the staging schema; query timeout (default is 120 minutes; set 30); activity retry 0, with reruns raising the `Attempt` parameter so staging stays immutable; parallel copies 1 per window, with a bounded number of concurrent windows; completion signal to the semantic model refresh.

## Dataflow Gen2

[`fabric/dataflow_gen2.pq`](../fabric/dataflow_gen2.pq), through the gateway ODBC connection, with an explicit Lakehouse destination. Filtering is at the source: the point and window are in the SQL text, and the driver refuses anything else. Schedule it, then compare the destination's row count to the control record.

## Control records

Schema: [`control_record.schema.json`](../fabric/schemas/control_record.schema.json). Samples: [`sample_control_records.json`](../fabric/schemas/sample_control_records.json). One record per source, dataset, point batch, UTC window (start inclusive, end exclusive) and attempt. It holds run ID, status, expected count if available, observed count, error and last committed watermark.

## Staging, then curated

**Staging** is immutable per attempt: `Files/staging/recorded_events/source=.../point=.../window=.../run=.../attempt=.../`, with the [column list here](../fabric/schemas/staging_recorded_events.json). It stores source identity, retrieval mode, raw timestamp, typed value, quality, units and lineage. Identity is `PointId`, so a tag rename changes `TagName` but not the series.

**Curated tables and their grains:**

| Table | Grain | Key | Status |
|---|---|---|---|
| `curated_points` | One point per source | `SourceServer, PointId` | Notebook written, Not run; rules tested |
| `curated_recorded_events` | One event | Window-replaced; `(SourceServer, PointId, EventUtcRaw, EventSeq)` is unique per commit | **Notebook written, Not run** |
| `curated_af_attributes` | Attribute, with its point reference | `SourceServer, AttributeId` | Table definition only; no backend |
| `curated_event_frames` | Frame | `SourceServer, FrameId` | Table definition only; no backend |

Interpolated values and summaries go in **separate tables**, never in recorded events.

### Why window replacement rather than key merge

No reliable source event key is known. A late insert at a repeated timestamp renumbers `EventSeq`, so merging on it would corrupt data. Each commit therefore atomically replaces one point and window with `replaceWhere`, which handles several things at once:

| Scenario | Behavior | Test in `test_curation_model.py` |
|---|---|---|
| Same window twice | Identical output | `test_replay_gives_identical_output` |
| Crash between Delta commit and checkpoint | Retry repeats the replace, and the watermark then moves | `test_crash_between_commit_and_checkpoint` |
| Parallel windows finish out of order | Watermark waits for a contiguous run | `test_out_of_order_completion_holds_watermark` |
| Failed window | Blocks the watermark beyond it | `test_failed_window_blocks_watermark_after_it` |
| Backfill while runs are active | Watermark doesn't move back | `test_backfill_does_not_move_watermark_backwards` |
| Amended event | Old value gone, same count | `test_amended_event_replaces_old_value_with_stable_count` |
| Deleted event | Gone only after a **complete** re-read | `test_deleted_event_disappears_only_after_a_complete_reread` |
| Partial, denied or failed read | Rejected; no deletion inferred | `test_failed_read_changes_nothing_and_infers_no_delete` |
| Late event | Included after overlap re-read | `test_late_event_shifts_seq_without_duplicates` |

Corrections: no documented change feed has been verified on the site, so plan overlap re-reads of recent windows on each run plus a periodic reconciliation of old windows. The horizon is a site decision. If a documented feed turns out to be available, it can narrow which windows get re-read, but replacement stays the mechanism.

Before accepting a batch, the notebook also checks the row count, the checksum when one is expected, and that each `EventUtc` agrees with `EventUtcRaw` to microseconds, the finest unit Spark keeps. A rejected batch writes a `FAILED` control record for that attempt.

To check this in Spark before a tenant exists: run `curate_recorded_events.py` with a local Spark + Delta install, using the fixture rows from `fixtures/expected_pressure_window.csv` as staging. It needs Java, which this host doesn't have.

## End-to-end example

| Stage | Pressure, `08:00:00` to `09:00:00` | Check |
|---|---|---|
| Source (fixture) | 7 events | Golden CSV |
| Driver | 7 rows, `ResultScope=BOUNDED` | `odbc_smoke` |
| Staging | 7 rows, run + attempt columns | `observed_count=7` |
| Curated | 7 rows: zero kept, Bad Input null, both `08:00:10.0000153` rows present | Unique key check |
| Watermark | `09:00:00` | Control record |
| Report | Events 7; Good 85.7%; last source event `08:15:00` | Same measures as `powerbi-acceptance.md` |
| Run status, `07:00` to `09:00` | 4 events; last is `I/O Timeout`, `SYSTEM` | Same path |

## Operations

- **Alerts:** row count outside expected range; watermark lag; repeated attempt failures; any `fixture` row or `PREVIEW` row in production.
- **Correlation:** pipeline `RunId` in control records and staging paths; driver correlation IDs in `TraceFile` on the gateway. Joining them today means matching on point, window and time, as the pipeline can't pass an ID to the driver.
- **Source load:** bound concurrent windows; PI Web API monitoring on the server.
- **Schema evolution:** the notebook rejects any mismatch, and a change is a reviewed update of the schema file.
- **Retention:** staging per a site decision (needed for replays and audit); curated per data owner.
- **Access:** whoever can read the Lakehouse can read copied PI data, regardless of PI permissions. Set workspace, OneLake and SQL endpoint permissions accordingly.
