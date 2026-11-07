// The driver-owned logical catalog: identical on every host and for every backend.
#pragma once
#include "common.h"

namespace strata {

inline constexpr const char* kCatalogName = "STRATA";
inline constexpr const char* kSchemaName = "HISTORIAN";

struct TableDef {
    std::string name;
    std::string description;
    std::vector<ColumnDef> columns;
    int index(std::string_view column) const {
        for (size_t i = 0; i < columns.size(); i++)
            if (iequals(columns[i].name, column)) return static_cast<int>(i);
        return -1;
    }
};

// Column order is part of the public contract; docs/data-contract.md mirrors it.
enum PointsCol { PC_SourceBackend, PC_SourceServer, PC_PointId, PC_TagName, PC_Descriptor, PC_PointClass, PC_PointType, PC_EngUnits, PC_DigitalSetName, PC_IsStep };
enum RecordedCol {
    RC_SourceBackend, RC_SourceServer, RC_PointId, RC_TagName, RC_EventUtc, RC_EventUtcRaw, RC_EventSeq, RC_RetrievalMode, RC_ValueKind,
    RC_NumericValue, RC_TextValue, RC_DigitalCode, RC_StateText, RC_EngUnits, RC_IsGood, RC_IsQuestionable, RC_IsSubstituted, RC_IsAnnotated, RC_ResultScope
};

const TableDef& pointsTable();
const TableDef& recordedValuesTable();
const TableDef* findTable(std::string_view name);

}  // namespace strata
