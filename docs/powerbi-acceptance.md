# Power BI acceptance (Gate A)

**Not run.** Record date, versions and results here when executed. Uses Power BI Desktop on a clean Windows x64 machine **without** PI OLE DB, PI SQL Client or AF SDK installed.

## Before you start

```powershell
Get-Package | Where-Object Name -match 'PI |OSIsoft|AVEVA'     # expect nothing relevant
.\scripts\Install-StrataDriver.ps1 -Action Install -MsiPath .\strataodbc-0.1.0-x64.msi -AllowUnsigned   # lab only
.\scripts\Provision-StrataDsn.ps1 -Name StrataFixture -Backend fixture
.\scripts\Provision-StrataDsn.ps1 -Name StrataFixturePreview -Backend fixture -NavigatorPreview
.\scripts\Test-StrataNode.ps1 -Dsn StrataFixture -ExpectedRows 7 -SmokeSql "SELECT EventUtc FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}"
```

Turn on tracing so you can see what Power BI really calls: use a DSN with `-TraceFile` for driver events, and ODBC Data Sources (64-bit) → Tracing for the Driver Manager trace. Keep both as evidence after removing secrets.

## Procedure

| # | Step | Expected | Result |
|---|---|---|---|
| 1 | Get Data → ODBC → DSN `StrataFixturePreview` → Default or Custom | Navigator opens | Not run |
| 2 | Expand `STRATA` → `HISTORIAN` | `Points` and `RecordedValues` listed | Not run |
| 3 | Select `Points` | 2 rows. No `SYN.DENIED`. Descriptor shows `Synthetic pressure – ΔP prüfstand` correctly | Not run |
| 4 | Select `RecordedValues` | At most 100 rows, all `ResultScope = PREVIEW`, one point. Trace shows `listPoints(limit=1)`, no scan | Not run |
| 5 | Repeat 1 and 4 with DSN `StrataFixture` | Clear error naming the required bounds; no data | Not run |
| 6 | Get Data → Blank Query, paste example 1 from `powerquery/examples.pq`, Load | **Exactly the 7 rows** in `fixtures/expected_pressure_window.csv` | Not run |
| 7 | Check the rows | The zero is 0, Bad Input has null value and `StateText`, both `08:00:10.0000153` rows present | Not run |
| 8 | In the model, measure time precision: does a table visual keep `08:00:10.0000153` apart from `08:00:10`? | Record what is seen. `EventUtcRaw` stays exact regardless | Not run |
| 9 | Apply a local filter step on `RecordedValues` via Navigator on `StrataFixture` | Loud failure, never a partial sample | Not run |
| 10 | Save the Driver Manager trace; list any call returning `IM001` or `HYC00` | Each one goes into the matrix and is implemented or explained | Not run |

Known unknowns only this test can answer:

- Whether Navigator handles catalog + schema display as expected.
- Which connection or statement attributes Power BI sets; unknown ones currently return `HYC00`.
- Whether any unexported function is needed.
- How `UID`/`PWD` arrive when a DSN connection string is used. Microsoft documents that attributes after `DSN=` aren't included, so test Database authentication explicitly.

## Report page

Build this on `PressureEvents` (bounded query for the pressure point), `RunEvents` (same for the run-state point) and `RefreshStamp` (example 4 in `powerquery/examples.pq`).

| Visual | Fields | Notes |
|---|---|---|
| Pressure trend (line) | X: `EventUtc`; Y: `NumericValue`; filter `ValueKind = NUMERIC` | Set "show items with no data" off. A gap stays a gap: do not replace nulls with 0 |
| Run-status timeline (table or stepped chart) | `EventUtc`, `StateText`, `DigitalCode`, `ValueKind` | Sort by `EventUtcRaw`, `EventSeq`. Stepped: a state holds until the next event |
| Quality indicator (cards) | Measures below | Counts of events, not percent of time |
| Freshness (two cards) | `Last source event`, `Last refresh` | Always shown side by side, never merged |

```dax
Events = COUNTROWS ( PressureEvents )
Good Events % = DIVIDE ( CALCULATE ( [Events], PressureEvents[IsGood] = TRUE () ), [Events] )
Bad Or System Events = CALCULATE ( [Events], PressureEvents[IsGood] = FALSE () )
Questionable Or Substituted = CALCULATE ( [Events], PressureEvents[IsQuestionable] = TRUE () || PressureEvents[IsSubstituted] = TRUE () )
Last Source Event UTC = MAX ( PressureEvents[EventUtc] )
Last Refresh UTC = MAX ( RefreshStamp[RefreshedUtc] )
Preview Rows In Model = CALCULATE ( [Events], PressureEvents[ResultScope] = "PREVIEW" )   // must be blank; alert on any number
```

`Good Events %` counts events, which over-represents busy periods. Use it for data quality, not as "percent of time good". A time-based percent-good needs PI's summary operation, which is not implemented.

Expected fixture values: Events 7, Good 85.7% (6 of 7), Bad 1, Questionable or Substituted 2, Last Source Event `2026-01-15 08:15:00`. Last Refresh is whenever it ran, deliberately distinct.

This page ships ready-made as a Power BI Project: open `samples/powerbi/project/StrataHistorian.pbip` and refresh, then compare it to the expected values in the project's README. It hasn't been opened in Desktop, so also record whether it opens cleanly. It uses DSN `StrataFixture` and the window `07:00` to `09:00`, which returns the same 7 pressure rows, and adds 4 run status rows.

## DirectQuery

Off. Import is the supported target. Reconsider only if Import cannot meet the need: it needs the separate `.mez`, truthful capability overrides, and tests of the SQL Power BI generates for visuals, filters, grouping, sorting, limits, nulls and concurrency. The driver refuses `GROUP BY` today, so most DirectQuery visuals would fail.
