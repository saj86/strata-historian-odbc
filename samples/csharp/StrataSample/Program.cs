// Usage: dotnet run -- [dsn] [tagName] [startUtc] [endUtc]
// Defaults read the synthetic fixture. Credentials, when needed, come from the environment and never from the DSN.
using System.Data;
using System.Data.Odbc;
using System.Globalization;

string dsn = args.ElementAtOrDefault(0) ?? "StrataFixture";
string tagName = args.ElementAtOrDefault(1) ?? "SYN.PRESSURE.PV";
DateTime start = ParseUtc(args.ElementAtOrDefault(2) ?? "2026-01-15T08:00:00Z");
DateTime end = ParseUtc(args.ElementAtOrDefault(3) ?? "2026-01-15T09:00:00Z");

var builder = new OdbcConnectionStringBuilder { Dsn = dsn };
if (Environment.GetEnvironmentVariable("STRATA_UID") is { Length: > 0 } uid)
{
    builder["UID"] = uid;
    builder["PWD"] = Environment.GetEnvironmentVariable("STRATA_PWD") ?? "";
}

using var connection = new OdbcConnection(builder.ConnectionString);
connection.Open();
Console.WriteLine($"Connected: {connection.Driver} {connection.ServerVersion}");

// 1. Discover points. Only points this identity may see are listed.
Console.WriteLine("\n--- Points");
using (var command = new OdbcCommand("SELECT PointId, TagName, EngUnits, SourceBackend FROM Points", connection))
using (var reader = command.ExecuteReader())
{
    while (reader.Read())
    {
        string units = reader.IsDBNull(2) ? "-" : reader.GetString(2);
        Console.WriteLine($"{reader.GetString(0),-18} {reader.GetString(1),-18} {units,-5} source={reader.GetString(3)}");
    }
}

// 2. Read a bounded window with bound parameters. Parameters are positional.
Console.WriteLine("\n--- Recorded values");
var events = await ReadRecordedAsync(connection, tagName, start, end, CancellationToken.None);
foreach (var e in events)
{
    // Keep "no number" distinct from zero, and show the state for system and digital values.
    string value = e.NumericValue is double n ? n.ToString(CultureInfo.InvariantCulture) : $"[{e.StateText}]";
    Console.WriteLine($"{e.EventUtcRaw}  seq={e.EventSeq}  {e.ValueKind,-8} {value,-14} good={e.IsGood}");
}
Console.WriteLine($"Rows: {events.Count}");

// 3. The driver refuses unbounded reads and writes. Branch on SQLSTATE, not on message text.
Console.WriteLine("\n--- Refusals");
foreach (string sql in new[]
{
    "SELECT * FROM RecordedValues",
    "DELETE FROM Points",
    "SELECT * FROM RecordedValues WHERE PointId = 'PT-DENIED-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}",
})
{
    try
    {
        using var command = new OdbcCommand(sql, connection);
        using var reader = command.ExecuteReader();
        Console.WriteLine($"Unexpectedly allowed: {sql}");
    }
    catch (OdbcException ex)
    {
        OdbcError error = ex.Errors[0];
        Console.WriteLine($"{error.SQLState}: {Describe(error.SQLState)}");
    }
}

// 4. Metadata without touching the source: WHERE 1=0 returns the schema and no rows.
Console.WriteLine("\n--- Schema probe");
using (var command = new OdbcCommand("SELECT EventUtc, NumericValue, IsGood FROM RecordedValues WHERE 1=0", connection))
using (var reader = command.ExecuteReader(CommandBehavior.SchemaOnly))
{
    for (int i = 0; i < reader.FieldCount; i++)
        Console.WriteLine($"{reader.GetName(i)}: {reader.GetFieldType(i).Name}");
}

