// Query name: RunEvents. A digital point: read StateText and DigitalCode; NumericValue stays null.
let
    Source = Odbc.Query(
        "dsn=StrataFixture",
        "SELECT TagName, EventUtc, EventUtcRaw, EventSeq, ValueKind, DigitalCode, StateText, IsGood " &
        "FROM RecordedValues WHERE TagName = 'SYN.RUNSTATE' " &
        "AND EventUtc >= {ts '2026-01-15 07:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}")
in
    Source
