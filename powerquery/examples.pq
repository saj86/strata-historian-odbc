// Power Query M for the built-in ODBC connector. No .mez is needed for any of these.
// Status: syntax not checked in Power BI (Not run). See docs/powerbi-acceptance.md.

// 1. Exact import used by Gate A: bounded native query.
let
    Source = Odbc.Query(
        "dsn=StrataFixture",
        "SELECT EventUtc, NumericValue, StateText, IsGood FROM RecordedValues " &
        "WHERE PointId = 'PT-PRESSURE-001' " &
        "AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}")
in
    Source

// 2. Navigator path: pick the table, then filter. Point and time filters must fold into SQL or
//    the driver rejects the read on a NavigatorPreview=0 DSN. It never returns a silent sample.
let
    Source = Odbc.DataSource("dsn=StrataFixturePreview", [HierarchicalNavigation = true]),
    Catalog = Source{[Name = "STRATA", Kind = "Database"]}[Data],
    Schema = Catalog{[Name = "HISTORIAN", Kind = "Schema"]}[Data],
    Points = Schema{[Name = "Points", Kind = "Table"]}[Data]
in
    Points

// 3. Incremental refresh: RangeStart inclusive, RangeEnd exclusive. Each partition runs its own bounded SQL.
//    Define RangeStart and RangeEnd as DateTime parameters holding UTC values.
let
    Sql = (start as datetime, stop as datetime) as text =>
        "SELECT PointId, TagName, EventUtc, EventUtcRaw, EventSeq, ValueKind, NumericValue, DigitalCode, StateText, EngUnits, IsGood, IsQuestionable, IsSubstituted, IsAnnotated, SourceBackend, SourceServer " &
        "FROM RecordedValues WHERE PointId = 'REPLACE-WITH-POINT-ID' " &
        "AND EventUtc >= {ts '" & DateTime.ToText(start, [Format = "yyyy-MM-dd HH:mm:ss", Culture = "en-US"]) & "'} " &
        "AND EventUtc < {ts '" & DateTime.ToText(stop, [Format = "yyyy-MM-dd HH:mm:ss", Culture = "en-US"]) & "'}",
    Source = Odbc.Query("dsn=StrataPI", Sql(RangeStart, RangeEnd)),
    // Power BI needs this step to recognize the policy; it removes nothing because the SQL is already bounded.
    Filtered = Table.SelectRows(Source, each [EventUtc] >= RangeStart and [EventUtc] < RangeEnd)
in
    Filtered

// 4. Refresh stamp: keeps refresh time distinct from last source-event time. Load as its own table.
let
    Source = #table(type table [RefreshedUtc = datetimezone], {{DateTimeZone.UtcNow()}})
in
    Source
