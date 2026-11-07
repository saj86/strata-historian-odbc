# SQL contract

Read-only and deliberately small. Anything outside this grammar is rejected with a specific SQLSTATE, never approximated.

```
SELECT [TOP n] (* | item [, item]*) FROM [[STRATA.]HISTORIAN.]table [[AS] alias]
  [WHERE predicate [AND predicate]*]  [ORDER BY column [ASC|DESC] [, ...]]  [LIMIT n]  [;]

item       := [qualifier.]column [[AS] alias]
predicate  := column (= | <> | != | < | <= | > | >=) value
            | column IN (value, ...) | column BETWEEN value AND value
            | column IS [NOT] NULL   | 1=0 | 1=1
value      := ? | 'text' | N'text' | number | TRUE | FALSE | {ts '...'} | {d '...'}
```

Identifiers may be quoted with `"`, `[ ]` or `` ` ``. Matching is case-insensitive. Text timestamps are read as UTC unless they carry an offset.

| Rejected | SQLSTATE |
|---|---|
| `INSERT`, `UPDATE`, `DELETE`, `MERGE`, DDL, `EXEC`, `CALL`, `GRANT`, transaction statements | `0A000` read-only |
| Aggregates, `GROUP BY`, `HAVING`, `DISTINCT`, joins, subqueries, `UNION`, `OR`, `NOT`, `LIKE`, `OFFSET`, functions, comments, SQL-92 `TIMESTAMP '...'`, `{fn ...}` | `0A000` |
| Syntax errors, multiple statements | `42000` |
| Unknown table or schema; unknown column | `42S02`; `42S22` |
| Unbounded or over-limit `RecordedValues` read; denied point | `42000` |

Aggregates are unimplemented on purpose: averaging raw recorded rows is misleading ([data-contract.md](data-contract.md)), so that belongs to a separately designed summaries feature.

## Bounds on RecordedValues

A read needs **all** of the following, or it is refused before any source access:

- `PointId = value` or `PointId IN (...)`, up to `MaxPointsPerQuery` (default 16)
- A lower and an upper `EventUtc` bound, spanning at most `MaxWindowDays` (default 31)

The row cap `MaxRows` (default 2,000,000) stops a query with an error. It does not truncate quietly.

## The worked example

```sql
SELECT EventUtc, NumericValue, StateText, IsGood
FROM RecordedValues
WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?
```

**Parameters**

| # | Column | Reported by `SQLDescribeParam` | C types accepted | Example value |
|---|---|---|---|---|
| 1 | PointId | `SQL_WVARCHAR` | `SQL_C_WCHAR`, `SQL_C_CHAR` | `PT-PRESSURE-001` |
| 2 | EventUtc (inclusive) | `SQL_TYPE_TIMESTAMP` | `SQL_C_TYPE_TIMESTAMP`, `SQL_C_TYPE_DATE`, text | `2026-01-15 08:00:00` |
| 3 | EventUtc (exclusive) | `SQL_TYPE_TIMESTAMP` | as above | `2026-01-15 09:00:00` |

**Result columns**: `EventUtc` TIMESTAMP(7) not null; `NumericValue` DOUBLE nullable; `StateText` WVARCHAR nullable; `IsGood` BIT not null.

**Expected fixture rows**, exactly as in `fixtures/expected_pressure_window.csv`:

| EventUtc | NumericValue | StateText | IsGood | Why it is here |
|---|---|---|---|---|
| 2026-01-15 08:00:00.0000000 | 101.5 | NULL | 1 | Normal |
| 2026-01-15 08:00:10.0000000 | 0 | NULL | 1 | A real zero |
| 2026-01-15 08:00:10.0000153 | 102.25 | NULL | 1 | Close change |
| 2026-01-15 08:00:10.0000153 | 102.5 | NULL | 1 | **Repeated timestamp**: distinct event |
| 2026-01-15 08:05:00.0000000 | NULL | Bad Input | 0 | Bad state: not zero |
| 2026-01-15 08:10:00.0000000 | 103 | NULL | 1 | Questionable flag set (not selected here) |
| 2026-01-15 08:15:00.0000000 | 104 | NULL | 1 | Substituted and annotated flags set (not selected here) |

The event at 10:00 falls outside the window. `09:00` to `10:00` is the empty window: success with zero rows.

**Exact upstream operation** (`strata_explain` output; the fixture trace is in the [evidence log](evidence-log.md)):

```
getPoint(PT-PRESSURE-001) + readRecorded(point=PT-PRESSURE-001, start=2026-01-15T08:00:00.0000000Z,
                                         end=2026-01-15T09:00:00.0000000Z, boundary=Inside, pageSize=1000)
