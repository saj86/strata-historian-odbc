// Native ODBC smoke test: goes through a real driver manager to the driver DLL.
// usage: odbc_smoke <dsn-no-preview> <dsn-preview> <driver-connstr> <golden.csv> <trace-file>
// Exit code 0 only if every check passed. Output is the Gate A diagnostic evidence.
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::atomic<int> failures{0}, checks{0};
#define CHECK(c) do { checks++; if (!(c)) { failures++; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct WS {
    std::vector<SQLWCHAR> v;
    const SQLWCHAR* c_str() const { return v.data(); }
};
static WS W(const std::string& s) {
    WS o;
    for (unsigned char c : s) o.v.push_back(c);  // ASCII test inputs
    o.v.push_back(0);
    return o;
}
static std::string N(const SQLWCHAR* w, long len = -1) {
    std::string o;
    for (long i = 0; len < 0 ? w[i] != 0 : i < len; i++) o.push_back(static_cast<char>(w[i] < 128 ? w[i] : '?'));
    return o;
}
static std::string state(SQLSMALLINT type, SQLHANDLE h, std::string* msg = nullptr) {
    SQLWCHAR st[6] = {0}, m[1024] = {0};
    SQLINTEGER native = 0;
    SQLSMALLINT len = 0;
    if (!SQL_SUCCEEDED(SQLGetDiagRecW(type, h, 1, st, &native, m, 1024, &len))) return "";
    if (msg) *msg = N(m);
    return N(st);
}

struct Conn {
    SQLHENV env = nullptr;
    SQLHDBC dbc = nullptr;
    SQLRETURN rc = SQL_ERROR;
    std::string outStr;
    explicit Conn(const std::string& cs) {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &env);
        SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(static_cast<intptr_t>(SQL_OV_ODBC3)), 0);
        SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
        WS w = W(cs);
        SQLWCHAR out[1024] = {0};
        SQLSMALLINT outLen = 0;
        rc = SQLDriverConnectW(dbc, nullptr, const_cast<SQLWCHAR*>(w.c_str()), SQL_NTS, out, 1024, &outLen, SQL_DRIVER_NOPROMPT);
        outStr = N(out);
    }
    ~Conn() {
        if (SQL_SUCCEEDED(rc)) SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
    }
    std::string err() { return state(SQL_HANDLE_DBC, dbc); }
};
struct Stmt {
    SQLHSTMT h = nullptr;
    explicit Stmt(Conn& c) { SQLAllocHandle(SQL_HANDLE_STMT, c.dbc, &h); }
    ~Stmt() { SQLFreeHandle(SQL_HANDLE_STMT, h); }
    SQLRETURN exec(const std::string& sql) { WS w = W(sql); return SQLExecDirectW(h, const_cast<SQLWCHAR*>(w.c_str()), SQL_NTS); }
    std::string err() { return state(SQL_HANDLE_STMT, h); }
    std::string text(int col) {
        SQLWCHAR b[2048];
        SQLLEN ind = 0;
        SQLRETURN r = SQLGetData(h, static_cast<SQLUSMALLINT>(col), SQL_C_WCHAR, b, sizeof b, &ind);
        if (!SQL_SUCCEEDED(r)) return "<ERR " + err() + ">";
        if (ind == SQL_NULL_DATA) return "<NULL>";
        return N(b);
    }
    long count() {
        long n = 0;
        while (SQL_SUCCEEDED(SQLFetch(h))) n++;
        return n;
    }
};
static const char* kQuery = "SELECT EventUtc, NumericValue, StateText, IsGood FROM RecordedValues WHERE PointId = ? AND EventUtc >= ? AND EventUtc < ?";

static SQL_TIMESTAMP_STRUCT ts(int y, int mo, int d, int h, int mi, int s) {
    SQL_TIMESTAMP_STRUCT t{};
    t.year = static_cast<SQLSMALLINT>(y); t.month = static_cast<SQLUSMALLINT>(mo); t.day = static_cast<SQLUSMALLINT>(d);
    t.hour = static_cast<SQLUSMALLINT>(h); t.minute = static_cast<SQLUSMALLINT>(mi); t.second = static_cast<SQLUSMALLINT>(s);
    return t;
}

static std::vector<std::string> readGolden(const std::string& path) {
    std::ifstream f(path);
    std::vector<std::string> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line.rfind("EventUtc,", 0) == 0) continue;
        rows.push_back(line);
    }
    return rows;
}

