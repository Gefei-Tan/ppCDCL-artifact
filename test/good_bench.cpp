#include <iostream>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include "emp-tool/emp-tool.h"
#include "emp-dpf/emp-dpf.h"
#include "src/ppCDCL.h"

using namespace std;

FloramMPC<NetIO> *base = nullptr;
bool USE_SHORTEST_WL = false;
WatchBackendRequest WATCH_BACKEND_REQUEST = WatchBackendRequest::Auto;
constexpr int DEFAULT_BACKEND_THREADS = 4;

int parse_backend_threads(const std::string &value)
{
    char *end = nullptr;
    long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 2)
    {
        throw std::runtime_error("threads must be an integer of at least 2");
    }
    if (parsed > std::numeric_limits<int>::max())
    {
        throw std::runtime_error("threads is too large");
    }
    return static_cast<int>(parsed);
}

void init_backend(int party,
                  int port,
                  int depth = 25,
                  int size_of_element = 7,
                  int threads = DEFAULT_BACKEND_THREADS,
                  int round_key_size = 94208,
                  const char *addr = "127.0.0.1")
{
    NetIO **io = new NetIO *[threads];
    for (int i = 0; i < max(2, threads); i++)
    {
        io[i] = new NetIO((party - 1) ? addr : nullptr, port + i, true);
    }
    block delta;
    PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE)
        delta ^= makeBlock(0, 1);
    else
        delta ^= makeBlock(0, 3);

    base = new FloramMPC<NetIO>(party, threads, io, delta, depth);
    emp::backend = base;

    Integer lowmc_keys;
    base->random_sample(lowmc_keys, round_key_size);
    base->round_key = lowmc_keys.bits;
}

template <typename VState>
vector<Integer> test_check_clause(ClauseList &cl, VState &big_V,
                                  WatchList &wls, Clause &w,
                                  const Integer &clause_idx,
                                  const Integer &fresh_falsified_lit)
{
    cl.get_clause(clause_idx, w);
    vector<Integer> best_literals = {
        Integer(VAR_SIZE_BIT, 0, PUBLIC),
        Integer(VAR_SIZE_BIT, 0, PUBLIC)};
    if (!USE_SHORTEST_WL)
    {
        for (auto const &l : w.literals)
        {
            Bit is_nonzero = l != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit if_l_is_not_falsified = !big_V.is_literal_falsified(l);
            Bit if_l_is_not_fresh_falsified = !l.equal(fresh_falsified_lit);
            Bit is_valid_literal = is_nonzero & if_l_is_not_falsified & if_l_is_not_fresh_falsified;

            Bit is_first_zero = best_literals[0] == Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit is_second_zero = best_literals[1] == best_literals[0];
            best_literals[0] = If(is_first_zero & is_valid_literal, l, best_literals[0]);
            best_literals[1] = If(is_second_zero & is_valid_literal, l, best_literals[1]);
        }
        return best_literals;
    }

    Integer best_size(CLAUSE_IDX_SIZE_IN_WL, MAX_CLAUSE_IN_WL + 1, PUBLIC);
    Integer second_best_size(CLAUSE_IDX_SIZE_IN_WL, MAX_CLAUSE_IN_WL + 1, PUBLIC);
    for (auto const &l : w.literals)
    {
        Bit is_nonzero = l != Integer(VAR_SIZE_BIT, 0, PUBLIC);
        Bit if_l_is_not_falsified = !big_V.is_literal_falsified(l);
        Bit if_l_is_not_fresh_falsified = !l.equal(fresh_falsified_lit);
        Bit is_distinct = (l != best_literals[0]) & (l != best_literals[1]);
        Bit is_valid_literal = is_nonzero & if_l_is_not_falsified &
                               if_l_is_not_fresh_falsified & is_distinct;

        Integer cur_size = wls.watchlist_size(l);
        Bit better_best = is_valid_literal & (cur_size < best_size);
        Bit better_second = is_valid_literal & !better_best & (cur_size < second_best_size);

        Integer old_best_size = best_size;
        Integer old_best_literal = best_literals[0];

        best_size = If(better_best, cur_size, best_size);
        best_literals[0] = If(better_best, l, best_literals[0]);
        second_best_size = If(better_best, old_best_size,
                              If(better_second, cur_size, second_best_size));
        best_literals[1] = If(better_best, old_best_literal,
                              If(better_second, l, best_literals[1]));
    }
    Bit one_found = (best_literals[0] != Integer(VAR_SIZE_BIT, 0, PUBLIC)) &
                    (best_literals[1] == Integer(VAR_SIZE_BIT, 0, PUBLIC));
    best_literals[1] = If(one_found, best_literals[0], best_literals[1]);
    return best_literals;
}

template <typename VState>
void test_add_implication(VState &big_V,
                          vector<Integer> &big_U,
                          Integer &a_idx)
{
    Bit did_assign = big_V.try_set_unit_literal(
        a_idx, Integer(ANTE_SIZE_BIT, 1), Integer(DL_SIZE_BIT, 1));
    Integer val = If(a_idx >= Integer(VAR_SIZE_BIT, 0),
                     Integer(2, 1), Integer(2, 2));
    assign_vector(big_U, a_idx.abs(), val, did_assign);
}

void from_U(WatchList &wls, vector<Integer> &big_U, Integer &a_idx)
{
    Bit flag_found = false;
    for (size_t i = 0; i < VAR_NUM + 1; ++i)
    {
        Bit flag_1 = big_U[i] == Integer(2, 1);
        Bit flag_2 = big_U[i] == Integer(2, 2);
        Bit first_valid = (flag_1 | flag_2) & !flag_found;
        a_idx = If(first_valid & flag_1, Integer(VAR_SIZE_BIT, i),
                   If(first_valid & flag_2, Integer(VAR_SIZE_BIT, -static_cast<int>(i)), a_idx));
        big_U[i] = If(first_valid, Integer(2, 0), big_U[i]);
        flag_found = flag_found | flag_1 | flag_2;
    }
    auto wl_vec = wls.get_watchlist_clauses(-a_idx);
    (void)wl_vec;
}

int terminal_open()
{
    Bit if_sat(false, ALICE);
    Bit if_unsat(false, BOB);
    Bit if_failure(false, ALICE);
    Integer status(2, CDCL::TERMINAL_RUNNING, PUBLIC);
    status = If(if_sat, Integer(2, CDCL::TERMINAL_SAT, PUBLIC), status);
    status = If(if_unsat, Integer(2, CDCL::TERMINAL_UNSAT, PUBLIC), status);
    status = If(if_failure, Integer(2, CDCL::TERMINAL_FAILURE, PUBLIC), status);
    return reveal_32(status, true);
}

