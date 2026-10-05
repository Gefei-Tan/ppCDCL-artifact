#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>

#include "emp-tool/emp-tool.h"
#include "emp-dpf/emp-dpf.h"
#include "src/ppCDCL.h"

using namespace std;

FloramMPC<NetIO> *base = nullptr;

void init_backend(int party,
                  int depth = 25,
                  int size_of_element = 7,
                  int threads = 8,
                  int round_key_size = 94208,
                  const char *addr = "127.0.0.1")
{
    NetIO **io = new NetIO *[threads];
    for (int i = 0; i < max(2, threads); i++)
    {
        io[i] = new NetIO((party - 1) ? addr : nullptr, 12345 + i, true);
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

void test_check_clause(ClauseList &cl, VariableStateOram &big_V, Clause &w)
{
    Integer idx = Integer(PHI_ORAM_SIZE_BIT, 1);
    cl.get_clause(idx, w);
    vector<Integer> non_falsified_literals = {Integer(VAR_SIZE_BIT, 0),
                                              Integer(VAR_SIZE_BIT, 0)};
    for (int i = 0; i < MAX_LIT_IN_CLAUSE; i++)
    {
        Integer l = Integer(VAR_SIZE_BIT, 1);
        Bit if_l_is_not_falsified = !big_V.is_literal_falsified(l);
        Bit if_l_is_not_fresh_falsified = !l.equal(l);
        Bit is_valid_literal = if_l_is_not_falsified & if_l_is_not_fresh_falsified;

        Bit is_first_zero = non_falsified_literals[0] == Integer(VAR_SIZE_BIT, 0);
        Bit is_second_zero = non_falsified_literals[1] == non_falsified_literals[0];

        non_falsified_literals[0] =
            If(is_first_zero & is_valid_literal, l, non_falsified_literals[0]);

        non_falsified_literals[1] =
            If(is_second_zero & is_valid_literal, l, non_falsified_literals[1]);
    }
}

// 3 WL ops: 1 add, 1 remove (read+write)
void test_update_wl(WatchList &wls)
{
    wls.add_clause(Integer(VAR_SIZE_BIT, 3), Integer(CLAUSE_IDX_SIZE_BIT, 1), true);
    wls.remove_clause(Integer(VAR_SIZE_BIT, 1), Integer(CLAUSE_IDX_SIZE_BIT, 1), true);
}

// 1 V READ, several V WRITEs, one big_U push
void test_add_implication(VariableStateOram &big_V,
                          vector<Integer> &big_U,
                          Integer &a_idx)
{
    big_V.is_variable_unassigned(a_idx);
    big_V.set_unit_literal(a_idx, Integer(ANTE_SIZE_BIT, 1),
                           Integer(DL_SIZE_BIT, 1), true);
    Integer val = If(a_idx >= Integer(VAR_SIZE_BIT, 0),
                     Integer(2, 1), Integer(2, 2));
    assign_vector(big_U, a_idx.abs(), val);
}

// 1 BIG_U pop, 1 WL read, 1 linear scan over VAR_NUM
void from_U(WatchList &wls, vector<Integer> &big_U, Integer &a_idx)
{
    Bit flag_found = false;
    for (size_t i = 0; i < VAR_NUM + 1; ++i)
    {
        Bit flag_1 = big_U[i] == Integer(2, 1);
        Bit flag_2 = big_U[i] == Integer(2, 2);
        a_idx = If(flag_1, Integer(VAR_SIZE_BIT, i),
                   If(flag_2, Integer(VAR_SIZE_BIT, i), a_idx));
        big_U[i] = If((flag_1 | flag_2) & !flag_found, Integer(2, 0), big_U[i]);
        flag_found = flag_found | flag_1 | flag_2;
    }
    auto wl_vec = wls.get_watchlist_clauses(a_idx);
    (void)wl_vec;
}

// 3 V write, 1 WL read, 1 scan over VAR_NUM, VAR_NUM V reads
void decision(WatchList &wls,
              VariableStateOram &big_V,
              vector<Integer> &activities,
              Integer &a_idx,
              vector<Integer> &big_U)
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

    assign_vector(big_U, a_idx.abs(), Integer(2, 0));
    big_V.set_decision(a_idx, Integer(DL_SIZE_BIT, 1));

    Bit if_all_assigned = true;
    for (int i = 1; i < VAR_NUM + 1; ++i)
    {
        if_all_assigned = if_all_assigned & !big_V.is_literal_satisfied(i);
    }
    (void)if_all_assigned;

    auto wl_vec = wls.get_watchlist_clauses(a_idx);
    (void)wl_vec;
}

// Conflict block: simulates 1/factor of the factor-scaled work and scales its
// time and gate count back up by factor. The time is also multiplied by red_num.
double conflict(VariableStateOram &big_V,
                ClauseList &cl,
                vector<Integer> &activities,
                Clause &w,
                WatchList &wls,
                int factor,
                int red_num,
                double &gates_per_conflict_block, int uip_loop_cap = 0)
{
    Integer dl;
    Integer ante = Integer(ANTE_SIZE_BIT, 0);
    auto t1 = clock_start();
    if (uip_loop_cap == 0)
    {
        uip_loop_cap = VAR_NUM + 1; // effectively no cap
    }

    // Gates are counted separately for the factor-scaled part and the constant part.
    uint64_t g_start = base->num_and();

    // Work scaled by factor
    for (int i = 0; i < uip_loop_cap / factor; ++i)
    {
        cl.get_clause(Integer(CLAUSE_IDX_SIZE_BIT, 1), w);
    }

    for (int i = 0; i < MAX_LIT_IN_CLAUSE * uip_loop_cap / factor; ++i)
    {
        big_V.get_antecedent(1, ante);
        big_V.get_decision_level(1, dl);
    }

    for (int i = 0; i < uip_loop_cap / factor; ++i)
    {
        big_V.get_decision_level(1, dl);
    }

    big_V.clean_up_after_backtrack(factor);
    uint64_t g_mid = base->num_and();
    auto t_scaled_end = time_from(t1);
    // Constant part, not scaled by factor
    auto t_const = clock_start();
    for (int i = 0; i < (int)activities.size(); ++i)
    {
        activities[i] = activities[i] + Integer(ACTIVITY_SIZE_BIT, 1);
    }
    cl.insert_clause(Integer(CON_CLAUSE_IDX_SIZE_BIT, -2), w);

    for (int i = 0; i < MAX_LIT_IN_CON_CLAUSE; ++i)
    {
        wls.add_clause(Integer(VAR_SIZE_BIT, 3), Integer(CLAUSE_IDX_SIZE_BIT, 1), true);
    }
    auto t_const_end = time_from(t_const);

    uint64_t g_end = base->num_and();

    // Time: the scaled part is multiplied by factor.
    double red_time = (t_scaled_end * factor + t_const_end) / 1e6 * red_num;

    // Gates: (g_mid - g_start) comes from work run on 1/factor of the domain,
    // so it is scaled by factor to approximate the full conflict block.
    // (g_end - g_mid) is constant work (for example insert_clause) and is not scaled.
    uint64_t scaled_part = (g_mid - g_start) * (uint64_t)factor;
    uint64_t constant_part = (g_end - g_mid);

    gates_per_conflict_block = static_cast<double>(scaled_part + constant_part);

    return red_time;
}

struct BlockTimes
{
    double decision_time;        // yellow
    double from_u_time;          // blue
    double conflict_time;        // red
    double check_clause_time;    // orange sub 1
    double update_wl_time;       // orange sub 2
    double add_implication_time; // orange sub 3
    double regular_total_time;   // check + update + add
    double total_time;           // all blocks
};

struct BlockGateCounts
{
    uint64_t from_u_gates;
    uint64_t decision_gates;
    uint64_t conflict_gates;
    uint64_t check_clause_gates;
    uint64_t update_wl_gates;
    uint64_t add_implication_gates;
    uint64_t regular_total_gates;
    uint64_t total_gates;
};

void test_all_cdcl(int var_num,
                   int clause_num,
                   int max_num_of_con_clause,
                   int max_lit_in_clause,
                   int max_lit_in_con_clause,
                   int max_clause_in_wl,
                   int conflict_factor,
                   int orange_num,
                   int yellow_num,
                   int blue_num,
                   int red_num,
                   BlockTimes &times,
                   BlockGateCounts &gates, int uip_loop_cap = 0)
{
    init_constant(var_num, clause_num, max_num_of_con_clause,
                  max_lit_in_clause, max_lit_in_con_clause,
                  max_clause_in_wl);

    WatchList wls(base);
    VariableStateOram big_V(base);
    ClauseList cl(base);
    vector<Integer> activities;
    vector<Integer> unit_clause;
    vector<Integer> big_U;

    // Simple clause to feed ORAMs
    vector<int> lits = {1, 2, 3};
    Clause w(lits);
    cl.insert_clause(Integer(CLAUSE_IDX_SIZE_BIT, 1), w);

    Integer a_idx(VAR_SIZE_BIT, 4);
    Integer w_idx(CLAUSE_IDX_SIZE_BIT, 3);
    (void)w_idx;

    for (int i = 0; i < VAR_NUM + 1; ++i)
    {
        activities.push_back(Integer(ACTIVITY_SIZE_BIT, 2, PUBLIC));
        big_U.push_back(Integer(2, 0));
        unit_clause.push_back(Integer(CLAUSE_IDX_SIZE_BIT, 0));
    }

    // Decision block (yellow)
    auto t1 = clock_start();
    uint64_t g_start = base->num_and();
    decision(wls, big_V, activities, a_idx, big_U);
    uint64_t g_end = base->num_and();
    uint64_t g_diff = g_end - g_start;

    times.decision_time = time_from(t1) / 1e6 * yellow_num;
    // Gate counts are per block: the block runs once, so g_diff is not
    // scaled by yellow_num.
    gates.decision_gates = (g_diff);

    // from_U block (blue)
    t1 = clock_start();
    g_start = base->num_and();
    from_U(wls, big_U, a_idx);
    g_end = base->num_and();
    g_diff = g_end - g_start;

    times.from_u_time = time_from(t1) / 1e6 * blue_num;
    gates.from_u_gates = (g_diff);

    // Orange sub-blocks
    auto t_orange = clock_start();

    // check_clause
    t1 = clock_start();
    g_start = base->num_and();
    cl.phi_oram_time = 0;
    test_check_clause(cl, big_V, w);
    g_end = base->num_and();
    g_diff = g_end - g_start;
    times.check_clause_time = time_from(t1) / 1e6 * orange_num;
    gates.check_clause_gates = (g_diff);

    // update_wl
    t1 = clock_start();
    g_start = base->num_and();
    WL_ORAM_TIME = 0;
    test_update_wl(wls);
    g_end = base->num_and();
    g_diff = g_end - g_start;
    times.update_wl_time = time_from(t1) / 1e6 * orange_num;
    gates.update_wl_gates = (g_diff);

    // add_implication
    t1 = clock_start();
    g_start = base->num_and();
    test_add_implication(big_V, big_U, a_idx);
    g_end = base->num_and();
    g_diff = g_end - g_start;
    times.add_implication_time = time_from(t1) / 1e6 * orange_num;
    gates.add_implication_gates = (g_diff);

    double orange_time = time_from(t_orange) / 1e6 * orange_num;
    (void)orange_time;

    // Conflict block (red)
    double conflict_gates_per_block = 0.0;
    times.conflict_time =
        conflict(big_V, cl, activities, w, wls,
                 conflict_factor, red_num,
                 conflict_gates_per_block, uip_loop_cap);
    // per-conflict-block gates (already scaled by factor, not by red_num)
    gates.conflict_gates = conflict_gates_per_block;

    // Derived totals
    times.regular_total_time =
        times.check_clause_time +
        times.update_wl_time +
        times.add_implication_time;

    times.total_time =
        times.decision_time +
        times.from_u_time +
        times.conflict_time +
        times.regular_total_time;

    gates.regular_total_gates =
        gates.check_clause_gates +
        gates.update_wl_gates +
        gates.add_implication_gates;

    gates.total_gates =
        gates.decision_gates +
        gates.from_u_gates +
        gates.conflict_gates +
        gates.regular_total_gates;
}

int main(int argc, char **argv)
{
    if (argc < 14)
    {
        cerr << "Usage: " << argv[0]
             << " party var_num clause_num max_num_of_con_clause "
             << "max_lit_in_clause max_lit_in_con_clause max_clause_in_wl "
             << "conflict_factor orange_num yellow_num blue_num red_num "
             << "output_csv\n";
        return 1;
    }

    int argi = 1;
    int party = atoi(argv[argi++]);
    int var_num = atoi(argv[argi++]);
    int clause_num = atoi(argv[argi++]);
    int max_num_of_con_clause = atoi(argv[argi++]);
    int max_lit_in_clause = atoi(argv[argi++]);
    int max_lit_in_con_clause = atoi(argv[argi++]);
    int max_clause_in_wl = atoi(argv[argi++]);
    int conflict_factor = atoi(argv[argi++]);
    int orange_num = atoi(argv[argi++]);
    int yellow_num = atoi(argv[argi++]);
    int blue_num = atoi(argv[argi++]);
    int red_num = atoi(argv[argi++]);
    string output_csv = argv[argi++];
    int uip_loop_cap = atoi(argv[argi++]);

    init_backend(party, 24);
    base->switch_to_gt();

    BlockTimes times{};
    BlockGateCounts gates{};
    test_all_cdcl(var_num, clause_num,
                  max_num_of_con_clause,
                  max_lit_in_clause,
                  max_lit_in_con_clause,
                  max_clause_in_wl,
                  conflict_factor,
                  orange_num, yellow_num,
                  blue_num, red_num,
                  times, gates, uip_loop_cap);

    // Only ALICE writes CSV
    if (party == ALICE)
    {
        ofstream csv(output_csv, ios::app);
        if (!csv)
        {
            cerr << "Error opening CSV file: " << output_csv << endl;
        }
        else
        {
            csv << var_num << ","
                << clause_num << ","
                << max_num_of_con_clause << ","
                << max_lit_in_clause << ","
                << max_lit_in_con_clause << ","
                << max_clause_in_wl << ","
                << conflict_factor << ","
                << orange_num << ","
                << yellow_num << ","
                << blue_num << ","
                << red_num << ","
                << times.decision_time << ","
                << times.from_u_time << ","
                << times.conflict_time << ","
                << times.check_clause_time << ","
                << times.update_wl_time << ","
                << times.add_implication_time << ","
                << times.regular_total_time << ","
                << times.total_time << ","
                // Gate counts per block
                << gates.decision_gates << ","
                << gates.from_u_gates << ","
                << gates.conflict_gates << ","
                << gates.check_clause_gates << ","
                << gates.update_wl_gates << ","
                << gates.add_implication_gates << ","
                << gates.regular_total_gates << ","
                << gates.total_gates
                << "\n";
        }
    }

    delete base;
    return 0;
}
