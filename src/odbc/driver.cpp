// ODBC 3.52 Unicode driver entry points. Exceptions never cross this boundary.
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>
#include <odbcinst.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <random>
#include <sstream>

#include "../backend/backend.h"
#include "config.h"
#include "wstr.h"

using namespace strata;

#define STRATA_DRIVER_NAME "strataodbc.dll"
#define STRATA_DRIVER_VERSION "00.01.0000"

namespace {

struct DiagRecord { std::string state, message; int native; };
struct Diag {
    std::vector<DiagRecord> records;
    void clear() { records.clear(); }
    void add(std::string state, std::string msg, int native = 0) { records.push_back({std::move(state), "[Strata][Historian ODBC] " + msg, native}); }
};

struct Env { Diag diag; SQLINTEGER odbcVersion = SQL_OV_ODBC3; };

struct Stmt;
struct Dbc {
    Env* env;
    Diag diag;
    bool connected = false;
    Config cfg;
    std::unique_ptr<Backend> backend;
    Trace trace;
    SQLUINTEGER loginTimeout = 0;
    std::vector<Stmt*> stmts;
};

struct ColBinding { SQLSMALLINT ctype = 0; char* target = nullptr; SQLLEN len = 0; SQLLEN* ind = nullptr; };
struct ParamBinding { SQLSMALLINT ctype = 0, sqlType = 0; char* value = nullptr; SQLLEN len = 0; SQLLEN* ind = nullptr; bool bound = false; };

struct Desc { Stmt* owner; int kind; };  // implicit descriptors only: 0 ARD, 1 APD, 2 IRD, 3 IPD

struct Stmt {
    Dbc* dbc;
    Diag diag;
    CancelToken cancel;
    std::atomic<bool> busy{false};
    std::optional<Query> query;
    std::vector<ColumnDef> columns;
    bool haveCursor = false;
    std::unique_ptr<RowSource> source;
    Plan plan;
    Row current;
    bool hasCurrent = false;
    std::optional<Row> peeked;
    bool sourceDone = false;
    std::optional<Error> pendingError;
    int gdColumn = -1;
    size_t gdOffset = 0;
    bool gdFinished = false;
    std::vector<ColBinding> cols;
    std::vector<ParamBinding> params;
    SQLULEN rowArraySize = 1, rowBindType = SQL_BIND_BY_COLUMN, maxRows = 0, queryTimeout = 0, paramsetSize = 1;
    SQLULEN* rowsFetchedPtr = nullptr;
    SQLUSMALLINT* rowStatusPtr = nullptr;
    SQLLEN* bindOffsetPtr = nullptr;
    SQLULEN* paramsProcessedPtr = nullptr;
    SQLUSMALLINT* paramStatusPtr = nullptr;
    SQLULEN noscan = SQL_NOSCAN_OFF, retrieveData = SQL_RD_ON;
    Desc ard{this, 0}, apd{this, 1}, ird{this, 2}, ipd{this, 3};
};

template <class H, class F>
SQLRETURN guarded(H* h, F&& f) {
    if (!h) return SQL_INVALID_HANDLE;
    h->diag.clear();
    try {
        return f();
    } catch (const Error& e) {
        h->diag.add(e.sqlstate, e.what(), e.native);
    } catch (const std::bad_alloc&) {
        h->diag.add("HY001", "Memory allocation error.");
    } catch (const std::exception& e) {
        h->diag.add("HY000", e.what());
    } catch (...) {
        h->diag.add("HY000", "Unknown internal error.");
    }
    return SQL_ERROR;
}

SQLRETURN writeTextChars(Diag& d, std::string_view utf8, SQLWCHAR* out, SQLSMALLINT capChars, SQLSMALLINT* lenChars) {
    std::u16string w = utf8ToUtf16(utf8);
    if (lenChars) *lenChars = static_cast<SQLSMALLINT>(std::min<size_t>(w.size(), 32767));
    if (out && capChars > 0) {
        size_t n = std::min<size_t>(w.size(), static_cast<size_t>(capChars) - 1);
        std::memcpy(out, w.data(), n * sizeof(SQLWCHAR));
        out[n] = 0;
    }
    if (out && w.size() >= static_cast<size_t>(std::max<SQLSMALLINT>(capChars, 0))) {
        d.add("01004", "String data, right truncated.");
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

template <class LenT>
SQLRETURN writeTextBytes(Diag& d, std::string_view utf8, SQLPOINTER out, SQLLEN capBytes, LenT* lenBytes) {
    std::u16string w = utf8ToUtf16(utf8);
    if (lenBytes) *lenBytes = static_cast<LenT>(w.size() * sizeof(SQLWCHAR));
    if (out && capBytes >= static_cast<SQLLEN>(sizeof(SQLWCHAR))) {
        size_t n = std::min<size_t>(w.size(), static_cast<size_t>(capBytes) / sizeof(SQLWCHAR) - 1);
        std::memcpy(out, w.data(), n * sizeof(SQLWCHAR));
        static_cast<SQLWCHAR*>(out)[n] = 0;
        if (n < w.size()) { d.add("01004", "String data, right truncated."); return SQL_SUCCESS_WITH_INFO; }
    } else if (out && !w.empty()) {
        d.add("01004", "String data, right truncated.");
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

std::string argText(const SQLWCHAR* s, SQLINTEGER len) { return s ? utf16ToUtf8(reinterpret_cast<const char16_t*>(s), len) : std::string{}; }

SQLSMALLINT sqlTypeOf(ColType t, const Dbc* d) {
    switch (t) {
        case ColType::Text: return SQL_WVARCHAR;
        case ColType::Int16: return SQL_SMALLINT;
        case ColType::Int32: return SQL_INTEGER;
        case ColType::Double: return SQL_DOUBLE;
        case ColType::Bit: return SQL_BIT;
        case ColType::Timestamp: return d && d->env->odbcVersion == SQL_OV_ODBC2 ? SQL_TIMESTAMP : SQL_TYPE_TIMESTAMP;
    }
    return SQL_WVARCHAR;
}
const char* typeNameOf(ColType t) {
    switch (t) {
        case ColType::Text: return "WVARCHAR";
        case ColType::Int16: return "SMALLINT";
        case ColType::Int32: return "INTEGER";
        case ColType::Double: return "DOUBLE";
        case ColType::Bit: return "BIT";
        case ColType::Timestamp: return "TIMESTAMP";
    }
    return "WVARCHAR";
}
SQLULEN columnSizeOf(const ColumnDef& c) {
    switch (c.type) {
        case ColType::Text: return static_cast<SQLULEN>(c.size);
        case ColType::Int16: return 5;
        case ColType::Int32: return 10;
        case ColType::Double: return 15;
        case ColType::Bit: return 1;
        case ColType::Timestamp: return 27;
    }
    return 0;
}
SQLSMALLINT decimalDigitsOf(ColType t) { return t == ColType::Timestamp ? 7 : 0; }
SQLLEN octetLengthOf(const ColumnDef& c) {
    switch (c.type) {
        case ColType::Text: return c.size * 2;
        case ColType::Int16: return 2;
        case ColType::Int32: return 4;
        case ColType::Double: return 8;
        case ColType::Bit: return 1;
        case ColType::Timestamp: return sizeof(SQL_TIMESTAMP_STRUCT);
    }
    return 0;
}

std::string numberText(double d) {
    char b[64];
    for (int p = 15; p <= 17; p++) {
        std::snprintf(b, sizeof b, "%.*g", p, d);
        if (std::strtod(b, nullptr) == d) break;
    }
    return b;
}
std::string valueText(const Value& v) {
    if (auto s = std::get_if<std::string>(&v)) return *s;
    if (auto i = std::get_if<int32_t>(&v)) return std::to_string(*i);
    if (auto d = std::get_if<double>(&v)) return numberText(*d);
    if (auto b = std::get_if<bool>(&v)) return *b ? "1" : "0";
    if (auto t = std::get_if<Ticks>(&v)) {
        CivilTime c = toCivil(*t);
        char buf[40];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d.%07d", c.year, c.month, c.day, c.hour, c.minute, c.second, c.ticks100ns);
        return buf;
    }
    return {};
}

// Converts one value into an application buffer. Returns true when the data was truncated.
// offset is non-null only for SQLGetData piecewise reads.
bool convertOut(Diag& diag, const Value& v, ColType colType, SQLSMALLINT ctype, char* target, SQLLEN cap, SQLLEN* ind, size_t* offset, bool* finished) {
    if (isNull(v)) {
        if (!ind) throw Error("22002", "NULL data fetched and no indicator variable was supplied.");
        *ind = SQL_NULL_DATA;
        if (finished) *finished = true;
        return false;
    }
    if (ctype == SQL_C_DEFAULT) {
        switch (colType) {
            case ColType::Text: ctype = SQL_C_WCHAR; break;
            case ColType::Int16: ctype = SQL_C_SSHORT; break;
            case ColType::Int32: ctype = SQL_C_SLONG; break;
            case ColType::Double: ctype = SQL_C_DOUBLE; break;
            case ColType::Bit: ctype = SQL_C_BIT; break;
            case ColType::Timestamp: ctype = SQL_C_TYPE_TIMESTAMP; break;
        }
    }
    auto setLen = [&](SQLLEN n) { if (ind) *ind = n; };

    if (ctype == SQL_C_WCHAR || ctype == SQL_C_CHAR) {
        std::string text = valueText(v);
        size_t start = offset ? *offset : 0;
        if (ctype == SQL_C_WCHAR) {
            std::u16string w = utf8ToUtf16(text);
            size_t remain = w.size() - std::min(start, w.size());
            setLen(static_cast<SQLLEN>(remain * sizeof(SQLWCHAR)));
            if (!target) { if (cap == 0 && remain) return true; return false; }
            if (cap < static_cast<SQLLEN>(sizeof(SQLWCHAR))) { if (cap < 0) throw Error("HY090", "Invalid buffer length."); return remain > 0; }
            size_t room = static_cast<size_t>(cap) / sizeof(SQLWCHAR) - 1;
            size_t n = std::min(room, remain);
            std::memcpy(target, w.data() + start, n * sizeof(SQLWCHAR));
            reinterpret_cast<SQLWCHAR*>(target)[n] = 0;
            if (offset) *offset = start + n;
            if (finished && n == remain) *finished = true;
            return n < remain;
        }
        size_t remain = text.size() - std::min(start, text.size());
        setLen(static_cast<SQLLEN>(remain));
        if (!target || cap < 1) return remain > 0;
        size_t n = std::min(static_cast<size_t>(cap) - 1, remain);
        std::memcpy(target, text.data() + start, n);
        target[n] = 0;
        if (offset) *offset = start + n;
        if (finished && n == remain) *finished = true;
        return n < remain;
    }

    if (finished) *finished = true;
    auto number = [&]() -> double {
        if (auto d = std::get_if<double>(&v)) return *d;
        if (auto i = std::get_if<int32_t>(&v)) return *i;
        if (auto b = std::get_if<bool>(&v)) return *b ? 1 : 0;
        if (auto s = std::get_if<std::string>(&v)) {
            char* e = nullptr;
            double d = std::strtod(s->c_str(), &e);
            if (s->empty() || *e != 0) throw Error("22018", "Invalid character value for cast specification.");
            return d;
        }
        throw Error("07006", "Restricted data type attribute violation.");
    };
    auto toInt = [&](auto* out, double lo, double hi) {
        double d = number();
        double tr = std::trunc(d);
        if (!(tr >= lo && tr <= hi)) throw Error("22003", "Numeric value out of range.");
        if (tr != d) diag.add("01S07", "Fractional truncation.");
        using T = std::remove_pointer_t<decltype(out)>;
        *out = static_cast<T>(tr);
        setLen(sizeof(T));
    };
    switch (ctype) {
        case SQL_C_SLONG: case SQL_C_LONG: toInt(reinterpret_cast<SQLINTEGER*>(target), -2147483648.0, 2147483647.0); return false;
        case SQL_C_ULONG: toInt(reinterpret_cast<SQLUINTEGER*>(target), 0, 4294967295.0); return false;
        case SQL_C_SSHORT: case SQL_C_SHORT: toInt(reinterpret_cast<SQLSMALLINT*>(target), -32768, 32767); return false;
        case SQL_C_USHORT: toInt(reinterpret_cast<SQLUSMALLINT*>(target), 0, 65535); return false;
        case SQL_C_STINYINT: case SQL_C_TINYINT: toInt(reinterpret_cast<SQLSCHAR*>(target), -128, 127); return false;
        case SQL_C_UTINYINT: toInt(reinterpret_cast<SQLCHAR*>(target), 0, 255); return false;
        case SQL_C_SBIGINT: toInt(reinterpret_cast<SQLBIGINT*>(target), -9.2233720368547758e18, 9.2233720368547748e18); return false;
        case SQL_C_UBIGINT: toInt(reinterpret_cast<SQLUBIGINT*>(target), 0, 1.8446744073709550e19); return false;
        case SQL_C_DOUBLE: *reinterpret_cast<double*>(target) = number(); setLen(sizeof(double)); return false;
        case SQL_C_FLOAT: *reinterpret_cast<float*>(target) = static_cast<float>(number()); setLen(sizeof(float)); return false;
        case SQL_C_BIT: {
            double d = number();
            if (d < 0 || d >= 2) throw Error("22003", "Numeric value out of range.");
            if (d != 0 && d != 1) diag.add("01S07", "Fractional truncation.");
            *reinterpret_cast<SQLCHAR*>(target) = d >= 1 ? 1 : 0;
            setLen(1);
            return false;
        }
        case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: case SQL_C_TYPE_DATE: case SQL_C_DATE: case SQL_C_TYPE_TIME: case SQL_C_TIME: {
            Ticks t;
            if (auto p = std::get_if<Ticks>(&v)) t = *p;
            else if (auto s = std::get_if<std::string>(&v)) {
                if (!parseUtc(*s, t)) throw Error("22018", "Invalid character value for cast specification.");
            } else throw Error("07006", "Restricted data type attribute violation.");
            CivilTime c = toCivil(t);
            if (ctype == SQL_C_TYPE_DATE || ctype == SQL_C_DATE) {
                auto* o = reinterpret_cast<SQL_DATE_STRUCT*>(target);
                o->year = static_cast<SQLSMALLINT>(c.year); o->month = static_cast<SQLUSMALLINT>(c.month); o->day = static_cast<SQLUSMALLINT>(c.day);
                if (c.hour || c.minute || c.second || c.ticks100ns) diag.add("01S07", "Fractional truncation: time part discarded.");
                setLen(sizeof *o);
            } else if (ctype == SQL_C_TYPE_TIME || ctype == SQL_C_TIME) {
                auto* o = reinterpret_cast<SQL_TIME_STRUCT*>(target);
                o->hour = static_cast<SQLUSMALLINT>(c.hour); o->minute = static_cast<SQLUSMALLINT>(c.minute); o->second = static_cast<SQLUSMALLINT>(c.second);
                if (c.ticks100ns) diag.add("01S07", "Fractional truncation: sub-second part discarded.");
                setLen(sizeof *o);
            } else {
                auto* o = reinterpret_cast<SQL_TIMESTAMP_STRUCT*>(target);
                o->year = static_cast<SQLSMALLINT>(c.year); o->month = static_cast<SQLUSMALLINT>(c.month); o->day = static_cast<SQLUSMALLINT>(c.day);
                o->hour = static_cast<SQLUSMALLINT>(c.hour); o->minute = static_cast<SQLUSMALLINT>(c.minute); o->second = static_cast<SQLUSMALLINT>(c.second);
                o->fraction = static_cast<SQLUINTEGER>(c.ticks100ns) * 100;  // ODBC fraction is nanoseconds
                setLen(sizeof *o);
            }
            return false;
        }
        default:
            throw Error("07006", "Restricted data type attribute violation: C type " + std::to_string(ctype) + " is not supported.");
    }
}

Value paramValue(const ParamBinding& b, int number) {
    if (!b.bound) throw Error("07002", "Parameter " + std::to_string(number) + " is not bound.");
    SQLLEN ind = b.ind ? *b.ind : SQL_NTS;
    if (ind == SQL_NULL_DATA) return Value{};
    if (ind == SQL_DATA_AT_EXEC || ind <= SQL_LEN_DATA_AT_EXEC_OFFSET) throw Error("HYC00", "Data-at-execution parameters are not implemented.");
    SQLSMALLINT c = b.ctype;
    if (c == SQL_C_DEFAULT) {
        switch (b.sqlType) {
            case SQL_WVARCHAR: case SQL_WCHAR: case SQL_WLONGVARCHAR: c = SQL_C_WCHAR; break;
            case SQL_VARCHAR: case SQL_CHAR: case SQL_LONGVARCHAR: c = SQL_C_CHAR; break;
            case SQL_INTEGER: c = SQL_C_SLONG; break;
            case SQL_SMALLINT: c = SQL_C_SSHORT; break;
            case SQL_BIGINT: c = SQL_C_SBIGINT; break;
            case SQL_DOUBLE: case SQL_FLOAT: c = SQL_C_DOUBLE; break;
            case SQL_BIT: c = SQL_C_BIT; break;
            case SQL_TYPE_TIMESTAMP: case SQL_TIMESTAMP: c = SQL_C_TYPE_TIMESTAMP; break;
            default: throw Error("HY003", "Unsupported SQL_C_DEFAULT parameter type.");
        }
    }
    const char* p = b.value;
    if (!p) throw Error("HY009", "Parameter value pointer is null.");
    switch (c) {
        case SQL_C_WCHAR: {
            auto* w = reinterpret_cast<const char16_t*>(p);
            return utf16ToUtf8(w, ind == SQL_NTS ? -1 : static_cast<SQLINTEGER>(ind / static_cast<SQLLEN>(sizeof(SQLWCHAR))));
        }
        case SQL_C_CHAR: return ind == SQL_NTS ? std::string(p) : std::string(p, static_cast<size_t>(ind));
        case SQL_C_SLONG: case SQL_C_LONG: return *reinterpret_cast<const SQLINTEGER*>(p);
        case SQL_C_SSHORT: case SQL_C_SHORT: return static_cast<int32_t>(*reinterpret_cast<const SQLSMALLINT*>(p));
        case SQL_C_SBIGINT: return static_cast<double>(*reinterpret_cast<const SQLBIGINT*>(p));
        case SQL_C_DOUBLE: return *reinterpret_cast<const double*>(p);
        case SQL_C_FLOAT: return static_cast<double>(*reinterpret_cast<const float*>(p));
        case SQL_C_BIT: return *reinterpret_cast<const SQLCHAR*>(p) != 0;
        case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: {
            auto* t = reinterpret_cast<const SQL_TIMESTAMP_STRUCT*>(p);
            return fromCivil(t->year, t->month, t->day, t->hour, t->minute, t->second, t->fraction / 100);
        }
        case SQL_C_TYPE_DATE: case SQL_C_DATE: {
            auto* t = reinterpret_cast<const SQL_DATE_STRUCT*>(p);
            return fromCivil(t->year, t->month, t->day, 0, 0, 0, 0);
        }
        default:
            throw Error("HY003", "Parameter C type " + std::to_string(c) + " is not supported.");
    }
}

std::string newCorrelationId() {
    static std::atomic<uint64_t> n{0};
    static const uint64_t seed = std::random_device{}();
    char b[40];
    std::snprintf(b, sizeof b, "%08llx-%06llx", static_cast<unsigned long long>(seed & 0xffffffffULL), static_cast<unsigned long long>(++n));
    return b;
}

void closeCursor(Stmt* s) {
    s->source.reset();
    s->haveCursor = false;
    s->hasCurrent = false;
    s->peeked.reset();
    s->sourceDone = false;
    s->pendingError.reset();
    s->gdColumn = -1;
}

void needConnection(Stmt* s) { if (!s->dbc->connected) throw Error("08003", "Connection does not exist."); }

// Advance one row, deferring failures so rows already delivered reach the caller first.
bool pullRow(Stmt* s, Row& out) {
    if (s->peeked) { out = std::move(*s->peeked); s->peeked.reset(); return true; }
    if (s->sourceDone || !s->source) return false;
    s->busy = true;
    struct Clear { std::atomic<bool>& b; ~Clear() { b = false; } } clear{s->busy};
    if (s->source->next(out)) return true;
    s->sourceDone = true;
    return false;
}

SQLRETURN executeStatement(Stmt* s) {
    needConnection(s);
    if (!s->query) throw Error("HY010", "Function sequence error: no prepared statement.");
    closeCursor(s);
    s->cancel.reset();
    s->cancel.setTimeoutSeconds(static_cast<unsigned long>(s->queryTimeout));
    if (s->paramsetSize != 1) throw Error("HYC00", "Parameter arrays are not implemented.");
    std::vector<Value> values;
    for (int i = 0; i < s->query->paramCount; i++) {
        if (static_cast<size_t>(i) >= s->params.size()) throw Error("07002", "Statement has " + std::to_string(s->query->paramCount) + " parameter markers but parameter " + std::to_string(i + 1) + " is not bound.");
        values.push_back(paramValue(s->params[static_cast<size_t>(i)], i + 1));
    }
    if (s->paramsProcessedPtr) *s->paramsProcessedPtr = 1;
    if (s->paramStatusPtr) *s->paramStatusPtr = SQL_PARAM_SUCCESS;

    Plan plan = buildPlan(*s->query, values, s->dbc->cfg.limits);
    if (s->maxRows > 0) plan.limit = plan.limit ? std::min<long long>(*plan.limit, static_cast<long long>(s->maxRows)) : static_cast<long long>(s->maxRows);
    std::string corr = newCorrelationId();
    const Dbc& d = *s->dbc;
    if (d.trace.enabled())
        d.trace.write(corr, "plan", "\"table\":" + Trace::quote(plan.table->name) + ",\"upstream\":" + Trace::quote(describeUpstream(plan, d.cfg.limits)));
    s->plan = std::move(plan);
    s->columns = s->plan.columns;
    s->source = openPlan(s->plan, *s->dbc->backend, s->cancel, s->dbc->cfg.limits, s->dbc->trace, corr);
    s->haveCursor = true;
    // Pull the first row now so a denied read or dead link fails at execute, where clients expect it.
    Row first;
    if (pullRow(s, first)) s->peeked = std::move(first);
    if (!s->plan.warning.empty()) {
        s->diag.add("01000", s->plan.warning);
        return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
}

void setMemoryResult(Stmt* s, std::vector<ColumnDef> columns, std::vector<Row> rows) {
    closeCursor(s);
    s->query.reset();
    s->columns = std::move(columns);
    s->source = openMemory(std::move(rows));
    s->haveCursor = true;
}

SQLRETURN fetchRowset(Stmt* s) {
    if (!s->haveCursor) throw Error("24000", "Invalid cursor state: no open result set.");
    if (s->pendingError) { Error e = *s->pendingError; s->pendingError.reset(); s->sourceDone = true; throw e; }
    s->gdColumn = -1;
    s->gdFinished = false;
    s->gdOffset = 0;
    SQLULEN want = s->rowArraySize;
    SQLULEN got = 0;
    bool anyInfo = false, anyRowError = false;
    if (s->rowsFetchedPtr) *s->rowsFetchedPtr = 0;
    s->hasCurrent = false;
    for (; got < want; got++) {
        Row row;
        bool have = false;
        try {
            have = pullRow(s, row);
        } catch (const Error& e) {
            if (got == 0) throw;
            s->pendingError = e;  // deliver good rows first; the next fetch reports the failure
        }
        if (!have) break;
        bool rowInfo = false, rowError = false;
        if (s->retrieveData == SQL_RD_ON) {
            for (size_t c = 0; c < s->cols.size() && c < row.size(); c++) {
                const ColBinding& b = s->cols[c];
                if (!b.target && !b.ind) continue;
                SQLLEN off = s->bindOffsetPtr ? *s->bindOffsetPtr : 0;
                size_t elem = s->rowBindType == SQL_BIND_BY_COLUMN ? static_cast<size_t>(b.len) : static_cast<size_t>(s->rowBindType);
                char* target = b.target ? b.target + off + got * elem : nullptr;
                size_t indStride = s->rowBindType == SQL_BIND_BY_COLUMN ? sizeof(SQLLEN) : static_cast<size_t>(s->rowBindType);
                SQLLEN* ind = b.ind ? reinterpret_cast<SQLLEN*>(reinterpret_cast<char*>(b.ind) + off + got * indStride) : nullptr;
                try {
                    size_t before = s->diag.records.size();
                    if (convertOut(s->diag, row[c], s->columns[c].type, b.ctype, target, b.len, ind, nullptr, nullptr)) {
                        s->diag.add("01004", "String data, right truncated.");
                    }
                    if (s->diag.records.size() != before) rowInfo = true;
                } catch (const Error& e) {
                    if (want == 1) throw;
                    s->diag.add(e.sqlstate, e.what());
                    rowError = true;
                }
            }
        }
        if (s->rowStatusPtr) s->rowStatusPtr[got] = rowError ? SQL_ROW_ERROR : rowInfo ? SQL_ROW_SUCCESS_WITH_INFO : SQL_ROW_SUCCESS;
        anyInfo = anyInfo || rowInfo;
        anyRowError = anyRowError || rowError;
        s->current = std::move(row);
        s->hasCurrent = true;
    }
    if (s->rowsFetchedPtr) *s->rowsFetchedPtr = got;
    if (s->rowStatusPtr) for (SQLULEN i = got; i < want; i++) s->rowStatusPtr[i] = SQL_ROW_NOROW;
    if (got == 0) return SQL_NO_DATA;
    return anyInfo || anyRowError ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

// LIKE-style catalog search pattern with \ escape; case-insensitive. Empty/absent matches anything.
bool patternMatch(std::string_view pat, std::string_view s) {
    size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
    auto lc = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    while (i < s.size()) {
        if (p < pat.size() && pat[p] == '%') { star = ++p; mark = i; continue; }
        if (p < pat.size() && pat[p] == '_') { p++; i++; continue; }
        if (p < pat.size()) {
            size_t q = p;
            if (pat[q] == '\\' && q + 1 < pat.size()) q++;
            if (lc(pat[q]) == lc(s[i])) { p = q + 1; i++; continue; }
        }
        if (star != std::string_view::npos) { p = star; i = ++mark; continue; }
        return false;
    }
    while (p < pat.size() && pat[p] == '%') p++;
    return p == pat.size();
}
bool matchArg(const std::string& pat, std::string_view s) { return pat.empty() || patternMatch(pat, s); }

ColumnDef tcol(const char* n, int size = 128, bool nullable = true) { return {n, ColType::Text, size, nullable, ""}; }
ColumnDef scol(const char* n, bool nullable = true) { return {n, ColType::Int16, 5, nullable, ""}; }
ColumnDef icol(const char* n, bool nullable = true) { return {n, ColType::Int32, 10, nullable, ""}; }
Value vs(const std::string& s) { return s; }
Value vi(long long i) { return static_cast<int32_t>(i); }

const std::vector<const TableDef*>& allTables() {
    static const std::vector<const TableDef*> t = {&pointsTable(), &recordedValuesTable()};
    return t;
}

SQLLEN stmtAttrInt(SQLPOINTER v) { return reinterpret_cast<SQLLEN>(v); }

constexpr SQLUSMALLINT kFunctions[] = {
    SQL_API_SQLALLOCHANDLE, SQL_API_SQLFREEHANDLE, SQL_API_SQLSETENVATTR, SQL_API_SQLGETENVATTR, SQL_API_SQLSETCONNECTATTR, SQL_API_SQLGETCONNECTATTR, SQL_API_SQLSETSTMTATTR, SQL_API_SQLGETSTMTATTR,
    SQL_API_SQLCONNECT, SQL_API_SQLDRIVERCONNECT, SQL_API_SQLDISCONNECT, SQL_API_SQLGETINFO, SQL_API_SQLGETFUNCTIONS, SQL_API_SQLGETTYPEINFO, SQL_API_SQLTABLES, SQL_API_SQLCOLUMNS,
    SQL_API_SQLPRIMARYKEYS, SQL_API_SQLFOREIGNKEYS, SQL_API_SQLSTATISTICS, SQL_API_SQLSPECIALCOLUMNS, SQL_API_SQLPROCEDURES, SQL_API_SQLPROCEDURECOLUMNS, SQL_API_SQLTABLEPRIVILEGES, SQL_API_SQLCOLUMNPRIVILEGES,
    SQL_API_SQLPREPARE, SQL_API_SQLEXECDIRECT, SQL_API_SQLEXECUTE, SQL_API_SQLNUMPARAMS, SQL_API_SQLDESCRIBEPARAM, SQL_API_SQLBINDPARAMETER, SQL_API_SQLNUMRESULTCOLS, SQL_API_SQLDESCRIBECOL, SQL_API_SQLCOLATTRIBUTE,
    SQL_API_SQLBINDCOL, SQL_API_SQLFETCH, SQL_API_SQLFETCHSCROLL, SQL_API_SQLGETDATA, SQL_API_SQLMORERESULTS, SQL_API_SQLROWCOUNT, SQL_API_SQLCLOSECURSOR, SQL_API_SQLFREESTMT, SQL_API_SQLCANCEL, SQL_API_SQLENDTRAN,
    SQL_API_SQLNATIVESQL, SQL_API_SQLGETDIAGREC, SQL_API_SQLGETDIAGFIELD, SQL_API_SQLGETDESCFIELD, SQL_API_SQLSETDESCFIELD,
};

struct InfoStr { SQLUSMALLINT id; const char* v; };
const InfoStr kInfoStr[] = {
    {SQL_DRIVER_NAME, STRATA_DRIVER_NAME}, {SQL_DRIVER_VER, STRATA_DRIVER_VERSION}, {SQL_DRIVER_ODBC_VER, "03.52"}, {SQL_ODBC_VER, "03.52"},
    {SQL_DBMS_NAME, "Strata Historian"}, {SQL_DBMS_VER, STRATA_DRIVER_VERSION}, {SQL_IDENTIFIER_QUOTE_CHAR, "\""}, {SQL_CATALOG_NAME_SEPARATOR, "."}, {SQL_CATALOG_TERM, "catalog"}, {SQL_SCHEMA_TERM, "schema"},
    {SQL_TABLE_TERM, "table"}, {SQL_PROCEDURE_TERM, ""}, {SQL_SEARCH_PATTERN_ESCAPE, "\\"}, {SQL_SPECIAL_CHARACTERS, "$"}, {SQL_KEYWORDS, ""}, {SQL_ACCESSIBLE_TABLES, "Y"}, {SQL_ACCESSIBLE_PROCEDURES, "N"},
    {SQL_DATA_SOURCE_READ_ONLY, "Y"}, {SQL_EXPRESSIONS_IN_ORDERBY, "N"}, {SQL_MULT_RESULT_SETS, "N"}, {SQL_MULTIPLE_ACTIVE_TXN, "N"}, {SQL_NEED_LONG_DATA_LEN, "N"}, {SQL_ORDER_BY_COLUMNS_IN_SELECT, "N"},
    {SQL_PROCEDURES, "N"}, {SQL_ROW_UPDATES, "N"}, {SQL_DESCRIBE_PARAMETER, "Y"}, {SQL_INTEGRITY, "N"}, {SQL_COLUMN_ALIAS, "Y"}, {SQL_COLLATION_SEQ, ""}, {SQL_CATALOG_NAME, "Y"}, {SQL_LIKE_ESCAPE_CLAUSE, "N"},
    {SQL_MAX_ROW_SIZE_INCLUDES_LONG, "N"}, {SQL_OUTER_JOINS, "N"}, {SQL_XOPEN_CLI_YEAR, "1995"},
};
struct InfoU16 { SQLUSMALLINT id; SQLUSMALLINT v; };
const InfoU16 kInfoU16[] = {
    {SQL_ACTIVE_ENVIRONMENTS, 0}, {SQL_MAX_DRIVER_CONNECTIONS, 0}, {SQL_MAX_CONCURRENT_ACTIVITIES, 0}, {SQL_ODBC_API_CONFORMANCE, SQL_OAC_NONE}, {SQL_ODBC_SQL_CONFORMANCE, SQL_OSC_MINIMUM},
    {SQL_CURSOR_COMMIT_BEHAVIOR, SQL_CB_PRESERVE}, {SQL_CURSOR_ROLLBACK_BEHAVIOR, SQL_CB_PRESERVE}, {SQL_TXN_CAPABLE, SQL_TC_NONE}, {SQL_CONCAT_NULL_BEHAVIOR, SQL_CB_NULL}, {SQL_CORRELATION_NAME, SQL_CN_ANY},
    {SQL_FILE_USAGE, SQL_FILE_NOT_SUPPORTED}, {SQL_GROUP_BY, SQL_GB_NOT_SUPPORTED}, {SQL_IDENTIFIER_CASE, SQL_IC_MIXED}, {SQL_QUOTED_IDENTIFIER_CASE, SQL_IC_MIXED}, {SQL_NON_NULLABLE_COLUMNS, SQL_NNC_NON_NULL},
    {SQL_NULL_COLLATION, SQL_NC_LOW}, {SQL_CATALOG_LOCATION, SQL_CL_START}, {SQL_MAX_CATALOG_NAME_LEN, 128}, {SQL_MAX_SCHEMA_NAME_LEN, 128}, {SQL_MAX_TABLE_NAME_LEN, 128}, {SQL_MAX_COLUMN_NAME_LEN, 128}, {SQL_MAX_CURSOR_NAME_LEN, 0},
    {SQL_MAX_COLUMNS_IN_SELECT, 0}, {SQL_MAX_COLUMNS_IN_ORDER_BY, 0}, {SQL_MAX_COLUMNS_IN_GROUP_BY, 0}, {SQL_MAX_COLUMNS_IN_INDEX, 0}, {SQL_MAX_COLUMNS_IN_TABLE, 0}, {SQL_MAX_TABLES_IN_SELECT, 1}, {SQL_MAX_USER_NAME_LEN, 0},
    {SQL_MAX_PROCEDURE_NAME_LEN, 0}, {SQL_MAX_IDENTIFIER_LEN, 128}, {SQL_TXN_ISOLATION_OPTION, 0}, {SQL_ODBC_SAG_CLI_CONFORMANCE, SQL_OSCC_NOT_COMPLIANT},
};
struct InfoU32 { SQLUSMALLINT id; SQLUINTEGER v; };
const InfoU32 kInfoU32[] = {
    {SQL_GETDATA_EXTENSIONS, SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND}, {SQL_ODBC_INTERFACE_CONFORMANCE, SQL_OIC_CORE}, {SQL_SQL_CONFORMANCE, SQL_SC_SQL92_ENTRY},
    {SQL_SCROLL_OPTIONS, SQL_SO_FORWARD_ONLY}, {SQL_CURSOR_SENSITIVITY, SQL_UNSPECIFIED}, {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, SQL_CA1_NEXT}, {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2, SQL_CA2_READ_ONLY_CONCURRENCY | SQL_CA2_MAX_ROWS_SELECT},
    {SQL_STATIC_CURSOR_ATTRIBUTES1, 0}, {SQL_STATIC_CURSOR_ATTRIBUTES2, 0}, {SQL_KEYSET_CURSOR_ATTRIBUTES1, 0}, {SQL_KEYSET_CURSOR_ATTRIBUTES2, 0}, {SQL_DYNAMIC_CURSOR_ATTRIBUTES1, 0}, {SQL_DYNAMIC_CURSOR_ATTRIBUTES2, 0},
    {SQL_ASYNC_MODE, SQL_AM_NONE}, {SQL_MAX_ASYNC_CONCURRENT_STATEMENTS, 0}, {SQL_ASYNC_DBC_FUNCTIONS, 0}, {SQL_BATCH_ROW_COUNT, 0}, {SQL_BATCH_SUPPORT, 0}, {SQL_PARAM_ARRAY_ROW_COUNTS, SQL_PARC_NO_BATCH}, {SQL_PARAM_ARRAY_SELECTS, SQL_PAS_NO_SELECT},
    {SQL_BOOKMARK_PERSISTENCE, 0}, {SQL_LOCK_TYPES, 0}, {SQL_POS_OPERATIONS, 0}, {SQL_POSITIONED_STATEMENTS, 0}, {SQL_STATIC_SENSITIVITY, 0}, {SQL_SCROLL_CONCURRENCY, SQL_SCCO_READ_ONLY},
    {SQL_AGGREGATE_FUNCTIONS, 0}, {SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, 0}, {SQL_SQL92_STRING_FUNCTIONS, 0}, {SQL_SQL92_DATETIME_FUNCTIONS, 0}, {SQL_SQL92_VALUE_EXPRESSIONS, 0}, {SQL_NUMERIC_FUNCTIONS, 0}, {SQL_STRING_FUNCTIONS, 0}, {SQL_SYSTEM_FUNCTIONS, 0}, {SQL_TIMEDATE_FUNCTIONS, 0}, {SQL_TIMEDATE_ADD_INTERVALS, 0}, {SQL_TIMEDATE_DIFF_INTERVALS, 0}, {SQL_CONVERT_FUNCTIONS, 0},
    {SQL_SQL92_PREDICATES, SQL_SP_COMPARISON | SQL_SP_IN | SQL_SP_BETWEEN | SQL_SP_ISNULL | SQL_SP_ISNOTNULL}, {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, 0}, {SQL_OJ_CAPABILITIES, 0}, {SQL_SUBQUERIES, 0}, {SQL_UNION, 0},
    {SQL_CATALOG_USAGE, SQL_CU_DML_STATEMENTS}, {SQL_SCHEMA_USAGE, SQL_SU_DML_STATEMENTS}, {SQL_DATETIME_LITERALS, 0},
    {SQL_ALTER_DOMAIN, 0}, {SQL_ALTER_TABLE, 0}, {SQL_CREATE_ASSERTION, 0}, {SQL_CREATE_CHARACTER_SET, 0}, {SQL_CREATE_COLLATION, 0}, {SQL_CREATE_DOMAIN, 0}, {SQL_CREATE_SCHEMA, 0}, {SQL_CREATE_TABLE, 0}, {SQL_CREATE_TRANSLATION, 0}, {SQL_CREATE_VIEW, 0},
    {SQL_DROP_ASSERTION, 0}, {SQL_DROP_CHARACTER_SET, 0}, {SQL_DROP_COLLATION, 0}, {SQL_DROP_DOMAIN, 0}, {SQL_DROP_SCHEMA, 0}, {SQL_DROP_TABLE, 0}, {SQL_DROP_TRANSLATION, 0}, {SQL_DROP_VIEW, 0}, {SQL_DDL_INDEX, 0}, {SQL_INDEX_KEYWORDS, SQL_IK_NONE},
    {SQL_INSERT_STATEMENT, 0}, {SQL_SQL92_FOREIGN_KEY_DELETE_RULE, 0}, {SQL_SQL92_FOREIGN_KEY_UPDATE_RULE, 0}, {SQL_SQL92_GRANT, 0}, {SQL_SQL92_REVOKE, 0}, {SQL_SQL92_ROW_VALUE_CONSTRUCTOR, 0},
    {SQL_STANDARD_CLI_CONFORMANCE, 0}, {SQL_DEFAULT_TXN_ISOLATION, 0}, {SQL_DRIVER_HDESC, 0}, {SQL_MAX_ROW_SIZE, 0}, {SQL_MAX_STATEMENT_LEN, 65536}, {SQL_MAX_CHAR_LITERAL_LEN, 4096}, {SQL_MAX_BINARY_LITERAL_LEN, 0}, {SQL_MAX_INDEX_SIZE, 0},
    {SQL_CONVERT_BIGINT, 0}, {SQL_CONVERT_BINARY, 0}, {SQL_CONVERT_BIT, 0}, {SQL_CONVERT_CHAR, 0}, {SQL_CONVERT_DATE, 0}, {SQL_CONVERT_DECIMAL, 0}, {SQL_CONVERT_DOUBLE, 0}, {SQL_CONVERT_FLOAT, 0}, {SQL_CONVERT_INTEGER, 0}, {SQL_CONVERT_LONGVARBINARY, 0}, {SQL_CONVERT_LONGVARCHAR, 0}, {SQL_CONVERT_NUMERIC, 0}, {SQL_CONVERT_REAL, 0}, {SQL_CONVERT_SMALLINT, 0}, {SQL_CONVERT_TIME, 0}, {SQL_CONVERT_TIMESTAMP, 0}, {SQL_CONVERT_TINYINT, 0}, {SQL_CONVERT_VARBINARY, 0}, {SQL_CONVERT_VARCHAR, 0}, {SQL_CONVERT_WCHAR, 0}, {SQL_CONVERT_WLONGVARCHAR, 0}, {SQL_CONVERT_WVARCHAR, 0},
};

}  // namespace

#define DRIVER_API extern "C" STRATA_EXPORT SQLRETURN SQL_API

// ---------------------------------------------------------------- handles
DRIVER_API SQLAllocHandle(SQLSMALLINT type, SQLHANDLE input, SQLHANDLE* output) {
    if (!output) return SQL_ERROR;
    *output = SQL_NULL_HANDLE;
    try {
        switch (type) {
            case SQL_HANDLE_ENV: *output = new Env(); return SQL_SUCCESS;
            case SQL_HANDLE_DBC: {
                if (!input) return SQL_INVALID_HANDLE;
                auto* d = new Dbc();
                d->env = static_cast<Env*>(input);
                *output = d;
                return SQL_SUCCESS;
            }
            case SQL_HANDLE_STMT: {
                if (!input) return SQL_INVALID_HANDLE;
                auto* d = static_cast<Dbc*>(input);
                if (!d->connected) { d->diag.clear(); d->diag.add("08003", "Connection does not exist."); return SQL_ERROR; }
                auto* s = new Stmt();
                s->dbc = d;
                d->stmts.push_back(s);
                *output = s;
                return SQL_SUCCESS;
            }
            default: return SQL_ERROR;  // SQL_HANDLE_DESC: explicit descriptors are not implemented
        }
    } catch (...) {
        return SQL_ERROR;
    }
}

DRIVER_API SQLFreeHandle(SQLSMALLINT type, SQLHANDLE h) {
    if (!h) return SQL_INVALID_HANDLE;
    switch (type) {
        case SQL_HANDLE_ENV: delete static_cast<Env*>(h); return SQL_SUCCESS;
        case SQL_HANDLE_DBC: {
            auto* d = static_cast<Dbc*>(h);
            for (auto* s : d->stmts) delete s;
            delete d;
            return SQL_SUCCESS;
        }
        case SQL_HANDLE_STMT: {
            auto* s = static_cast<Stmt*>(h);
            auto& v = s->dbc->stmts;
            v.erase(std::remove(v.begin(), v.end(), s), v.end());
            delete s;
            return SQL_SUCCESS;
        }
        default: return SQL_ERROR;
    }
}

DRIVER_API SQLFreeStmt(SQLHSTMT h, SQLUSMALLINT option) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        switch (option) {
            case SQL_CLOSE: closeCursor(s); return SQL_SUCCESS;
            case SQL_UNBIND: s->cols.clear(); return SQL_SUCCESS;
            case SQL_RESET_PARAMS: s->params.clear(); return SQL_SUCCESS;
            case SQL_DROP: {
                auto& v = s->dbc->stmts;
                v.erase(std::remove(v.begin(), v.end(), s), v.end());
                delete s;
                return SQL_SUCCESS;
            }
            default: throw Error("HY092", "Invalid option.");
        }
    });
}

DRIVER_API SQLCloseCursor(SQLHSTMT h) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (!s->haveCursor) throw Error("24000", "Invalid cursor state.");
        closeCursor(s);
        return SQL_SUCCESS;
    });
}

