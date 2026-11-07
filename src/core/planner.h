// Turns a parsed query plus bound parameters into a bounded plan, or refuses.
#pragma once
#include <memory>
#include <optional>

#include "../backend/backend.h"
#include "catalog.h"
#include "sql.h"

namespace strata {

struct Limits {
    int maxWindowDays = 31;
    long long maxRows = 2'000'000;
    int pageSize = 1000;
    int maxPointsPerQuery = 16;
    bool navigatorPreview = false;
    int previewWindowHours = 24;
    int previewRows = 100;
};

enum class PlanKind { Empty, Points, Recorded, Preview };

struct BoundPredicate {
    int column;
    CmpOp op;
    std::vector<Value> values;
    bool ignoreCase = false;  // TagName: PI tag names are not case-sensitive
};

struct Plan {
    PlanKind kind = PlanKind::Empty;
    const TableDef* table = nullptr;
    std::vector<int> projection;
    std::vector<ColumnDef> columns;
    std::vector<BoundPredicate> residual;  // always re-applied locally, so exclusive bounds stay exact
    std::vector<std::string> pointIds;
    std::vector<std::string> tagNames;  // resolved to points at read time; empty when PointId was given
    Ticks start{}, end{};
    std::optional<std::string> tagName;
    std::optional<long long> limit;
    std::vector<std::pair<int, bool>> order;
    bool needsSort = false;
    std::string warning;
};

// Metadata for a parsed statement without any source access (SQLPrepare/SQLDescribeCol).
std::vector<ColumnDef> describe(const Query& q);
ColType parameterType(const Query& q, int index);
Plan buildPlan(const Query& q, const std::vector<Value>& params, const Limits& limits);
std::string describeUpstream(const Plan& plan, const Limits& limits);

class RowSource {
public:
    virtual ~RowSource() = default;
    virtual bool next(Row& out) = 0;  // false at end; throws Error on failure (never truncates silently)
};

std::unique_ptr<RowSource> openMemory(std::vector<Row> rows);
std::unique_ptr<RowSource> openPlan(const Plan& plan, Backend& backend, CancelToken& cancel, const Limits& limits, const Trace& trace, const std::string& correlationId);

}  // namespace strata
