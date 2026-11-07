// Portable contract tests. These prove parser, planner and backend behavior against synthetic
// inputs. They are not evidence of connectivity to a PI server.
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>

#include "../src/backend/piwebapi_backend.h"
#include "../src/core/json.h"
#include "../src/core/planner.h"
#include "../src/odbc/config.h"

using namespace strata;

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EXPECT_STATE(state, expr) do { checks++; std::string got_ = "<no error>"; try { (void)(expr); } catch (const Error& e_) { got_ = e_.sqlstate; } if (got_ != (state)) { failures++; std::printf("FAIL %s:%d: %s -> %s, wanted %s\n", __FILE__, __LINE__, #expr, got_.c_str(), (state)); } } while (0)

static const char* kWhere = "SELECT EventUtc, NumericValue, StateText, IsGood FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?";
static std::vector<Value> window(const char* id, const char* a, const char* b) { return {std::string(id), std::string(a), std::string(b)}; }

static std::vector<Row> run(const std::string& sql, const std::vector<Value>& params, Backend& b, Limits lim = {}) {
    Query q = parseSql(sql);
    Plan p = buildPlan(q, params, lim);
    CancelToken c;
    Trace t;
    auto src = openPlan(p, b, c, lim, t, "test");
    std::vector<Row> out;
    Row r;
    while (src->next(r)) out.push_back(r);
    return out;
}

static void testTime() {
    Ticks t;
    CHECK(parseUtc("2026-01-15T08:00:10.0000153Z", t));
    CHECK(formatUtc(t) == "2026-01-15T08:00:10.0000153Z");
    CHECK(parseUtc("2026-01-15 08:00:10", t));
    CHECK(formatUtc(t) == "2026-01-15T08:00:10.0000000Z");
    CHECK(parseUtc("2026-01-15T09:00:00+01:00", t));
    CHECK(formatUtc(t) == "2026-01-15T08:00:00.0000000Z");
    CHECK(parseUtc("2024-02-29", t));
    CHECK(!parseUtc("2025-02-29", t));
    CHECK(!parseUtc("2026-13-01", t));
    CHECK(!parseUtc("yesterday", t));
    CHECK(!parseUtc("2026-01-15T25:00:00Z", t));
    // Sub-100ns digits truncate in the tick value; the raw column keeps them.
    CHECK(parseUtc("2026-01-15T08:00:10.00001539Z", t));
    CHECK(formatUtc(t) == "2026-01-15T08:00:10.0000153Z");
    // Before the epoch and across a leap day.
    CHECK(parseUtc("1969-12-31T23:59:59.5Z", t));
    CHECK(formatUtc(t) == "1969-12-31T23:59:59.5000000Z");
}

static void testParser() {
    Query q = parseSql(kWhere);
    CHECK(q.paramCount == 3 && q.items.size() == 4 && q.where.size() == 3);
    // SQL shapes Power Query generates: quoting, three-part names, aliases, qualified columns.
    q = parseSql("select \"$Table\".\"EventUtc\" as \"EventUtc\" from \"STRATA\".\"HISTORIAN\".\"RecordedValues\" \"$Table\" where \"$Table\".\"PointId\" = 'x' and \"$Table\".\"EventUtc\" >= {ts '2026-01-15 08:00:00'} and \"$Table\".\"EventUtc\" < {ts '2026-01-15 09:00:00'}");
    CHECK(q.table == "RecordedValues" && q.items[0].alias == "EventUtc" && q.where.size() == 3);
    q = parseSql("SELECT TOP 5 * FROM Points ORDER BY TagName DESC;");
    CHECK(q.star && *q.limit == 5 && q.order[0].desc);
    q = parseSql("SELECT * FROM Points LIMIT 3");
    CHECK(*q.limit == 3);
    q = parseSql("SELECT PointId FROM Points WHERE 1=0");
    CHECK(q.constFalse);
    q = parseSql("SELECT * FROM RecordedValues WHERE PointId IN ('a','b') AND EventUtc BETWEEN ? AND ?");
    CHECK(q.where.size() == 3 && q.paramCount == 2);
    q = parseSql("SELECT * FROM Points WHERE TagName = 'it''s' AND EngUnits IS NOT NULL");
    CHECK(std::get<std::string>(q.where[0].operands[0].literal) == "it's");

    for (const char* w : {"INSERT INTO Points VALUES (1)", "UPDATE Points SET TagName='x'", "DELETE FROM Points", "DROP TABLE Points", "EXEC foo", "CREATE TABLE t (a int)"})
        EXPECT_STATE("0A000", parseSql(w));
    for (const char* u : {"SELECT COUNT(*) FROM Points", "SELECT DISTINCT TagName FROM Points", "SELECT * FROM Points GROUP BY TagName", "SELECT * FROM Points p JOIN RecordedValues r ON p.PointId = r.PointId", "SELECT * FROM (SELECT * FROM Points) t", "SELECT * FROM Points WHERE TagName = 'a' OR TagName = 'b'", "SELECT * FROM Points UNION SELECT * FROM Points", "SELECT * FROM Points WHERE TagName LIKE 'a%'", "SELECT * FROM Points -- x", "SELECT * FROM Points WHERE TagName IN (SELECT TagName FROM Points)", "SELECT * FROM Points OFFSET 2", "SELECT {fn NOW()} FROM Points"})
        EXPECT_STATE("0A000", parseSql(u));
    for (const char* s : {"SELECT", "SELECT * FROM", "SELECT * FROM Points WHERE", "SELECT * FROM Points; SELECT * FROM Points", "SELECT 'unterminated FROM Points", "SELECT * FROM Points WHERE TagName = NULL"})
        EXPECT_STATE("42000", parseSql(s));
    EXPECT_STATE("42S02", parseSql("SELECT * FROM Nope"));
    EXPECT_STATE("42S02", parseSql("SELECT * FROM OTHER.Points"));
    EXPECT_STATE("42S02", parseSql("SELECT * FROM OTHER.HISTORIAN.Points"));
    EXPECT_STATE("42S22", parseSql("SELECT Nope FROM Points"));
    EXPECT_STATE("42S22", parseSql("SELECT * FROM Points WHERE Nope = 1"));
    EXPECT_STATE("22007", parseSql("SELECT * FROM RecordedValues WHERE EventUtc > {ts 'bad'}"));
}

static void testPlanner() {
    Limits lim;
    Query q = parseSql(kWhere);
    Plan p = buildPlan(q, window("A", "2026-01-15T08:00:00Z", "2026-01-15T09:00:00Z"), lim);
    CHECK(p.kind == PlanKind::Recorded && p.pointIds.size() == 1);
    CHECK(formatUtc(p.start) == "2026-01-15T08:00:00.0000000Z" && formatUtc(p.end) == "2026-01-15T09:00:00.0000000Z");
    CHECK(describeUpstream(p, lim).find("readRecorded(point=A, start=2026-01-15T08:00:00.0000000Z, end=2026-01-15T09:00:00.0000000Z, boundary=Inside, pageSize=1000)") != std::string::npos);

    auto plan = [&](const char* sql, std::vector<Value> ps = {}) { return buildPlan(parseSql(sql), ps, lim); };
    // Every unbounded shape is refused rather than turned into an archive scan.
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues"));
    EXPECT_STATE("42000", plan("SELECT TOP 10 * FROM RecordedValues"));
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues WHERE PointId = 'a'"));
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues WHERE EventUtc >= {ts '2026-01-01 00:00:00'} AND EventUtc < {ts '2026-01-02 00:00:00'}"));
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues WHERE PointId = 'a' AND EventUtc >= {ts '2026-01-01 00:00:00'}"));
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues WHERE PointId = 'a' AND EventUtc < {ts '2026-01-01 00:00:00'}"));
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues WHERE IsGood = 1"));
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues WHERE PointId <> 'a' AND EventUtc >= {ts '2026-01-01 00:00:00'} AND EventUtc < {ts '2026-01-02 00:00:00'}"));
    // Window and point caps.
    EXPECT_STATE("42000", plan("SELECT * FROM RecordedValues WHERE PointId = 'a' AND EventUtc >= {ts '2026-01-01 00:00:00'} AND EventUtc < {ts '2026-03-01 00:00:00'}"));
    Limits tight = lim;
    tight.maxPointsPerQuery = 1;
    EXPECT_STATE("42000", buildPlan(parseSql("SELECT * FROM RecordedValues WHERE PointId IN ('a','b') AND EventUtc >= {ts '2026-01-01 00:00:00'} AND EventUtc < {ts '2026-01-02 00:00:00'}"), {}, tight));
    // Missing and NULL parameters.
    EXPECT_STATE("07002", buildPlan(parseSql(kWhere), {std::string("a")}, lim));
    EXPECT_STATE("HY000", buildPlan(parseSql(kWhere), {Value{}, std::string("2026-01-01"), std::string("2026-01-02")}, lim));
    EXPECT_STATE("HY000", buildPlan(parseSql(kWhere), {std::string("a"), Value{}, std::string("2026-01-02")}, lim));
    EXPECT_STATE("22007", buildPlan(parseSql(kWhere), {std::string("a"), std::string("not a date"), std::string("2026-01-02")}, lim));

    // Probes never touch a source.
    CHECK(plan("SELECT * FROM RecordedValues WHERE 1=0").kind == PlanKind::Empty);
    CHECK(plan("SELECT TOP 0 * FROM RecordedValues").kind == PlanKind::Empty);
    CHECK(plan("SELECT * FROM RecordedValues WHERE PointId = 'a' AND EventUtc >= {ts '2026-01-02 00:00:00'} AND EventUtc < {ts '2026-01-01 00:00:00'}").kind == PlanKind::Empty);

    // Preview is opt-in, has no predicates, and is capped.
    Limits prev = lim;
    prev.navigatorPreview = true;
    Plan pv = buildPlan(parseSql("SELECT * FROM RecordedValues"), {}, prev);
    CHECK(pv.kind == PlanKind::Preview && *pv.limit == prev.previewRows && !pv.warning.empty());
    pv = buildPlan(parseSql("SELECT TOP 100000 * FROM RecordedValues"), {}, prev);
    CHECK(*pv.limit == prev.previewRows);
    // A folded filter must not silently read the preview instead of the requested data.
    EXPECT_STATE("42000", buildPlan(parseSql("SELECT * FROM RecordedValues WHERE IsGood = 1"), {}, prev));
    EXPECT_STATE("42000", buildPlan(parseSql("SELECT * FROM RecordedValues WHERE PointId = 'a'"), {}, prev));

    // Ordering: natural order streams, anything else sorts.
    const char* base = "SELECT * FROM RecordedValues WHERE PointId = 'a' AND EventUtc >= {ts '2026-01-01 00:00:00'} AND EventUtc < {ts '2026-01-02 00:00:00'}";
    CHECK(!plan(base).needsSort);
    CHECK(!plan((std::string(base) + " ORDER BY EventUtc").c_str()).needsSort);
    CHECK(!plan((std::string(base) + " ORDER BY EventUtc, EventSeq").c_str()).needsSort);
    CHECK(plan((std::string(base) + " ORDER BY EventUtc DESC").c_str()).needsSort);
    CHECK(plan((std::string(base) + " ORDER BY NumericValue").c_str()).needsSort);
}

static void testFixtureSemantics() {
    auto b = makeFixtureBackend();
    auto rows = run(kWhere, window("PT-PRESSURE-001", "2026-01-15T08:00:00Z", "2026-01-15T09:00:00Z"), *b);
    CHECK(rows.size() == 7);
    if (rows.size() == 7) {
        CHECK(std::get<double>(rows[1][1]) == 0.0);          // real zero is a zero
        CHECK(isNull(rows[4][1]));                           // Bad Input is not zero
        CHECK(std::get<std::string>(rows[4][2]) == "Bad Input");
        CHECK(std::get<bool>(rows[4][3]) == false);
    }
    // Empty window is a success with no rows.
    CHECK(run(kWhere, window("PT-PRESSURE-001", "2026-01-15T09:00:00Z", "2026-01-15T10:00:00Z"), *b).empty());
    // End bound is exclusive; closed end includes it.
    CHECK(run(kWhere, window("PT-PRESSURE-001", "2026-01-15T09:00:00Z", "2026-01-15T10:00:00Z"), *b).empty());
    CHECK(run("SELECT EventUtc FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc <= ?", window("PT-PRESSURE-001", "2026-01-15T09:00:00Z", "2026-01-15T10:00:00Z"), *b).size() == 1);
    // Start bound exclusive.
    CHECK(run("SELECT EventUtc FROM RecordedValues WHERE PointId = ? AND EventUtc > ? AND EventUtc < ?", window("PT-PRESSURE-001", "2026-01-15T08:00:00Z", "2026-01-15T08:00:10Z"), *b).empty());
    // Repeated timestamps stay distinct.
    auto seq = run("SELECT EventUtcRaw, EventSeq, NumericValue FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc <= ?", window("PT-PRESSURE-001", "2026-01-15T08:00:10.0000153Z", "2026-01-15T08:00:10.0000153Z"), *b);
    CHECK(seq.size() == 2);
    if (seq.size() == 2) {
        CHECK(std::get<std::string>(seq[0][0]) == std::get<std::string>(seq[1][0]));
        CHECK(std::get<int32_t>(seq[0][1]) == 0 && std::get<int32_t>(seq[1][1]) == 1);
        CHECK(std::get<double>(seq[0][2]) != std::get<double>(seq[1][2]));
    }
    // Digital point.
    auto run2 = run("SELECT ValueKind, DigitalCode, StateText, NumericValue FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?", window("PT-RUNSTATE-001", "2026-01-15T07:00:00Z", "2026-01-15T09:00:00Z"), *b);
    CHECK(run2.size() == 4);
    if (run2.size() == 4) {
        CHECK(std::get<std::string>(run2[1][0]) == "DIGITAL" && std::get<int32_t>(run2[1][1]) == 1 && std::get<std::string>(run2[1][2]) == "Running" && isNull(run2[1][3]));
        CHECK(std::get<std::string>(run2[3][0]) == "SYSTEM" && std::get<std::string>(run2[3][2]) == "I/O Timeout");
    }
    // Denied: metadata hidden and values refused.
    auto all = run("SELECT PointId FROM Points", {}, *b);
    CHECK(all.size() == 2);
    for (auto& r : all) CHECK(std::get<std::string>(r[0]) != "PT-DENIED-001");
    EXPECT_STATE("42000", run(kWhere, window("PT-DENIED-001", "2026-01-15T08:00:00Z", "2026-01-15T09:00:00Z"), *b));
    EXPECT_STATE("42S02", run(kWhere, window("PT-NOPE", "2026-01-15T08:00:00Z", "2026-01-15T09:00:00Z"), *b));
    // Paging across a boundary is deterministic for several page sizes.
    for (int ps : {1000, 999, 2500, 2501, 7}) {
        Limits l;
        l.pageSize = ps;
        auto r = run("SELECT NumericValue FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?", window("PT-RAMP-001", "2026-01-16T00:00:00Z", "2026-01-17T00:00:00Z"), *b, l);
        CHECK(r.size() == 2500);
        bool ordered = true;
        for (size_t i = 0; i < r.size(); i++) if (std::get<double>(r[i][0]) != static_cast<double>(i)) ordered = false;
        CHECK(ordered);
    }
    // Row cap stops loudly; it never truncates quietly.
    Limits cap;
    cap.maxRows = 100;
    EXPECT_STATE("HY000", run("SELECT NumericValue FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?", window("PT-RAMP-001", "2026-01-16T00:00:00Z", "2026-01-17T00:00:00Z"), *b, cap));
    // Partial failure surfaces.
    EXPECT_STATE("08S01", run("SELECT NumericValue FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?", window("PT-FAULT-001", "2026-01-16T00:00:00Z", "2026-01-17T00:00:00Z"), *b));
    // Residual filter, sort, limit.
    auto good = run("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'} AND IsGood = 0", {}, *b);
    CHECK(good.size() == 1);
    auto sorted = run("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'} AND NumericValue IS NOT NULL ORDER BY NumericValue DESC LIMIT 2", {}, *b);
    CHECK(sorted.size() == 2 && std::get<double>(sorted[0][0]) == 104.0 && std::get<double>(sorted[1][0]) == 103.0);
    // Preview names itself in the data.
    Limits prev;
    prev.navigatorPreview = true;
    auto pv = run("SELECT PointId, ResultScope FROM RecordedValues", {}, *b, prev);
    CHECK(!pv.empty() && pv.size() <= 100);
    for (auto& r : pv) CHECK(std::get<std::string>(r[1]) == "PREVIEW");
    auto bounded = run("SELECT ResultScope, SourceBackend FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}", {}, *b);
    CHECK(std::get<std::string>(bounded[0][0]) == "BOUNDED" && std::get<std::string>(bounded[0][1]) == "fixture");
    // A long backoff sleep ends promptly on cancel, not when the sleep expires.
    {
        CancelToken c;
        std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); c.cancel(); });
        auto t0 = std::chrono::steady_clock::now();
        EXPECT_STATE("HY008", c.sleepMs(10000));
        t.join();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    }
    // Cancellation and timeout.
    {
        Limits l;
        l.pageSize = 1;
        Query q = parseSql(kWhere);
        Plan p = buildPlan(q, window("PT-SLOW-001", "2026-01-16T00:00:00Z", "2026-01-17T00:00:00Z"), l);
        CancelToken c;
        Trace t;
        auto src = openPlan(p, *b, c, l, t, "test");
        Row r;
        CHECK(src->next(r));
        c.cancel();
        EXPECT_STATE("HY008", src->next(r));
    }
}