// Safe from another thread. Without an operation in flight this has no effect (ODBC 3.5 behavior).
DRIVER_API SQLCancel(SQLHSTMT h) {
    auto* s = static_cast<Stmt*>(h);
    if (!s) return SQL_INVALID_HANDLE;
    if (s->busy.load()) s->cancel.cancel();
    return SQL_SUCCESS;
}

// ---------------------------------------------------------------- attributes
DRIVER_API SQLSetEnvAttr(SQLHENV h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER) {
    auto* e = static_cast<Env*>(h);
    return guarded(e, [&]() -> SQLRETURN {
        switch (attr) {
            case SQL_ATTR_ODBC_VERSION: e->odbcVersion = static_cast<SQLINTEGER>(stmtAttrInt(value)); return SQL_SUCCESS;
            case SQL_ATTR_OUTPUT_NTS:
                if (stmtAttrInt(value) == SQL_TRUE) return SQL_SUCCESS;
                throw Error("HYC00", "SQL_ATTR_OUTPUT_NTS=FALSE is not implemented.");
            case SQL_ATTR_CONNECTION_POOLING: case SQL_ATTR_CP_MATCH: return SQL_SUCCESS;
            default: throw Error("HY092", "Invalid environment attribute.");
        }
    });
}
DRIVER_API SQLGetEnvAttr(SQLHENV h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER, SQLINTEGER* len) {
    auto* e = static_cast<Env*>(h);
    return guarded(e, [&]() -> SQLRETURN {
        SQLINTEGER v;
        switch (attr) {
            case SQL_ATTR_ODBC_VERSION: v = e->odbcVersion; break;
            case SQL_ATTR_OUTPUT_NTS: v = SQL_TRUE; break;
            default: throw Error("HY092", "Invalid environment attribute.");
        }
        if (value) *static_cast<SQLINTEGER*>(value) = v;
        if (len) *len = sizeof v;
        return SQL_SUCCESS;
    });
}

