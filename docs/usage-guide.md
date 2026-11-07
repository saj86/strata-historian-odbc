# Strata Historian ODBC Driver: Usage Guide

## Overview and status

Any Windows x64 application that speaks ODBC can read historian data through this driver with ordinary read-only SQL, including PowerShell, C# and Power BI. It exposes two tables, `Points` and `RecordedValues`, and needs no PI OLE DB or PI SQL Client on the machine.

**So far it has only been tested with synthetic fixture data.** Every result below comes from the fixture, which is labelled `SourceBackend = fixture` on each row. The PI Web API backend is written but has never talked to a real server.

| Area | Status |
| --- | --- |
| Driver logic through a real ODBC driver manager (macOS, unixODBC) | Passed |
| Windows x64 DLL compiles and links | Passed |
| Driver running on Windows | Not run |
| Power BI Desktop and gateway refresh | Not run |
| Live read from a PI server | Not run |

**Check licensing before using it on real data.** AVEVA's [PI System software schedule](https://www.aveva.com/content/dam/aveva/documents/legal/policies/AVEVA-Software-Schedule-PI-System-v1-5-June-2026.pdf), section 3.3(b)(iv), requires a PSA licence for non-AVEVA programs that programmatically access PI data. Confirm with your AVEVA contract owner that this driver is covered.

## Install and create a DSN

Install the driver once per machine, then create a 64-bit System DSN; your applications connect to the DSN by name. Use an elevated 64-bit PowerShell for all of this. These Windows steps have not been run yet, so report anything that doesn't match.

**Test machine: use the unsigned package.** It needs no Visual Studio or Python, and is not for gateway or production hosts.

```powershell
cd C:\strata-test     # the unzipped folder
powershell -ExecutionPolicy Bypass -File .\scripts\Test-Package.ps1
```

This installs the driver, creates the `StrataFixture` and `StrataFixturePreview` DSNs, and runs the tests. It should end with `ALL PASSED`. To remove it:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\Install-Portable.ps1 -Uninstall
```

**Any machine: use the installer.** Build the MSI on Windows with one command. It needs the .NET SDK, but not Visual Studio:

```powershell
.\scripts\Build-Installer.ps1
```

Double-click the MSI for the setup wizard: Welcome, licence, then choose the features and folder. Features are the driver (required), documents and examples, and an experimental OLE DB data link. On the last page, leave **Open ODBC Data Sources** ticked to set up a data source straight away. The installer and the setup window haven't been run yet, so report anything that doesn't match.

**Set up a data source in Windows.** Open **ODBC Data Sources (64-bit)** from the Start menu, choose the **System DSN** tab, then **Add**, pick **Strata Historian ODBC Driver**, and select **Finish**. A settings window opens:

| Setting | What to enter |
| --- | --- |
| Data source name | A name your reports will use, for example `StrataPI`. Use the same name on every gateway |
| Data from | **PI Web API**, or **Test data** for the synthetic fixture |
| PI Web API URL | For example `https://piweb.example.local/piwebapi` |
| PI data server | The PI Data Archive name, as `Test-PiWebApi.ps1` lists it |
| Sign-in | Windows, user name and password, or none, matching what PI Web API offers |
| Limits and options | Usually leave the defaults. Tick the preview box only on authoring machines |

Fill in **User name** and **Password** only to try **Test**: they're used once and never saved. Select **OK** to save. **Configure** reopens the same window later.

**Gateway or production host: use the signed MSI.** Build it with your organization's certificate, then install it silently and create the DSN with a script:

```powershell
.\scripts\Install-StrataDriver.ps1 -Action Install -MsiPath .\strataodbc-0.1.0-x64.msi -ExpectedThumbprint <thumbprint>
.\scripts\Provision-StrataDsn.ps1 -Name StrataPI -Backend piwebapi -BaseUrl https://piweb.example.local/piwebapi -DataServer <data server name> -Auth Basic
```

The DSN never holds a username or password. Your application or the gateway connection supplies them at connect time.

**Not sure whether PI Web API is installed?** Ask your PI administrator for approval, then run this read-only check. It sends GET requests only, prints no values, and shows the version and sign-in methods:

```powershell
.\scripts\Test-PiWebApi.ps1 -BaseUrl https://YOUR-SERVER/piwebapi
```

## Connection settings and SQL

Every read from `RecordedValues` must name a point, by `TagName` or `PointId`, and a time window, or the driver refuses it before contacting the source. Connect with `DSN=<name>`, plus `UID` and `PWD` only when the DSN uses `Auth=Basic`.

