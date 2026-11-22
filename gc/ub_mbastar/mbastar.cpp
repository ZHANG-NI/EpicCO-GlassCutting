#include "gc/ub_mbastar/mbastar.hpp"

#include <set>
#include <algorithm>
#include <limits>

using namespace gc;

/**
 * A queued partial plan is kept lazily: a reference-counted parent and the
 * insertion that leads from it to the plan. The plan itself is built (copy
 * of the parent + add_item) only when it is popped, so the many children that
 * are evicted or dominated before being expanded never cost a copy.
 */
struct Parent
{
    Solution* s;
    Cpt refs;
};

struct QEntry
{
    double v;
    Parent* par;
    Insertion i;
    uint64_t key;
};

struct QEntryCompare
{
    bool operator()(const QEntry& a, const QEntry& b) const { return a.v < b.v; }
};

namespace
{

typedef std::multiset<QEntry, QEntryCompare> Queue;
typedef Queue::iterator QueueIt;

/** Data kept from one bounded search to the next (reused buffers). */
struct MBAStarState
{
    // Solutions released by the search are kept and reused (their vectors
    // keep their capacity), instead of being freed and allocated again.
    std::vector<Solution*> pool;
    std::vector<Parent*> par_pool;
    std::vector<uint64_t> h_keys;
    std::vector<Cpt> h_val;
    std::vector<std::vector<std::pair<QueueIt, Front>>> g_vec;
    std::vector<Cpt> g_free;
    Cpt node_number = 0;