DRIVER_API SQLSetConnectAttrW(SQLHDBC h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        switch (attr) {
            case SQL_ATTR_LOGIN_TIMEOUT: d->loginTimeout = static_cast<SQLUINTEGER>(stmtAttrInt(value)); return SQL_SUCCESS;
            case SQL_ATTR_CONNECTION_TIMEOUT: case SQL_ATTR_PACKET_SIZE: case SQL_ATTR_QUIET_MODE: case SQL_ATTR_ANSI_APP: case SQL_ATTR_ODBC_CURSORS: return SQL_SUCCESS;
            case SQL_ATTR_METADATA_ID:
                if (stmtAttrInt(value) == SQL_FALSE) return SQL_SUCCESS;
                d->diag.add("01S02", "SQL_ATTR_METADATA_ID stays FALSE.");
                return SQL_SUCCESS_WITH_INFO;
            case SQL_ATTR_ACCESS_MODE:
                if (stmtAttrInt(value) == SQL_MODE_READ_ONLY) return SQL_SUCCESS;
                d->diag.add("01S02", "This driver is read-only; access mode stays READ_ONLY.");
                return SQL_SUCCESS_WITH_INFO;
            case SQL_ATTR_AUTOCOMMIT:
                if (stmtAttrInt(value) == SQL_AUTOCOMMIT_ON) return SQL_SUCCESS;
                d->diag.add("01S02", "Transactions are not supported; autocommit stays ON.");
                return SQL_SUCCESS_WITH_INFO;
            case SQL_ATTR_ASYNC_ENABLE:
                if (stmtAttrInt(value) == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
                throw Error("HYC00", "Asynchronous execution is not implemented.");
            case SQL_ATTR_TXN_ISOLATION: case SQL_ATTR_CURRENT_CATALOG: case SQL_ATTR_TRACE: case SQL_ATTR_TRACEFILE:
                return SQL_SUCCESS;
            default: throw Error("HYC00", "Connection attribute " + std::to_string(attr) + " is not implemented.");
        }
    });
}
DRIVER_API SQLGetConnectAttrW(SQLHDBC h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER cap, SQLINTEGER* len) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        SQLUINTEGER v;
        switch (attr) {
            case SQL_ATTR_ACCESS_MODE: v = SQL_MODE_READ_ONLY; break;
            case SQL_ATTR_AUTOCOMMIT: v = SQL_AUTOCOMMIT_ON; break;
            case SQL_ATTR_LOGIN_TIMEOUT: v = d->loginTimeout; break;
            case SQL_ATTR_CONNECTION_TIMEOUT: case SQL_ATTR_PACKET_SIZE: case SQL_ATTR_TXN_ISOLATION: v = 0; break;
            case SQL_ATTR_CONNECTION_DEAD: v = d->connected ? SQL_CD_FALSE : SQL_CD_TRUE; break;
            case SQL_ATTR_ASYNC_ENABLE: v = SQL_ASYNC_ENABLE_OFF; break;
            case SQL_ATTR_AUTO_IPD: case SQL_ATTR_METADATA_ID: v = SQL_FALSE; break;
            case SQL_ATTR_CURRENT_CATALOG: return writeTextBytes(d->diag, kCatalogName, value, cap, len);
            default: throw Error("HYC00", "Connection attribute " + std::to_string(attr) + " is not implemented.");
        }
        if (value) *static_cast<SQLUINTEGER*>(value) = v;
        if (len) *len = sizeof v;
        return SQL_SUCCESS;
    });
}

