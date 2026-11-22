#pragma once

#include "gc/lib/instance.hpp"
#include "gc/lib/solution.hpp"

namespace gc
{

struct AStarData
{
    const Instance& ins;
    Solution& sol_best;
    Info info;
};

void sol_astar(AStarData d);
Solution sol_astar(const Instance& ins, Info info = Info());

}

