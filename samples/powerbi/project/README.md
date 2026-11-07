# Power BI project

A Power BI Project (PBIP) with the sample model and a one-page report, as plain text files. **It has passed Microsoft's JSON schemas and TMDL parser, but hasn't been opened in Power BI Desktop yet.**

## Open it

1. Install the driver and the fixture DSNs on a Windows x64 machine.
2. Open `StrataHistorian.pbip` in a recent Power BI Desktop.
3. The project holds no data, so choose **Refresh**. If asked, choose Default or Custom credentials and approve running the native queries.

| On the page | Expected from the fixture |
|---|---|
| Data Source | `fixture` |
| Events | 7 |
| Good Events % | 85.7% |
| Bad Or System Events | 1 |
| Questionable Or Substituted | 2 |
| Last Source Event UTC | 2026-01-15 08:15:00 |
| Last Refresh UTC | The time you refreshed |
| Preview Rows In Model | Blank |
| Run status table | 4 rows, the last one `I/O Timeout` |

The chart has a gap at the `Bad Input` event. At `08:00:10`, a real 0 appears next to the repeated events at `08:00:10.0000153`. The chart averages values at the same time, so if the model's time resolution merges those events, the point shows their average. `EventUtcRaw` keeps the exact times.

## Use real data

Go to Transform data > Edit parameters and change `Dsn`, `PressurePointId`, `RunPointId`, `WindowStartUtc` and `WindowEndUtc`. Times are UTC, with the start inclusive and the end exclusive. Take point IDs from the `Points` table. The window must stay within the DSN's `MaxWindowDays`.

## If Desktop reports an error

Keep the message: it names the file and location. The likeliest causes are in the report's definition, which was written by hand. `report.json` has no base theme, because the schema doesn't require one and no Microsoft theme file is included. If Desktop rejects that, this is the first thing to change.

## Check it without Desktop

Run `make check-pbip` from the repository root. It runs consistency tests, Microsoft's JSON schemas and TMDL parser, and the project's SQL through the driver. It can't check that DAX or M runs, or how the page looks.