DRIVER_API SQLSetStmtAttrW(SQLHSTMT h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        SQLULEN v = static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(value));
        auto fixed = [&](SQLULEN only, const char* what) -> SQLRETURN {
            if (v == only) return SQL_SUCCESS;
            s->diag.add("01S02", std::string("Option value changed: ") + what);
            return SQL_SUCCESS_WITH_INFO;
        };
        switch (attr) {
            case SQL_ATTR_ROW_ARRAY_SIZE:
                if (v == 0) throw Error("HY024", "Row array size must be at least 1.");
                s->rowArraySize = v;
                return SQL_SUCCESS;
            case SQL_ATTR_ROW_BIND_TYPE: s->rowBindType = v; return SQL_SUCCESS;
            case SQL_ATTR_ROW_BIND_OFFSET_PTR: s->bindOffsetPtr = static_cast<SQLLEN*>(value); return SQL_SUCCESS;
            case SQL_ATTR_ROW_STATUS_PTR: s->rowStatusPtr = static_cast<SQLUSMALLINT*>(value); return SQL_SUCCESS;
            case SQL_ATTR_ROWS_FETCHED_PTR: s->rowsFetchedPtr = static_cast<SQLULEN*>(value); return SQL_SUCCESS;
            case SQL_ATTR_PARAMS_PROCESSED_PTR: s->paramsProcessedPtr = static_cast<SQLULEN*>(value); return SQL_SUCCESS;
            case SQL_ATTR_PARAM_STATUS_PTR: s->paramStatusPtr = static_cast<SQLUSMALLINT*>(value); return SQL_SUCCESS;
            case SQL_ATTR_PARAMSET_SIZE: s->paramsetSize = v; return SQL_SUCCESS;
            case SQL_ATTR_PARAM_BIND_TYPE: return fixed(SQL_PARAM_BIND_BY_COLUMN, "parameter binding stays column-wise.");
            case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
                if (!value) return SQL_SUCCESS;
                throw Error("HYC00", "Parameter bind offsets are not implemented.");
            case SQL_ATTR_MAX_ROWS: s->maxRows = v; return SQL_SUCCESS;
            case SQL_ATTR_QUERY_TIMEOUT: s->queryTimeout = v; return SQL_SUCCESS;
            case SQL_ATTR_NOSCAN: s->noscan = v; return SQL_SUCCESS;
            case SQL_ATTR_RETRIEVE_DATA: s->retrieveData = v; return SQL_SUCCESS;
            case SQL_ATTR_MAX_LENGTH: return fixed(0, "MAX_LENGTH stays 0 (no limit).");
            case SQL_ATTR_CURSOR_TYPE: return fixed(SQL_CURSOR_FORWARD_ONLY, "only forward-only cursors are available.");
            case SQL_ATTR_CONCURRENCY: return fixed(SQL_CONCUR_READ_ONLY, "only read-only concurrency is available.");
            case SQL_ATTR_CURSOR_SCROLLABLE: return fixed(SQL_NONSCROLLABLE, "cursors are not scrollable.");
            case SQL_ATTR_CURSOR_SENSITIVITY: return fixed(SQL_UNSPECIFIED, "sensitivity stays unspecified.");
            case SQL_ATTR_USE_BOOKMARKS: return fixed(SQL_UB_OFF, "bookmarks are not implemented.");
            case SQL_ATTR_ENABLE_AUTO_IPD: return fixed(SQL_FALSE, "automatic IPD is not implemented.");
            case SQL_ATTR_METADATA_ID: return fixed(SQL_FALSE, "METADATA_ID stays FALSE.");
            case SQL_ATTR_ASYNC_ENABLE:
                if (v == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
                throw Error("HYC00", "Asynchronous execution is not implemented.");
            case SQL_ATTR_APP_ROW_DESC: case SQL_ATTR_APP_PARAM_DESC:
                if (!value || value == &s->ard || value == &s->apd) return SQL_SUCCESS;
                throw Error("HYC00", "Explicit descriptors are not implemented.");
            default: throw Error("HYC00", "Statement attribute " + std::to_string(attr) + " is not implemented.");
        }
    });
}
DRIVER_API SQLGetStmtAttrW(SQLHSTMT h, SQLINTEGER attr, SQLPOINTER value, SQLINTEGER, SQLINTEGER* len) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        SQLULEN v = 0;
        switch (attr) {
            case SQL_ATTR_APP_ROW_DESC: v = reinterpret_cast<SQLULEN>(&s->ard); break;
            case SQL_ATTR_APP_PARAM_DESC: v = reinterpret_cast<SQLULEN>(&s->apd); break;
            case SQL_ATTR_IMP_ROW_DESC: v = reinterpret_cast<SQLULEN>(&s->ird); break;
            case SQL_ATTR_IMP_PARAM_DESC: v = reinterpret_cast<SQLULEN>(&s->ipd); break;
            case SQL_ATTR_ROW_ARRAY_SIZE: v = s->rowArraySize; break;
            case SQL_ATTR_ROW_BIND_TYPE: v = s->rowBindType; break;
            case SQL_ATTR_ROW_BIND_OFFSET_PTR: v = reinterpret_cast<SQLULEN>(s->bindOffsetPtr); break;
            case SQL_ATTR_ROW_STATUS_PTR: v = reinterpret_cast<SQLULEN>(s->rowStatusPtr); break;
            case SQL_ATTR_ROWS_FETCHED_PTR: v = reinterpret_cast<SQLULEN>(s->rowsFetchedPtr); break;
            case SQL_ATTR_PARAMS_PROCESSED_PTR: v = reinterpret_cast<SQLULEN>(s->paramsProcessedPtr); break;
            case SQL_ATTR_PARAM_STATUS_PTR: v = reinterpret_cast<SQLULEN>(s->paramStatusPtr); break;
            case SQL_ATTR_PARAMSET_SIZE: v = s->paramsetSize; break;
            case SQL_ATTR_PARAM_BIND_TYPE: v = SQL_PARAM_BIND_BY_COLUMN; break;
            case SQL_ATTR_PARAM_BIND_OFFSET_PTR: v = 0; break;
            case SQL_ATTR_MAX_ROWS: v = s->maxRows; break;
            case SQL_ATTR_MAX_LENGTH: v = 0; break;
            case SQL_ATTR_QUERY_TIMEOUT: v = s->queryTimeout; break;
            case SQL_ATTR_NOSCAN: v = s->noscan; break;
            case SQL_ATTR_RETRIEVE_DATA: v = s->retrieveData; break;
            case SQL_ATTR_CURSOR_TYPE: v = SQL_CURSOR_FORWARD_ONLY; break;
            case SQL_ATTR_CONCURRENCY: v = SQL_CONCUR_READ_ONLY; break;
            case SQL_ATTR_CURSOR_SCROLLABLE: v = SQL_NONSCROLLABLE; break;
            case SQL_ATTR_CURSOR_SENSITIVITY: v = SQL_UNSPECIFIED; break;
            case SQL_ATTR_USE_BOOKMARKS: v = SQL_UB_OFF; break;
            case SQL_ATTR_ENABLE_AUTO_IPD: case SQL_ATTR_METADATA_ID: v = SQL_FALSE; break;
            case SQL_ATTR_ASYNC_ENABLE: v = SQL_ASYNC_ENABLE_OFF; break;
            case SQL_ATTR_ROW_NUMBER: throw Error("HY109", "Row numbers are not available on a forward-only cursor.");
            default: throw Error("HYC00", "Statement attribute " + std::to_string(attr) + " is not implemented.");
        }
        if (value) *static_cast<SQLULEN*>(value) = v;
        if (len) *len = sizeof v;
        return SQL_SUCCESS;
    });
}

