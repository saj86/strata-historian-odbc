// Query name: PressureEvents. Paste into Power Query: New Source > Blank Query > Advanced Editor.
// Change the DSN, point and UTC window for real data. Bounds stay in the SQL so the read is bounded.
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
