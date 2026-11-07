// Constrained read-only SQL. Anything outside this grammar is rejected, never approximated:
//   SELECT [TOP n] (* | item[, item]*) FROM [[cat.]schema.]table [[AS] alias]
//   [WHERE pred [AND pred]*] [ORDER BY col [ASC|DESC][, ...]] [LIMIT n]
#pragma once
#include <optional>

#include "common.h"

namespace strata {

struct Operand {
    bool isParam = false;
    int param = -1;
    Value literal;
};
enum class CmpOp { Eq, Ne, Lt, Le, Gt, Ge, IsNull, IsNotNull, In };
struct Predicate {
    std::string column;
    CmpOp op;
    std::vector<Operand> operands;
};
struct SelectItem {
    std::string column;
    std::string alias;
};
struct OrderItem {
    std::string column;
    bool desc;
};
struct Query {
    std::string table;
    bool star = false;
    std::vector<SelectItem> items;
    std::vector<Predicate> where;
    bool constFalse = false;  // WHERE 1=0: schema probe, must not touch the source
    std::optional<long long> limit;
    std::vector<OrderItem> order;
    int paramCount = 0;
};

Query parseSql(std::string_view sql);

}  // namespace strata