// ---------------------------------------------------------------- connect
static std::string dsnValue(const std::string& dsn, const std::string& key) {
    SQLWCHAR out[2048];
    std::u16string wdsn = utf8ToUtf16(dsn), wkey = utf8ToUtf16(key);
    static const SQLWCHAR kEmpty[] = {0};
    static const SQLWCHAR kIni[] = {'o', 'd', 'b', 'c', '.', 'i', 'n', 'i', 0};
    int n = SQLGetPrivateProfileStringW(reinterpret_cast<const SQLWCHAR*>(wdsn.c_str()), reinterpret_cast<const SQLWCHAR*>(wkey.c_str()), kEmpty, out, 2048, kIni);
    return n > 0 ? utf16ToUtf8(reinterpret_cast<const char16_t*>(out), n) : std::string{};
}

static SQLRETURN connectWith(Dbc* d, const KeyValues& kv) {
    if (d->connected) throw Error("08002", "Connection name in use.");
    Config cfg = resolveConfig(kv, dsnValue);
    auto backend = makeBackend(cfg);
    CancelToken token;
    token.setTimeoutSeconds(d->loginTimeout);
    backend->testConnection(token);
    d->cfg = std::move(cfg);
    d->trace = Trace(d->cfg.traceFile);
    d->backend = std::move(backend);
    d->connected = true;
    d->trace.write(newCorrelationId(), "connect", "\"backend\":" + Trace::quote(d->backend->name()) + ",\"server\":" + Trace::quote(d->backend->server()) + ",\"driver\":\"" STRATA_DRIVER_VERSION "\"");
    return SQL_SUCCESS;
}