| DSN setting | Default | Purpose |
| --- | --- | --- |
| `Backend` | Required | `fixture` or `piwebapi`. There is no fallback to fixture data |
| `BaseUrl`, `DataServer` | None | PI Web API address (`https` required) and data server name |
| `Auth` | Basic if `UID` is given, otherwise Anonymous | `Anonymous`, `Basic` or `Negotiate` |
| `MaxWindowDays` | 31 | Longest time window in one query |
| `MaxPointsPerQuery` | 16 | Most points in one query |
| `MaxRows` | 2,000,000 | Row limit per query. Going over is an error, not a cut-off result |
| `PageSize` | 1000 | Events per upstream request |
| `RequestTimeoutMs`, `MaxRetries` | 30000, 3 | Per upstream request |
| `NavigatorPreview` | 0 | 1 allows a capped sample for Power BI Navigator. Keep 0 on gateways |
| `TraceFile` | Off | Writes each upstream request to a JSON-lines file, with no credentials |

The catalog is `STRATA` and the schema is `HISTORIAN`, and both are optional in queries.

| Table | Holds | Main columns |
| --- | --- | --- |
| `Points` | Points this identity may see | `PointId`, `TagName`, `Descriptor`, `PointType`, `EngUnits`, `DigitalSetName`, `IsStep` |
| `RecordedValues` | Archive events | `PointId`, `TagName`, `EventUtc`, `EventUtcRaw`, `EventSeq`, `ValueKind`, `NumericValue`, `TextValue`, `DigitalCode`, `StateText`, `EngUnits`, `IsGood`, `IsQuestionable`, `IsSubstituted`, `IsAnnotated` |

The basic query has this shape, with the start inclusive, the end exclusive and all times in UTC:

```sql
SELECT EventUtc, NumericValue, StateText, IsGood
FROM RecordedValues
WHERE TagName = ? AND EventUtc >= ? AND EventUtc < ?
```

`TagName` ignores letter case. For long-lived pipelines, prefer `PointId` from the `Points` table: a tag rename changes the tag but not the `PointId`.

Three habits keep results correct:

- **Treat NULL as "no number".** A real zero is `0`. A bad state such as `Bad Input` has a NULL `NumericValue` and its label in `StateText`.
- **Don't identify an event by timestamp alone.** Two events can share one, so use `EventUtcRaw` with `EventSeq` to tell them apart within a read.
- **Don't average raw rows.** Recorded events are irregular, so a plain average measures how often data was logged. Summaries aren't implemented yet.

**Supported:** `SELECT` with columns and aliases, `TOP`, `LIMIT`, `WHERE` joined with `AND`, `IN`, `BETWEEN`, `IS [NOT] NULL`, `ORDER BY`, `?` parameters and `{ts '...'}` literals.

**Refused:** writes, joins, `GROUP BY`, aggregates, `OR`, `LIKE`, subqueries and functions.

## PowerShell examples

The `samples/powershell` folder has a small module, `Strata.psm1`, and a runnable `Example.ps1`. They ran on macOS with PowerShell 7 against the fixture and returned the expected 7 rows; Windows is not run yet. Use 64-bit PowerShell.

Run the full example against the fixture:

```powershell
.\samples\powershell\Example.ps1
```

Or use the module directly. Read a window into objects, then filter or export them:

```powershell
Import-Module .\samples\powershell\Strata.psm1
$connection = New-StrataConnection -Dsn StrataFixture

Get-StrataPoint -Connection $connection |
    Format-Table PointId, TagName, PointType, EngUnits

$start = [datetime]::SpecifyKind('2026-01-15 08:00:00', 'Utc')
$end   = [datetime]::SpecifyKind('2026-01-15 09:00:00', 'Utc')

$rows = Get-StrataRecordedValue -Connection $connection -TagName SYN.PRESSURE.PV -Start $start -End $end
$rows | Format-Table EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, IsGood
$rows | Export-Csv .\recorded.csv -NoTypeInformation -Encoding UTF8

$connection.Dispose()
```

Expected output from the fixture:

```text
EventUtcRaw                  EventSeq ValueKind NumericValue StateText IsGood
2026-01-15T08:00:00.0000000Z        0 NUMERIC        101.500           True
2026-01-15T08:00:10.0000000Z        0 NUMERIC          0.000           True
2026-01-15T08:00:10.0000153Z        0 NUMERIC        102.250           True
2026-01-15T08:00:10.0000153Z        1 NUMERIC        102.500           True
2026-01-15T08:05:00.0000000Z        0 SYSTEM                 Bad Input False
2026-01-15T08:10:00.0000000Z        0 NUMERIC        103.000           True
2026-01-15T08:15:00.0000000Z        0 NUMERIC        104.000           True
```

