#include "planner.h"

#include <algorithm>
#include <cmath>

namespace strata {
namespace {

Value coerce(const Value& v, ColType to, const std::string& col) {
    if (isNull(v)) return v;
    auto bad = [&](const char* state, const std::string& why) -> Error { return Error(state, "Cannot use value for column '" + col + "': " + why); };
    switch (to) {
        case ColType::Text:
            if (auto s = std::get_if<std::string>(&v)) return *s;
            throw bad("22018", "expected text.");
        case ColType::Timestamp:
            if (auto t = std::get_if<Ticks>(&v)) return *t;
            if (auto s = std::get_if<std::string>(&v)) {
                Ticks t;
                if (parseUtc(*s, t)) return t;
                throw bad("22007", "invalid date/time '" + *s + "'.");
            }
            throw bad("22018", "expected a timestamp.");
        case ColType::Double:
            if (auto d = std::get_if<double>(&v)) return *d;
            if (auto i = std::get_if<int32_t>(&v)) return static_cast<double>(*i);
            if (auto b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
            if (auto s = std::get_if<std::string>(&v)) {
                char* e = nullptr;
                double d = std::strtod(s->c_str(), &e);
                if (!s->empty() && *e == 0) return d;
            }
            throw bad("22018", "expected a number.");
        case ColType::Int16:
        case ColType::Int32: {
            double d;
            if (auto i = std::get_if<int32_t>(&v)) return *i;
            if (auto p = std::get_if<double>(&v)) d = *p;
            else if (auto s = std::get_if<std::string>(&v)) {
                char* e = nullptr;
                d = std::strtod(s->c_str(), &e);
                if (s->empty() || *e != 0) throw bad("22018", "expected an integer.");
            } else throw bad("22018", "expected an integer.");
            if (d != std::floor(d) || d < -2147483648.0 || d > 2147483647.0) throw bad("22003", "not a 32-bit integer.");
            return static_cast<int32_t>(d);
        }
        case ColType::Bit:
            if (auto b = std::get_if<bool>(&v)) return *b;
            if (auto i = std::get_if<int32_t>(&v)) { if (*i == 0 || *i == 1) return *i == 1; }
            if (auto d = std::get_if<double>(&v)) { if (*d == 0 || *d == 1) return *d == 1; }
            if (auto s = std::get_if<std::string>(&v)) {
                if (*s == "1" || iequals(*s, "true")) return true;
                if (*s == "0" || iequals(*s, "false")) return false;
            }
            throw bad("22018", "expected 0 or 1.");
    }
    throw bad("HY000", "unknown type.");
}

// -1, 0, 1; both values non-NULL and of the same alternative after coercion.
int compareValues(const Value& a, const Value& b) {
    if (auto x = std::get_if<std::string>(&a)) { auto& y = std::get<std::string>(b); return x->compare(y) < 0 ? -1 : (*x == y ? 0 : 1); }
    if (auto x = std::get_if<Ticks>(&a)) { auto& y = std::get<Ticks>(b); return *x < y ? -1 : (*x == y ? 0 : 1); }
    if (auto x = std::get_if<double>(&a)) { double y = std::get<double>(b); return *x < y ? -1 : (*x == y ? 0 : 1); }
    if (auto x = std::get_if<int32_t>(&a)) { int32_t y = std::get<int32_t>(b); return *x < y ? -1 : (*x == y ? 0 : 1); }
    if (auto x = std::get_if<bool>(&a)) { bool y = std::get<bool>(b); return *x == y ? 0 : (!*x ? -1 : 1); }
    return 0;
}

bool matches(const BoundPredicate& p, const Row& r) {
    const Value& v = r[static_cast<size_t>(p.column)];
    switch (p.op) {
        case CmpOp::IsNull: return isNull(v);
        case CmpOp::IsNotNull: return !isNull(v);
        default: break;
    }
    if (isNull(v)) return false;
    if (p.op == CmpOp::In) {
        for (auto& o : p.values)
            if (!isNull(o) && compareValues(v, o) == 0) return true;
        return false;
    }
    if (isNull(p.values[0])) return false;
    int c = compareValues(v, p.values[0]);
    switch (p.op) {
        case CmpOp::Eq: return c == 0;
        case CmpOp::Ne: return c != 0;
        case CmpOp::Lt: return c < 0;
        case CmpOp::Le: return c <= 0;
        case CmpOp::Gt: return c > 0;
        case CmpOp::Ge: return c >= 0;
        default: return false;
    }
}

Row emptyRecorded() { return Row(recordedValuesTable().columns.size()); }

template <class T>
Value opt(const std::optional<T>& v) {
    if (v) return Value(*v);
    return Value{};
}

class MemorySource final : public RowSource {
public:
    explicit MemorySource(std::vector<Row> rows) : rows_(std::move(rows)) {}
    bool next(Row& out) override {
        if (i_ >= rows_.size()) return false;
        out = std::move(rows_[i_++]);
        return true;
    }
private:
    std::vector<Row> rows_;
    size_t i_ = 0;
};

// Streams base-table rows from the backend page by page, in source order.
class BaseSource {
public:
    virtual ~BaseSource() = default;
    virtual bool next(Row& out) = 0;
};

class RecordedBase final : public BaseSource {
public:
    RecordedBase(const Plan& plan, Backend& b, CancelToken& c, const Limits& l, const Trace& tr, std::string corr)
        : b_(b), c_(c), lim_(l), trace_(tr), corr_(std::move(corr)), scope_(plan.kind == PlanKind::Preview ? "PREVIEW" : "BOUNDED"), ids_(plan.pointIds), end_(plan.end), start_(plan.start) {
        if (plan.kind == PlanKind::Preview) {
            // One point, one recent window, capped row count. Never a table scan.
            PointQuery pq;
            pq.pageSize = 1;
            auto page = b_.listPoints(pq, c_);
            if (!page.points.empty()) ids_.push_back(page.points.front().pointId);
            end_ = b_.previewEnd();
            start_ = Ticks{end_.v - l.previewWindowHours * kTicksPerHour};
        }
    }
    bool next(Row& out) override {
        for (;;) {
            if (i_ < page_.size()) {
                makeRow(page_[i_++], out);
                return true;
            }
            page_.clear();
            i_ = 0;
            if (started_ && !cont_.empty()) { fetch(); continue; }
            if (p_ >= ids_.size()) return false;
            info_ = b_.getPoint(ids_[p_++], c_);
            started_ = true;
            cont_.clear();
            seqRaw_.clear();
            seq_ = 0;
            fetch();
            if (page_.empty() && cont_.empty()) continue;
        }
    }
private:
    void fetch() {
        c_.check();
        RecordedRequest r;
        r.pointId = info_.pointId;
        r.start = start_;
        r.end = end_;
        r.pageSize = lim_.pageSize;
        r.continuation = cont_;
        r.correlationId = corr_;
        auto pg = b_.readRecorded(r, c_);
        if (trace_.enabled())
            trace_.write(corr_, "readRecorded",
                         "\"backend\":" + Trace::quote(b_.name()) + ",\"point\":" + Trace::quote(r.pointId) + ",\"start\":" + Trace::quote(formatUtc(r.start)) + ",\"end\":" + Trace::quote(formatUtc(r.end)) +
                             ",\"pageSize\":" + std::to_string(r.pageSize) + ",\"rows\":" + std::to_string(pg.events.size()) + ",\"more\":" + (pg.continuation.empty() ? "false" : "true"));
        page_ = std::move(pg.events);
        cont_ = std::move(pg.continuation);
    }
    void makeRow(RecordedEvent& e, Row& r) {
        if (e.rawTime == seqRaw_) seq_++;
        else { seqRaw_ = e.rawTime; seq_ = 0; }
        r = emptyRecorded();
        r[RC_SourceBackend] = b_.name();
        r[RC_SourceServer] = b_.server();
        r[RC_PointId] = info_.pointId;
        r[RC_TagName] = info_.tagName;
        r[RC_EventUtc] = e.time;
        r[RC_EventUtcRaw] = e.rawTime;
        r[RC_EventSeq] = seq_;
        r[RC_RetrievalMode] = std::string("RECORDED");
        const char* k = e.kind == ValueKind::Numeric ? "NUMERIC" : e.kind == ValueKind::Digital ? "DIGITAL" : e.kind == ValueKind::Text ? "TEXT" : "SYSTEM";
        r[RC_ValueKind] = std::string(k);
        r[RC_NumericValue] = opt(e.numeric);
        r[RC_TextValue] = opt(e.text);
        r[RC_DigitalCode] = opt(e.digitalCode);
        r[RC_StateText] = opt(e.stateText);
        r[RC_EngUnits] = opt(e.units);
        r[RC_IsGood] = e.good;
        r[RC_IsQuestionable] = e.questionable;
        r[RC_IsSubstituted] = e.substituted;
        r[RC_IsAnnotated] = e.annotated;
        r[RC_ResultScope] = scope_;
    }
    Backend& b_;
    CancelToken& c_;
    const Limits& lim_;
    const Trace& trace_;
    std::string corr_, scope_;
    std::vector<std::string> ids_;
    Ticks end_, start_;
    size_t p_ = 0;
    PointInfo info_;
    bool started_ = false;
    std::vector<RecordedEvent> page_;
    size_t i_ = 0;
    std::string cont_;
    std::string seqRaw_;
    int32_t seq_ = 0;
};

class PointsBase final : public BaseSource {
public:
    PointsBase(const Plan& plan, Backend& b, CancelToken& c, const Limits& l, const Trace& tr, std::string corr)
        : plan_(plan), b_(b), c_(c), lim_(l), trace_(tr), corr_(std::move(corr)) {}
    bool next(Row& out) override {
        for (;;) {
            if (i_ < page_.size()) {
                const PointInfo& p = page_[i_++];
                out = Row(pointsTable().columns.size());
                out[PC_SourceBackend] = b_.name();
                out[PC_SourceServer] = p.sourceServer;
                out[PC_PointId] = p.pointId;
                out[PC_TagName] = p.tagName;
                out[PC_Descriptor] = opt(p.descriptor);
                out[PC_PointClass] = opt(p.pointClass);
                out[PC_PointType] = opt(p.pointType);
                out[PC_EngUnits] = opt(p.engUnits);
                out[PC_DigitalSetName] = opt(p.digitalSet);
                out[PC_IsStep] = opt(p.step);
                return true;
            }
            if (done_) return false;
            PointQuery q;
            q.tagName = plan_.tagName;
            q.pageSize = lim_.pageSize;
            q.continuation = cont_;
            auto pg = b_.listPoints(q, c_);
            if (trace_.enabled())
                trace_.write(corr_, "listPoints", "\"backend\":" + Trace::quote(b_.name()) + ",\"tagName\":" + Trace::quote(plan_.tagName.value_or("")) + ",\"rows\":" + std::to_string(pg.points.size()));
            page_ = std::move(pg.points);
            i_ = 0;
            cont_ = std::move(pg.continuation);
            done_ = cont_.empty();
        }
    }
private:
    const Plan& plan_;
    Backend& b_;
    CancelToken& c_;
    const Limits& lim_;
    const Trace& trace_;
    std::string corr_, cont_;
    std::vector<PointInfo> page_;
    size_t i_ = 0;
    bool done_ = false;
};

// Applies residual predicates, sort, limit, row cap and projection above a base source.
class PlanSource final : public RowSource {
public:
    PlanSource(Plan plan, std::function<std::unique_ptr<BaseSource>(const Plan&)> open, const Limits& l)
        : plan_(std::move(plan)), lim_(l) {
        base_ = open(plan_);
    }
    bool next(Row& out) override {
        if (plan_.needsSort && !sorted_) sortAll();
        if (plan_.limit && produced_ >= *plan_.limit) return false;
        Row r;
        if (plan_.needsSort) {
            if (sortIndex_ >= sortedRows_.size()) return false;
            r = std::move(sortedRows_[sortIndex_++]);
        } else if (!nextFiltered(r)) {
            return false;
        }
        produced_++;
        project(r, out);
        return true;
    }
private:
    bool nextFiltered(Row& r) {
        while (base_->next(r)) {
            if (++scanned_ > lim_.maxRows)
                throw Error("HY000", "Result exceeds MaxRows=" + std::to_string(lim_.maxRows) + " for one query. The result is incomplete and was stopped; narrow the window.");
            bool ok = true;
            for (auto& p : plan_.residual)
                if (!matches(p, r)) { ok = false; break; }
            if (ok) return true;
        }
        return false;
    }
    void sortAll() {
        Row r;
        while (nextFiltered(r)) sortedRows_.push_back(std::move(r));
        std::stable_sort(sortedRows_.begin(), sortedRows_.end(), [&](const Row& a, const Row& b) {
            for (auto& [col, desc] : plan_.order) {
                const Value& x = a[static_cast<size_t>(col)];
                const Value& y = b[static_cast<size_t>(col)];
                if (isNull(x) || isNull(y)) {
                    if (isNull(x) && isNull(y)) continue;
                    return desc ? isNull(y) : isNull(x);  // NULLs sort low
                }
                int c = compareValues(x, y);
                if (c != 0) return desc ? c > 0 : c < 0;
            }
            return false;
        });
        sorted_ = true;
    }
    void project(Row& r, Row& out) {
        out.resize(plan_.projection.size());
        for (size_t i = 0; i < plan_.projection.size(); i++) out[i] = r[static_cast<size_t>(plan_.projection[i])];
    }
    Plan plan_;
    const Limits& lim_;
    std::unique_ptr<BaseSource> base_;
    long long produced_ = 0, scanned_ = 0;
    bool sorted_ = false;
    std::vector<Row> sortedRows_;
    size_t sortIndex_ = 0;
};

}  // namespace

std::vector<ColumnDef> describe(const Query& q) {
    const TableDef* t = findTable(q.table);
    if (q.star) return t->columns;
    std::vector<ColumnDef> out;
    for (auto& i : q.items) {
        ColumnDef c = t->columns[static_cast<size_t>(t->index(i.column))];
        c.name = i.alias;
        out.push_back(std::move(c));
    }
    return out;
}

ColType parameterType(const Query& q, int index) {
    const TableDef* t = findTable(q.table);
    for (auto& p : q.where)
        for (auto& o : p.operands)
            if (o.isParam && o.param == index) return t->columns[static_cast<size_t>(t->index(p.column))].type;
    throw Error("07009", "Invalid parameter number.");
}

Plan buildPlan(const Query& q, const std::vector<Value>& params, const Limits& lim) {
    if (static_cast<int>(params.size()) < q.paramCount) throw Error("07002", "Statement has " + std::to_string(q.paramCount) + " parameter markers but only " + std::to_string(params.size()) + " are bound.");
    Plan plan;
    plan.table = findTable(q.table);
    const TableDef& t = *plan.table;
    plan.columns = describe(q);
    if (q.star) for (size_t i = 0; i < t.columns.size(); i++) plan.projection.push_back(static_cast<int>(i));
    else for (auto& i : q.items) plan.projection.push_back(t.index(i.column));
    plan.limit = q.limit;
    for (auto& o : q.order) plan.order.push_back({t.index(o.column), o.desc});

    for (auto& p : q.where) {
        BoundPredicate b{t.index(p.column), p.op, {}};
        ColType ct = t.columns[static_cast<size_t>(b.column)].type;
        for (auto& o : p.operands) {
            const Value& raw = o.isParam ? params[static_cast<size_t>(o.param)] : o.literal;
            b.values.push_back(coerce(raw, ct, p.column));
        }
        plan.residual.push_back(std::move(b));
    }

    // Zero-row probes never reach the source.
    if (q.constFalse || (q.limit && *q.limit == 0)) { plan.kind = PlanKind::Empty; return plan; }

    if (&t == &pointsTable()) {
        plan.kind = PlanKind::Points;
        for (auto& p : plan.residual)
            if (p.column == PC_TagName && p.op == CmpOp::Eq && !isNull(p.values[0])) plan.tagName = std::get<std::string>(p.values[0]);
        plan.needsSort = !plan.order.empty();
        return plan;
    }

    // RecordedValues: every read must be bounded by points and time.
    if (plan.residual.empty()) {
        if (!lim.navigatorPreview)
            throw Error("42000", "RecordedValues requires PointId and an EventUtc window. Example: WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?. (An unfiltered preview is available only on a DSN with NavigatorPreview=1.)");
        plan.kind = PlanKind::Preview;
        long long cap = lim.previewRows;
        plan.limit = q.limit ? std::min<long long>(*q.limit, cap) : cap;
        plan.needsSort = !plan.order.empty();
        plan.warning = "PREVIEW ONLY: capped sample of one point (ResultScope='PREVIEW'), not the requested history. Add PointId and EventUtc bounds to read data.";
        return plan;
    }

    std::optional<Ticks> lo, hi;
    bool emptyWindow = false;
    bool havePoints = false;
    for (auto& p : plan.residual) {
        if (p.column == RC_PointId) {
            if (p.op == CmpOp::Eq || p.op == CmpOp::In) {
                std::vector<std::string> ids;
                for (auto& v : p.values) {
                    if (isNull(v)) throw Error("HY000", "PointId bound must not be NULL.");
                    auto& s = std::get<std::string>(v);
                    if (std::find(ids.begin(), ids.end(), s) == ids.end()) ids.push_back(s);
                }
                if (havePoints) throw Error("0A000", "Unsupported SQL: only one PointId predicate is accepted.");
                plan.pointIds = std::move(ids);
                havePoints = true;
            }
        } else if (p.column == RC_EventUtc) {
            if (p.op == CmpOp::IsNull) { emptyWindow = true; continue; }
            if (p.op == CmpOp::IsNotNull || p.op == CmpOp::Ne) continue;
            if (p.op == CmpOp::In) throw Error("0A000", "Unsupported SQL: IN on EventUtc is not implemented.");
            if (isNull(p.values[0])) throw Error("HY000", "EventUtc bound must not be NULL.");
            Ticks v = std::get<Ticks>(p.values[0]);
            if (p.op == CmpOp::Ge || p.op == CmpOp::Gt || p.op == CmpOp::Eq) if (!lo || v > *lo) lo = v;
            if (p.op == CmpOp::Le || p.op == CmpOp::Lt || p.op == CmpOp::Eq) if (!hi || v < *hi) hi = v;
        }
    }
    if (!havePoints) throw Error("42000", "RecordedValues requires PointId = value or PointId IN (...). Refusing an unbounded archive read.");
    if (!lo || !hi) throw Error("42000", "RecordedValues requires both a lower and an upper EventUtc bound. Refusing an unbounded archive read.");
    if (static_cast<int>(plan.pointIds.size()) > lim.maxPointsPerQuery)
        throw Error("42000", "Query names " + std::to_string(plan.pointIds.size()) + " points; MaxPointsPerQuery is " + std::to_string(lim.maxPointsPerQuery) + ".");
    if (emptyWindow || *hi < *lo) { plan.kind = PlanKind::Empty; return plan; }
    if (hi->v - lo->v > lim.maxWindowDays * kTicksPerDay)
        throw Error("42000", "EventUtc window exceeds MaxWindowDays=" + std::to_string(lim.maxWindowDays) + ". Split the read, for example with incremental refresh partitions.");
    plan.kind = PlanKind::Recorded;
    plan.start = *lo;
    plan.end = *hi;

    // Source order is PointId as given, then EventUtc, then EventSeq. Sort only when that differs.
    bool natural = plan.pointIds.size() == 1 || plan.order.empty();
    size_t ci = 0;
    for (auto& [col, desc] : plan.order) {
        if (col == RC_PointId && plan.pointIds.size() == 1 && !desc) continue;
        static const int seq[] = {RC_EventUtc, RC_EventSeq};
        if (desc || ci >= 2 || col != seq[ci]) { natural = false; break; }
        ci++;
    }
    plan.needsSort = !plan.order.empty() && !natural;
    return plan;
}

std::string describeUpstream(const Plan& plan, const Limits& lim) {
    switch (plan.kind) {
        case PlanKind::Empty: return "none (zero-row result; no source access)";
        case PlanKind::Points: return "listPoints(tagName=" + plan.tagName.value_or("*") + ", pageSize=" + std::to_string(lim.pageSize) + ")";
        case PlanKind::Preview: return "listPoints(limit=1) + readRecorded(firstPoint, last " + std::to_string(lim.previewWindowHours) + "h, maxRows=" + std::to_string(lim.previewRows) + ")";
        case PlanKind::Recorded: {
            std::string s;
            for (auto& id : plan.pointIds) {
                if (!s.empty()) s += "; ";
                s += "getPoint(" + id + ") + readRecorded(point=" + id + ", start=" + formatUtc(plan.start) + ", end=" + formatUtc(plan.end) + ", boundary=Inside, pageSize=" + std::to_string(lim.pageSize) + ")";
            }
            return s;
        }
    }
    return "";
}

std::unique_ptr<RowSource> openMemory(std::vector<Row> rows) { return std::make_unique<MemorySource>(std::move(rows)); }

std::unique_ptr<RowSource> openPlan(const Plan& plan, Backend& b, CancelToken& c, const Limits& lim, const Trace& trace, const std::string& corr) {
    if (plan.kind == PlanKind::Empty) return openMemory({});
    auto open = [&](const Plan& stored) -> std::unique_ptr<BaseSource> {
        if (stored.kind == PlanKind::Points) return std::make_unique<PointsBase>(stored, b, c, lim, trace, corr);
        return std::make_unique<RecordedBase>(stored, b, c, lim, trace, corr);
    };
    return std::make_unique<PlanSource>(plan, open, lim);
}

}  // namespace strata