DRIVER_API SQLConnectW(SQLHDBC h, SQLWCHAR* dsn, SQLSMALLINT dsnLen, SQLWCHAR* uid, SQLSMALLINT uidLen, SQLWCHAR* pwd, SQLSMALLINT pwdLen) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        KeyValues kv;
        kv["DSN"] = argText(dsn, dsnLen);
        if (uid) kv["UID"] = argText(uid, uidLen);
        if (pwd) kv["PWD"] = argText(pwd, pwdLen);
        return connectWith(d, kv);
    });
}

DRIVER_API SQLDriverConnectW(SQLHDBC h, SQLHWND, SQLWCHAR* in, SQLSMALLINT inLen, SQLWCHAR* out, SQLSMALLINT outCap, SQLSMALLINT* outLen, SQLUSMALLINT) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        // No dialog is ever raised: gateway hosts are unattended. Missing settings are errors.
        KeyValues kv = parseConnectionString(argText(in, inLen));
        SQLRETURN r = connectWith(d, kv);
        if (out || outLen) {
            SQLRETURN w = writeTextChars(d->diag, redactedConnectionString(kv), out, outCap, outLen);
            if (w != SQL_SUCCESS) r = w;
        }
        return r;
    });
}

DRIVER_API SQLDisconnect(SQLHDBC h) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        if (!d->connected) throw Error("08003", "Connection does not exist.");
        for (auto* s : d->stmts) delete s;
        d->stmts.clear();
        d->backend.reset();
        d->connected = false;
        return SQL_SUCCESS;
    });
}

// ---------------------------------------------------------------- capability reporting
DRIVER_API SQLGetFunctions(SQLHDBC h, SQLUSMALLINT id, SQLUSMALLINT* out) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        if (!out) throw Error("HY009", "Null output pointer.");
        if (id == SQL_API_ODBC3_ALL_FUNCTIONS) {
            std::memset(out, 0, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * sizeof(SQLUSMALLINT));
            for (auto f : kFunctions) out[f >> 4] |= static_cast<SQLUSMALLINT>(1 << (f & 0xF));
            return SQL_SUCCESS;
        }
        if (id == SQL_API_ALL_FUNCTIONS) {
            std::memset(out, 0, 100 * sizeof(SQLUSMALLINT));
            for (auto f : kFunctions) if (f < 100) out[f] = SQL_TRUE;
            return SQL_SUCCESS;
        }
        *out = std::find(std::begin(kFunctions), std::end(kFunctions), id) != std::end(kFunctions) ? SQL_TRUE : SQL_FALSE;
        return SQL_SUCCESS;
    });
}

DRIVER_API SQLGetInfoW(SQLHDBC h, SQLUSMALLINT id, SQLPOINTER value, SQLSMALLINT cap, SQLSMALLINT* len) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        switch (id) {
            case SQL_DATA_SOURCE_NAME: return writeTextBytes(d->diag, d->cfg.dsn, value, cap, len);
            case SQL_SERVER_NAME: return writeTextBytes(d->diag, d->backend ? d->backend->server() : std::string{}, value, cap, len);
            case SQL_DATABASE_NAME: return writeTextBytes(d->diag, kCatalogName, value, cap, len);
            case SQL_USER_NAME: return writeTextBytes(d->diag, d->cfg.user, value, cap, len);
            default: break;
        }
        for (auto& i : kInfoStr) if (i.id == id) return writeTextBytes(d->diag, i.v, value, cap, len);
        for (auto& i : kInfoU16) if (i.id == id) {
            if (value) *static_cast<SQLUSMALLINT*>(value) = i.v;
            if (len) *len = sizeof(SQLUSMALLINT);
            return SQL_SUCCESS;
        }
        for (auto& i : kInfoU32) if (i.id == id) {
            if (value) *static_cast<SQLUINTEGER*>(value) = i.v;
            if (len) *len = sizeof(SQLUINTEGER);
            return SQL_SUCCESS;
        }
        throw Error("HY096", "SQLGetInfo type " + std::to_string(id) + " is not implemented.");
    });
}

// ---------------------------------------------------------------- catalog functions
DRIVER_API SQLGetTypeInfoW(SQLHSTMT h, SQLSMALLINT wanted) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        needConnection(s);
        std::vector<ColumnDef> cols = {tcol("TYPE_NAME", 128, false), scol("DATA_TYPE", false), icol("COLUMN_SIZE"), tcol("LITERAL_PREFIX"), tcol("LITERAL_SUFFIX"), tcol("CREATE_PARAMS"), scol("NULLABLE", false), scol("CASE_SENSITIVE", false), scol("SEARCHABLE", false), scol("UNSIGNED_ATTRIBUTE"), scol("FIXED_PREC_SCALE", false), scol("AUTO_UNIQUE_VALUE"), tcol("LOCAL_TYPE_NAME"), scol("MINIMUM_SCALE"), scol("MAXIMUM_SCALE"), scol("SQL_DATA_TYPE", false), scol("SQL_DATETIME_SUB"), icol("NUM_PREC_RADIX"), scol("INTERVAL_PRECISION")};
        struct Ty { ColType t; int size; const char* pre; const char* suf; int searchable; int radix; };
        const Ty types[] = {
            {ColType::Bit, 1, "", "", SQL_PRED_BASIC, 2},
            {ColType::Int32, 10, "", "", SQL_PRED_BASIC, 10},
            {ColType::Double, 15, "", "", SQL_PRED_BASIC, 10},
            {ColType::Timestamp, 27, "{ts '", "'}", SQL_PRED_BASIC, 0},
            {ColType::Text, 4000, "'", "'", SQL_PRED_BASIC, 0},
        };
        std::vector<Row> rows;
        for (auto& t : types) {
            SQLSMALLINT st = sqlTypeOf(t.t, s->dbc);
            if (wanted != SQL_ALL_TYPES && wanted != st) continue;
            Row r(cols.size());
            r[0] = vs(typeNameOf(t.t));
            r[1] = vi(st);
            r[2] = vi(t.size);
            if (*t.pre) r[3] = vs(t.pre);
            if (*t.suf) r[4] = vs(t.suf);
            r[6] = vi(SQL_NULLABLE);
            r[7] = vi(t.t == ColType::Text ? SQL_TRUE : SQL_FALSE);
            r[8] = vi(t.searchable);
            if (t.t == ColType::Int32 || t.t == ColType::Double) r[9] = vi(SQL_FALSE);
            r[10] = vi(SQL_FALSE);
            if (t.t == ColType::Timestamp) { r[13] = vi(7); r[14] = vi(7); }
            r[15] = vi(t.t == ColType::Timestamp ? SQL_DATETIME : st);
            if (t.t == ColType::Timestamp) r[16] = vi(SQL_CODE_TIMESTAMP);
            if (t.radix) r[17] = vi(t.radix);
            rows.push_back(std::move(r));
        }
        setMemoryResult(s, std::move(cols), std::move(rows));
        return SQL_SUCCESS;
    });
}

DRIVER_API SQLTablesW(SQLHSTMT h, SQLWCHAR* cat, SQLSMALLINT catLen, SQLWCHAR* sch, SQLSMALLINT schLen, SQLWCHAR* tab, SQLSMALLINT tabLen, SQLWCHAR* typ, SQLSMALLINT typLen) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        needConnection(s);
        std::string c = argText(cat, catLen), sc = argText(sch, schLen), t = argText(tab, tabLen), ty = argText(typ, typLen);
        std::vector<ColumnDef> cols = {tcol("TABLE_CAT"), tcol("TABLE_SCHEM"), tcol("TABLE_NAME"), tcol("TABLE_TYPE"), tcol("REMARKS", 512)};
        std::vector<Row> rows;
        auto mk = [&](Value a, Value b, Value d, Value e, Value f) { rows.push_back(Row{std::move(a), std::move(b), std::move(d), std::move(e), std::move(f)}); };
        if (c == "%" && sc.empty() && t.empty()) mk(vs(kCatalogName), {}, {}, {}, {});
        else if (sc == "%" && c.empty() && t.empty()) mk({}, vs(kSchemaName), {}, {}, {});
        else if (ty == "%" && c.empty() && sc.empty() && t.empty()) mk({}, {}, {}, vs("TABLE"), {});
        else {
            // TableType is a comma-separated list, optionally quoted.
            bool wantsTable = ty.empty() || ty == "%" || toUpper(ty).find("TABLE") != std::string::npos;
            if (wantsTable && matchArg(c, kCatalogName) && matchArg(sc, kSchemaName))
                for (auto* def : allTables())
                    if (matchArg(t, def->name)) mk(vs(kCatalogName), vs(kSchemaName), vs(def->name), vs("TABLE"), vs(def->description));
        }
        setMemoryResult(s, std::move(cols), std::move(rows));
        return SQL_SUCCESS;
    });
}

DRIVER_API SQLColumnsW(SQLHSTMT h, SQLWCHAR* cat, SQLSMALLINT catLen, SQLWCHAR* sch, SQLSMALLINT schLen, SQLWCHAR* tab, SQLSMALLINT tabLen, SQLWCHAR* col, SQLSMALLINT colLen) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        needConnection(s);
        std::string c = argText(cat, catLen), sc = argText(sch, schLen), t = argText(tab, tabLen), cl = argText(col, colLen);
        std::vector<ColumnDef> cols = {tcol("TABLE_CAT"), tcol("TABLE_SCHEM"), tcol("TABLE_NAME", 128, false), tcol("COLUMN_NAME", 128, false), scol("DATA_TYPE", false), tcol("TYPE_NAME", 128, false), icol("COLUMN_SIZE"), icol("BUFFER_LENGTH"), scol("DECIMAL_DIGITS"), scol("NUM_PREC_RADIX"), scol("NULLABLE", false), tcol("REMARKS", 512), tcol("COLUMN_DEF"), scol("SQL_DATA_TYPE", false), scol("SQL_DATETIME_SUB"), icol("CHAR_OCTET_LENGTH"), icol("ORDINAL_POSITION", false), tcol("IS_NULLABLE")};
        std::vector<Row> rows;
        if (matchArg(c, kCatalogName) && matchArg(sc, kSchemaName)) {
            for (auto* def : allTables()) {
                if (!matchArg(t, def->name)) continue;
                int ordinal = 0;
                for (auto& d : def->columns) {
                    ordinal++;
                    if (!matchArg(cl, d.name)) continue;
                    SQLSMALLINT st = sqlTypeOf(d.type, s->dbc);
                    Row r(cols.size());
                    r[0] = vs(kCatalogName); r[1] = vs(kSchemaName); r[2] = vs(def->name); r[3] = vs(d.name);
                    r[4] = vi(st); r[5] = vs(typeNameOf(d.type)); r[6] = vi(static_cast<long long>(columnSizeOf(d))); r[7] = vi(octetLengthOf(d));
                    if (d.type == ColType::Timestamp) r[8] = vi(7);
                    else if (d.type == ColType::Int32) r[8] = vi(0);
                    if (d.type == ColType::Int32 || d.type == ColType::Double) r[9] = vi(10);
                    r[10] = vi(d.nullable ? SQL_NULLABLE : SQL_NO_NULLS);
                    r[11] = vs(d.description);
                    r[13] = vi(d.type == ColType::Timestamp ? SQL_DATETIME : st);
                    if (d.type == ColType::Timestamp) r[14] = vi(SQL_CODE_TIMESTAMP);
                    if (d.type == ColType::Text) r[15] = vi(d.size * 2);
                    r[16] = vi(ordinal);
                    r[17] = vs(d.nullable ? "YES" : "NO");
                    rows.push_back(std::move(r));
                }
            }
        }
        setMemoryResult(s, std::move(cols), std::move(rows));
        return SQL_SUCCESS;
    });
}