// Scripted transport. Responses below follow the PI Web API documented/secondary-source shape
// recorded in docs/evidence-log.md. This proves the mapping code, not a server's behavior.
struct Fake : HttpTransport {
    std::deque<HttpResponse> script;
    std::vector<std::string> urls;
    HttpResponse get(const HttpRequest& rq, CancelToken&) override {
        urls.push_back(rq.url);
        if (script.empty()) throw Error("08S01", "fake: script exhausted");
        auto r = script.front();
        script.pop_front();
        if (r.status == -1) throw Error("08S01", "fake: link down");
        return r;
    }
};
static HttpResponse ok(std::string body) { return HttpResponse{200, std::move(body), ""}; }
static std::string item(const char* ts, const char* value, const char* extra = "\"Good\":true,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false") {
    return std::string("{\"Timestamp\":\"") + ts + "\",\"Value\":" + value + ",\"UnitsAbbreviation\":\"kPa\"," + extra + "}";
}
static std::string page(std::initializer_list<std::string> items) {
    std::string o = "{\"Items\":[";
    bool first = true;
    for (auto& i : items) { if (!first) o += ","; o += i; first = false; }
    return o + "]}";
}
struct Live {
    Fake* fake;
    std::unique_ptr<Backend> b;
    CancelToken c;
    explicit Live(int retries = 2, std::string tracePath = {}) {
        auto f = std::make_unique<Fake>();
        fake = f.get();
        PiWebApiConfig cfg;
        static int n = 0;
        cfg.baseUrl = "https://mock" + std::to_string(n++) + ".example/piwebapi/";
        cfg.dataServer = "SYN SERVER";
        cfg.maxRetries = retries;
        cfg.maxRetryAfterSeconds = 1;
        cfg.trace = Trace(tracePath);
        b = makePiWebApiBackend(cfg, std::move(f));
    }
    RecordedPage read(int pageSize = 3, std::string cont = {}) {
        RecordedRequest r;
        r.pointId = "P1abc";
        parseUtc("2026-01-15T08:00:00Z", r.start);
        parseUtc("2026-01-15T09:00:00Z", r.end);
        r.pageSize = pageSize;
        r.continuation = std::move(cont);
        return b->readRecorded(r, c);
    }
};