    // Level-synchronous selection (iterative beam): one bounded queue of
    // size q per number of placed items, expanded level by level, instead
    // of one queue where plans of all depths compete under the guide. Used
    // until a search completes without pruning anything; then best-first.
    bool beam_mode = true;
};

/**
 * One bounded best-first search (queue of at most q_sizemax partial
 * solutions) from the partial solution 'start'. Returns false iff it was
 * stopped by the time limit.
 */
bool mbastar_iteration(MBAStarData& d, MBAStarState& st, const SolutionCompare& comp,
        const Solution& start, Cpt q_sizemax, const char* tag, bool* pruned_out = nullptr)
{
    const GlobalParam& gp = d.ins.global_param();
    std::vector<Solution*>& pool = st.pool;
    auto release_sol = [&pool](Solution* s) { pool.push_back(s); };
    std::vector<Parent*>& par_pool = st.par_pool;
    auto new_parent = [&par_pool](Solution* s) {
        Parent* p;
        if (par_pool.empty()) {
            p = new Parent;
        } else {
            p = par_pool.back();
            par_pool.pop_back();
        }
        p->s = s;
        p->refs = 0;
        return p;
    };
    // Release a queue entry: drop its reference to its parent.
    auto release = [&](const QEntry& e) {
        if (--e.par->refs == 0) {
            release_sol(e.par->s);
            par_pool.push_back(e.par);
        }
    };
    auto acquire = [&pool](const Solution& from) {
        if (pool.empty())
            return new Solution(from);
        Solution* s = pool.back();
        pool.pop_back();
        *s = from;
        return s;
    };
    std::vector<uint64_t>& h_keys = st.h_keys;
    std::vector<Cpt>& h_val = st.h_val;
    std::vector<std::vector<std::pair<QueueIt, Front>>>& g_vec = st.g_vec;
    std::vector<Cpt>& g_free = st.g_free;
    Cpt& node_number = st.node_number;

    const bool beam_mode = st.beam_mode;
    std::vector<Queue> lq(d.ins.item_number() + 1);
    Cpt lvl = 0;
    bool pruned = false;
    // Number of nodes of level 'lvl' already expanded that have a queued
    // child. In beam mode, the only children that go to the level being
    // expanded are defect blocks (no item placed); the level keeps at most
    // q live nodes in total, expanded ones included, so that blocks compete
    // for its q slots instead of filling the room left by the expanded
    // nodes. (Expanded nodes without any child do not hold a slot: if the
    // best nodes of a level are dead ends, blocks may replace them.)
    Cpt lvl_popped = 0;
    Cpt lvl_of_popped = -1;
    // For each set of placed items, the queued solutions that contain it.
    // Open-addressing table (key = hash of the set of placed items, which
    // is already well mixed) -> index of a group vector; the group
    // vectors are recycled (no allocation once warm).
    Cpt h_size = 16;
    // Start small (it grows with the number of groups): a huge q_sizemax
    // (exhaustive searches, growing tail queues) must not cost a huge table.
    while (h_size < 2 * (q_sizemax + 16) && h_size < (1 << 16))
        h_size *= 2;
    uint64_t h_mask = h_size - 1;
    Cpt h_count = 0;
    h_keys.assign(h_size, 0);
    h_val.assign(h_size, -1);
    g_free.clear();
    for (Cpt k=0; k<(Cpt)g_vec.size(); ++k)
        g_free.push_back(k);
    auto h_find = [&](uint64_t key) {
        uint64_t i = key & h_mask;
        while (h_val[i] != -1 && h_keys[i] != key)
            i = (i + 1) & h_mask;
        return i;
    };
    auto h_grow = [&]() {
        std::vector<uint64_t> ok = h_keys;
        std::vector<Cpt> ov = h_val;
        h_size *= 2;
        h_mask = h_size - 1;
        h_keys.assign(h_size, 0);
        h_val.assign(h_size, -1);
        for (size_t k=0; k<ok.size(); ++k) {
            if (ov[k] == -1)
                continue;
            uint64_t i = ok[k] & h_mask;
            while (h_val[i] != -1)
                i = (i + 1) & h_mask;
            h_keys[i] = ok[k];
            h_val[i] = ov[k];
        }
    };
    auto h_erase = [&](uint64_t i) {
        h_count--;
        g_free.push_back(h_val[i]);
        // Backward-shift deletion (linear probing).
        uint64_t j = i;
        for (;;) {
            h_val[i] = -1;
            for (;;) {
                j = (j + 1) & h_mask;
                if (h_val[j] == -1)
                    return;
                uint64_t k = h_keys[j] & h_mask;
                bool in_between = (i <= j)? (i < k && k <= j): (i < k || k <= j);
                if (!in_between)
                    break;
            }
            h_keys[i] = h_keys[j];
            h_val[i] = h_val[j];
            i = j;
        }
    };
    auto group_erase = [&](uint64_t key, QueueIt it) {
        uint64_t slot = h_find(key);
        if (h_val[slot] == -1)
            return;
        std::vector<std::pair<QueueIt, Front>>& v = g_vec[h_val[slot]];
        for (Cpt k=0; k<(Cpt)v.size(); ++k) {
            if (v[k].first == it) {
                if (k + 1 != (Cpt)v.size()) v[k] = v.back();
                v.pop_back();
                break;
            }
        }
        if (v.empty())
            h_erase(slot);
    };
    {
        // The root is the only entry without an insertion (j1 = j2 = -2).
        Insertion root_ins;
        root_ins.j1 = -2;
        root_ins.j2 = -2;
        Solution* root = acquire(start);
        Parent* rp = new_parent(root);
        rp->refs = 1;
        lq[(beam_mode)? root->item_number(): 0].insert({0.0, rp, root_ins, root->stack_key()});
    }

    for (;;) {
        while (lvl <= d.ins.item_number() && lq[lvl].empty())
            lvl++;
        if (lvl > d.ins.item_number())
            break;
        Queue& q = lq[lvl];
        if (lvl != lvl_of_popped) {
            lvl_of_popped = lvl;
            lvl_popped = 0;
        }
        if (!d.info.check_time()) {
            // Time is up: the process ends with the search; do not spend
            // time freeing the queued solutions.
            for (Queue& ql: lq)
                ql.clear();
            return false;
        }
        node_number++;

        Solution* sol_cur_ptr;
        {
            const QEntry e = *q.begin();
            group_erase(e.key, q.begin());
            q.erase(q.begin());
            if (e.i.j1 == -2) {
                // Root.
                sol_cur_ptr = e.par->s;
                par_pool.push_back(e.par);
            } else {
                sol_cur_ptr = acquire(*e.par->s);
                sol_cur_ptr->add_item(e.i, d.info);
                release(e);
            }
        }
        const Solution& sol_cur = *sol_cur_ptr;
        Parent* par_cur = nullptr;

        LOG_FOLD(d.info, "solution" << std::endl << sol_cur);

        // Children that would be rejected below (guide not better than the
        // worst one of a full target queue, waste not better than the
        // best) are skipped while the insertions are generated.
        {
            Solution::ChildBound& cb = Solution::child_bound;
            cb.on = true;
            cb.comp = &comp;
            cb.skipped = 0;
            cb.has_best = d.sol_best.is_complete();
            cb.best_waste = (cb.has_best)? d.sol_best.waste(): 0;
            Solution::diversity.set(sol_cur);
            ItemId n = sol_cur.item_number();
            for (int t=1; t<=2; ++t) {
                bool full = false;
                double wv = 0;
                Cpt l = (beam_mode)? n + t: 0;
                if (l <= d.ins.item_number() && (Cpt)lq[l].size() >= q_sizemax) {
                    full = true;
                    wv = std::prev(lq[l].end())->v;
                }
                if (t == 1) { cb.full1 = full; cb.w1 = wv; } else { cb.full2 = full; cb.w2 = wv; }
            }
        }
        std::vector<Insertion> insertions = sol_cur.all_valid_insertions(d.info, true);
        if (Solution::child_bound.skipped > 0)
            pruned = true;
        // The symmetry rules assume that the symmetric twin of a node is
        // explored; in a bounded queue it may have been dropped, and a
        // node whose every extension is cut by them would be a dead end.
        // Then extend it without the symmetry rules. (Not when children
        // were skipped by the bound: the node is not a dead end.)
        if (insertions.empty() && Solution::child_bound.skipped == 0) {
            Solution::child_bound.on = false;
            insertions = sol_cur.all_valid_insertions(d.info, false);
        }
        // A node that cannot place any item where it stands (and had no
        // item insertion skipped by the bound) escapes only through defect
        // blocks: they are not capped (see below).
        bool stuck = (Solution::child_bound.skipped == 0);
        for (const Insertion& i: insertions) {
            if (!stuck)
                break;
            if (i.j1 != -1 || i.j2 != -1)
                stuck = false;
        }
        Solution::child_bound.on = false;
        for (const Insertion& i: insertions) {
            LOG(d.info, i << std::endl);
            ItemId c_n;
            Area c_item_area, c_area, c_waste;
            sol_cur.child_measures(i, c_n, c_item_area, c_area, c_waste);
            if (d.sol_best.is_complete() && c_waste >= d.sol_best.waste()) {
                LOG(d.info, " waste cut ×" << std::endl);
                continue;
            }
            double v_tmp = 0;
            Queue& cq = lq[(beam_mode)? c_n: 0];
            // Room of the target queue.
            Cpt cap = (beam_mode && (Cpt)c_n == lvl && !stuck)? q_sizemax - lvl_popped: q_sizemax;
            if (c_n != d.ins.item_number()) {
                v_tmp = comp.value(d.ins, c_n, c_item_area, c_area, c_waste)
                    * Solution::diversity.factor(sol_cur, i.j1, i.j2);
                if (cap <= 0 || ((Cpt)cq.size() >= cap && !(v_tmp < std::prev(cq.end())->v))) {
                    pruned = true;
                    continue;
                }
            }
            if (c_n == d.ins.item_number()) {
                Solution sol_tmp(sol_cur);
                sol_tmp.add_item(i, d.info);
                if (!d.sol_best.is_complete() || sol_tmp.waste() < d.sol_best.waste()) {
                    std::stringstream ss;
                    ss << "MBA* " << d.criterion_id << " " << d.growth_factor << " " << tag << " " << q_sizemax;
                    d.sol_best.update(sol_tmp, d.info, ss);
                }
                continue;
            }

            // Dominance among queued solutions with the same items.
            uint64_t key = sol_cur.child_stack_key(i);
            Front f = sol_cur.child_front(i);
            if (2 * (h_count + 1) > h_size)
                h_grow();
            uint64_t slot = h_find(key);
            if (h_val[slot] == -1) {
                Cpt g;
                if (g_free.empty()) {
                    g = g_vec.size();
                    g_vec.emplace_back();
                } else {
                    g = g_free.back();
                    g_free.pop_back();
                }
                g_vec[g].clear();
                h_keys[slot] = key;
                h_val[slot] = g;
                h_count++;
            }
            std::vector<std::pair<QueueIt, Front>>& v = g_vec[h_val[slot]];
            bool dominated = false;
            for (Cpt k=0; k<(Cpt)v.size();) {
                if (Solution::dominates(v[k].second, f, gp)) {
                    dominated = true;
                    break;
                }
                if (Solution::dominates(f, v[k].second, gp)) {
                    release(*v[k].first);
                    cq.erase(v[k].first);
                    if (k + 1 != (Cpt)v.size()) v[k] = v.back();
                    v.pop_back();
                } else {
                    ++k;
                }
            }
            if (dominated) {
                if (v.empty())
                    h_erase(slot);
                continue;
            }

            if (par_cur == nullptr) {
                // One reference held by this expansion itself, so that
                // evicting the children again does not free the node
                // while it is being expanded.
                par_cur = new_parent(sol_cur_ptr);
                par_cur->refs = 1;
            }
            par_cur->refs++;
            QueueIt it = cq.insert({v_tmp, par_cur, i, key});
            v.push_back({it, f});
            if ((Cpt)cq.size() > cap) {
                pruned = true;
                QueueIt last = std::prev(cq.end());
                const QEntry le = *last;
                group_erase(le.key, last);
                cq.erase(last);
                release(le);
            }
        }
        // The node is kept as long as one of its children is queued.
        if (par_cur == nullptr) {
            release_sol(sol_cur_ptr);
        } else if (--par_cur->refs == 0) {
            // Every queued child was evicted or dominated again.
            release_sol(sol_cur_ptr);
            par_pool.push_back(par_cur);
        }
        if (par_cur != nullptr && lvl == lvl_of_popped)
            lvl_popped++;
    }
    if (pruned_out != nullptr)
        *pruned_out = pruned;
    return true;
}

/**
 * Partial solution made of the first k plates of the complete solution 'sol'
 * (the state just before the insertion that opens plate k+1), rebuilt by
 * replaying its insertions.
 */
Solution prefix_plates(const Solution& sol, PlateId k, Info& info)
{
    Solution res(sol.instance());
    PlateId p = 0;
    for (const Insertion& i: sol.insertions()) {
        if (i.df == -1) {
            if (p == k)
                break;
            p++;
        }
        res.add_item(i, info);
    }
    return res;
}

}

