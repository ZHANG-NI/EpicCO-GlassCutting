#pragma once

#include "gc/lib/instance.hpp"
#include "gc/lib/solution.hpp"

namespace gc
{

struct DFSData
{
    const Instance& ins;
    Solution& sol_best;
    Info info = Info();
};

void sol_dfs(DFSData d);

}