static void testPiWebApi() {
    {   // request shape, value mapping
        Live l;
        l.fake->script.push_back(ok(page({item("2026-01-15T08:00:00Z", "0"),
                                          item("2026-01-15T08:00:01.5Z", R"({"Name":"Running","Value":1,"IsSystem":false})"),
                                          item("2026-01-15T08:00:02Z", R"({"Name":"Bad Input","Value":307,"IsSystem":true})", "\"Good\":false,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false")})));
        auto pg = l.read(10);
        const std::string& u = l.fake->urls[0];
        CHECK(u.rfind("https://mock", 0) == 0);
        CHECK(u.find("/piwebapi/streams/P1abc/recorded?startTime=2026-01-15T08%3A00%3A00.0000000Z&endTime=2026-01-15T09%3A00%3A00.0000000Z&boundaryType=Inside&maxCount=10") != std::string::npos);
        CHECK(pg.continuation.empty() && pg.events.size() == 3);
        CHECK(pg.events[0].kind == ValueKind::Numeric && *pg.events[0].numeric == 0.0 && pg.events[0].rawTime == "2026-01-15T08:00:00Z");
        CHECK(pg.events[1].kind == ValueKind::Digital && *pg.events[1].digitalCode == 1 && *pg.events[1].stateText == "Running" && !pg.events[1].numeric);
        CHECK(pg.events[2].kind == ValueKind::System && !pg.events[2].good && !pg.events[2].numeric);
    }
    {   // paging resumes at the last timestamp and skips what was delivered, including repeats
        Live l;
        l.fake->script.push_back(ok(page({item("2026-01-15T08:00:00Z", "1"), item("2026-01-15T08:00:01Z", "2"), item("2026-01-15T08:00:01Z", "3")})));
        auto p1 = l.read(3);
        CHECK(p1.events.size() == 3 && p1.continuation == "2026-01-15T08:00:01Z|2");
        l.fake->script.push_back(ok(page({item("2026-01-15T08:00:01Z", "2"), item("2026-01-15T08:00:01Z", "3"), item("2026-01-15T08:00:02Z", "4")})));
        auto p2 = l.read(3, p1.continuation);
        CHECK(l.fake->urls[1].find("startTime=2026-01-15T08%3A00%3A01Z") != std::string::npos);
        CHECK(p2.events.size() == 1 && *p2.events[0].numeric == 4.0 && p2.continuation == "2026-01-15T08:00:02Z|1");
        l.fake->script.push_back(ok(page({item("2026-01-15T08:00:02Z", "4")})));
        auto p3 = l.read(3, p2.continuation);
        CHECK(p3.events.empty() && p3.continuation.empty());
        // A page full of one timestamp cannot progress: explicit error, not a loop or a drop.
        Live k;
        k.fake->script.push_back(ok(page({item("2026-01-15T08:00:01Z", "2"), item("2026-01-15T08:00:01Z", "3"), item("2026-01-15T08:00:01Z", "4")})));
        EXPECT_STATE("HY000", k.read(3));
        // History shrinking between pages is detected.
        Live s;
        s.fake->script.push_back(ok(page({item("2026-01-15T08:00:01Z", "2")})));
        EXPECT_STATE("08S01", s.read(3, "2026-01-15T08:00:01Z|2"));
    }
    {   // an empty window is success
        Live l;
        l.fake->script.push_back(ok("{\"Items\":[]}"));
        CHECK(l.read(3).events.empty());
    }
    // Status mapping: denied stays denied.
    for (auto [status, state] : std::initializer_list<std::pair<int, const char*>>{{401, "28000"}, {403, "42000"}, {404, "42S02"}, {409, "HYC00"}, {500, "08S01"}}) {
        Live l;
        l.fake->script.push_back(HttpResponse{status, "", ""});
        EXPECT_STATE(state, l.read());
        CHECK(l.fake->urls.size() == 1);  // non-transient: not retried
    }
    {   // Retry-After honored and retries capped
        Live l(1);
        l.fake->script.push_back(HttpResponse{429, "", "0"});
        l.fake->script.push_back(ok("{\"Items\":[]}"));
        CHECK(l.read().events.empty());
        CHECK(l.fake->urls.size() == 2);
        Live capped(1);
        for (int i = 0; i < 5; i++) capped.fake->script.push_back(HttpResponse{503, "", "0"});
        EXPECT_STATE("08S01", capped.read());
        CHECK(capped.fake->urls.size() == 2);  // first try + one retry
        Live tooLong(3);
        tooLong.fake->script.push_back(HttpResponse{429, "", "9999"});
        EXPECT_STATE("08S01", tooLong.read());
        CHECK(tooLong.fake->urls.size() == 1);  // policy refuses to wait that long
    }
    {   // the trace records upstream bytes per request, and no credentials or URLs
        auto path = std::filesystem::temp_directory_path() / "strata-core-trace.jsonl";
        std::filesystem::remove(path);
        Live l(1, path.string());
        std::string body = "{\"Items\":[]}";
        l.fake->script.push_back(ok(body));
        l.read();
        std::ifstream f(path);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(text.find("\"op\":\"readRecorded\",\"attempt\":1,\"status\":200,\"bytes\":" + std::to_string(body.size())) != std::string::npos);
        CHECK(text.find("mock") == std::string::npos);
        f.close();
        std::filesystem::remove(path);
    }
    {   // network drop retried then reported
        Live l(1);
        l.fake->script.push_back(HttpResponse{-1, "", ""});
        l.fake->script.push_back(HttpResponse{-1, "", ""});
        EXPECT_STATE("08S01", l.read());
        CHECK(l.fake->urls.size() == 2);
    }
    // A response we cannot interpret never becomes a value.
    for (const char* bad : {
             "{\"Items\":[{\"Timestamp\":\"2026-01-15T08:00:00Z\",\"Good\":true,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false}]}",  // no Value
             "{\"Items\":[{\"Value\":1,\"Good\":true,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false}]}",  // no Timestamp
             "{\"Items\":[{\"Timestamp\":\"2026-01-15T08:00:00Z\",\"Value\":1}]}",  // no flags
             "{\"Items\":[{\"Timestamp\":\"2026-01-15T08:00:00Z\",\"Value\":null,\"Good\":true,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false}]}",
             "{\"Items\":[{\"Timestamp\":\"nope\",\"Value\":1,\"Good\":true,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false}]}",
             "{\"Items\":[{\"Timestamp\":\"2026-01-15T08:00:00Z\",\"Value\":{\"Name\":\"x\"},\"Good\":true,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false}]}",
             "{\"Items\":7}", "{}"}) {
        Live l;
        l.fake->script.push_back(ok(bad));
        EXPECT_STATE("HY000", l.read());
    }
    {   // truncated body is a link failure, never a short page
        Live l;
        l.fake->script.push_back(ok("{\"Items\":[{\"Timestamp\":\"2026-"));
        EXPECT_STATE("08S01", l.read());
        Live e;
        e.fake->script.push_back(ok("{\"Items\":[],\"Errors\":[\"boom\"]}"));
        EXPECT_STATE("HY000", e.read());
        Live i;
        i.fake->script.push_back(ok("{\"Items\":[{\"Errors\":[\"boom\"],\"Timestamp\":\"2026-01-15T08:00:00Z\",\"Value\":1,\"Good\":true,\"Questionable\":false,\"Substituted\":false,\"Annotated\":false}]}"));
        EXPECT_STATE("HY000", i.read());
    }
    {   // metadata: server resolved once, IDOnly WebIds requested, exact-name filter pushed down
        Live l;
        l.fake->script.push_back(ok(R"({"WebId":"F1DSabc","Name":"SYN SERVER"})"));
        l.fake->script.push_back(ok(R"({"Items":[{"WebId":"P1abc","Name":"SYN.PV","Descriptor":"d","PointClass":"classic","PointType":"Float64","EngineeringUnits":"kPa","DigitalSetName":"","Step":false}]})"));
        PointQuery q;
        q.tagName = "SYN.PV";
        q.pageSize = 1;
        auto pg = l.b->listPoints(q, l.c);
        CHECK(l.fake->urls[0].find("/dataservers?name=SYN%20SERVER") != std::string::npos);
        CHECK(l.fake->urls[1].find("/dataservers/F1DSabc/points?maxCount=1&startIndex=0&webIdType=IDOnly&nameFilter=SYN.PV") != std::string::npos);
        CHECK(pg.points.size() == 1 && pg.points[0].pointId == "P1abc" && *pg.points[0].engUnits == "kPa" && !pg.points[0].digitalSet && *pg.points[0].step == false);
        CHECK(pg.continuation == "1");
        l.fake->script.push_back(ok(R"({"WebId":"P1abc","Name":"SYN.PV"})"));
        auto info = l.b->getPoint("P1abc", l.c);
        CHECK(info.tagName == "SYN.PV" && !info.step && l.fake->urls.size() == 3);
    }
}

