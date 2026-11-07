// Query name: Points. Relate this to the event tables on PointId, not on TagName: a tag can be renamed.
let
    Source = Odbc.Query("dsn=StrataFixture", "SELECT PointId, TagName, Descriptor, PointType, EngUnits, DigitalSetName, IsStep FROM Points")
in
    Source