**Long ranges:** `Get-StrataRecordedValue` splits the window into chunks, 24 hours by default, so it stays within `MaxWindowDays`. Change the size with `-ChunkHours`.

**Real data:** prompt for the password rather than storing it, and use your own tag name.

```powershell
$connection = New-StrataConnection -Dsn StrataPI -Credential (Get-Credential)
```

**Refusals:** a refused query throws, and the ODBC error is the inner exception:

```powershell
$command = $connection.CreateCommand()
$command.CommandText = 'SELECT * FROM RecordedValues'
try {
    $command.ExecuteReader().Dispose()
} catch [System.Management.Automation.MethodInvocationException] {
    $_.Exception.InnerException.Errors[0].SQLState   # 42000
}
```

## Power BI examples

Import data with a bounded SQL query through Power BI's built-in ODBC connector; no custom connector is needed. **None of this has run in Power BI yet,** so treat the first attempt as a test. The queries are in `samples/powerbi`.

**Fastest start: open the ready-made project.** `samples/powerbi/project/StrataHistorian.pbip` is a Power BI Project with the model and a one-page report already built. It passed Microsoft's JSON schemas and TMDL parser, but hasn't been opened in Power BI Desktop yet.

1. Install the driver and fixture DSNs, then open `StrataHistorian.pbip` in a recent Power BI Desktop.
2. The project holds no data, so choose **Refresh**. If asked, choose Default or Custom credentials and approve running the native queries.
3. Check the page: Data Source `fixture`, Events 7, Good Events 85.7%, Bad Or System Events 1, Questionable Or Substituted 2, Last Source Event 2026-01-15 08:15:00, and 4 run status rows. Preview Rows In Model should be blank.

For real data, go to Transform data > Edit parameters and change `Dsn`, `PressureTag`, `RunTag`, `WindowStartUtc` and `WindowEndUtc`.

The steps below build the same thing by hand.

**Import a window.** In Power BI Desktop choose Get Data > Blank Query > Advanced Editor, then paste:

```powerquery
let
    Dsn = "StrataFixture",
    Tag = "SYN.PRESSURE.PV",
    WindowStartUtc = #datetime(2026, 1, 15, 8, 0, 0),
    WindowEndUtc = #datetime(2026, 1, 15, 9, 0, 0),
    Literal = (d as datetime) as text => DateTime.ToText(d, [Format = "yyyy-MM-dd HH:mm:ss", Culture = "en-US"]),
    Sql =
        "SELECT PointId, TagName, EventUtc, EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, EngUnits, IsGood, IsQuestionable, IsSubstituted, IsAnnotated, ResultScope, SourceBackend " &
        "FROM RecordedValues WHERE TagName = '" & Text.Replace(Tag, "'", "''") & "' " &
        "AND EventUtc >= {ts '" & Literal(WindowStartUtc) & "'} AND EventUtc < {ts '" & Literal(WindowEndUtc) & "'}",
    Source = Odbc.Query("dsn=" & Dsn, Sql)
in
    Source
```

Name the query `PressureEvents`. With the fixture it should load exactly 7 rows.

**Browse tables.** Get Data > ODBC > `StrataFixturePreview` opens Navigator with `STRATA` > `HISTORIAN` > `Points` and `RecordedValues`. Selecting `RecordedValues` shows at most 100 sample rows from one point, marked `ResultScope = PREVIEW`. That's only for looking around: don't build a report on it. The same step on `StrataFixture` returns an error instead of data.

**Add measures** from `samples/powerbi/Measures.dax`. Load `RefreshStamp.pq` first, because it holds the refresh time.

```dax
Events = COUNTROWS ( PressureEvents )
Good Events % = DIVIDE ( CALCULATE ( [Events], PressureEvents[IsGood] = TRUE () ), [Events] )
Last Source Event UTC = MAX ( PressureEvents[EventUtc] )
Last Refresh UTC = MAX ( RefreshStamp[RefreshedUtc] )
Preview Rows In Model = CALCULATE ( [Events], PressureEvents[ResultScope] = "PREVIEW" )
```

The fixture should give Events 7, Good Events 85.7% and Last Source Event 2026-01-15 08:15:00. `Preview Rows In Model` should be blank, since no rows match; a number there means preview rows got into the model. `Good Events %` counts events rather than time, so don't read it as percent of time good.