static void testConfig() {
    auto kv = parseConnectionString("DSN=x;PWD={a;b}}c};uid=me; Backend = fixture ;");
    CHECK(kv["PWD"] == "a;b}c" && kv["UID"] == "me" && kv["DSN"] == "x");
    EXPECT_STATE("08001", parseConnectionString("PWD={unterminated"));
    EXPECT_STATE("08001", parseConnectionString("noequals"));

    auto none = [](const std::string&, const std::string&) { return std::string{}; };
    // Credentials in a DSN are never read.
    bool askedForSecret = false;
    auto spy = [&](const std::string&, const std::string& k) { if (k == "PWD" || k == "UID") askedForSecret = true; return k == "BACKEND" ? std::string("fixture") : std::string{}; };
    Config c = resolveConfig(KeyValues{{"DSN", "x"}}, spy);
    CHECK(!askedForSecret && c.backend == "fixture" && c.auth == AuthMode::Anonymous);
    // Explicit Basic requires a password from the client.
    EXPECT_STATE("28000", resolveConfig(KeyValues{{"AUTH", "Basic"}}, none));
    EXPECT_STATE("28000", resolveConfig(KeyValues{{"UID", "me"}}, none));
    EXPECT_STATE("08001", resolveConfig(KeyValues{{"AUTH", "Kerberos5"}}, none));
    EXPECT_STATE("08001", resolveConfig(KeyValues{{"PAGESIZE", "0"}}, none));
    EXPECT_STATE("08001", resolveConfig(KeyValues{{"NAVIGATORPREVIEW", "maybe"}}, none));
    // No silent fallback to fixture; native stays unsupported.
    Config empty;
    EXPECT_STATE("08001", makeBackend(empty));
    Config native;
    native.backend = "native";
    EXPECT_STATE("08001", makeBackend(native));
    Config noUrl;
    noUrl.backend = "piwebapi";
    EXPECT_STATE("08001", makeBackend(noUrl));
    EXPECT_STATE("08001", validateBaseUrl("http://pi.example.local/piwebapi"));
    validateBaseUrl("https://pi.example.local/piwebapi");
    validateBaseUrl("http://127.0.0.1:8765/piwebapi");
    // Secrets never echo.
    std::string red = redactedConnectionString(KeyValues{{"DSN", "x"}, {"UID", "me"}, {"PWD", "secret"}});
    CHECK(red.find("secret") == std::string::npos && red.find("me") == std::string::npos && red.find("DSN=x") != std::string::npos);
}

static void testJson() {
    auto n = json::parse(R"({"a":[1,2.5,-3e2,true,false,null],"s":"\u00e9\ud83d\ude00\n"})");
    CHECK(n.find("a")->a.size() == 6 && n.find("a")->a[2].n == -300);
    CHECK(n.find("s")->s == "\xC3\xA9\xF0\x9F\x98\x80\n");
    for (const char* bad : {"", "{", "[1,]", "{\"a\":}", "tru", "\"\\q\"", "1 2", "\"\\ud83d\""}) {
        checks++;
        bool threw = false;
        try { json::parse(bad); } catch (const std::exception&) { threw = true; }
        if (!threw) { failures++; std::printf("FAIL json accepted: %s\n", bad); }
    }
    std::string deep(100, '[');
    checks++;
    bool threw = false;
    try { json::parse(deep); } catch (const std::exception&) { threw = true; }
    if (!threw) { failures++; std::printf("FAIL deep json accepted\n"); }
}

int main() {
    testTime();
    testJson();
    testParser();
    testPlanner();
    testFixtureSemantics();
    testPiWebApi();
    testConfig();
    std::printf("core_tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