```

With `Backend=piwebapi` that becomes:

```
GET {BaseUrl}/points/PT-PRESSURE-001?webIdType=IDOnly&selectedFields=WebId;Name;...
GET {BaseUrl}/streams/PT-PRESSURE-001/recorded?startTime=2026-01-15T08%3A00%3A00.0000000Z
    &endTime=2026-01-15T09%3A00%3A00.0000000Z&boundaryType=Inside&maxCount=1000&selectedFields=Items.Timestamp;...
```

Both source bounds are inclusive, so the driver re-applies the SQL predicates locally. That drops an event exactly at 09:00 and keeps the end exclusive. A live `PointId` is an `IDOnly` WebId from the `Points` table.

**M import equivalent.** `Odbc.Query` takes a SQL string, so the bounds are literal:

```powerquery
Odbc.Query("dsn=StrataFixture",
    "SELECT EventUtc, NumericValue, StateText, IsGood FROM RecordedValues " &
    "WHERE PointId = 'PT-PRESSURE-001' " &
    "AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}")
```

The smoke test proves that this literal form and the bound-parameter form return identical rows.

## Metadata probes

None of these reach a source: `SQLPrepare`, `SQLNumResultCols`, `SQLDescribeCol`, `SQLColAttribute`, `SQLDescribeParam`, `SQLTables`, `SQLColumns`, `SQLGetTypeInfo`, `WHERE 1=0`, `TOP 0`. Only `SQLExecute` of a bounded query reads history.

## Navigator preview of a large table

Navigator issues an unfiltered `SELECT` when a table is selected. The driver cannot tell that apart from a real full import, so behavior is set per DSN:

| | `NavigatorPreview=1` (authoring DSN) | `NavigatorPreview=0` (default; gateway DSN) |
|---|---|---|
| Unfiltered `SELECT ... FROM RecordedValues` | One point, last `PreviewWindowHours` (24), at most `PreviewRows` (100). Returns `SQL_SUCCESS_WITH_INFO` with `01000`, and every row has `ResultScope='PREVIEW'` | Refused with `42000` and the fix in the message |
| Any `WHERE` that does not bound point and time | Refused | Refused |
| Bounded query | Normal, `ResultScope='BOUNDED'` | Normal |

Why a preview is only an authoring convenience: **a filter Power Query applies locally instead of folding would run on the preview sample and look like a successful import.** Three guards limit that risk:

1. Gateway nodes use `NavigatorPreview=0` under the same DSN name, so such a model fails its first refresh loudly.
2. `ResultScope` is in the data, and the report page can show it.
3. The Fabric notebook rejects any batch containing preview rows.

Import from explicit bounded SQL, as in `powerquery/examples.pq`. Whether the DSN-level difference behaves as described in Power BI is part of Gate A and Gate C.

## SQLGetInfo caveats

ODBC has no value below `SQL_SC_SQL92_ENTRY` or `SQL_OIC_CORE`, so these two necessarily overstate the driver: it implements a subset of each. Clients should go by the finer-grained answers, which are accurate and tested: aggregates 0, joins 0, `GROUP BY` unsupported, forward-only cursor, predicates listed above. Functions not implemented report `FALSE` in `SQLGetFunctions`: `SQLBulkOperations`, `SQLSetPos`, `SQLBrowseConnect`, `SQLPutData`, `SQLParamData`, `SQLGetCursorName`, `SQLSetCursorName`, `SQLCopyDesc`, `SQLGetDescRec`, `SQLSetDescRec`. A Power BI Desktop trace will show whether any is needed.
