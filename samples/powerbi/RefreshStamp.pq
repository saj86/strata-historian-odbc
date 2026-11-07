// Query name: RefreshStamp. Keeps "when did we refresh" separate from "when was the last source event".
let
    Source = #table(type table [RefreshedUtc = datetimezone], {{DateTimeZone.UtcNow()}})
in
    Source
