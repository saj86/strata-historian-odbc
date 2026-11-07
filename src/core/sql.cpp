#include "sql.h"

#include <cctype>
#include <set>

#include "catalog.h"

namespace strata {
namespace {

enum class T { End, Ident, QIdent, Str, Num, Param, Sym, TsLit };
struct Tok {
    T t = T::End;
    std::string s;
    size_t pos = 0;
};

[[noreturn]] void syntax(const std::string& m) { throw Error("42000", "SQL syntax error: " + m); }
[[noreturn]] void unsupported(const std::string& m) {
    throw Error("0A000", "Unsupported SQL: " + m + " See docs/sql-contract.md for the supported subset.");
}

std::vector<Tok> lex(std::string_view s) {
    std::vector<Tok> out;
    size_t i = 0;
    auto isIdStart = [](unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; };
    auto isId = [&](unsigned char c) { return isIdStart(c) || std::isdigit(c); };
    auto readString = [&](size_t& p) {  // p sits on the opening quote
        std::string v;
        p++;
        for (;;) {
            if (p >= s.size()) syntax("unterminated string literal");
            if (s[p] == '\'') {
                if (p + 1 < s.size() && s[p + 1] == '\'') { v += '\''; p += 2; continue; }
                p++;
                return v;
            }
            v += s[p++];
        }
    };
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t st = i;
        if (std::isspace(c)) { i++; continue; }
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') unsupported("SQL comments are not accepted.");
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') unsupported("SQL comments are not accepted.");
        if ((c == 'N' || c == 'n') && i + 1 < s.size() && s[i + 1] == '\'') {
            i++;
            std::string v = readString(i);
            out.push_back({T::Str, v, st});
            continue;
        }
        if (isIdStart(c)) {
            while (i < s.size() && isId(static_cast<unsigned char>(s[i]))) i++;
            out.push_back({T::Ident, std::string(s.substr(st, i - st)), st});
            continue;
        }
        if (std::isdigit(c) || (c == '-' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            i++;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E'))))
                i++;
            out.push_back({T::Num, std::string(s.substr(st, i - st)), st});
            continue;
        }
        switch (c) {
            case '"': case '[': case '`': {
                char close = c == '[' ? ']' : static_cast<char>(c);
                std::string v;
                i++;
                for (;;) {
                    if (i >= s.size()) syntax("unterminated quoted identifier");
                    if (s[i] == close) {
                        if (i + 1 < s.size() && s[i + 1] == close) { v += close; i += 2; continue; }
                        i++;
                        break;
                    }
                    v += s[i++];
                }
                out.push_back({T::QIdent, v, st});
                continue;
            }
            case '\'': {
                std::string v = readString(i);
                out.push_back({T::Str, v, st});
                continue;
            }
            case '?': i++; out.push_back({T::Param, "?", st}); continue;
            case '{': {  // ODBC date/time escapes only
                i++;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                size_t k = i;
                while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) i++;
                std::string kw = toUpper(s.substr(k, i - k));
                if (kw != "TS" && kw != "D") unsupported("ODBC escape {" + kw + " ...} is not supported.");
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                if (i >= s.size() || s[i] != '\'') syntax("expected a quoted value in ODBC escape");
                std::string v = readString(i);
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
                if (i >= s.size() || s[i] != '}') syntax("expected '}' closing ODBC escape");
                i++;
                out.push_back({T::TsLit, v, st});
                continue;
            }
            case '<':
                if (i + 1 < s.size() && (s[i + 1] == '=' || s[i + 1] == '>')) { out.push_back({T::Sym, std::string(s.substr(i, 2)), st}); i += 2; continue; }
                i++; out.push_back({T::Sym, "<", st}); continue;
            case '>':
                if (i + 1 < s.size() && s[i + 1] == '=') { out.push_back({T::Sym, ">=", st}); i += 2; continue; }
                i++; out.push_back({T::Sym, ">", st}); continue;
            case '!':
                if (i + 1 < s.size() && s[i + 1] == '=') { out.push_back({T::Sym, "<>", st}); i += 2; continue; }
                syntax("unexpected '!'");
            case '=': case '(': case ')': case ',': case '.': case '*': case ';':
                i++; out.push_back({T::Sym, std::string(1, static_cast<char>(c)), st}); continue;
            default:
                syntax(std::string("unexpected character '") + static_cast<char>(c) + "'");
        }
    }
    out.push_back({T::End, "", s.size()});
    return out;
}

class Parser {
public:
    explicit Parser(std::string_view sql) : toks_(lex(sql)) {}