template <typename VState>
void decision(WatchList &wls,
              VState &big_V,
              vector<Integer> &activities,
              Integer &a_idx)
{
    Integer max_score(ACTIVITY_SIZE_BIT, 0);
    for (auto &i : activities)
    {
        max_score = If(max_score < i, i, max_score);
        Integer dummy(VAR_SIZE_BIT, 0);
        (void)dummy;
    }
    for (int i = 0; i < VAR_NUM + 1; i++)
    {
        Integer tmp = Integer(ASSIGNMENT_SIZE_BIT, 0);
        big_V.get_assignment(i, tmp);
    }

    big_V.set_decision(a_idx, Integer(DL_SIZE_BIT, 1));

    auto wl_vec = wls.get_watchlist_clauses(a_idx);
    (void)wl_vec;
}

struct BenchIndexedCandidateChoice
{
    Bit found;
    Integer var;
    Integer order;
};

Bit bench_mark_seen_touched_and_candidate(
    vector<Bit> &seen,
    vector<Bit> &touched,
    vector<Bit> &candidate_active,
    vector<Integer> &candidate_order,
    const Integer &idx,
    const Integer &order,
    const Bit &eligible,
    const Bit &at_current_level)
{
    Bit marked = Bit(false, PUBLIC);
    for (int i = 1; i < VAR_NUM + 1; ++i)
    {
        Bit match = idx == Integer(idx.size(), i, PUBLIC);
        Bit mark_here = eligible & match & !seen[i];
        Bit candidate_here = mark_here & at_current_level;
        seen[i] = seen[i] ^ mark_here;
        touched[i] = touched[i] | mark_here;
        candidate_active[i] = candidate_active[i] | candidate_here;
        candidate_order[i] = If(candidate_here, order, candidate_order[i]);
        marked = marked ^ mark_here;
    }
    return marked;
}

BenchIndexedCandidateChoice bench_select_indexed_candidate(
    const vector<Bit> &candidate_active,
    const vector<Integer> &candidate_order,
    const Bit &flag)
{
    BenchIndexedCandidateChoice choice{
        Bit(false, PUBLIC),
        Integer(VAR_SIZE_BIT, 0, PUBLIC),
        Integer(candidate_order[0].size(), 0, PUBLIC)};
    for (int i = 1; i < VAR_NUM + 1; ++i)
    {
        Bit valid = flag & candidate_active[i];
        Bit better = valid &
                     (!choice.found | (candidate_order[i] > choice.order));
        choice.var = If(better, Integer(VAR_SIZE_BIT, i, PUBLIC), choice.var);
        choice.order = If(better, candidate_order[i], choice.order);
        choice.found = choice.found | valid;
    }
    return choice;
}

void bench_clear_indexed_candidate(vector<Bit> &seen,
                                   vector<Bit> &candidate_active,
                                   const Integer &idx,
                                   const Bit &flag)
{
    for (int i = 1; i < VAR_NUM + 1; ++i)
    {
        Bit clear_here = flag & (idx == Integer(idx.size(), i, PUBLIC));
        seen[i] = seen[i] ^ (clear_here & seen[i]);
        candidate_active[i] = candidate_active[i] ^
                              (clear_here & candidate_active[i]);
    }
}

Bit bench_add_literal_to_clause(Clause &out, const Integer &lit, const Bit &flag)
{
    Bit duplicate = Bit(false, PUBLIC);
    for (int i = 0; i < (int)out.literals.size(); ++i)
    {
        duplicate = duplicate | out.literals[i].equal(lit);
    }

    Bit inserted = Bit(false, PUBLIC);
    for (int i = 0; i < (int)out.literals.size(); ++i)
    {
        Bit empty = out.literals[i] == Integer(VAR_SIZE_BIT, 0, PUBLIC);
        Bit learned_slot = Bit(i < MAX_LIT_IN_CON_CLAUSE, PUBLIC);
        Bit place_here =
            flag & !duplicate & learned_slot & empty & !inserted;
        out.literals[i] = If(place_here, lit, out.literals[i]);
        inserted = inserted | place_here;
    }
    return flag & !duplicate & !inserted;
}

template <typename VState>
void bench_compute_beta_below_current(VState &big_V, const Clause &c, Integer &beta, const Bit &flag)
{
    for (int i = 0; i < (int)c.literals.size(); ++i)
    {
        Integer lit = c.literals[i];
        Integer dl(DL_SIZE_BIT, 0, PUBLIC);
        big_V.get_decision_level(lit.abs(), dl);
        Bit is_nonzero = lit != Integer(VAR_SIZE_BIT, 0, PUBLIC);
        Bit below_current = dl < big_V.cur_dl;
        beta = If(flag & is_nonzero & below_current & (dl > beta), dl, beta);
    }
}

void bench_bump_touched_variables(vector<Integer> &activities,
                                  const vector<Bit> &touched,
                                  const Clause &learned,
                                  const Bit &flag)
{
    for (int i = 1; i < VAR_NUM + 1; ++i)
    {
        Integer idx(VAR_SIZE_BIT, i, PUBLIC);
        Bit should_bump = touched[i];
        for (int j = 0; j < (int)learned.literals.size(); ++j)
        {
            should_bump = should_bump | (learned.literals[j].abs() == idx);
        }

        Integer inc = If(flag & should_bump,
                         Integer(ACTIVITY_SIZE_BIT, CDCL::activity_bump, PUBLIC),
                         Integer(ACTIVITY_SIZE_BIT, 0, PUBLIC));
        activities[i] = activities[i] + inc;
    }
}

void bench_decay_activities(vector<Integer> &activities, const Bit &flag)
{
    Integer numerator(ACTIVITY_SIZE_BIT + 8, 95, PUBLIC);
    Integer denominator(ACTIVITY_SIZE_BIT + 8, 100, PUBLIC);
    for (int idx = 1; idx < (int)activities.size(); ++idx)
    {
        Integer extended = activities[idx];
        extended.resize(ACTIVITY_SIZE_BIT + 8);
        Integer decayed = (extended * numerator) / denominator;
        decayed.resize(ACTIVITY_SIZE_BIT);
        activities[idx] = If(flag, decayed, activities[idx]);
    }
}

template <typename VState>
void bench_add_implication(VState &big_V,
                           vector<Integer> &big_U,
                           const Integer &literal,
                           const Integer &antecedent,
                           const Integer &dl,
                           const Bit &flag)
{
    Bit did_assign = big_V.try_set_unit_literal(literal, antecedent, dl, flag);
    Integer val = If(literal >= Integer(VAR_SIZE_BIT, 0, PUBLIC),
                     Integer(2, 1, PUBLIC), Integer(2, 2, PUBLIC));
    assign_vector(big_U, literal.abs(), val, did_assign);
}

