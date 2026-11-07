// Prints how a statement becomes a bounded upstream request, without contacting a source.
// usage: strata_explain "<sql>" [param ...]
#include <cstdio>

#include "../src/core/planner.h"

int main(int argc, char** argv) {
    using namespace strata;
    if (argc < 2) { std::fprintf(stderr, "usage: strata_explain \"<sql>\" [param ...]\n"); return 2; }
    try {
        Query q = parseSql(argv[1]);
        std::vector<Value> params;
        for (int i = 2; i < argc; i++) params.push_back(std::string(argv[i]));
        Limits lim;
        Plan p = buildPlan(q, params, lim);
        std::printf("table:    %s\nupstream: %s\n", p.table->name.c_str(), describeUpstream(p, lim).c_str());
        std::printf("columns: ");
        for (auto& c : p.columns) std::printf(" %s", c.name.c_str());
        std::printf("\nresidual predicates re-applied locally: %zu\n", p.residual.size());
        return 0;
    } catch (const Error& e) {
        std::printf("REJECTED %s: %s\n", e.sqlstate.c_str(), e.what());
        return 1;
    }
}
