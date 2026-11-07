// The second interface: how the driver obtains history. Transport details stay behind it;
// the SQL parser and ODBC layer never see them. Fixture and live share this typed contract.
#pragma once
#include <memory>
#include <optional>

#include "../core/common.h"

namespace strata {

struct PointInfo {
    std::string sourceServer, pointId, tagName;
    std::optional<std::string> descriptor, pointClass, pointType, engUnits, digitalSet;
    std::optional<bool> step;
};
struct PointQuery {
    std::optional<std::string> tagName;  // exact match pushed upstream when present
    int pageSize = 1000;
    std::string continuation;
};
struct PointPage {
    std::vector<PointInfo> points;
    std::string continuation;  // empty when complete
};

enum class ValueKind { Numeric, Digital, Text, System };
struct RecordedEvent {
    Ticks time;
    std::string rawTime;
    ValueKind kind = ValueKind::Numeric;
    std::optional<double> numeric;
    std::optional<std::string> text;
    std::optional<int32_t> digitalCode;
    std::optional<std::string> stateText;
    std::optional<std::string> units;
    bool good = false, questionable = false, substituted = false, annotated = false;
};
// Both ends inclusive at the source; exclusive SQL bounds are enforced above this interface.
struct RecordedRequest {
    std::string pointId;
    Ticks start, end;
    int pageSize = 1000;
    std::string continuation;
    std::string correlationId;
};
struct RecordedPage {
    std::vector<RecordedEvent> events;
    std::string continuation;  // empty when the window is complete
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::string name() const = 0;     // value of the SourceBackend column
    virtual std::string server() const = 0;   // value of the SourceServer column
    virtual void testConnection(CancelToken& cancel) = 0;
    virtual PointPage listPoints(const PointQuery& q, CancelToken& cancel) = 0;
    // Throws 42000 when the identity may not read the point; never returns an empty page instead.
    virtual PointInfo getPoint(const std::string& pointId, CancelToken& cancel) = 0;
    virtual RecordedPage readRecorded(const RecordedRequest& r, CancelToken& cancel) = 0;
    virtual Ticks previewEnd() const = 0;
};

std::unique_ptr<Backend> makeFixtureBackend();

}  // namespace strata
