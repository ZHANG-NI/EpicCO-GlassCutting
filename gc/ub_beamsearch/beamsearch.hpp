#pragma once

#include "gc/lib/instance.hpp"
#include "gc/lib/solution.hpp"

namespace gc
{

struct BeamSearchData
{
    const Instance& ins;
    double growth_factor;
    int criterion_id;
    Solution& sol_best;
    Info info = Info();
};

void sol_beamsearch(BeamSearchData d);

}

