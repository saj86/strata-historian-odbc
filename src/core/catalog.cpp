#include "catalog.h"

namespace strata {

const TableDef& pointsTable() {
    static const TableDef t{"Points", "Historian points visible to the connected identity.", {
        {"SourceBackend", ColType::Text, 32, false, "Backend that produced this row: fixture or piwebapi."},
        {"SourceServer", ColType::Text, 128, false, "Data server identity."},
        {"PointId", ColType::Text, 256, false, "Stable point identity; survives tag renames."},
        {"TagName", ColType::Text, 256, false, "Current tag name."},
        {"Descriptor", ColType::Text, 512, true, "Point descriptor."},
        {"PointClass", ColType::Text, 64, true, "Point class."},
        {"PointType", ColType::Text, 64, true, "Source point type."},
        {"EngUnits", ColType::Text, 64, true, "Engineering units."},
        {"DigitalSetName", ColType::Text, 128, true, "Digital state set; NULL for non-digital points."},
        {"IsStep", ColType::Bit, 1, true, "Stepped interpretation; NULL when the source does not expose it."},
    }};
    return t;
}

const TableDef& recordedValuesTable() {
    static const TableDef t{"RecordedValues", "Recorded archive events. Requires PointId and a bounded EventUtc window.", {
        {"SourceBackend", ColType::Text, 32, false, "Backend that produced this row: fixture or piwebapi."},
        {"SourceServer", ColType::Text, 128, false, "Data server identity."},
        {"PointId", ColType::Text, 256, false, "Stable point identity."},
        {"TagName", ColType::Text, 256, false, "Tag name at read time."},
        {"EventUtc", ColType::Timestamp, 27, false, "UTC event time, 100 ns resolution."},
        {"EventUtcRaw", ColType::Text, 40, false, "Event time exactly as the source returned it."},
        {"EventSeq", ColType::Int32, 11, false, "Source-order ordinal among events sharing EventUtcRaw for this point and read."},
        {"RetrievalMode", ColType::Text, 16, false, "Always RECORDED for this table."},
        {"ValueKind", ColType::Text, 16, false, "NUMERIC, DIGITAL, TEXT or SYSTEM (system state such as Bad Input)."},
        {"NumericValue", ColType::Double, 24, true, "Numeric value. NULL unless ValueKind is NUMERIC; a real zero stays 0."},
        {"TextValue", ColType::Text, 1024, true, "String value. NULL unless ValueKind is TEXT."},
        {"DigitalCode", ColType::Int32, 11, true, "Digital or system state code when available."},
        {"StateText", ColType::Text, 128, true, "Digital or system state label."},
        {"EngUnits", ColType::Text, 64, true, "Units returned with the event."},
        {"IsGood", ColType::Bit, 1, false, "Source Good flag."},
        {"IsQuestionable", ColType::Bit, 1, false, "Source Questionable flag."},
        {"IsSubstituted", ColType::Bit, 1, false, "Source Substituted flag."},
        {"IsAnnotated", ColType::Bit, 1, false, "Source Annotated flag."},
        {"ResultScope", ColType::Text, 16, false, "BOUNDED for a requested window; PREVIEW for a capped Navigator sample."},
    }};
    return t;
}

const TableDef* findTable(std::string_view name) {
    if (iequals(name, pointsTable().name)) return &pointsTable();
    if (iequals(name, recordedValuesTable().name)) return &recordedValuesTable();
    return nullptr;
}

}  // namespace strata