template <typename VState>
Bit bench_construct_wl(VState &big_V, WatchList &wls,
                       const Integer &clause_idx, const Clause &c,
                       const Bit &flag)
{
    Integer wl_clause_idx = clause_idx;
    wl_clause_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
    Integer watch_lit1(VAR_SIZE_BIT, 0, PUBLIC);
    Integer watch_lit2(VAR_SIZE_BIT, 0, PUBLIC);
    Integer watch_pos1(WATCH_POSITION_SIZE_BIT, 0, PUBLIC);
    Integer watch_pos2(WATCH_POSITION_SIZE_BIT, 0, PUBLIC);
    Integer fallback_lit(VAR_SIZE_BIT, 0, PUBLIC);
    Integer fallback_pos(WATCH_POSITION_SIZE_BIT, 0, PUBLIC);
    Bit lit1_assigned = Bit(false, PUBLIC);
    Bit lit2_assigned = Bit(false, PUBLIC);
    Bit fallback_assigned = Bit(false, PUBLIC);

    for (int i = 0; i < (int)c.literals.size(); ++i)
    {
        Bit is_nonzero = c.literals[i] != Integer(VAR_SIZE_BIT, 0, PUBLIC);
        Bit take_fallback = is_nonzero & !fallback_assigned;
        fallback_lit = If(take_fallback, c.literals[i], fallback_lit);
        fallback_pos = If(take_fallback,
                          Integer(WATCH_POSITION_SIZE_BIT, i + 1, PUBLIC),
                          fallback_pos);
        fallback_assigned = fallback_assigned | take_fallback;
        Bit if_not_falsified = is_nonzero & !big_V.is_literal_falsified(c.literals[i]);
        Bit take_first = if_not_falsified & !lit1_assigned;
        watch_lit1 = If(take_first, c.literals[i], watch_lit1);
        watch_pos1 = If(take_first,
                        Integer(WATCH_POSITION_SIZE_BIT, i + 1, PUBLIC), watch_pos1);
        lit1_assigned = lit1_assigned | if_not_falsified;
    }
    watch_lit1 = If(lit1_assigned, watch_lit1, fallback_lit);
    watch_pos1 = If(lit1_assigned, watch_pos1, fallback_pos);
    lit1_assigned = lit1_assigned | fallback_assigned;

    for (int i = 0; i < (int)c.literals.size(); ++i)
    {
        Bit is_nonzero = c.literals[i] != Integer(VAR_SIZE_BIT, 0, PUBLIC);
        Bit should_be_second = is_nonzero & lit1_assigned & !lit2_assigned &
                               (c.literals[i] != watch_lit1);
        watch_lit2 = If(should_be_second, c.literals[i], watch_lit2);
        watch_pos2 = If(should_be_second,
                        Integer(WATCH_POSITION_SIZE_BIT, i + 1, PUBLIC), watch_pos2);
        lit2_assigned = lit2_assigned | should_be_second;
    }

    Bit valid_lit1 = watch_lit1 != Integer(VAR_SIZE_BIT, 0, PUBLIC);
    Bit valid_lit2 = watch_lit2 != Integer(VAR_SIZE_BIT, 0, PUBLIC);
    WatchList::InstallResult install = wls.install_clause_watchers(
        wl_clause_idx,
        watch_lit1, watch_pos1, valid_lit1,
        watch_lit2, watch_pos2, valid_lit2,
        flag);
    return install.overflow;
}

template <typename VState>
void bench_backtrack(VState &big_V,
                     vector<Integer> &big_U,
                     Integer &wl_scan_ctr,
                     Bit &scanned_wl_this_step,
                     const Integer &beta,
                     const Bit &flag)
{
    for (int i = 0; i < (int)big_U.size(); ++i)
    {
        big_U[i] = If(flag, Integer(2, 0, PUBLIC), big_U[i]);
    }
    big_V.cur_dl = If(flag, beta, big_V.cur_dl);
    big_V.clean_up_after_backtrack(flag);
    wl_scan_ctr = If(
        flag,
        Integer(watchlist_scan_cursor_bits(), -1, PUBLIC),
        wl_scan_ctr);
    scanned_wl_this_step = scanned_wl_this_step & !flag;
}

template <typename VState>
void bench_push_unit_clause_to_big_U(VState &big_V,
                                     vector<Integer> &big_U,
                                     const Bit &flag)
{
    for (int i = 1; i < VAR_NUM + 1; ++i)
    {
        Integer var_dl;
        Integer var_ass;
        big_V.get_decision_level(i, var_dl);
        big_V.get_assignment(i, var_ass);
        Bit is_root_level = var_dl == Integer(DL_SIZE_BIT, 0, PUBLIC);
        Bit is_assigned = var_ass != big_V.not_assigned;
        Integer val = If(var_ass == big_V.true_assigned,
                         Integer(2, 1, PUBLIC),
                         If(var_ass == big_V.false_assigned,
                            Integer(2, 2, PUBLIC),
                            Integer(2, 0, PUBLIC)));
        val = If(is_root_level & is_assigned, val,
                 Integer(2, 0, PUBLIC));
        big_U[i] = If(flag, val, big_U[i]);
    }
}

template <typename VState>
void bench_restart(VState &big_V,
                   vector<Integer> &big_U,
                   Bit &if_conflict_pending,
                   Bit &if_decision_pending,
                   Bit &if_sat,
                   Integer &a_idx,
                   Integer &w_idx,
                   Integer &wl_scan_ctr,
                   Bit &scanned_wl_this_step,
                   const Bit &flag)
{
    bench_backtrack(big_V, big_U, wl_scan_ctr, scanned_wl_this_step,
                    Integer(DL_SIZE_BIT, 0, PUBLIC), flag);
    if_conflict_pending = if_conflict_pending & !flag;
    if_decision_pending = if_decision_pending & !flag;
    if_sat = if_sat & !flag;
    a_idx = If(flag, Integer(VAR_SIZE_BIT, -1, PUBLIC), a_idx);
    w_idx = If(flag, Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC), w_idx);
    bench_push_unit_clause_to_big_U(big_V, big_U, flag);
}