void gc::sol_mbastar(MBAStarData d)
{
    LOG_FOLD_START(d.info, "MBA*" << std::endl);
    MBAStarState st;
    SolutionCompare comp(d.criterion_id);
    Solution::diversity.init(d.ins);

    double dur_prev = -1;
    double dur_last = -1;
    Cpt q_last = 0;
    bool endgame = false;
    for (Cpt q_sizemax=2; q_sizemax < 100000000; q_sizemax=q_sizemax*d.growth_factor) {
        if (q_sizemax == (Cpt)(q_sizemax*d.growth_factor))
            q_sizemax++;

        // A search from scratch completes its plans only at its end: if the
        // next one cannot finish before the time limit, its time is better
        // spent re-optimizing the tail of the best solution.
        if (d.sol_best.is_complete() && dur_last > 0.05 && dur_prev > 0) {
            double ratio = std::min(2.5, std::max(1.3, dur_last / dur_prev));
            if (d.info.elapsed_time() + ratio * dur_last > d.info.timelimit) {
                endgame = true;
                break;
            }
        }

        double t_start = d.info.elapsed_time();
        bool pruned = true;
        if (!mbastar_iteration(d, st, comp, Solution(d.ins), q_sizemax, "root", &pruned))
            break;
        dur_prev = dur_last;
        dur_last = d.info.elapsed_time() - t_start;
        q_last = q_sizemax;
        std::stringstream ss;
        ss << "MBA* " << d.criterion_id << " " << d.growth_factor;
        PUT(d.info, ss.str(), "QueueMaxSize", q_sizemax);
        if (st.beam_mode && !pruned) {
            // The level-by-level search was complete: switch to best-first
            // restarts (whose dominance applies in another order).
            st.beam_mode = false;
            q_sizemax = 1;
            dur_prev = -1;
            dur_last = -1;
        }
    }

    if (endgame) {
        // Fix the first k plates of the best solution and search the rest
        // again with a larger queue, from the last plate backwards. If the
        // best solution is improved, its first k plates are unchanged, so
        // going on with k-1 is consistent.
        Cpt q_tail = 2 * q_last;
        PlateId k = -1;
        while (d.info.check_time()) {
            if (k < 1 || k >= d.sol_best.plate_number()) {
                if (k != -1)
                    q_tail = std::min((Cpt)1000000000, (Cpt)(q_tail * d.growth_factor) + 1);
                k = d.sol_best.plate_number() - 1;
            }
            if (k < 1) {
                // A single plate: search from scratch again.
                if (!mbastar_iteration(d, st, comp, Solution(d.ins), q_tail, "root"))
                    break;
                q_tail = std::min((Cpt)1000000000, (Cpt)(q_tail * d.growth_factor) + 1);
                continue;
            }
            Solution start = prefix_plates(d.sol_best, k, d.info);
            if (!mbastar_iteration(d, st, comp, start, q_tail, "tail"))
                break;
            k--;
        }
    }

    std::stringstream ss;
    ss << "MBA* " << d.criterion_id << " " << d.growth_factor;
    PUT(d.info, ss.str(), "Nodes", st.node_number);
    LOG_FOLD_END(d.info, "");
}