DRIVER_API SQLPrimaryKeysW(SQLHSTMT h, SQLWCHAR* cat, SQLSMALLINT catLen, SQLWCHAR* sch, SQLSMALLINT schLen, SQLWCHAR* tab, SQLSMALLINT tabLen) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        needConnection(s);
        std::string c = argText(cat, catLen), sc = argText(sch, schLen), t = argText(tab, tabLen);
        std::vector<ColumnDef> cols = {tcol("TABLE_CAT"), tcol("TABLE_SCHEM"), tcol("TABLE_NAME", 128, false), tcol("COLUMN_NAME", 128, false), scol("KEY_SEQ", false), tcol("PK_NAME")};
        std::vector<Row> rows;
        // RecordedValues deliberately declares no key: EventSeq is read-relative, not a source identity.
        if (iequals(t, "Points") && matchArg(c, kCatalogName) && matchArg(sc, kSchemaName)) {
            rows.push_back(Row{vs(kCatalogName), vs(kSchemaName), vs("Points"), vs("SourceServer"), vi(1), vs("PK_Points")});
            rows.push_back(Row{vs(kCatalogName), vs(kSchemaName), vs("Points"), vs("PointId"), vi(2), vs("PK_Points")});
        }
        setMemoryResult(s, std::move(cols), std::move(rows));
        return SQL_SUCCESS;
    });
}

#define EMPTY_RESULT(...)                                                  \
    auto* s = static_cast<Stmt*>(h);                                       \
    return guarded(s, [&]() -> SQLRETURN {                                 \
        needConnection(s);                                                 \
        setMemoryResult(s, std::vector<ColumnDef>{__VA_ARGS__}, {});       \
        return SQL_SUCCESS;                                                \
    })

DRIVER_API SQLForeignKeysW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT) {
    EMPTY_RESULT(tcol("PKTABLE_CAT"), tcol("PKTABLE_SCHEM"), tcol("PKTABLE_NAME", 128, false), tcol("PKCOLUMN_NAME", 128, false), tcol("FKTABLE_CAT"), tcol("FKTABLE_SCHEM"), tcol("FKTABLE_NAME", 128, false), tcol("FKCOLUMN_NAME", 128, false), scol("KEY_SEQ", false), scol("UPDATE_RULE"), scol("DELETE_RULE"), tcol("FK_NAME"), tcol("PK_NAME"), scol("DEFERRABILITY"));
}
DRIVER_API SQLStatisticsW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLUSMALLINT, SQLUSMALLINT) {
    EMPTY_RESULT(tcol("TABLE_CAT"), tcol("TABLE_SCHEM"), tcol("TABLE_NAME", 128, false), scol("NON_UNIQUE"), tcol("INDEX_QUALIFIER"), tcol("INDEX_NAME"), scol("TYPE", false), scol("ORDINAL_POSITION"), tcol("COLUMN_NAME"), tcol("ASC_OR_DESC"), icol("CARDINALITY"), icol("PAGES"), tcol("FILTER_CONDITION"));
}
DRIVER_API SQLSpecialColumnsW(SQLHSTMT h, SQLUSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLUSMALLINT, SQLUSMALLINT) {
    EMPTY_RESULT(scol("SCOPE"), tcol("COLUMN_NAME", 128, false), scol("DATA_TYPE", false), tcol("TYPE_NAME", 128, false), icol("COLUMN_SIZE"), icol("BUFFER_LENGTH"), scol("DECIMAL_DIGITS"), scol("PSEUDO_COLUMN"));
}
DRIVER_API SQLProceduresW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT) {
    EMPTY_RESULT(tcol("PROCEDURE_CAT"), tcol("PROCEDURE_SCHEM"), tcol("PROCEDURE_NAME", 128, false), scol("NUM_INPUT_PARAMS"), scol("NUM_OUTPUT_PARAMS"), scol("NUM_RESULT_SETS"), tcol("REMARKS"), scol("PROCEDURE_TYPE"));
}
DRIVER_API SQLProcedureColumnsW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT) {
    EMPTY_RESULT(tcol("PROCEDURE_CAT"), tcol("PROCEDURE_SCHEM"), tcol("PROCEDURE_NAME", 128, false), tcol("COLUMN_NAME", 128, false), scol("COLUMN_TYPE", false), scol("DATA_TYPE", false), tcol("TYPE_NAME", 128, false), icol("COLUMN_SIZE"), icol("BUFFER_LENGTH"), scol("DECIMAL_DIGITS"), scol("NUM_PREC_RADIX"), scol("NULLABLE", false), tcol("REMARKS"), tcol("COLUMN_DEF"), scol("SQL_DATA_TYPE", false), scol("SQL_DATETIME_SUB"), icol("CHAR_OCTET_LENGTH"), icol("ORDINAL_POSITION", false), tcol("IS_NULLABLE"));
}
DRIVER_API SQLTablePrivilegesW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT) {
    EMPTY_RESULT(tcol("TABLE_CAT"), tcol("TABLE_SCHEM"), tcol("TABLE_NAME", 128, false), tcol("GRANTOR"), tcol("GRANTEE", 128, false), tcol("PRIVILEGE", 128, false), tcol("IS_GRANTABLE"));
}
DRIVER_API SQLColumnPrivilegesW(SQLHSTMT h, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT, SQLWCHAR*, SQLSMALLINT) {
    EMPTY_RESULT(tcol("TABLE_CAT"), tcol("TABLE_SCHEM"), tcol("TABLE_NAME", 128, false), tcol("COLUMN_NAME", 128, false), tcol("GRANTOR"), tcol("GRANTEE", 128, false), tcol("PRIVILEGE", 128, false), tcol("IS_GRANTABLE"));
}

// ---------------------------------------------------------------- statements
DRIVER_API SQLPrepareW(SQLHSTMT h, SQLWCHAR* sql, SQLINTEGER len) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        needConnection(s);
        if (!sql) throw Error("HY009", "Null statement text.");
        closeCursor(s);
        s->query = parseSql(argText(sql, len));
        s->columns = describe(*s->query);  // no source access: metadata probes stay cheap
        return SQL_SUCCESS;
    });
}
DRIVER_API SQLExecute(SQLHSTMT h) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN { return executeStatement(s); });
}
DRIVER_API SQLExecDirectW(SQLHSTMT h, SQLWCHAR* sql, SQLINTEGER len) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        needConnection(s);
        if (!sql) throw Error("HY009", "Null statement text.");
        closeCursor(s);
        s->query = parseSql(argText(sql, len));
        s->columns = describe(*s->query);
        return executeStatement(s);
    });
}
DRIVER_API SQLNativeSqlW(SQLHDBC h, SQLWCHAR* in, SQLINTEGER inLen, SQLWCHAR* out, SQLINTEGER cap, SQLINTEGER* outLen) {
    auto* d = static_cast<Dbc*>(h);
    return guarded(d, [&]() -> SQLRETURN {
        SQLSMALLINT n = 0;
        SQLRETURN r = writeTextChars(d->diag, argText(in, inLen), out, static_cast<SQLSMALLINT>(std::min<SQLINTEGER>(cap, 32767)), &n);
        if (outLen) *outLen = n;
        return r;
    });
}
DRIVER_API SQLNumParams(SQLHSTMT h, SQLSMALLINT* out) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (!s->query) throw Error("HY010", "Function sequence error.");
        if (out) *out = static_cast<SQLSMALLINT>(s->query->paramCount);
        return SQL_SUCCESS;
    });
}
DRIVER_API SQLDescribeParam(SQLHSTMT h, SQLUSMALLINT n, SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits, SQLSMALLINT* nullable) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (!s->query) throw Error("HY010", "Function sequence error.");
        if (n < 1 || n > s->query->paramCount) throw Error("07009", "Invalid parameter number.");
        ColType t = parameterType(*s->query, n - 1);
        if (type) *type = sqlTypeOf(t, s->dbc);
        if (size) *size = t == ColType::Text ? 256 : columnSizeOf(ColumnDef{"", t, 0, true, ""});
        if (digits) *digits = decimalDigitsOf(t);
        if (nullable) *nullable = SQL_NO_NULLS;
        return SQL_SUCCESS;
    });
}
DRIVER_API SQLBindParameter(SQLHSTMT h, SQLUSMALLINT n, SQLSMALLINT io, SQLSMALLINT ctype, SQLSMALLINT sqlType, SQLULEN, SQLSMALLINT, SQLPOINTER value, SQLLEN, SQLLEN* ind) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (n < 1) throw Error("07009", "Invalid parameter number.");
        if (io != SQL_PARAM_INPUT) throw Error("HY105", "Only input parameters are supported.");
        if (s->params.size() < n) s->params.resize(n);
        s->params[n - 1] = ParamBinding{ctype, sqlType, static_cast<char*>(value), 0, ind, true};
        return SQL_SUCCESS;
    });
}
DRIVER_API SQLNumResultCols(SQLHSTMT h, SQLSMALLINT* out) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (out) *out = static_cast<SQLSMALLINT>(s->columns.size());
        return SQL_SUCCESS;
    });
}
static const ColumnDef& columnAt(Stmt* s, SQLUSMALLINT n) {
    if (n < 1 || n > s->columns.size()) throw Error("07009", "Invalid descriptor index.");
    return s->columns[n - 1u];
}
DRIVER_API SQLDescribeColW(SQLHSTMT h, SQLUSMALLINT n, SQLWCHAR* name, SQLSMALLINT cap, SQLSMALLINT* nameLen, SQLSMALLINT* type, SQLULEN* size, SQLSMALLINT* digits, SQLSMALLINT* nullable) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        const ColumnDef& c = columnAt(s, n);
        if (type) *type = sqlTypeOf(c.type, s->dbc);
        if (size) *size = columnSizeOf(c);
        if (digits) *digits = decimalDigitsOf(c.type);
        if (nullable) *nullable = c.nullable ? SQL_NULLABLE : SQL_NO_NULLS;
        return writeTextChars(s->diag, c.name, name, cap, nameLen);
    });
}
DRIVER_API SQLColAttributeW(SQLHSTMT h, SQLUSMALLINT n, SQLUSMALLINT field, SQLPOINTER text, SQLSMALLINT cap, SQLSMALLINT* len, SQLLEN* num) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (field == SQL_DESC_COUNT) { if (num) *num = static_cast<SQLLEN>(s->columns.size()); return SQL_SUCCESS; }
        const ColumnDef& c = columnAt(s, n);
        auto str = [&](std::string_view v) { return writeTextBytes(s->diag, v, text, cap, len); };
        auto set = [&](SQLLEN v) { if (num) *num = v; return static_cast<SQLRETURN>(SQL_SUCCESS); };
        switch (field) {
            case SQL_DESC_NAME: case SQL_DESC_LABEL: case SQL_COLUMN_NAME: case SQL_DESC_BASE_COLUMN_NAME: return str(c.name);
            case SQL_DESC_TABLE_NAME: case SQL_DESC_BASE_TABLE_NAME: return str(s->plan.table && s->query ? s->plan.table->name : (s->query ? s->query->table : std::string{}));
            case SQL_DESC_SCHEMA_NAME: return str(s->query ? kSchemaName : "");
            case SQL_DESC_CATALOG_NAME: return str(s->query ? kCatalogName : "");
            case SQL_DESC_TYPE_NAME: case SQL_DESC_LOCAL_TYPE_NAME: return str(typeNameOf(c.type));
            case SQL_DESC_LITERAL_PREFIX: return str(c.type == ColType::Text ? "'" : c.type == ColType::Timestamp ? "{ts '" : "");
            case SQL_DESC_LITERAL_SUFFIX: return str(c.type == ColType::Text ? "'" : c.type == ColType::Timestamp ? "'}" : "");
            case SQL_DESC_CONCISE_TYPE: return set(sqlTypeOf(c.type, s->dbc));
            case SQL_DESC_TYPE: return set(c.type == ColType::Timestamp ? SQL_DATETIME : sqlTypeOf(c.type, s->dbc));
            case SQL_DESC_DATETIME_INTERVAL_CODE: return set(c.type == ColType::Timestamp ? SQL_CODE_TIMESTAMP : 0);
            case SQL_DESC_LENGTH: case SQL_COLUMN_LENGTH: return set(c.type == ColType::Text ? c.size : octetLengthOf(c));
            case SQL_DESC_OCTET_LENGTH: return set(octetLengthOf(c));
            case SQL_DESC_PRECISION: case SQL_COLUMN_PRECISION: return set(c.type == ColType::Timestamp ? 7 : static_cast<SQLLEN>(columnSizeOf(c)));
            case SQL_DESC_SCALE: case SQL_COLUMN_SCALE: return set(0);
            case SQL_DESC_DISPLAY_SIZE: return set(c.type == ColType::Double ? 24 : c.type == ColType::Int32 ? 11 : c.type == ColType::Int16 ? 6 : static_cast<SQLLEN>(columnSizeOf(c)));
            case SQL_DESC_NULLABLE: case SQL_COLUMN_NULLABLE: return set(c.nullable ? SQL_NULLABLE : SQL_NO_NULLS);
            case SQL_DESC_NUM_PREC_RADIX: return set(c.type == ColType::Int16 || c.type == ColType::Int32 || c.type == ColType::Double ? 10 : c.type == ColType::Bit ? 2 : 0);
            case SQL_DESC_UNSIGNED: return set(c.type == ColType::Int16 || c.type == ColType::Int32 || c.type == ColType::Double ? SQL_FALSE : SQL_TRUE);
            case SQL_DESC_CASE_SENSITIVE: return set(c.type == ColType::Text ? SQL_TRUE : SQL_FALSE);
            case SQL_DESC_SEARCHABLE: return set(SQL_PRED_BASIC);
            case SQL_DESC_UPDATABLE: return set(SQL_ATTR_READONLY);
            case SQL_DESC_AUTO_UNIQUE_VALUE: case SQL_DESC_FIXED_PREC_SCALE: return set(SQL_FALSE);
            case SQL_DESC_UNNAMED: return set(SQL_NAMED);
            default: throw Error("HY091", "Invalid descriptor field identifier.");
        }
    });
}
DRIVER_API SQLBindCol(SQLHSTMT h, SQLUSMALLINT n, SQLSMALLINT ctype, SQLPOINTER target, SQLLEN cap, SQLLEN* ind) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (n < 1) throw Error("07009", "Bookmark columns are not implemented.");
        if (cap < 0) throw Error("HY090", "Invalid buffer length.");
        if (s->cols.size() < n) s->cols.resize(n);
        s->cols[n - 1u] = ColBinding{ctype, static_cast<char*>(target), cap, ind};
        return SQL_SUCCESS;
    });
}
DRIVER_API SQLFetch(SQLHSTMT h) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN { return fetchRowset(s); });
}
DRIVER_API SQLFetchScroll(SQLHSTMT h, SQLSMALLINT orientation, SQLLEN) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (orientation != SQL_FETCH_NEXT) throw Error("HY106", "Only SQL_FETCH_NEXT is available on a forward-only cursor.");
        return fetchRowset(s);
    });
}
DRIVER_API SQLGetData(SQLHSTMT h, SQLUSMALLINT n, SQLSMALLINT ctype, SQLPOINTER target, SQLLEN cap, SQLLEN* ind) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (!s->hasCurrent) throw Error("24000", "Invalid cursor state: no current row.");
        if (s->rowArraySize > 1) throw Error("HYC00", "SQLGetData with a rowset larger than one row is not implemented.");
        if (n < 1 || n > s->current.size()) throw Error("07009", "Invalid descriptor index.");
        if (s->gdColumn != n) { s->gdColumn = n; s->gdOffset = 0; s->gdFinished = false; }
        if (s->gdFinished) return SQL_NO_DATA;
        size_t before = s->diag.records.size();
        bool truncated = convertOut(s->diag, s->current[n - 1u], s->columns[n - 1u].type, ctype, static_cast<char*>(target), cap, ind, &s->gdOffset, &s->gdFinished);
        if (truncated) s->diag.add("01004", "String data, right truncated.");
        return s->diag.records.size() != before ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    });
}
DRIVER_API SQLMoreResults(SQLHSTMT h) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        closeCursor(s);
        return SQL_NO_DATA;
    });
}
DRIVER_API SQLRowCount(SQLHSTMT h, SQLLEN* out) {
    auto* s = static_cast<Stmt*>(h);
    return guarded(s, [&]() -> SQLRETURN {
        if (out) *out = -1;
        return SQL_SUCCESS;
    });
}
DRIVER_API SQLEndTran(SQLSMALLINT, SQLHANDLE, SQLSMALLINT) { return SQL_SUCCESS; }  // nothing to commit: read-only