// Benchmarks one conflict block. Returns its time in seconds and stores its
// AND-gate count in gates_per_conflict_block.
template <typename VState>
double conflict(VState &big_V,
                ClauseList &cl,
                WatchList &wls,
                vector<Integer> &activities,
                vector<Integer> &big_U,
                Clause &w,
                int factor,
                double &gates_per_conflict_block, int uip_loop_cap = 0)
{
    // Match production's public bound tightening.  Both inputs to min are
    // public, and rounds beyond VAR_NUM cannot perform useful pivots.
    int analysis_rounds = uip_loop_cap == 0
                              ? VAR_NUM
                              : std::min(uip_loop_cap, VAR_NUM);
    factor = std::max(1, factor);
    factor = std::min(factor, analysis_rounds);
    int sampled_rounds = std::max(1, analysis_rounds / factor);
    double scale = static_cast<double>(analysis_rounds) / static_cast<double>(sampled_rounds);

    // These values are private solver state in production. They are fed in
    // before the timer starts, so the benchmark measures operations on secret
    // wires without charging input setup to conflict analysis.
    Bit flag = Bit(true, ALICE);
    Integer conflict_clause_idx(CLAUSE_IDX_SIZE_IN_WL, 1, ALICE);
    Integer conflict_ctr(CON_CLAUSE_IDX_SIZE_BIT, 1, ALICE);
    Bit if_failure(false, ALICE);
    Bit if_conflict_pending(false, ALICE);
    Bit if_decision_pending(false, ALICE);
    Bit if_sat(false, ALICE);
    Integer restart_a_idx(VAR_SIZE_BIT, 1, ALICE);
    Integer restart_w_idx(CLAUSE_IDX_SIZE_IN_WL, 1, ALICE);
    Integer restart_wl_scan_ctr(watchlist_scan_cursor_bits(), 0, ALICE);
    Bit restart_scanned_wl_this_step(true, ALICE);

    auto t_setup = clock_start();
    uint64_t g_setup_start = base->num_and();
    Bit valid_conflict = flag & !(conflict_clause_idx == Integer(conflict_clause_idx.size(), 0, PUBLIC));
    Bit root_conflict = valid_conflict & (big_V.cur_dl == Integer(DL_SIZE_BIT, 0, PUBLIC));
    Bit do_analysis = valid_conflict & !root_conflict;
    Bit stopped = Bit(false, PUBLIC);
    Bit uip_cap = Bit(false, PUBLIC);
    Integer c_idx = conflict_clause_idx;
    c_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
    Integer pivot(VAR_SIZE_BIT, 0, PUBLIC);
    Integer pathC(VAR_SIZE_BIT, 0, PUBLIC);
    int order_bits = big_V.order_ctr_bitlen();
    vector<Bit> seen(VAR_NUM + 1, Bit(false, PUBLIC));
    vector<Bit> touched(VAR_NUM + 1, Bit(false, PUBLIC));
    vector<Bit> candidate_active(VAR_NUM + 1, Bit(false, PUBLIC));
    vector<Integer> candidate_order(VAR_NUM + 1,
                                    Integer(order_bits, 0, PUBLIC));
    Clause out_clause;
    Bit learned_length_cap = Bit(false, PUBLIC);
    auto t_setup_end = time_from(t_setup);

    auto t1 = clock_start();

    uint64_t g_start = base->num_and();
    // Scaled part: mirror CDCL::conflict_analysis loop shape.
    for (int round = 0; round < sampled_rounds; ++round)
    {
        Clause c;
        cl.get_clause(c_idx, c);

        for (int i = 0; i < (int)c.literals.size(); ++i)
        {
            Integer lit = c.literals[i];
            Integer var = lit.abs();
            Integer dl(DL_SIZE_BIT, 0, PUBLIC);
            big_V.get_decision_level(var, dl);
            Integer ord(order_bits, 0, PUBLIC);
            big_V.get_order_counter(var, ord);

            Bit is_nonzero = lit != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit eligible = do_analysis & is_nonzero &
                           !(var == pivot.abs()) &
                           !(dl == Integer(DL_SIZE_BIT, 0, PUBLIC));
            Bit current_level = dl == big_V.cur_dl;
            Bit is_new = bench_mark_seen_touched_and_candidate(
                seen, touched, candidate_active, candidate_order,
                var, ord, eligible, current_level);
            Bit at_current_level = is_new & current_level;
            Bit below_current_level = is_new & !current_level;

            pathC = pathC + If(at_current_level,
                               Integer(VAR_SIZE_BIT, 1, PUBLIC),
                               Integer(VAR_SIZE_BIT, 0, PUBLIC));
            learned_length_cap = learned_length_cap |
                                 bench_add_literal_to_clause(
                                     out_clause, lit, below_current_level);
        }

        BenchIndexedCandidateChoice choice = bench_select_indexed_candidate(
            candidate_active, candidate_order, do_analysis);
        Bit found_candidate = choice.found;
        Integer best_var = choice.var;

        Bit pick_candidate = found_candidate;
        Integer assignment(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        big_V.get_assignment(best_var, assignment);
        Integer next_pivot = If(assignment == big_V.true_assigned, best_var, -best_var);
        pivot = If(pick_candidate, next_pivot, pivot);

        bench_clear_indexed_candidate(seen, candidate_active, best_var,
                                      pick_candidate);
        pathC = If(pick_candidate & (pathC > Integer(VAR_SIZE_BIT, 0, PUBLIC)),
                   pathC - Integer(VAR_SIZE_BIT, 1, PUBLIC),
                   pathC);

        Bit stop = pick_candidate & (pathC == Integer(VAR_SIZE_BIT, 0, PUBLIC));
        stopped = stopped | stop;
        // The sampled circuit runs sampled_rounds iterations, so its last
        // iteration models production's public final-round cap check.
        // Comparing against analysis_rounds would make the check unreachable
        // when conflict_factor > 1.
        Bit last_round = Bit(round == sampled_rounds - 1, PUBLIC);
        Bit no_candidate = do_analysis & !found_candidate;
        uip_cap = uip_cap | no_candidate | (do_analysis & last_round & !stop);

        Integer next_idx(ANTE_SIZE_BIT, 0, PUBLIC);
        big_V.get_antecedent(pivot.abs(), next_idx);
        next_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
        Bit continue_analysis = do_analysis & !stop & !uip_cap & found_candidate;
        c_idx = If(continue_analysis, next_idx, c_idx);
        do_analysis = continue_analysis;
    }

    uint64_t g_mid = base->num_and();
    auto t_scaled_end = time_from(t1);
    // Constant part: mirror conflict_analysis tail, backtrack, and backtrack_aftermath.
    auto t_const = clock_start();
    Integer alpha = -pivot;
    Bit analysis_valid = valid_conflict & !root_conflict & stopped & !uip_cap &
                         !(alpha == Integer(VAR_SIZE_BIT, 0, PUBLIC));
    learned_length_cap = learned_length_cap |
                         bench_add_literal_to_clause(
                             out_clause, alpha, analysis_valid);

    Bit learned_count_cap =
        analysis_valid &
        (conflict_ctr > Integer(conflict_ctr.size(),
                                MAX_NUM_OF_CONFLICT_CLAUSE, PUBLIC));
    Bit final_valid =
        analysis_valid & !learned_length_cap & !learned_count_cap;
    Bit cap_hit = valid_conflict & !root_conflict &
                  (uip_cap | learned_length_cap | learned_count_cap);

    Integer beta(DL_SIZE_BIT, 0, PUBLIC);
    bench_compute_beta_below_current(big_V, out_clause, beta, final_valid);

    Integer ctr_for_idx = conflict_ctr;
    ctr_for_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
    Integer learned_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC) - ctr_for_idx;
    cl.insert_con_clause(ctr_for_idx, out_clause, final_valid);
    Bit real_analysis = valid_conflict & !root_conflict;
    bench_bump_touched_variables(activities, touched, out_clause, real_analysis);
    bench_decay_activities(activities, real_analysis);
    conflict_ctr = conflict_ctr + If(final_valid,
                                     Integer(CON_CLAUSE_IDX_SIZE_BIT, 1, PUBLIC),
                                     Integer(CON_CLAUSE_IDX_SIZE_BIT, 0, PUBLIC));

    bench_backtrack(big_V, big_U,
                    restart_wl_scan_ctr, restart_scanned_wl_this_step,
                    beta, final_valid);

    // Keep the fallback in the signed clause-ID domain.  CON_ORAM_SIZE_BIT is
    // only the physical learned-store address width and may be narrower (for
    // example, 8 versus 12 bits on flat100-6).
    Integer fallback_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC) -
                           (ctr_for_idx - Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC));
    Integer last_conflict = If(final_valid, learned_idx, fallback_idx);
    if_failure = if_failure |
                 bench_construct_wl(big_V, wls, last_conflict, out_clause,
                                    final_valid);
    Integer ante_idx = last_conflict;
    ante_idx.resize(ANTE_SIZE_BIT);
    bench_add_implication(big_V, big_U, alpha, ante_idx, big_V.cur_dl, final_valid);
    bench_restart(big_V, big_U,
                  if_conflict_pending, if_decision_pending, if_sat,
                  restart_a_idx, restart_w_idx,
                  restart_wl_scan_ctr, restart_scanned_wl_this_step,
                  cap_hit);
    auto t_const_end = time_from(t_const);

    uint64_t g_end = base->num_and();

    double red_time =
        (t_setup_end + t_scaled_end * scale + t_const_end) / 1e6;

    uint64_t setup_part = g_start - g_setup_start;
    uint64_t scaled_part = static_cast<uint64_t>((g_mid - g_start) * scale);
    uint64_t constant_part = (g_end - g_mid);
    gates_per_conflict_block =
        static_cast<double>(setup_part + scaled_part + constant_part);

    return red_time;
}

