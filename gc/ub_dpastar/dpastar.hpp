#pragma once

#include "gc/lib/instance.hpp"
#include "gc/lib/solution.hpp"

namespace gc
{

struct DPAStarData
{
    const Instance& ins;
    Solution& sol_best;
    Info info = Info();
};

void sol_dpastar_1(DPAStarData d);
void sol_dpastar_2(DPAStarData d);
void sol_dpastar(DPAStarData d);

}