    Query run() {
        Query q;
        rejectNonSelect();
        expectKw("SELECT");
        if (isKw("DISTINCT")) unsupported("SELECT DISTINCT is not implemented.");
        if (isKw("ALL")) i_++;
        if (acceptKw("TOP")) {
            bool paren = acceptSym("(");
            q.limit = parseCount();
            if (paren) expectSym(")");
        }
        parseSelectList(q);
        expectKw("FROM");
        parseFrom(q);
        if (acceptKw("WHERE")) parseWhere(q);
        if (isKw("GROUP") || isKw("HAVING")) unsupported("GROUP BY/HAVING are not implemented; use summaries in the model or the curated layer.");
        if (isKw("UNION") || isKw("INTERSECT") || isKw("EXCEPT")) unsupported("set operations are not implemented.");
        if (acceptKw("ORDER")) {
            expectKw("BY");
            do {
                OrderItem o{parseColumnRef(), false};
                if (acceptKw("DESC")) o.desc = true;
                else acceptKw("ASC");
                q.order.push_back(std::move(o));
            } while (acceptSym(","));
        }
        if (acceptKw("LIMIT")) {
            long long n = parseCount();
            if (q.limit) syntax("TOP and LIMIT cannot be combined");
            q.limit = n;
            if (isKw("OFFSET")) unsupported("OFFSET is not implemented.");
        }
        if (isKw("OFFSET") || isKw("FETCH")) unsupported("OFFSET/FETCH is not implemented.");
        acceptSym(";");
        if (cur().t != T::End) syntax("unexpected text near '" + cur().s + "'; only one statement is accepted");
        q.paramCount = params_;
        validate(q);
        return q;
    }

private:
    const Tok& cur() const { return toks_[i_]; }
    bool isKw(const char* kw) const { return cur().t == T::Ident && iequals(cur().s, kw); }
    bool acceptKw(const char* kw) { if (isKw(kw)) { i_++; return true; } return false; }
    void expectKw(const char* kw) { if (!acceptKw(kw)) syntax(std::string("expected ") + kw + " near '" + cur().s + "'"); }
    bool isSym(const char* s) const { return cur().t == T::Sym && cur().s == s; }
    bool acceptSym(const char* s) { if (isSym(s)) { i_++; return true; } return false; }
    void expectSym(const char* s) { if (!acceptSym(s)) syntax(std::string("expected '") + s + "' near '" + cur().s + "'"); }