**Build a simple report page** from these visuals:

| Visual | Fields | Note |
| --- | --- | --- |
| Pressure trend (line) | `EventUtc`, `NumericValue` | Filter to `ValueKind = NUMERIC`. Leave gaps as gaps |
| Run status (table) | `EventUtc`, `StateText`, `ValueKind` from `RunEvents.pq` | Sort by `EventUtcRaw`, then `EventSeq` |
| Quality (cards) | `Good Events %`, `Events` |  |
| Freshness (two cards) | `Last Source Event UTC`, `Last Refresh UTC` | Keep both visible: they answer different questions |

**Publish and refresh through a gateway:**

1. On every gateway node, install the same driver version and create a DSN with the **same name**, using `NavigatorPreview=0`.
2. In Desktop, publish the report to your workspace.
3. In the Power BI service, open Manage connections and gateways and create an on-premises **ODBC** connection on your gateway cluster, with the connection string `dsn=StrataPI` and your credentials.
4. In the semantic model's settings, map the data source to that connection.
5. Choose Refresh now and check the refresh history.

Microsoft notes that attributes after `DSN=` in a connection string aren't included, so keep every setting in the DSN itself.

**Large history:** use incremental refresh with `samples/powerbi/IncrementalRefresh.pq`, so that each partition reads only its own time span. Power BI may warn that the query doesn't fold, and that's expected here because the bounds are already in the SQL. Turn on `TraceFile` to confirm what each partition read.

## Excel

Excel uses a similar ODBC path, so it's a quick first check once the driver is installed. It hasn't been run yet.

1. In Excel, choose Data > Get Data > From Other Sources > From ODBC.
2. Pick the `StrataFixture` DSN, open Advanced options, and paste this SQL:

```sql
SELECT EventUtc, NumericValue, StateText, IsGood FROM RecordedValues WHERE TagName = 'SYN.PRESSURE.PV' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}
```

3. Choose Default or Custom credentials, then Load. You should get 7 rows.

## OLE DB (experimental)

Programs that only speak OLE DB can use the driver through Microsoft's built-in **OLE DB Provider for ODBC Drivers** (MSDASQL), which is part of Windows. This hasn't been tested yet, so expect some features to fail, and report the errors.

- **Data Link Properties window:** the installer's **OLE DB data link** Start menu item opens the classic OLE DB connection window. Choose your data source on the Connection tab, then **Test Connection**. To keep changes, copy the `.udl` file to your own folder first, because the installed copy is read-only.
- **Connection string:** `Provider=MSDASQL;DSN=StrataPI`
- **Example:** `samples\powershell\OleDb-Example.ps1`, in Windows PowerShell 5.1.

A native OLE DB provider, registered under its own name as PI OLE DB is, would be a separate project.

## C# examples

Use the standard `System.Data.Odbc` package: there is no Strata-specific client library. The sample in `samples/csharp/StrataSample` targets .NET 10 and ran on macOS against the fixture, including cancellation; Windows is not run yet. On Windows the project builds as x64 to match the driver.

```powershell
cd samples\csharp\StrataSample
dotnet run
dotnet run -- StrataFixture SYN.RUNSTATE 2026-01-15T07:00:00Z 2026-01-15T09:00:00Z
```

The arguments are DSN, tag name, start and end. For a DSN that uses `Auth=Basic`, the sample reads the login from the `STRATA_UID` and `STRATA_PWD` environment variables.

**Connect:**

```csharp
using System.Data.Odbc;

var builder = new OdbcConnectionStringBuilder { Dsn = "StrataFixture" };
using var connection = new OdbcConnection(builder.ConnectionString);
connection.Open();
```

**Read a window with parameters.** Parameters are positional. Check `IsDBNull` on nullable columns so that a bad reading never turns into 0:

```csharp
const string sql = """
    SELECT EventUtc, EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, IsGood
    FROM RecordedValues
    WHERE TagName = ? AND EventUtc >= ? AND EventUtc < ?
    """;

using var command = new OdbcCommand(sql, connection) { CommandTimeout = 300 };
command.Parameters.Add("Tag", OdbcType.NVarChar, 256).Value = "SYN.PRESSURE.PV";
command.Parameters.Add("Start", OdbcType.DateTime).Value = new DateTime(2026, 1, 15, 8, 0, 0, DateTimeKind.Utc);
command.Parameters.Add("End", OdbcType.DateTime).Value = new DateTime(2026, 1, 15, 9, 0, 0, DateTimeKind.Utc);

using var reader = command.ExecuteReader();
while (reader.Read())
{
    string raw = reader.GetString(1);
    int seq = reader.GetInt32(2);
    string kind = reader.GetString(3);
    double? value = reader.IsDBNull(4) ? null : reader.GetDouble(4);
    string? state = reader.IsDBNull(5) ? null : reader.GetString(5);
    bool good = reader.GetBoolean(6);
    Console.WriteLine($"{raw} seq={seq} {kind} {value?.ToString() ?? $"[{state}]"} good={good}");
}
```

