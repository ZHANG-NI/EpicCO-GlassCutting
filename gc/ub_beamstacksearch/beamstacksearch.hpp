#pragma once

#include "gc/lib/instance.hpp"
#include "gc/lib/solution.hpp"

namespace gc
{

struct BeamStackSearchData
{
    const Instance& ins;
    int criterion_id;
    Cpt size;
    Solution& sol_best;
    Info info = Info();
};

void sol_beamstacksearch(BeamStackSearchData d);

}