    void rejectNonSelect() {
        static const std::set<std::string> blocked = {"INSERT", "UPDATE", "DELETE", "MERGE", "DROP", "CREATE", "ALTER", "TRUNCATE", "EXEC", "EXECUTE", "CALL", "GRANT", "REVOKE", "BEGIN", "COMMIT", "ROLLBACK", "SET", "REPLACE", "UPSERT"};
        if (cur().t == T::Ident && blocked.count(toUpper(cur().s)))
            throw Error("0A000", "This driver is read-only: " + toUpper(cur().s) + " statements are blocked.");
    }
    long long parseCount() {
        if (cur().t != T::Num || cur().s.find_first_not_of("0123456789") != std::string::npos) syntax("expected a non-negative integer row count");
        long long n = 0;
        try { n = std::stoll(cur().s); } catch (...) { syntax("row count out of range"); }
        i_++;
        return n;
    }
    std::string ident() {
        if (cur().t == T::QIdent || (cur().t == T::Ident && !isReserved(cur().s))) return toks_[i_++].s;
        syntax("expected an identifier near '" + cur().s + "'");
    }
    static bool isReserved(const std::string& s) {
        static const std::set<std::string> r = {"SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "ORDER", "BY", "GROUP", "HAVING", "AS", "ON", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "CROSS", "UNION", "LIMIT", "TOP", "IN", "IS", "NULL", "BETWEEN", "LIKE", "OFFSET", "FETCH", "ASC", "DESC", "DISTINCT", "INTERSECT", "EXCEPT", "CASE", "EXISTS"};
        return r.count(toUpper(s)) > 0;
    }
    std::string parseColumnRef() {
        std::string a = ident();
        while (acceptSym(".")) {
            a = ident();  // qualifier (table alias) is validated by name only
        }
        if (isSym("(")) unsupported("function '" + a + "' is not implemented.");
        return a;
    }
    void parseSelectList(Query& q) {
        if (acceptSym("*")) { q.star = true; return; }
        do {
            size_t save = i_;
            std::string first = ident();
            if (isSym("(")) { i_ = save; unsupported("function or aggregate '" + first + "(...)' is not implemented."); }
            std::string col = first;
            while (acceptSym(".")) {
                if (acceptSym("*")) { q.star = true; return; }
                col = ident();
            }
            std::string alias = col;
            if (acceptKw("AS")) alias = ident();
            else if (cur().t == T::QIdent || (cur().t == T::Ident && !isReserved(cur().s))) alias = ident();
            q.items.push_back({col, alias});
        } while (acceptSym(","));
    }
    void parseFrom(Query& q) {
        if (isSym("(")) unsupported("derived tables and subqueries are not implemented.");
        std::vector<std::string> parts{ident()};
        while (acceptSym(".")) parts.push_back(ident());
        if (parts.size() > 3) syntax("table name has too many parts");
        if (parts.size() == 3 && !iequals(parts[0], kCatalogName)) throw Error("42S02", "Unknown catalog '" + parts[0] + "'.");
        if (parts.size() >= 2 && !iequals(parts[parts.size() - 2], kSchemaName)) throw Error("42S02", "Unknown schema '" + parts[parts.size() - 2] + "'.");
        q.table = parts.back();
        if (acceptKw("AS")) ident();
        else if (cur().t == T::QIdent || (cur().t == T::Ident && !isReserved(cur().s))) ident();
        if (isSym(",") || isKw("JOIN") || isKw("INNER") || isKw("LEFT") || isKw("RIGHT") || isKw("FULL") || isKw("CROSS")) unsupported("joins are not implemented; relate Points and RecordedValues in the model.");
    }
    Operand parseOperand() {
        Operand o;
        const Tok& t = cur();
        switch (t.t) {
            case T::Param:
                o.isParam = true;
                o.param = params_++;
                break;
            case T::Str:
                o.literal = t.s;
                break;
            case T::TsLit: {
                Ticks v;
                if (!parseUtc(t.s, v)) throw Error("22007", "Invalid date/time literal '" + t.s + "'.");
                o.literal = v;
                break;
            }
            case T::Num: {
                char* end = nullptr;
                double d = std::strtod(t.s.c_str(), &end);
                if (end != t.s.c_str() + t.s.size()) syntax("bad number '" + t.s + "'");
                o.literal = d;
                break;
            }
            case T::Ident:
                if (iequals(t.s, "TRUE")) { o.literal = true; break; }
                if (iequals(t.s, "FALSE")) { o.literal = false; break; }
                if (iequals(t.s, "NULL")) syntax("use IS NULL / IS NOT NULL to test for NULL");
                unsupported("expressions other than literals and ? are not implemented on the right of a predicate.");
            default:
                syntax("expected a literal or '?' near '" + t.s + "'");
        }
        i_++;
        return o;
    }
    void parseWhere(Query& q) {
        int parens = 0;
        for (;;) {
            while (acceptSym("(")) parens++;
            parsePredicate(q);
            while (parens > 0 && acceptSym(")")) parens--;
            if (acceptKw("AND")) continue;
            if (isKw("OR")) unsupported("OR is not implemented; split the read or use IN on PointId.");
            break;
        }
        if (parens != 0) syntax("unbalanced parentheses in WHERE");
    }
    void parsePredicate(Query& q) {
        if (isKw("NOT")) unsupported("NOT is not implemented.");
        if (cur().t == T::Num) {  // constant probes: 1=0, 0=1, 1=1
            std::string a = toks_[i_++].s;
            expectSym("=");
            if (cur().t != T::Num) syntax("expected a number");
            std::string b = toks_[i_++].s;
            if (a != b) q.constFalse = true;
            return;
        }
        if (cur().t == T::Param || cur().t == T::Str || cur().t == T::TsLit) unsupported("the column must be on the left of a predicate.");
        Predicate p;
        p.column = parseColumnRef();
        if (acceptKw("IS")) {
            bool neg = acceptKw("NOT");
            expectKw("NULL");
            p.op = neg ? CmpOp::IsNotNull : CmpOp::IsNull;
        } else if (acceptKw("IN")) {
            p.op = CmpOp::In;
            expectSym("(");
            if (isKw("SELECT")) unsupported("subqueries are not implemented.");
            do p.operands.push_back(parseOperand()); while (acceptSym(","));
            expectSym(")");
        } else if (acceptKw("BETWEEN")) {
            Predicate lo{p.column, CmpOp::Ge, {parseOperand()}};
            expectKw("AND");
            p.op = CmpOp::Le;
            p.operands.push_back(parseOperand());
            q.where.push_back(std::move(lo));
        } else if (isKw("LIKE") || isKw("NOT")) {
            unsupported("LIKE/NOT predicates are not implemented; filter on TagName = value, or in the model.");
        } else {
            if (cur().t != T::Sym) syntax("expected a comparison near '" + cur().s + "'");
            std::string op = toks_[i_++].s;
            if (op == "=") p.op = CmpOp::Eq;
            else if (op == "<>") p.op = CmpOp::Ne;
            else if (op == "<") p.op = CmpOp::Lt;
            else if (op == "<=") p.op = CmpOp::Le;
            else if (op == ">") p.op = CmpOp::Gt;
            else if (op == ">=") p.op = CmpOp::Ge;
            else syntax("expected a comparison operator near '" + op + "'");
            p.operands.push_back(parseOperand());
        }
        q.where.push_back(std::move(p));
    }
    void validate(const Query& q) {
        const TableDef* t = findTable(q.table);
        if (!t) throw Error("42S02", "Unknown table '" + q.table + "'.");
        auto need = [&](const std::string& c) {
            if (t->index(c) < 0) throw Error("42S22", "Unknown column '" + c + "' in " + t->name + ".");
        };
        for (auto& i : q.items) need(i.column);
        for (auto& p : q.where) need(p.column);
        for (auto& o : q.order) need(o.column);
    }
    std::vector<Tok> toks_;
    size_t i_ = 0;
    int params_ = 0;
};
}  // namespace

Query parseSql(std::string_view sql) { return Parser(sql).run(); }

}  // namespace strata