// The prepared-query path with bound parameters, compared to the hand-written golden rows.
static void preparedGolden(Conn& c, const std::vector<std::string>& golden, bool print) {
    Stmt s(c);
    WS q = W(kQuery);
    CHECK(SQL_SUCCEEDED(SQLPrepareW(s.h, const_cast<SQLWCHAR*>(q.c_str()), SQL_NTS)));
    SQLSMALLINT nParams = 0, nCols = 0;
    CHECK(SQL_SUCCEEDED(SQLNumParams(s.h, &nParams)) && nParams == 3);
    // Metadata is available before execution.
    CHECK(SQL_SUCCEEDED(SQLNumResultCols(s.h, &nCols)) && nCols == 4);
    SQLSMALLINT ptype = 0, ndig = 0, pnull = 0;
    SQLULEN psize = 0;
    CHECK(SQL_SUCCEEDED(SQLDescribeParam(s.h, 2, &ptype, &psize, &ndig, &pnull)) && ptype == SQL_TYPE_TIMESTAMP);
    CHECK(SQL_SUCCEEDED(SQLDescribeParam(s.h, 1, &ptype, &psize, &ndig, &pnull)) && ptype == SQL_WVARCHAR);

    WS point = W("PT-PRESSURE-001");
    SQLLEN pointInd = SQL_NTS, zero = 0;
    SQL_TIMESTAMP_STRUCT a = ts(2026, 1, 15, 8, 0, 0), b = ts(2026, 1, 15, 9, 0, 0);
    CHECK(SQL_SUCCEEDED(SQLBindParameter(s.h, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, 256, 0, const_cast<SQLWCHAR*>(point.c_str()), 0, &pointInd)));
    CHECK(SQL_SUCCEEDED(SQLBindParameter(s.h, 2, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 27, 7, &a, 0, &zero)));
    CHECK(SQL_SUCCEEDED(SQLBindParameter(s.h, 3, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 27, 7, &b, 0, &zero)));
    CHECK(SQLExecute(s.h) == SQL_SUCCESS);
    std::vector<std::string> got;
    while (SQL_SUCCEEDED(SQLFetch(s.h))) {
        std::string row = s.text(1) + "," + s.text(2) + "," + s.text(3) + "," + s.text(4);
        got.push_back(row);
        if (print) std::printf("    %s\n", row.c_str());
    }
    CHECK(got == golden);
    // Re-execute after close, with new window values through the same bindings: an empty window.
    CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.h, SQL_CLOSE)));
    a = ts(2026, 1, 15, 9, 0, 0);
    b = ts(2026, 1, 15, 10, 0, 0);
    CHECK(SQLExecute(s.h) == SQL_SUCCESS);
    CHECK(SQLFetch(s.h) == SQL_NO_DATA);
}

