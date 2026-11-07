// Query name: RecordedIncremental. Create DateTime parameters RangeStart and RangeEnd holding UTC values first.
// RangeStart is inclusive and RangeEnd exclusive, in both the SQL and the filter step.
// Power BI may warn that the query doesn't fold. The SQL already carries the bounds; confirm with the driver's TraceFile.
let
    Literal = (d as datetime) as text => DateTime.ToText(d, [Format = "yyyy-MM-dd HH:mm:ss", Culture = "en-US"]),
    Sql =
        "SELECT PointId, TagName, EventUtc, EventUtcRaw, EventSeq, ValueKind, NumericValue, DigitalCode, StateText, EngUnits, IsGood, IsQuestionable, IsSubstituted, IsAnnotated " &
        "FROM RecordedValues WHERE TagName = 'REPLACE-WITH-TAG-NAME' " &
        "AND EventUtc >= {ts '" & Literal(RangeStart) & "'} AND EventUtc < {ts '" & Literal(RangeEnd) & "'}",
    Source = Odbc.Query("dsn=StrataPI", Sql),
    Filtered = Table.SelectRows(Source, each [EventUtc] >= RangeStart and [EventUtc] < RangeEnd)
in
    Filtered