struct BlockTotals
{
    double benchmark_setup_time;
    double decision_time;
    double from_u_time;
    double conflict_time;
    double periodic_restart_time;
    double check_clause_time;
    double update_wl_time;
    double add_implication_time;
    double terminal_open_time;
    double regular_total_time;
    double total_time;

    double benchmark_setup_gates;
    double decision_gates;
    double from_u_gates;
    double conflict_gates;
    double periodic_restart_gates;
    std::uint64_t periodic_restart_count;
    double check_clause_gates;
    double update_wl_gates;
    double add_implication_gates;
    double terminal_open_gates;
    double regular_total_gates;
    double total_gates;
};

template <typename VState>
void test_all_cdcl_impl(
    int var_num,
    int clause_num,
    int max_num_of_con_clause,
    int max_lit_in_clause,
    int max_lit_in_con_clause,
    int max_clause_in_wl,
    int conflict_factor,
    long long global_sample_factor,
    long long conflict_sample_factor,
    long long orange_total,
    long long yellow_total,
    long long blue_total,
    long long red_total,
    BlockTotals &out, int uip_loop_cap,
    WatchBackendRequest watch_backend_request)
{
    if (max_num_of_con_clause <= 0 ||
        max_lit_in_clause <= 0 ||
        max_lit_in_con_clause <= 0 ||
        max_clause_in_wl <= 0)
    {
        // Raise non-positive parameters to 1
        if (max_num_of_con_clause <= 0)
            max_num_of_con_clause = 1;
        if (max_lit_in_clause <= 0)
            max_lit_in_clause = 1;
        if (max_lit_in_con_clause <= 0)
            max_lit_in_con_clause = 1;
        if (max_clause_in_wl <= 0)
            max_clause_in_wl = 1;
        std::cout << "Warning: Some max_* parameters are non-positive. Adjusted to 1 as follows:\n";
        std::cout << "max_num_of_con_clause: " << max_num_of_con_clause << "\n";
        std::cout << "max_lit_in_clause: " << max_lit_in_clause << "\n";
        std::cout << "max_lit_in_con_clause: " << max_lit_in_con_clause << "\n";
        std::cout << "max_clause_in_wl: " << max_clause_in_wl << "\n";
    }
    init_constant(var_num, clause_num, max_num_of_con_clause,
                  max_lit_in_clause, max_lit_in_con_clause, max_clause_in_wl);

    auto setup_start = clock_start();
    const uint64_t setup_gates_start = base->num_and();
    WatchList wls(base, watch_backend_request);
    VState big_V(base);
    ClauseList cl(base);
    vector<Integer> activities;
    vector<Integer> unit_clause;
    vector<Integer> big_U;

    vector<int> lits = {1, 2, 3};
    Clause w(lits);
    cl.insert_phi_clause(Integer(CLAUSE_IDX_SIZE_BIT, 1), w, Bit(true, PUBLIC));
    WatchList::InstallResult fixture_install = wls.install_clause_watchers(
        Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC),
        Integer(VAR_SIZE_BIT, 1, PUBLIC), Integer(WATCH_POSITION_SIZE_BIT, 1, PUBLIC), Bit(true, PUBLIC),
        Integer(VAR_SIZE_BIT, 2, PUBLIC), Integer(WATCH_POSITION_SIZE_BIT, 2, PUBLIC), Bit(true, PUBLIC),
        Bit(true, PUBLIC));
    if (!fixture_install.installed.reveal() || fixture_install.overflow.reveal())
        throw std::runtime_error("failed to initialize stable watcher benchmark fixture");
    if (base->party == ALICE)
    {
        std::cout << "BENCH_WATCH_BACKEND requested="
                  << watch_backend_name(watch_backend_request)
                  << " effective=" << watch_backend_name(wls.backend_kind())
                  << " reason=" << wls.selection_reason()
                  << " occ_bits=" << WATCH_OCC_PAYLOAD_BITS
                  << " pair_bits=" << WATCH_PAIR_PAYLOAD_BITS << std::endl;
    }

    Integer a_idx(VAR_SIZE_BIT, 4);
    Integer w_idx(CLAUSE_IDX_SIZE_BIT, 3);
    (void)w_idx;

    for (int i = 0; i < VAR_NUM + 1; ++i)
    {
        activities.push_back(Integer(ACTIVITY_SIZE_BIT, 2, PUBLIC));
        big_U.push_back(Integer(2, 0));
        unit_clause.push_back(Integer(CLAUSE_IDX_SIZE_BIT, 0));
    }
    out.benchmark_setup_time = time_from(setup_start) / 1e6;
    out.benchmark_setup_gates =
        static_cast<double>(base->num_and() - setup_gates_start);
    long long MIN_SAMPLE = 10LL;
    // Sample sizes, at least MIN_SAMPLE
    long long y_sample = max(MIN_SAMPLE, yellow_total / max(MIN_SAMPLE, global_sample_factor));
    long long b_sample = max(MIN_SAMPLE, blue_total / max(MIN_SAMPLE, global_sample_factor));
    long long o_sample = max(MIN_SAMPLE, orange_total / max(MIN_SAMPLE, global_sample_factor));
    long long effective_conf_sample_factor =
        max(MIN_SAMPLE, global_sample_factor) * max(MIN_SAMPLE, conflict_sample_factor);
    long long r_sample = max(MIN_SAMPLE, red_total / effective_conf_sample_factor);

    // Decision block
    double acc_time = 0.0;
    uint64_t acc_gates = 0;
    for (long long i = 0; i < y_sample; ++i)
    {
        auto t1 = clock_start();
        uint64_t g_start = base->num_and();
        decision(wls, big_V, activities, a_idx);
        uint64_t g_end = base->num_and();
        acc_time += time_from(t1) / 1e6;
        acc_gates += (g_end - g_start);
    }
    double per_decision_time = acc_time / (double)y_sample;
    double per_decision_gates = (double)acc_gates / (double)y_sample;
    out.decision_time = per_decision_time * (double)yellow_total;
    out.decision_gates = per_decision_gates;

    // from_U block
    acc_time = 0.0;
    acc_gates = 0;
    for (long long i = 0; i < b_sample; ++i)
    {
        auto t1 = clock_start();
        uint64_t g_start = base->num_and();
        from_U(wls, big_U, a_idx);
        uint64_t g_end = base->num_and();
        acc_time += time_from(t1) / 1e6;
        acc_gates += (g_end - g_start);
    }
    double per_from_u_time = acc_time / (double)b_sample;
    double per_from_u_gates = (double)acc_gates / (double)b_sample;
    out.from_u_time = per_from_u_time * (double)blue_total;
    out.from_u_gates = per_from_u_gates;

    // Orange sub-blocks: check clause, update watchlist, add implication
    double acc_check_time = 0.0, acc_update_time = 0.0, acc_imp_time = 0.0;
    uint64_t acc_check_g = 0, acc_update_g = 0, acc_imp_g = 0;

    // Production selects these values from private ORAM/state. Build the
    // deterministic fixture as ALICE inputs outside the timed regions, and use
    // the exact signed cursor width from CDCL::wl_scan_ctr_bits().
    Integer watch_clause_idx(CLAUSE_IDX_SIZE_IN_WL, 1, ALICE);
    Integer source_slot(watchlist_scan_cursor_bits(), 0, ALICE);
    Integer source_lit_one(VAR_SIZE_BIT, 1, ALICE);
    Integer source_lit_three(VAR_SIZE_BIT, 3, ALICE);
    Bit watch_move_active(true, ALICE);
    Bit all_watch_moves_valid(true, PUBLIC);
    for (long long i = 0; i < o_sample; ++i)
    {
        // Alternate 1->3 and 3->1.  Both destination lists use slot zero, so
        // every timed iteration is the same real production transaction and
        // the synthetic state returns to its initial invariant every pair.
        Integer source_lit = (i & 1) == 0 ? source_lit_one : source_lit_three;
        // check
        auto t1 = clock_start();
        uint64_t g_start = base->num_and();
        cl.phi_oram_time = 0;
        vector<Integer> candidates = test_check_clause(
            cl, big_V, wls, w, watch_clause_idx, source_lit);
        uint64_t g_end = base->num_and();
        acc_check_time += time_from(t1) / 1e6;
        acc_check_g += (g_end - g_start);

        // update WL
        t1 = clock_start();
        g_start = base->num_and();
        WL_ORAM_TIME = 0;
        WatchList::MoveResult update = wls.move_clause_from_frame(
            watch_clause_idx, source_lit, source_slot, w,
            candidates, watch_move_active);
        all_watch_moves_valid = all_watch_moves_valid & update.moved & !update.overflow;
        acc_update_time += time_from(t1) / 1e6;
        g_end = base->num_and();
        acc_update_g += (g_end - g_start);

        // add implication
        t1 = clock_start();
        g_start = base->num_and();
        test_add_implication(big_V, big_U, a_idx);
        g_end = base->num_and();
        acc_imp_time += time_from(t1) / 1e6;
        acc_imp_g += (g_end - g_start);
    }
    if (!all_watch_moves_valid.reveal())
        throw std::runtime_error("watcher benchmark fixture drifted or overflowed");

    double per_check_time = acc_check_time / (double)o_sample;
    double per_update_time = acc_update_time / (double)o_sample;
    double per_imp_time = acc_imp_time / (double)o_sample;

    double per_check_gates = (double)acc_check_g / (double)o_sample;
    double per_update_gates = (double)acc_update_g / (double)o_sample;
    double per_imp_gates = (double)acc_imp_g / (double)o_sample;

    out.check_clause_time = per_check_time * (double)orange_total;
    out.update_wl_time = per_update_time * (double)orange_total;
    out.add_implication_time = per_imp_time * (double)orange_total;

    out.check_clause_gates = per_check_gates;
    out.update_wl_gates = per_update_gates;
    out.add_implication_gates = per_imp_gates;

    // The production loop reveals one packed terminal code per public step.
    // Sample it separately because it includes synchronization, then add it
    // to the regular per-step total reported in the SUMMARY line.
    double acc_terminal_time = 0.0;
    uint64_t acc_terminal_gates = 0;
    const long long terminal_samples = 100;
    volatile int opened_status = 0;
    for (long long i = 0; i < terminal_samples; ++i)
    {
        auto t1 = clock_start();
        uint64_t g_start = base->num_and();
        opened_status = terminal_open();
        uint64_t g_end = base->num_and();
        acc_terminal_time += time_from(t1) / 1e6;
        acc_terminal_gates += (g_end - g_start);
    }
    (void)opened_status;
    out.terminal_open_time =
        (acc_terminal_time / static_cast<double>(terminal_samples)) * static_cast<double>(orange_total);
    out.terminal_open_gates =
        static_cast<double>(acc_terminal_gates) / static_cast<double>(terminal_samples);

    // regular = sum of the orange sub-blocks and terminal_open
    out.regular_total_time =
        out.check_clause_time +
        out.update_wl_time +
        out.add_implication_time +
        out.terminal_open_time;
    out.regular_total_gates =
        out.check_clause_gates +
        out.update_wl_gates +
        out.add_implication_gates +
        out.terminal_open_gates;

    // Conflict block
    double acc_conf_time = 0.0;
    double acc_conf_gates = 0.0;
    for (long long i = 0; i < r_sample; ++i)
    {
        double gates_conf_block = 0.0;
        double t_conf = conflict(big_V, cl, wls, activities, big_U, w,
                                 conflict_factor,
                                 gates_conf_block, uip_loop_cap);
        acc_conf_time += t_conf;
        acc_conf_gates += gates_conf_block;
    }
    double per_conf_time = acc_conf_time / (double)r_sample;
    double per_conf_gates = acc_conf_gates / (double)r_sample;

    // A second restart circuit runs at public Luby boundaries after the red
    // conflict block. It is distinct from the secret cap-triggered restart
    // included in conflict(). Its invocation count depends only on the
    // public number of red slots and the public Luby policy.
    const long long restart_samples = 10;
    double acc_restart_time = 0.0;
    uint64_t acc_restart_gates = 0;
    Bit periodic_conflict_pending(false, ALICE);
    Bit periodic_decision_pending(false, ALICE);
    Bit periodic_sat(false, ALICE);
    Bit periodic_unsat(false, ALICE);
    Bit periodic_solver_active(true, ALICE);
    Integer periodic_a_idx(VAR_SIZE_BIT, 1, ALICE);
    Integer periodic_w_idx(CLAUSE_IDX_SIZE_IN_WL, 1, ALICE);
    Integer periodic_wl_scan_ctr(watchlist_scan_cursor_bits(), 0, ALICE);
    Bit periodic_scanned_wl_this_step(true, ALICE);
    for (long long i = 0; i < restart_samples; ++i)
    {
        auto t1 = clock_start();
        uint64_t g_start = base->num_and();
        Bit periodic_flag = periodic_solver_active &
                            !periodic_sat & !periodic_unsat;
        bench_restart(big_V, big_U,
                      periodic_conflict_pending, periodic_decision_pending,
                      periodic_sat, periodic_a_idx, periodic_w_idx,
                      periodic_wl_scan_ctr,
                      periodic_scanned_wl_this_step,
                      periodic_flag);
        uint64_t g_end = base->num_and();
        acc_restart_time += time_from(t1) / 1e6;
        acc_restart_gates += g_end - g_start;
    }
    const double per_restart_time =
        acc_restart_time / static_cast<double>(restart_samples);
    const double per_restart_gates =
        static_cast<double>(acc_restart_gates) /
        static_cast<double>(restart_samples);
    out.periodic_restart_count = ppcdcl::policy::count_luby_restarts(
        static_cast<std::uint64_t>(std::max(0LL, red_total)));
    out.periodic_restart_time =
        per_restart_time * static_cast<double>(out.periodic_restart_count);
    out.periodic_restart_gates = per_restart_gates;

    // The e2e solver only runs conflict analysis on fixed public red slots.
    // Each red slot still executes the same circuit with a secret active flag.
    out.conflict_time = per_conf_time * (double)red_total +
                        out.periodic_restart_time;
    out.conflict_gates = per_conf_gates;
    if (red_total > 0)
    {
        out.conflict_gates +=
            per_restart_gates *
            static_cast<double>(out.periodic_restart_count) /
            static_cast<double>(red_total);
    }

    // Totals
    out.total_time =
        out.decision_time +
        out.from_u_time +
        out.regular_total_time +
        out.conflict_time;

    out.total_gates =
        out.decision_gates +
        out.from_u_gates +
        out.regular_total_gates +
        out.conflict_gates;
}