int main(int argc, char** argv) {
    if (argc < 6) { std::printf("usage: odbc_smoke <dsn> <preview-dsn> <driver-connstr> <golden.csv> <trace-file>\n"); return 2; }
    std::string dsn = argv[1], previewDsn = argv[2], driverCs = argv[3], tracePath = argv[5];
    auto golden = readGolden(argv[4]);
    CHECK(golden.size() == 7);
    auto section = [](const char* n) { std::printf("[%s]\n", n); };

    section("connect");
    Conn c("DSN=" + dsn);
    CHECK(SQL_SUCCEEDED(c.rc));
    if (!SQL_SUCCEEDED(c.rc)) {
        std::string m;
        std::printf("  cannot connect: %s %s\n", state(SQL_HANDLE_DBC, c.dbc, &m).c_str(), m.c_str());
        return 1;
    }
    {
        Conn noBackend(driverCs);
        CHECK(noBackend.rc == SQL_ERROR && noBackend.err() == "08001");
        Conn native(driverCs + "Backend=native;");
        std::string m;
        CHECK(native.rc == SQL_ERROR && state(SQL_HANDLE_DBC, native.dbc, &m) == "08001" && m.find("unsupported-backend") != std::string::npos);
        Conn unk(driverCs + "Backend=magic;");
        CHECK(unk.rc == SQL_ERROR);
        Conn plainHttp(driverCs + "Backend=piwebapi;BaseUrl=http://pi.example.local/piwebapi;DataServer=x;");
        CHECK(plainHttp.rc == SQL_ERROR && plainHttp.err() == "08001");
        Conn secret(driverCs + "Backend=fixture;Auth=Basic;UID=someone;PWD=hunter2;");
        CHECK(SQL_SUCCEEDED(secret.rc) && secret.outStr.find("hunter2") == std::string::npos && secret.outStr.find("BACKEND=fixture") != std::string::npos);
    }

    section("capabilities: SQLGetInfo and SQLGetFunctions match behavior");
    {
        SQLWCHAR b[256] = {0};
        SQLSMALLINT n = 0;
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_DRIVER_NAME, b, sizeof b, &n)));
        std::printf("  driver=%s", N(b).c_str());
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_DRIVER_VER, b, sizeof b, &n)));
        std::printf(" version=%s\n", N(b).c_str());
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_DATA_SOURCE_READ_ONLY, b, sizeof b, &n)) && N(b) == "Y");
        SQLUSMALLINT gb = 99;
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_GROUP_BY, &gb, sizeof gb, &n)) && gb == SQL_GB_NOT_SUPPORTED);
        SQLUINTEGER aggs = 99, joins = 99, scroll = 0, gd = 0;
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_AGGREGATE_FUNCTIONS, &aggs, sizeof aggs, &n)) && aggs == 0);
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_SQL92_RELATIONAL_JOIN_OPERATORS, &joins, sizeof joins, &n)) && joins == 0);
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_SCROLL_OPTIONS, &scroll, sizeof scroll, &n)) && scroll == SQL_SO_FORWARD_ONLY);
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_GETDATA_EXTENSIONS, &gd, sizeof gd, &n)) && (gd & SQL_GD_ANY_COLUMN));
        // Literal forms: only the ODBC escape parses, so the SQL-92 form is not advertised.
        SQLUINTEGER lit = 99;
        CHECK(SQL_SUCCEEDED(SQLGetInfoW(c.dbc, SQL_DATETIME_LITERALS, &lit, sizeof lit, &n)) && lit == 0);
        {
            Stmt st(c);
            CHECK(st.exec("SELECT EventUtc FROM RecordedValues WHERE PointId = 'a' AND EventUtc >= TIMESTAMP '2026-01-15 08:00:00' AND EventUtc < TIMESTAMP '2026-01-15 09:00:00'") == SQL_ERROR && st.err() == "0A000");
        }
        // Advertised false: the driver manager must refuse them.
        SQLUSMALLINT supported = 9;
        CHECK(SQL_SUCCEEDED(SQLGetFunctions(c.dbc, SQL_API_SQLBULKOPERATIONS, &supported)) && supported == SQL_FALSE);
        CHECK(SQL_SUCCEEDED(SQLGetFunctions(c.dbc, SQL_API_SQLSETPOS, &supported)) && supported == SQL_FALSE);
        CHECK(SQL_SUCCEEDED(SQLGetFunctions(c.dbc, SQL_API_SQLBROWSECONNECT, &supported)) && supported == SQL_FALSE);
        // Advertised true: each one is exercised elsewhere in this program.
        for (SQLUSMALLINT f : {SQL_API_SQLFETCHSCROLL, SQL_API_SQLGETDATA, SQL_API_SQLCOLUMNS, SQL_API_SQLTABLES, SQL_API_SQLPRIMARYKEYS, SQL_API_SQLCANCEL, SQL_API_SQLDESCRIBEPARAM, SQL_API_SQLGETTYPEINFO}) {
            CHECK(SQL_SUCCEEDED(SQLGetFunctions(c.dbc, f, &supported)) && supported == SQL_TRUE);
        }
        // Scrollable cursors are refused, not faked.
        Stmt st(c);
        CHECK(SQLSetStmtAttrW(st.h, SQL_ATTR_CURSOR_TYPE, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(SQL_CURSOR_STATIC)), 0) == SQL_SUCCESS_WITH_INFO && st.err() == "01S02");
    }

    section("catalog: SQLTables, SQLColumns, SQLGetTypeInfo, SQLPrimaryKeys");
    {
        Stmt s(c);
        CHECK(SQL_SUCCEEDED(SQLTablesW(s.h, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0)));
        std::vector<std::string> tables;
        while (SQL_SUCCEEDED(SQLFetch(s.h))) {
            tables.push_back(s.text(3));
            std::printf("  table %s.%s.%s (%s)\n", s.text(1).c_str(), s.text(2).c_str(), s.text(3).c_str(), s.text(4).c_str());
        }
        CHECK((tables == std::vector<std::string>{"Points", "RecordedValues"}));
        SQLFreeStmt(s.h, SQL_CLOSE);
        WS cat = W("%"), e = W("");
        CHECK(SQL_SUCCEEDED(SQLTablesW(s.h, const_cast<SQLWCHAR*>(cat.c_str()), SQL_NTS, const_cast<SQLWCHAR*>(e.c_str()), SQL_NTS, const_cast<SQLWCHAR*>(e.c_str()), SQL_NTS, nullptr, 0)));
        CHECK(SQL_SUCCEEDED(SQLFetch(s.h)) && s.text(1) == "STRATA" && SQLFetch(s.h) == SQL_NO_DATA);
        SQLFreeStmt(s.h, SQL_CLOSE);
        WS rv = W("Recorded%"), col = W("Event%");
        CHECK(SQL_SUCCEEDED(SQLColumnsW(s.h, nullptr, 0, nullptr, 0, const_cast<SQLWCHAR*>(rv.c_str()), SQL_NTS, const_cast<SQLWCHAR*>(col.c_str()), SQL_NTS)));
        std::vector<std::string> cols;
        while (SQL_SUCCEEDED(SQLFetch(s.h))) {
            cols.push_back(s.text(4));
            SQLSMALLINT dt = 0;
            SQLLEN ind = 0;
            CHECK(SQL_SUCCEEDED(SQLGetData(s.h, 5, SQL_C_SSHORT, &dt, 0, &ind)));
            if (s.text(4) == "EventUtc") CHECK(dt == SQL_TYPE_TIMESTAMP);
        }
        CHECK((cols == std::vector<std::string>{"EventUtc", "EventUtcRaw", "EventSeq"}));
        SQLFreeStmt(s.h, SQL_CLOSE);
        CHECK(SQL_SUCCEEDED(SQLColumnsW(s.h, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0)));
        CHECK(s.count() == 29);  // 10 Points + 19 RecordedValues
        SQLFreeStmt(s.h, SQL_CLOSE);
        CHECK(SQL_SUCCEEDED(SQLGetTypeInfoW(s.h, SQL_ALL_TYPES)));
        CHECK(s.count() == 5);
        SQLFreeStmt(s.h, SQL_CLOSE);
        CHECK(SQL_SUCCEEDED(SQLGetTypeInfoW(s.h, SQL_TYPE_TIMESTAMP)));
        CHECK(s.count() == 1);
        SQLFreeStmt(s.h, SQL_CLOSE);
        WS pts = W("Points");
        CHECK(SQL_SUCCEEDED(SQLPrimaryKeysW(s.h, nullptr, 0, nullptr, 0, const_cast<SQLWCHAR*>(pts.c_str()), SQL_NTS)));
        CHECK(s.count() == 2);
        SQLFreeStmt(s.h, SQL_CLOSE);
        WS rvn = W("RecordedValues");
        CHECK(SQL_SUCCEEDED(SQLPrimaryKeysW(s.h, nullptr, 0, nullptr, 0, const_cast<SQLWCHAR*>(rvn.c_str()), SQL_NTS)));
        CHECK(s.count() == 0);  // no fabricated key
    }

    section("prepared query with bound parameters: golden rows");
    preparedGolden(c, golden, true);

    section("literal SQL with ODBC escapes (the Power BI native-query shape)");
    {
        Stmt s(c);
        CHECK(SQL_SUCCEEDED(s.exec("select \"$Table\".\"EventUtc\" as \"EventUtc\", \"$Table\".\"NumericValue\", \"$Table\".\"StateText\", \"$Table\".\"IsGood\" from \"STRATA\".\"HISTORIAN\".\"RecordedValues\" \"$Table\" where \"$Table\".\"PointId\" = 'PT-PRESSURE-001' and \"$Table\".\"EventUtc\" >= {ts '2026-01-15 08:00:00'} and \"$Table\".\"EventUtc\" < {ts '2026-01-15 09:00:00'}")));
        std::vector<std::string> got;
        while (SQL_SUCCEEDED(SQLFetch(s.h))) got.push_back(s.text(1) + "," + s.text(2) + "," + s.text(3) + "," + s.text(4));
        CHECK(got == golden);
    }

    section("bounded reads and zero-row probes");
    {
        Stmt s(c);
        CHECK(s.exec("SELECT * FROM RecordedValues") == SQL_ERROR && s.err() == "42000");
        CHECK(s.exec("SELECT * FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001'") == SQL_ERROR && s.err() == "42000");
        CHECK(s.exec("SELECT * FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2020-01-01 00:00:00'} AND EventUtc < {ts '2026-01-01 00:00:00'}") == SQL_ERROR && s.err() == "42000");
        CHECK(s.exec("INSERT INTO Points VALUES (1)") == SQL_ERROR && s.err() == "0A000");
        CHECK(s.exec("SELECT COUNT(*) FROM Points") == SQL_ERROR && s.err() == "0A000");
        CHECK(s.exec("SELECT * FROM NoSuch") == SQL_ERROR && s.err() == "42S02");
        CHECK(s.exec("SELECT NoSuch FROM Points") == SQL_ERROR && s.err() == "42S22");
        CHECK(SQL_SUCCEEDED(s.exec("SELECT * FROM RecordedValues WHERE 1=0")));
        SQLSMALLINT n = 0;
        CHECK(SQL_SUCCEEDED(SQLNumResultCols(s.h, &n)) && n == 19);
        CHECK(SQLFetch(s.h) == SQL_NO_DATA);
        SQLFreeStmt(s.h, SQL_CLOSE);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT TOP 0 * FROM RecordedValues")));
        CHECK(SQLFetch(s.h) == SQL_NO_DATA);
        // Prepare-only probe never executes.
        WS q = W("SELECT * FROM RecordedValues");
        SQLFreeStmt(s.h, SQL_CLOSE);
        CHECK(SQL_SUCCEEDED(SQLPrepareW(s.h, const_cast<SQLWCHAR*>(q.c_str()), SQL_NTS)));
        CHECK(SQL_SUCCEEDED(SQLNumResultCols(s.h, &n)) && n == 19);
    }

    section("semantics: denied, zero vs bad vs missing, repeats");
    {
        Stmt s(c);
        CHECK(s.exec("SELECT * FROM RecordedValues WHERE PointId = 'PT-DENIED-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}") == SQL_ERROR && s.err() == "42000");
        CHECK(SQL_SUCCEEDED(s.exec("SELECT TagName FROM Points")));
        bool sawDenied = false;
        while (SQL_SUCCEEDED(SQLFetch(s.h))) if (s.text(1).find("DENIED") != std::string::npos) sawDenied = true;
        CHECK(!sawDenied);
        SQLFreeStmt(s.h, SQL_CLOSE);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:05:00'} AND EventUtc < {ts '2026-01-15 08:05:01'}")));
        CHECK(SQL_SUCCEEDED(SQLFetch(s.h)));
        double d = -1;
        // NULL with no indicator is an error, not a zero.
        CHECK(SQLGetData(s.h, 1, SQL_C_DOUBLE, &d, 0, nullptr) == SQL_ERROR && s.err() == "22002");
        SQLLEN ind = 0;
        CHECK(SQL_SUCCEEDED(SQLGetData(s.h, 1, SQL_C_DOUBLE, &d, 0, &ind)) && ind == SQL_NULL_DATA);
    }

    section("binding, conversions, Unicode, truncation");
    {
        Stmt s(c);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT Descriptor, TagName, IsStep FROM Points WHERE TagName = 'SYN.PRESSURE.PV'")));
        SQLWCHAR small[5];
        SQLLEN ind = 0;
        CHECK(SQL_SUCCEEDED(SQLFetch(s.h)));
        std::u16string whole;
        SQLRETURN r = SQLGetData(s.h, 1, SQL_C_WCHAR, small, sizeof small, &ind);
        CHECK(r == SQL_SUCCESS_WITH_INFO && s.err() == "01004" && ind > 0);
        SQLLEN firstTotal = ind;
        while (r == SQL_SUCCESS_WITH_INFO || r == SQL_SUCCESS) {
            for (int i = 0; small[i]; i++) whole.push_back(static_cast<char16_t>(small[i]));
            if (r == SQL_SUCCESS) break;
            r = SQLGetData(s.h, 1, SQL_C_WCHAR, small, sizeof small, &ind);
        }
        CHECK(SQLGetData(s.h, 1, SQL_C_WCHAR, small, sizeof small, &ind) == SQL_NO_DATA);
        CHECK(whole == u"Synthetic pressure \u2013 \u0394P pr\u00fcfstand");
        CHECK(static_cast<size_t>(firstTotal) == whole.size() * sizeof(SQLWCHAR));
        // Same column read as UTF-8 in SQL_C_CHAR.
        char utf8[128];
        CHECK(SQL_SUCCEEDED(SQLGetData(s.h, 2, SQL_C_CHAR, utf8, sizeof utf8, &ind)) && std::string(utf8) == "SYN.PRESSURE.PV");
        unsigned char bit = 9;
        CHECK(SQL_SUCCEEDED(SQLGetData(s.h, 3, SQL_C_BIT, &bit, 0, &ind)) && bit == 0);
        SQLFreeStmt(s.h, SQL_CLOSE);

        // Bound single-row fetch with typed columns.
        SQL_TIMESTAMP_STRUCT t{};
        double v = 0;
        SQLWCHAR raw[64];
        SQLINTEGER seq = -1;
        SQLLEN i1 = 0, i2 = 0, i3 = 0, i4 = 0;
        CHECK(SQL_SUCCEEDED(SQLBindCol(s.h, 1, SQL_C_TYPE_TIMESTAMP, &t, 0, &i1)));
        CHECK(SQL_SUCCEEDED(SQLBindCol(s.h, 2, SQL_C_DOUBLE, &v, 0, &i2)));
        CHECK(SQL_SUCCEEDED(SQLBindCol(s.h, 3, SQL_C_WCHAR, raw, sizeof raw, &i3)));
        CHECK(SQL_SUCCEEDED(SQLBindCol(s.h, 4, SQL_C_SLONG, &seq, 0, &i4)));
        CHECK(SQL_SUCCEEDED(s.exec("SELECT EventUtc, NumericValue, EventUtcRaw, EventSeq FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:10'} AND EventUtc < {ts '2026-01-15 08:00:11'}")));
        CHECK(SQLFetch(s.h) == SQL_SUCCESS && v == 0.0 && t.hour == 8 && t.second == 10 && t.fraction == 0 && seq == 0);
        CHECK(SQLFetch(s.h) == SQL_SUCCESS && v == 102.25 && t.fraction == 15300 && N(raw) == "2026-01-15T08:00:10.0000153Z" && seq == 0);
        CHECK(SQLFetch(s.h) == SQL_SUCCESS && v == 102.5 && t.fraction == 15300 && seq == 1);
        CHECK(SQLFetch(s.h) == SQL_NO_DATA);
        CHECK(SQL_SUCCEEDED(SQLFreeStmt(s.h, SQL_UNBIND)));
        SQLFreeStmt(s.h, SQL_CLOSE);

        // Metadata.
        CHECK(SQL_SUCCEEDED(s.exec("SELECT EventUtc AS T, NumericValue, IsGood FROM RecordedValues WHERE 1=0")));
        SQLWCHAR name[64];
        SQLSMALLINT nl = 0, dt = 0, dd = 0, nu = 0;
        SQLULEN cs = 0;
        CHECK(SQL_SUCCEEDED(SQLDescribeColW(s.h, 1, name, 64, &nl, &dt, &cs, &dd, &nu)) && N(name) == "T" && dt == SQL_TYPE_TIMESTAMP && cs == 27 && dd == 7 && nu == SQL_NO_NULLS);
        CHECK(SQL_SUCCEEDED(SQLDescribeColW(s.h, 2, name, 64, &nl, &dt, &cs, &dd, &nu)) && dt == SQL_DOUBLE && nu == SQL_NULLABLE);
        SQLLEN attr = 0;
        CHECK(SQL_SUCCEEDED(SQLColAttributeW(s.h, 3, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr, &attr)) && attr == SQL_BIT);
        CHECK(SQLDescribeColW(s.h, 99, name, 64, &nl, &dt, &cs, &dd, &nu) == SQL_ERROR && s.err() == "07009");
    }

    section("rowsets and paging across a backend page boundary");
    for (int stride : {0, 1}) {
        Stmt s(c);
        const SQLULEN N_ROWS = 64;
        std::vector<double> colValues(N_ROWS);
        std::vector<SQLLEN> colInd(N_ROWS);
        struct R { double v; SQLLEN ind; } rows[64];
        SQLUSMALLINT status[64];
        SQLULEN fetched = 0;
        SQLSetStmtAttrW(s.h, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(N_ROWS)), 0);
        SQLSetStmtAttrW(s.h, SQL_ATTR_ROW_STATUS_PTR, status, 0);
        SQLSetStmtAttrW(s.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0);
        if (stride) {
            SQLSetStmtAttrW(s.h, SQL_ATTR_ROW_BIND_TYPE, reinterpret_cast<SQLPOINTER>(sizeof(R)), 0);
            CHECK(SQL_SUCCEEDED(SQLBindCol(s.h, 1, SQL_C_DOUBLE, &rows[0].v, sizeof(double), &rows[0].ind)));
        } else {
            CHECK(SQL_SUCCEEDED(SQLBindCol(s.h, 1, SQL_C_DOUBLE, colValues.data(), sizeof(double), colInd.data())));
        }
        CHECK(SQL_SUCCEEDED(s.exec("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-RAMP-001' AND EventUtc >= {ts '2026-01-16 00:00:00'} AND EventUtc < {ts '2026-01-17 00:00:00'}")));
        long total = 0;
        bool ordered = true;
        for (;;) {
            SQLRETURN r = SQLFetchScroll(s.h, SQL_FETCH_NEXT, 0);
            if (r == SQL_NO_DATA) break;
            CHECK(SQL_SUCCEEDED(r));
            if (!SQL_SUCCEEDED(r)) break;
            for (SQLULEN i = 0; i < fetched; i++) {
                double v = stride ? rows[i].v : colValues[i];
                if (v != static_cast<double>(total) || status[i] != SQL_ROW_SUCCESS) ordered = false;
                total++;
            }
        }
        std::printf("  stride=%d rows=%ld (fixture page size 1000)\n", stride, total);
        CHECK(total == 2500 && ordered);
        CHECK(SQLFetchScroll(s.h, SQL_FETCH_FIRST, 0) == SQL_ERROR && s.err() == "HY106");
    }

    section("partial failure is reported after delivered rows");
    {
        Stmt s(c);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-FAULT-001' AND EventUtc >= {ts '2026-01-16 00:00:00'} AND EventUtc < {ts '2026-01-17 00:00:00'}")));
        long rows = 0;
        SQLRETURN r;
        while ((r = SQLFetch(s.h)) == SQL_SUCCESS) rows++;
        std::string st = s.err();
        std::printf("  delivered=%ld then %s\n", rows, st.c_str());
        CHECK(rows == 1000 && r == SQL_ERROR && st == "08S01");
    }

    section("partial failure inside a rowset: good rows first, then the error");
    {
        Stmt s(c);
        double vals[64];
        SQLLEN inds[64];
        SQLULEN fetched = 0;
        SQLSetStmtAttrW(s.h, SQL_ATTR_ROW_ARRAY_SIZE, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(64)), 0);
        SQLSetStmtAttrW(s.h, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0);
        CHECK(SQL_SUCCEEDED(SQLBindCol(s.h, 1, SQL_C_DOUBLE, vals, sizeof(double), inds)));
        CHECK(SQL_SUCCEEDED(s.exec("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-FAULT-001' AND EventUtc >= {ts '2026-01-16 00:00:00'} AND EventUtc < {ts '2026-01-17 00:00:00'}")));
        long rows = 0;
        SQLRETURN r;
        while ((r = SQLFetchScroll(s.h, SQL_FETCH_NEXT, 0)) == SQL_SUCCESS) rows += static_cast<long>(fetched);
        std::string st = s.err();
        std::printf("  delivered=%ld then %s\n", rows, st.c_str());
        CHECK(rows == 1000 && r == SQL_ERROR && st == "08S01");
    }

    section("cancel and timeout during reads");
    {
        Conn slow(driverCs + "Backend=fixture;PageSize=1;");
        CHECK(SQL_SUCCEEDED(slow.rc));
        Stmt s(slow);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-SLOW-001' AND EventUtc >= {ts '2026-01-16 00:00:00'} AND EventUtc < {ts '2026-01-17 00:00:00'}")));
        std::thread canceler([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            SQLCancel(s.h);
        });
        auto t0 = std::chrono::steady_clock::now();
        SQLRETURN r;
        long rows = 0;
        while ((r = SQLFetch(s.h)) == SQL_SUCCESS) rows++;
        std::string st = s.err();
        canceler.join();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  canceled after %lld ms, %ld rows: %s\n", static_cast<long long>(ms), rows, st.c_str());
        CHECK(r == SQL_ERROR && st == "HY008" && ms < 5000);
        SQLFreeStmt(s.h, SQL_CLOSE);

        SQLSetStmtAttrW(s.h, SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(static_cast<uintptr_t>(1)), 0);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT NumericValue FROM RecordedValues WHERE PointId = 'PT-SLOW-001' AND EventUtc >= {ts '2026-01-16 00:00:00'} AND EventUtc < {ts '2026-01-17 00:00:00'}")));
        t0 = std::chrono::steady_clock::now();
        while ((r = SQLFetch(s.h)) == SQL_SUCCESS) {}
        st = s.err();
        ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  timed out after %lld ms: %s\n", static_cast<long long>(ms), st.c_str());
        CHECK(r == SQL_ERROR && st == "HYT00" && ms < 5000);
    }

    section("Navigator preview policy");
    {
        Conn pv("DSN=" + previewDsn);
        CHECK(SQL_SUCCEEDED(pv.rc));
        Stmt s(pv);
        SQLRETURN r = s.exec("select \"PointId\", \"EventUtc\", \"ResultScope\" from \"STRATA\".\"HISTORIAN\".\"RecordedValues\"");
        CHECK(r == SQL_SUCCESS_WITH_INFO && s.err() == "01000");
        long rows = 0;
        std::string lastScope, point;
        while (SQL_SUCCEEDED(SQLFetch(s.h))) { rows++; point = s.text(1); lastScope = s.text(3); }
        std::printf("  preview rows=%ld point=%s scope=%s\n", rows, point.c_str(), lastScope.c_str());
        CHECK(rows > 0 && rows <= 100 && lastScope == "PREVIEW" && point == "PT-PRESSURE-001");
        SQLFreeStmt(s.h, SQL_CLOSE);
        // A preview DSN still refuses a partly-bounded read.
        CHECK(s.exec("SELECT * FROM RecordedValues WHERE IsGood = 1") == SQL_ERROR && s.err() == "42000");
    }

    section("concurrent connections");
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; i++) {
            threads.emplace_back([&] {
                for (int j = 0; j < 5; j++) {
                    Conn tc("DSN=" + dsn);
                    CHECK(SQL_SUCCEEDED(tc.rc));
                    if (SQL_SUCCEEDED(tc.rc)) preparedGolden(tc, golden, false);
                }
            });
        }
        for (auto& t : threads) t.join();
    }

    section("handle cleanup and state errors");
    {
        Stmt s(c);
        CHECK(SQLFetch(s.h) == SQL_ERROR);
        CHECK(SQLCloseCursor(s.h) == SQL_ERROR);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT PointId FROM Points")));
        CHECK(SQL_SUCCEEDED(SQLCloseCursor(s.h)));
        SQLHSTMT orphan = nullptr;
        CHECK(SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, c.dbc, &orphan)));
        SQLFreeHandle(SQL_HANDLE_STMT, orphan);
    }

    section("trace shows the bounded upstream request and no probe traffic");
    {
        Conn t(driverCs + "Backend=fixture;TraceFile=" + tracePath + ";PWD=hunter2;UID=someone;Auth=Basic;");
        CHECK(SQL_SUCCEEDED(t.rc));
        Stmt s(t);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT * FROM RecordedValues WHERE 1=0")));
        SQLFreeStmt(s.h, SQL_CLOSE);
        CHECK(SQL_SUCCEEDED(s.exec("SELECT EventUtc FROM RecordedValues WHERE PointId = 'PT-PRESSURE-001' AND EventUtc >= {ts '2026-01-15 08:00:00'} AND EventUtc < {ts '2026-01-15 09:00:00'}")));
        while (SQL_SUCCEEDED(SQLFetch(s.h))) {}
        std::ifstream f(tracePath);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string text = ss.str();
        std::printf("%s", text.c_str());
        CHECK(text.find("\"none (zero-row result; no source access)\"") != std::string::npos);
        CHECK(text.find("\"event\":\"readRecorded\",\"backend\":\"fixture\",\"point\":\"PT-PRESSURE-001\",\"start\":\"2026-01-15T08:00:00.0000000Z\",\"end\":\"2026-01-15T09:00:00.0000000Z\",\"pageSize\":1000,\"rows\":7") != std::string::npos);
        size_t reads = 0;
        for (size_t p = text.find("\"event\":\"readRecorded\""); p != std::string::npos; p = text.find("\"event\":\"readRecorded\"", p + 1)) reads++;
        CHECK(reads == 1);
        CHECK(text.find("hunter2") == std::string::npos && text.find("someone") == std::string::npos);
    }

    std::printf("odbc_smoke: %d checks, %d failures\n", checks.load(), failures.load());
    return failures.load() ? 1 : 0;
}