// 5. Cancel a long read. The token calls OdbcCommand.Cancel, which reaches the driver's SQLCancel.
// This uses a deliberately slow test point that only the fixture has, so skip it on a real DSN.
Console.WriteLine("\n--- Cancellation");
if (dsn.StartsWith("StrataFixture", StringComparison.OrdinalIgnoreCase))
{
    using var slow = new OdbcConnection($"DSN={dsn};PageSize=1");
    slow.Open();
    using var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));
    var clock = System.Diagnostics.Stopwatch.StartNew();
    try
    {
        var rows = await ReadRecordedByIdAsync(slow, "PT-SLOW-001", ParseUtc("2026-01-16T00:00:00Z"), ParseUtc("2026-01-17T00:00:00Z"), cts.Token);
        Console.WriteLine($"Finished before the cancel fired: {rows.Count} rows");
    }
    catch (OperationCanceledException)
    {
        Console.WriteLine($"Canceled after {clock.ElapsedMilliseconds} ms.");
    }
}

static Task<List<RecordedEvent>> ReadRecordedAsync(OdbcConnection connection, string tagName, DateTime startUtc, DateTime endUtc, CancellationToken token) =>
    ReadAsync(connection, "TagName", tagName, startUtc, endUtc, token);

static Task<List<RecordedEvent>> ReadRecordedByIdAsync(OdbcConnection connection, string pointId, DateTime startUtc, DateTime endUtc, CancellationToken token) =>
    ReadAsync(connection, "PointId", pointId, startUtc, endUtc, token);

static async Task<List<RecordedEvent>> ReadAsync(OdbcConnection connection, string column, string point, DateTime startUtc, DateTime endUtc, CancellationToken token)
{
    string sql = $"""
        SELECT EventUtc, EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, IsGood
        FROM RecordedValues
        WHERE {column} = ? AND EventUtc >= ? AND EventUtc < ?
        """;
    using var command = new OdbcCommand(sql, connection) { CommandTimeout = 300 };
    command.Parameters.Add("Point", OdbcType.NVarChar, 256).Value = point;
    command.Parameters.Add("Start", OdbcType.DateTime).Value = startUtc;   // inclusive
    command.Parameters.Add("End", OdbcType.DateTime).Value = endUtc;       // exclusive

    using CancellationTokenRegistration registration = token.Register(command.Cancel);
    var rows = new List<RecordedEvent>();
    try
    {
        // ODBC calls block, so run the read off the caller's thread.
        await Task.Run(() =>
        {
            using var reader = command.ExecuteReader();
            while (reader.Read())
            {
                token.ThrowIfCancellationRequested();
                rows.Add(new RecordedEvent(
                    DateTime.SpecifyKind(reader.GetDateTime(0), DateTimeKind.Utc),
                    reader.GetString(1),
                    reader.GetInt32(2),
                    reader.GetString(3),
                    reader.IsDBNull(4) ? null : reader.GetDouble(4),
                    reader.IsDBNull(5) ? null : reader.GetString(5),
                    reader.GetBoolean(6)));
            }
        }, token);
    }
    catch (OdbcException ex) when (ex.Errors.Count > 0 && ex.Errors[0].SQLState == "HY008")
    {
        throw new OperationCanceledException("Read canceled.", ex, token);
    }
    return rows;
}

static DateTime ParseUtc(string text) =>
    DateTimeOffset.Parse(text, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal).UtcDateTime;

static string Describe(string sqlState) => sqlState switch
{
    "42000" => "refused: unbounded read, over a limit, or access denied",
    "0A000" => "refused: not supported, or a write",
    "42S02" => "not found, or not visible to this identity",
    "42S22" => "unknown column",
    "28000" => "authentication failed",
    "HYT00" => "timed out",
    "HY008" => "canceled",
    "08S01" => "connection failed; the result is incomplete, so retry the window",
    _ => "see the troubleshooting section of the guide",
};

/// <summary>EventUtcRaw and EventSeq together keep events that share a timestamp apart.</summary>
record RecordedEvent(DateTime EventUtc, string EventUtcRaw, int EventSeq, string ValueKind, double? NumericValue, string? StateText, bool IsGood);
