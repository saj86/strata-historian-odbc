// Offline deterministic backend. SourceBackend='fixture' on every row: this is never PI data.
#include <algorithm>

#include "backend.h"

namespace strata {
namespace {

struct FixtureEvent {
    const char* raw;
    ValueKind kind;
    double num;
    int code;
    const char* state;
    bool good, questionable, substituted, annotated;
};

constexpr const char* PRESSURE = "PT-PRESSURE-001";
constexpr const char* RUNSTATE = "PT-RUNSTATE-001";
constexpr const char* DENIED = "PT-DENIED-001";
constexpr const char* RAMP = "PT-RAMP-001";    // 2500 events: page boundaries (not listed)
constexpr const char* FAULT = "PT-FAULT-001";  // fails on page 2: partial failure (not listed)
constexpr const char* SLOW = "PT-SLOW-001";    // 50 ms per page: cancel and timeout (not listed)

const FixtureEvent kPressure[] = {
    {"2026-01-15T08:00:00.0000000Z", ValueKind::Numeric, 101.5, 0, nullptr, true, false, false, false},
    {"2026-01-15T08:00:10.0000000Z", ValueKind::Numeric, 0.0, 0, nullptr, true, false, false, false},  // a real zero
    {"2026-01-15T08:00:10.0000153Z", ValueKind::Numeric, 102.25, 0, nullptr, true, false, false, false},  // close change
    {"2026-01-15T08:00:10.0000153Z", ValueKind::Numeric, 102.5, 0, nullptr, true, false, false, false},  // repeated timestamp
    {"2026-01-15T08:05:00.0000000Z", ValueKind::System, 0, 307, "Bad Input", false, false, false, false},  // bad state
    {"2026-01-15T08:10:00.0000000Z", ValueKind::Numeric, 103.0, 0, nullptr, true, true, false, false},  // questionable
    {"2026-01-15T08:15:00.0000000Z", ValueKind::Numeric, 104.0, 0, nullptr, true, false, true, true},  // substituted+annotated
    {"2026-01-15T10:00:00.0000000Z", ValueKind::Numeric, 105.0, 0, nullptr, true, false, false, false},  // after the empty 09:00-10:00 window
};
const FixtureEvent kRun[] = {
    {"2026-01-15T07:59:00.0000000Z", ValueKind::Digital, 0, 0, "Stopped", true, false, false, false},
    {"2026-01-15T08:00:00.0000000Z", ValueKind::Digital, 0, 1, "Running", true, false, false, false},
    {"2026-01-15T08:12:00.0000000Z", ValueKind::Digital, 0, 0, "Stopped", true, false, false, false},
    {"2026-01-15T08:15:00.0000000Z", ValueKind::System, 0, 246, "I/O Timeout", false, false, false, false},
};

RecordedEvent toEvent(const FixtureEvent& f, const char* units) {
    RecordedEvent e;
    e.rawTime = f.raw;
    parseUtc(f.raw, e.time);
    e.kind = f.kind;
    if (f.kind == ValueKind::Numeric) e.numeric = f.num;
    if (f.state) { e.stateText = f.state; e.digitalCode = f.code; }
    if (units) e.units = units;
    e.good = f.good;
    e.questionable = f.questionable;
    e.substituted = f.substituted;
    e.annotated = f.annotated;
    return e;
}

std::vector<RecordedEvent> generated(int count) {
    std::vector<RecordedEvent> v;
    v.reserve(static_cast<size_t>(count));
    const Ticks t0 = fromCivil(2026, 1, 16, 0, 0, 0, 0);
    for (int i = 0; i < count; i++) {
        RecordedEvent e;
        e.time = Ticks{t0.v + i * kTicksPerSecond};
        e.rawTime = formatUtc(e.time);
        e.numeric = static_cast<double>(i);
        e.units = "count";
        e.good = true;
        v.push_back(std::move(e));
    }
    return v;
}

class FixtureBackend final : public Backend {
public:
    std::string name() const override { return "fixture"; }
    std::string server() const override { return "FIXTURE-V1"; }
    void testConnection(CancelToken& c) override { c.check(); }
    Ticks previewEnd() const override { return fromCivil(2026, 1, 15, 12, 0, 0, 0); }

    PointPage listPoints(const PointQuery& q, CancelToken& c) override {
        c.check();
        // The denied point is invisible: metadata is authorized as well as values.
        std::vector<PointInfo> all = {info(PRESSURE), info(RUNSTATE)};
        PointPage page;
        for (auto& p : all)
            if (!q.tagName || iequals(*q.tagName, p.tagName)) page.points.push_back(p);
        return page;
    }
    PointInfo getPoint(const std::string& id, CancelToken& c) override {
        c.check();
        if (id == DENIED) throw Error("42000", "Access denied reading point '" + id + "'.", 403);
        if (id == PRESSURE || id == RUNSTATE || id == RAMP || id == FAULT || id == SLOW) return info(id);
        throw Error("42S02", "Point '" + id + "' was not found or is not visible to this identity.", 404);
    }
    RecordedPage readRecorded(const RecordedRequest& r, CancelToken& c) override {
        c.check();
        if (r.pointId == DENIED) throw Error("42000", "Access denied reading point '" + r.pointId + "'.", 403);
        std::vector<RecordedEvent> all;
        if (r.pointId == PRESSURE) for (auto& f : kPressure) all.push_back(toEvent(f, "kPa"));
        else if (r.pointId == RUNSTATE) for (auto& f : kRun) all.push_back(toEvent(f, nullptr));
        else if (r.pointId == RAMP || r.pointId == FAULT || r.pointId == SLOW) all = generated(2500);
        else throw Error("42S02", "Point '" + r.pointId + "' was not found or is not visible to this identity.", 404);

        size_t skip = r.continuation.empty() ? 0 : std::stoul(r.continuation);
        if (r.pointId == FAULT && skip > 0) throw Error("08S01", "Fixture fault: the source connection failed after a complete page; the result is incomplete.", 1);
        if (r.pointId == SLOW) c.sleepMs(50);

        std::vector<RecordedEvent> inWindow;
        for (auto& e : all)
            if (e.time >= r.start && e.time <= r.end) inWindow.push_back(std::move(e));
        RecordedPage page;
        size_t stop = std::min(inWindow.size(), skip + static_cast<size_t>(r.pageSize));
        for (size_t i = skip; i < stop; i++) page.events.push_back(std::move(inWindow[i]));
        if (stop < inWindow.size()) page.continuation = std::to_string(stop);
        return page;
    }

private:
    PointInfo info(const std::string& id) const {
        PointInfo p;
        p.sourceServer = server();
        p.pointId = id;
        if (id == PRESSURE) {
            p.tagName = "SYN.PRESSURE.PV";
            p.descriptor = "Synthetic pressure \xE2\x80\x93 \xCE\x94P pr\xC3\xBC" "fstand";  // en dash, Delta, u-umlaut: Unicode path test
            p.pointClass = "classic"; p.pointType = "Float64"; p.engUnits = "kPa"; p.step = false;
        } else if (id == RUNSTATE) {
            p.tagName = "SYN.RUNSTATE";
            p.descriptor = "Synthetic run state";
            p.pointClass = "classic"; p.pointType = "Digital"; p.digitalSet = "RunState"; p.step = true;
        } else {
            p.tagName = "SYN.TEST." + id;
            p.pointClass = "classic"; p.pointType = "Float64"; p.engUnits = "count"; p.step = false;
        }
        return p;
    }
};
}  // namespace

std::unique_ptr<Backend> makeFixtureBackend() { return std::make_unique<FixtureBackend>(); }

}  // namespace strata