int main(int argc, char **argv)
{
    std::vector<std::string> positional;
    bool use_oram_v = false;
    WatchBackendRequest watch_backend_request = WatchBackendRequest::Auto;
    int backend_threads = DEFAULT_BACKEND_THREADS;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            std::string lower = arg;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower == "--oramv" || lower == "--oram-v")
            {
                use_oram_v = true;
                continue;
            }
            if (lower == "--no-oramv" || lower == "--no-oram-v")
            {
                use_oram_v = false;
                continue;
            }
            if (lower == "--shortest-wl" || lower == "--shortest-watchlist" ||
                lower == "--wl-shortest")
            {
                USE_SHORTEST_WL = true;
                continue;
            }
            if (lower == "--no-shortest-wl" || lower == "--no-shortest-watchlist" ||
                lower == "--first-wl" || lower == "--first-fit-wl")
            {
                USE_SHORTEST_WL = false;
                continue;
            }
            const std::string threads_eq = "--threads=";
            const std::string watch_backend_eq = "--watch-backend=";
            if (lower.compare(0, watch_backend_eq.size(), watch_backend_eq) == 0)
            {
                watch_backend_request = parse_watch_backend_request(
                    arg.substr(watch_backend_eq.size()));
                continue;
            }
            if (lower == "--watch-backend")
            {
                if (i + 1 >= argc)
                    throw std::runtime_error(arg + " requires auto, block, or indexed");
                watch_backend_request = parse_watch_backend_request(argv[++i]);
                continue;
            }
            if (lower.compare(0, threads_eq.size(), threads_eq) == 0)
            {
                backend_threads = parse_backend_threads(arg.substr(threads_eq.size()));
                continue;
            }
            if (lower == "--threads")
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error(arg + " requires an integer argument");
                }
                backend_threads = parse_backend_threads(argv[++i]);
                continue;
            }
            positional.push_back(arg);
        }
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Argument error: " << ex.what() << std::endl;
        return 1;
    }

    if (positional.size() < 14 || positional.size() > 16)
    {
        cerr << "Usage: " << argv[0]
             << " party var_num clause_num max_num_of_con_clause "
             << "max_lit_in_clause max_lit_in_con_clause max_clause_in_wl "
             << "conflict_factor global_sample_factor conflict_sample_factor "
             << "orange_total yellow_total blue_total red_total [uip] "
             << "[--oramV|--no-oramV] [--shortest-wl|--no-shortest-wl] "
             << "[--watch-backend auto|block|indexed] "
             << "[--threads N] "
             << "(defaults: linear variable state, first-fit watchlist, 4 backend threads)\n";
        return 1;
    }

    int argi = 0;
    int party = atoi(positional[argi++].c_str());
    int port = 10086;
    if (positional.size() == 16)
    {
        // Optional base port, so concurrent runs can use different ports
        port = atoi(positional[argi++].c_str());
    }
    int var_num = atoi(positional[argi++].c_str());
    int clause_num = atoi(positional[argi++].c_str());
    int max_num_of_con_clause = atoi(positional[argi++].c_str());
    int max_lit_in_clause = atoi(positional[argi++].c_str());
    int max_lit_in_con_clause = atoi(positional[argi++].c_str());
    int max_clause_in_wl = atoi(positional[argi++].c_str());
    int conflict_factor = atoi(positional[argi++].c_str());
    long long global_sample_factor = atoll(positional[argi++].c_str());
    long long conflict_sample_factor = atoll(positional[argi++].c_str());
    long long orange_total = atoll(positional[argi++].c_str());
    long long yellow_total = atoll(positional[argi++].c_str());
    long long blue_total = atoll(positional[argi++].c_str());
    long long red_total = atoll(positional[argi++].c_str());
    int uip_loop_cap = (argi < static_cast<int>(positional.size())) ? atoi(positional[argi++].c_str()) : 0;
    if (max_lit_in_con_clause == 0)
    {
        uip_loop_cap = var_num + 1; // effectively no cap
    }
    std::cout << "Using " << (use_oram_v ? "ORAM" : "linear-scan")
              << " variable-state backend.\n";
    std::cout << "Using " << (USE_SHORTEST_WL ? "shortest" : "first-fit")
              << " watchlist replacement strategy.\n";
    std::cout << "BENCH_CONFIG party=" << party
              << " threads=" << backend_threads
              << " base_port=" << port << std::endl;

    init_backend(party, port, 24, 7, backend_threads);
    base->switch_to_gt();

    BlockTotals totals{};
    if (use_oram_v)
    {
        test_all_cdcl_impl<VariableStateOram>(var_num, clause_num,
                                              max_num_of_con_clause,
                                              max_lit_in_clause,
                                              max_lit_in_con_clause,
                                              max_clause_in_wl,
                                              conflict_factor,
                                              global_sample_factor,
                                              conflict_sample_factor,
                                              orange_total,
                                              yellow_total,
                                              blue_total,
                                              red_total,
                                              totals, uip_loop_cap,
                                              watch_backend_request);
    }
    else
    {
        test_all_cdcl_impl<VariableState>(var_num, clause_num,
                                          max_num_of_con_clause,
                                          max_lit_in_clause,
                                          max_lit_in_con_clause,
                                          max_clause_in_wl,
                                          conflict_factor,
                                          global_sample_factor,
                                          conflict_sample_factor,
                                          orange_total,
                                          yellow_total,
                                          blue_total,
                                          red_total,
                                          totals, uip_loop_cap,
                                          watch_backend_request);
    }

    if (party == ALICE)
    {
        // Human-readable summary
        cout << "Estimated solving cost:\n";
        cout << "  benchmark_setup = " << totals.benchmark_setup_time
             << " s (reported separately; not in solving total)\n";
        cout << "  decision_time   = " << totals.decision_time << " s\n";
        cout << "  from_u_time     = " << totals.from_u_time << " s\n";
        cout << "  regular_time    = " << totals.regular_total_time << " s\n";
        cout << "    check_clause  = " << totals.check_clause_time << " s\n";
        cout << "    update_wl     = " << totals.update_wl_time << " s\n";
        cout << "    add_imp       = " << totals.add_implication_time << " s\n";
        cout << "    terminal_open = " << totals.terminal_open_time << " s\n";
        cout << "  conflict_time   = " << totals.conflict_time << " s\n";
        cout << "    periodic_restart = " << totals.periodic_restart_time
             << " s (" << totals.periodic_restart_count << " public slots)\n";
        cout << "  total_time      = " << totals.total_time << " s\n";

        // Per-block times
        cout << "Estimated per-block times:\n";
        cout << "decision_time = " << totals.decision_time / std::max(1LL, yellow_total) << " s\n";
        cout << "from_u_time = " << totals.from_u_time / std::max(1LL, blue_total) << " s\n";
        cout << "regular_time = " << totals.regular_total_time / orange_total << " s\n";
        cout << "check_clause_time = " << totals.check_clause_time / orange_total << " s\n";
        cout << "update_wl_time = " << totals.update_wl_time / orange_total << " s\n";
        cout << "add_implication_time = " << totals.add_implication_time / orange_total << " s\n";
        cout << "terminal_open_time = " << totals.terminal_open_time / orange_total << " s\n";
        long long conflict_den = std::max(1LL, red_total);
        cout << "conflict_time = " << totals.conflict_time / conflict_den << " s\n";
        cout << "BENCH_CONFLICT_POLICY activity_bump=" << CDCL::activity_bump
             << " activity_gate=real_analysis\n";
        cout << "BENCH_CONFLICT_GATES per_block=" << totals.conflict_gates << "\n";
        cout << "BENCH_PERIODIC_RESTART count="
             << totals.periodic_restart_count
             << " total_seconds=" << totals.periodic_restart_time
             << " per_restart_gates=" << totals.periodic_restart_gates
             << "\n";
        cout << "BENCH_WATCH_UPDATE_GATES per_block="
             << totals.update_wl_gates << "\n";
        cout << "BENCH_SETUP seconds=" << totals.benchmark_setup_time
             << " gates=" << totals.benchmark_setup_gates
             << " included_in_summary=0\n";

        // Machine-readable summary parsed by the Python scripts:
        // SUMMARY total dec fromU conf regular check update imp yellow blue red orange
        cout << "SUMMARY "
             << totals.total_time << " "
             << totals.decision_time << " "
             << totals.from_u_time << " "
             << totals.conflict_time << " "
             << totals.regular_total_time << " "
             << totals.check_clause_time << " "
             << totals.update_wl_time << " "
             << totals.add_implication_time << " "
             << yellow_total << " "
             << blue_total << " "
             << red_total << " "
             << orange_total
             << "\n";
    }

    delete base;
    return 0;
}