**Handle refusals by SQLSTATE,** not by message text:

```csharp
try
{
    using var refused = new OdbcCommand("SELECT * FROM RecordedValues", connection);
    using var rows = refused.ExecuteReader();
}
catch (OdbcException ex)
{
    Console.WriteLine(ex.Errors[0].SQLState);   // 42000
}
```

**Cancel a long read.** ODBC calls block, so run the read on another thread and connect the token to `OdbcCommand.Cancel`. A canceled read fails with SQLSTATE `HY008`:

```csharp
using CancellationTokenRegistration registration = token.Register(command.Cancel);
await Task.Run(() =>
{
    using var reader = command.ExecuteReader();
    while (reader.Read())
    {
        token.ThrowIfCancellationRequested();
        // use the row
    }
}, token);
```

The sample's complete `ReadRecordedAsync` method turns `HY008` into an `OperationCanceledException`. Its last lines of output against the fixture:

```text
--- Refusals
42000: refused: unbounded read, over a limit, or access denied
0A000: refused: not supported, or a write
42000: refused: unbounded read, over a limit, or access denied

--- Schema probe
EventUtc: DateTime
NumericValue: Double
IsGood: Boolean

--- Cancellation
Canceled after 308 ms.
```

## Troubleshooting

Start from the SQLSTATE in the error: the driver uses it to say why a call failed, and the message text gives the detail.

| SQLSTATE | Meaning | What to do |
| --- | --- | --- |
| `42000` | Read refused: no point or time bounds, over a limit, or access denied | Add `TagName` or `PointId` and both `EventUtc` bounds, or split the window. If it says access denied, ask the PI administrator |
| `0A000` | Not supported: a write, join, `GROUP BY`, `OR`, `LIKE` or similar | Rewrite within the supported SQL, or do that step in your application or model |
| `42S02` | Table, tag or point not found, or not visible to this identity | Check the name against the `Points` table |
| `42S22` | Unknown column | Check the column list in this guide |
| `07002` | A `?` has no bound parameter | Bind one value per `?`, in order |
| `22007` | Bad date or time value | Use `yyyy-MM-dd HH:mm:ss`, or a typed timestamp parameter |
| `28000` | Authentication failed, or `Auth=Basic` with no login | Supply `UID` and `PWD` at connect time; the driver ignores any stored in a DSN |
| `08001` | Can't connect: missing `Backend`, invalid setting, or TLS certificate failure | Read the message. Fix certificates in the host's trust store, since validation can't be turned off |
| `08S01` | Connection failed during a read | The result is incomplete, so retry the whole window |
| `HYT00`, `HY008` | Timed out, canceled | Increase the timeout or narrow the window |
| `HY000` | Other failure, including going over `MaxRows`, or an unexpected server response | Read the message, and narrow the window if it names `MaxRows` |

Other common problems:

- **`IM002`, data source name not found:** usually a 32-bit application or PowerShell. The DSN is in the 64-bit hive, so use a 64-bit process.
- **`unsupported-backend`:** the DSN has `Backend=native`, which isn't implemented. Use `piwebapi`.
- **Every row shows `SourceBackend = fixture`:** you're connected to the test DSN, and this is synthetic data.
- **Rows show `ResultScope = PREVIEW`:** a capped sample reached your query. Add point and time bounds.
- **Refresh works in Desktop but fails on the gateway:** expected if the model relied on the preview, as gateway DSNs refuse it. Use bounded SQL. If that isn't the cause, check that each node has the same driver version and DSN name.

To check a machine, run this. It reports the driver, DSN, network, certificate and a bounded query, and exits non-zero on failure:

```powershell
.\scripts\Test-StrataNode.ps1 -Dsn StrataFixture -ExpectedRows 7 -SmokeSql "SELECT EventUtc FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}"
```

For a report, keep the `SQLSTATE`, the full message and the `TraceFile` output. The trace holds point IDs and time windows, so review it before sharing.
