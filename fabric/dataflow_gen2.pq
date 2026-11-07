// Dataflow Gen2 variant. Choose this route when the site is Windows-integrated only: the connector matrix
// lists Anonymous, Basic and Windows for Dataflow Gen2, but only Anonymous and Basic for Pipeline Copy.
// Status: Not run. Add a Lakehouse destination explicitly in the editor and compare row counts to the control record.
let
    WindowStartUtc = #datetime(2026, 1, 15, 8, 0, 0),
    WindowEndUtc = #datetime(2026, 1, 15, 9, 0, 0),
    PointId = "PT-PRESSURE-001",
    ToLiteral = (d as datetime) as text => DateTime.ToText(d, [Format = "yyyy-MM-dd HH:mm:ss", Culture = "en-US"]),
    Sql = "SELECT * FROM RecordedValues WHERE PointId = '" & Text.Replace(PointId, "'", "''") & "' " &
          "AND EventUtc >= {ts '" & ToLiteral(WindowStartUtc) & "'} AND EventUtc < {ts '" & ToLiteral(WindowEndUtc) & "'}",
    Source = Odbc.Query("dsn=StrataPI", Sql),
    Typed = Table.TransformColumnTypes(Source, {{"EventUtc", type datetime}, {"NumericValue", type number}, {"EventSeq", Int64.Type}, {"IsGood", type logical}})
in
    Typed
