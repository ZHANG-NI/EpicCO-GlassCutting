// host_main.cpp - interface of the program (this file is LOCKED: it must stay byte-identical).
//   prog <instance prefix> <time limit s> <seed> <solution csv> [<algorithm string> ...]
// The instance is <prefix>_batch.csv, <prefix>_defects.csv and global_param.csv of the same directory. Without
// algorithm strings the default list below runs, one thread per entry, all sharing the best solution; with ONE
// algorithm string that search runs in the main thread. The search has no random number: the seed is accepted and
// not used. The solution file is rewritten at every improvement. One line is printed at the end:
//   RESULT items <n> waste <w> plates <p> solutions <k> time_best <s>      (or: RESULT none)
// The function run() maps an algorithm string to the search drivers.
#include "gc/ub_dfs/dfs.hpp"
#include "gc/ub_astar/astar.hpp"
#include "gc/ub_dpastar/dpastar.hpp"
#include "gc/ub_beamsearch/beamsearch.hpp"
#include "gc/ub_beamstacksearch/beamstacksearch.hpp"
#include "gc/ub_mbastar/mbastar.hpp"

#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace gc;

void run(std::string algo, const Instance& ins, Solution& sol, Info info)
{
    std::stringstream ss(algo);
    std::istream_iterator<std::string> begin(ss);
    std::istream_iterator<std::string> end;
    std::vector<std::string> vstrings(begin, end);

    std::map<std::string, std::string> args;
    for (auto it=std::next(vstrings.begin());
            it!=vstrings.end() && std::next(it)!=vstrings.end();
            it=std::next(std::next(it)))
        args[*it] = *std::next(it);

    if (vstrings[0] == "DFS") {
        sol_dfs({
                .ins           = ins,
                .sol_best      = sol,
                .info          = info
                });
    } else if (vstrings[0] == "A*") {
        sol_astar({
                .ins           = ins,
                .sol_best      = sol,
                .info          = info
                });
    } else if (vstrings[0] == "MBA*") {
        auto it = args.find("f");
        double f = (it != args.end())? std::stod(it->second): 1.5;
        it = args.find("c");
        int c = (it != args.end())? std::stoi(it->second): 1;
        sol_mbastar({
                .ins           = ins,
                .growth_factor = f,
                .criterion_id  = c,
                .sol_best      = sol,
                .info          = info
                });
    } else if (vstrings[0] == "BS") {
        auto it = args.find("f");
        double f = (it != args.end())? std::stod(it->second): 1.5;
        it = args.find("c");
        int c = (it != args.end())? std::stoi(it->second): 1;
        sol_beamsearch({
                .ins           = ins,
                .growth_factor = f,
                .criterion_id  = c,
                .sol_best      = sol,
                .info          = info
                });
    } else if (vstrings[0] == "BSS") {
        auto it = args.find("c");
        int c = (it != args.end())? std::stoi(it->second): 1;
        it = args.find("s");
        int s = (it != args.end())? std::stoi(it->second): 1024;
        sol_beamstacksearch({
                .ins           = ins,
                .criterion_id  = c,
                .size          = s,
                .sol_best      = sol,
                .info          = info
                });
    } else if (vstrings[0] == "DPA*") {
        auto it = args.find("s");
        double s = (it != args.end())? std::stod(it->second): -1;
        if (ins.stack_number() == 1) {
            sol_dpastar_1({
                    .ins           = ins,
                    .sol_best      = sol,
                    .info          = info
                    });
        } else if (ins.stack_number() == 2) {
            sol_dpastar_2({
                    .ins           = ins,
                    .sol_best      = sol,
                    .info          = info
                    });
        } else if (ins.state_number() <= s) {
            sol_dpastar({
                    .ins           = ins,
                    .sol_best      = sol,
                    .info          = info
                    });
        }
    } else {
        VER(info, "WARNING: unknown algorithm \"" << vstrings[0] << "\"" << std::endl);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 5) {
        std::fprintf(stderr, "usage: prog <instance prefix> <time limit s> <seed> <solution csv> [<algorithm string> ...]\n");
        return 2;
    }
    std::string prefix = argv[1];
    double time_limit = std::atof(argv[2]);
    std::string certificate_path = argv[4];
    std::vector<std::string> algorithms;
    for (int i = 5; i < argc; ++i)
        algorithms.push_back(argv[i]);
    if (algorithms.empty())
        algorithms = {
            "MBA* c 0 f 1.33",
            "MBA* c 1 f 1.33",
            "MBA* c 0 f 1.5",
            "MBA* c 1 f 1.5",
            "DPA* s -1",
        };
    std::string dir = ".";
    std::size_t pos = prefix.find_last_of('/');
    if (pos != std::string::npos)
        dir = prefix.substr(0, pos);

    Instance ins(prefix + "_batch.csv", prefix + "_defects.csv", dir + "/global_param.csv");
    if (ins.item_number() == 0) {
        std::printf("RESULT none\n");
        return 3;
    }

    Info info = Info()
        .set_verbose(false)
        .set_timelimit(time_limit)
        .set_certfile(certificate_path)
        .set_outputfile("")
        .set_onlywriteattheend(false)
        .set_logfile("")
        .set_log2stderr(false)
        .set_loglevelmax(999)
        ;

    Solution sol(ins);

    if (algorithms.size() == 1) {
        std::string s = algorithms[0];
        std::replace(s.begin(), s.end(), ' ', '-');
        run(algorithms[0], ins, sol, Info(info, true, s));
    } else {
        std::vector<std::thread> threads;
        for (Cpt i=0; i<(Cpt)algorithms.size(); ++i) {
            std::string s = algorithms[i];
            std::replace(s.begin(), s.end(), ' ', '-');
            threads.push_back(std::thread(run,
                        algorithms[i], std::ref(ins), std::ref(sol), Info(info, true, s)));
        }
        for (std::thread& thread: threads)
            thread.join();
    }

    algorithm_end(sol, info);
    if (!sol.is_complete()) {
        std::printf("RESULT none\n");
        return 0;
    }
    int k = info.output->sol_number;
    double t_best = 0.0;
    std::string key = "Solution" + std::to_string(k);
    if (info.output->j.contains(key) && info.output->j[key].contains("Time"))
        t_best = info.output->j[key]["Time"].get<double>();
    std::printf("RESULT items %d waste %lld plates %d solutions %d time_best %.2f\n",
            (int)ins.item_number(), (long long)sol.waste(), (int)sol.plate_number(), k, t_best);
    return 0;
}