// ---------------------------------------------------------------- implicit descriptors (reduced)
DRIVER_API SQLGetDescFieldW(SQLHDESC h, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value, SQLINTEGER cap, SQLINTEGER* len) {
    auto* d = static_cast<Desc*>(h);
    if (!d) return SQL_INVALID_HANDLE;
    Stmt* s = d->owner;
    return guarded(s, [&]() -> SQLRETURN {
        auto put = [&](auto v) { if (value) *static_cast<decltype(v)*>(value) = v; if (len) *len = sizeof v; return static_cast<SQLRETURN>(SQL_SUCCESS); };
        if (field == SQL_DESC_COUNT) return put(static_cast<SQLSMALLINT>(d->kind == 2 ? s->columns.size() : d->kind == 0 ? s->cols.size() : s->params.size()));
        if (field == SQL_DESC_ARRAY_SIZE && d->kind == 0) return put(s->rowArraySize);
        if (d->kind == 2 && rec >= 1) {
            const ColumnDef& c = columnAt(s, static_cast<SQLUSMALLINT>(rec));
            switch (field) {
                case SQL_DESC_NAME: return writeTextBytes(s->diag, c.name, value, cap, len);
                case SQL_DESC_CONCISE_TYPE: return put(sqlTypeOf(c.type, s->dbc));
                case SQL_DESC_NULLABLE: return put(static_cast<SQLSMALLINT>(c.nullable ? SQL_NULLABLE : SQL_NO_NULLS));
                case SQL_DESC_OCTET_LENGTH: return put(octetLengthOf(c));
                default: break;
            }
        }
        throw Error("HYC00", "Descriptor field " + std::to_string(field) + " is not implemented.");
    });
}
DRIVER_API SQLSetDescFieldW(SQLHDESC h, SQLSMALLINT, SQLSMALLINT field, SQLPOINTER value, SQLINTEGER) {
    auto* d = static_cast<Desc*>(h);
    if (!d) return SQL_INVALID_HANDLE;
    Stmt* s = d->owner;
    return guarded(s, [&]() -> SQLRETURN {
        if (d->kind == 0 && field == SQL_DESC_ARRAY_SIZE) {
            SQLULEN v = static_cast<SQLULEN>(reinterpret_cast<uintptr_t>(value));
            if (v == 0) throw Error("HY024", "Row array size must be at least 1.");
            s->rowArraySize = v;
            return SQL_SUCCESS;
        }
        throw Error("HYC00", "Descriptor field " + std::to_string(field) + " cannot be set.");
    });
}

// ---------------------------------------------------------------- diagnostics
static Diag* diagFor(SQLSMALLINT type, SQLHANDLE h) {
    switch (type) {
        case SQL_HANDLE_ENV: return &static_cast<Env*>(h)->diag;
        case SQL_HANDLE_DBC: return &static_cast<Dbc*>(h)->diag;
        case SQL_HANDLE_STMT: return &static_cast<Stmt*>(h)->diag;
        case SQL_HANDLE_DESC: return &static_cast<Desc*>(h)->owner->diag;
        default: return nullptr;
    }
}
DRIVER_API SQLGetDiagRecW(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, SQLWCHAR* state, SQLINTEGER* native, SQLWCHAR* msg, SQLSMALLINT cap, SQLSMALLINT* len) {
    if (!h) return SQL_INVALID_HANDLE;
    Diag* d = diagFor(type, h);
    if (!d) return SQL_INVALID_HANDLE;
    if (rec < 1) return SQL_ERROR;
    if (static_cast<size_t>(rec) > d->records.size()) return SQL_NO_DATA;
    const DiagRecord& r = d->records[static_cast<size_t>(rec) - 1];
    if (state) {
        std::u16string w = utf8ToUtf16(r.state);
        for (size_t i = 0; i < 5; i++) state[i] = i < w.size() ? w[i] : u' ';
        state[5] = 0;
    }
    if (native) *native = r.native;
    Diag sink;
    return writeTextChars(sink, r.message, msg, cap, len);
}
DRIVER_API SQLGetDiagFieldW(SQLSMALLINT type, SQLHANDLE h, SQLSMALLINT rec, SQLSMALLINT field, SQLPOINTER value, SQLSMALLINT cap, SQLSMALLINT* len) {
    if (!h) return SQL_INVALID_HANDLE;
    Diag* d = diagFor(type, h);
    if (!d) return SQL_INVALID_HANDLE;
    Diag sink;
    switch (field) {
        case SQL_DIAG_NUMBER:
            if (value) *static_cast<SQLINTEGER*>(value) = static_cast<SQLINTEGER>(d->records.size());
            return SQL_SUCCESS;
        case SQL_DIAG_CURSOR_ROW_COUNT: case SQL_DIAG_ROW_COUNT:
            if (value) *static_cast<SQLLEN*>(value) = -1;
            return SQL_SUCCESS;
        case SQL_DIAG_DYNAMIC_FUNCTION: return writeTextBytes(sink, "SELECT CURSOR", value, cap, len);
        case SQL_DIAG_DYNAMIC_FUNCTION_CODE:
            if (value) *static_cast<SQLINTEGER*>(value) = SQL_DIAG_SELECT_CURSOR;
            return SQL_SUCCESS;
        default: break;
    }
    if (rec < 1) return SQL_ERROR;
    if (static_cast<size_t>(rec) > d->records.size()) return SQL_NO_DATA;
    const DiagRecord& r = d->records[static_cast<size_t>(rec) - 1];
    switch (field) {
        case SQL_DIAG_SQLSTATE: return writeTextBytes(sink, r.state, value, cap, len);
        case SQL_DIAG_MESSAGE_TEXT: return writeTextBytes(sink, r.message, value, cap, len);
        case SQL_DIAG_NATIVE:
            if (value) *static_cast<SQLINTEGER*>(value) = r.native;
            return SQL_SUCCESS;
        case SQL_DIAG_CLASS_ORIGIN: return writeTextBytes(sink, r.state.rfind("IM", 0) == 0 ? "ODBC 3.0" : "ISO 9075", value, cap, len);
        case SQL_DIAG_SUBCLASS_ORIGIN: return writeTextBytes(sink, "ODBC 3.0", value, cap, len);
        case SQL_DIAG_CONNECTION_NAME: case SQL_DIAG_SERVER_NAME: return writeTextBytes(sink, "", value, cap, len);
        case SQL_DIAG_ROW_NUMBER:
            if (value) *static_cast<SQLLEN*>(value) = SQL_ROW_NUMBER_UNKNOWN;
            return SQL_SUCCESS;
        case SQL_DIAG_COLUMN_NUMBER:
            if (value) *static_cast<SQLINTEGER*>(value) = SQL_COLUMN_NUMBER_UNKNOWN;
            return SQL_SUCCESS;
        default: return SQL_ERROR;
    }
}
