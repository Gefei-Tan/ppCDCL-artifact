#include <iostream>
#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"
#include "emp-dpf/emp-dpf.h"

FloramMPC<NetIO> *base;

void init_backend(int party, int depth = 25, int size_of_element = 7, int threads = 2, int round_key_size = 94208, const char *addr = "127.0.0.1")
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
    vector<Integer> non_falsified_literals = {Integer(VAR_SIZE_BIT, 0), Integer(VAR_SIZE_BIT, 0)};
    for (int i = 0; i < MAX_LIT_IN_CLAUSE; i++)
    {
        Integer l = Integer(VAR_SIZE_BIT, 1);
        Bit if_l_is_not_falsified = !big_V.is_literal_falsified(l);
        Bit if_l_is_not_fresh_falsified = !l.equal(l);
        Bit is_valid_literal = if_l_is_not_falsified & if_l_is_not_fresh_falsified;

        Bit is_first_zero = non_falsified_literals[0] == Integer(VAR_SIZE_BIT, 0);
        Bit is_second_zero = non_falsified_literals[1] == non_falsified_literals[0];

        non_falsified_literals[0] = If(is_first_zero & is_valid_literal,
                                       l,
                                       non_falsified_literals[0]);

        // The second position is updated while it still equals the first (both zero, or only one found so far)
        non_falsified_literals[1] = If(is_second_zero & is_valid_literal,
                                       l,
                                       non_falsified_literals[1]);
    }
}

// 3 WL ops: 1 add, 1 remove (read+write)
void test_update_wl(WatchList &wls)
{

    wls.add_clause(Integer(VAR_SIZE_BIT, 3), Integer(CLAUSE_IDX_SIZE_BIT, 1), true);
    wls.remove_clause(Integer(VAR_SIZE_BIT, 1), Integer(CLAUSE_IDX_SIZE_BIT, 1), true);
}

// 1 V READ, 3 V WRITE, one big_U push
void test_add_implication(VariableStateOram &big_V, vector<Integer> &big_U, Integer &a_idx)
{
    auto t1 = clock_start();
    big_V.is_variable_unassigned(a_idx);
    big_V.set_unit_literal(a_idx, Integer(ANTE_SIZE_BIT, 1), Integer(DL_SIZE_BIT, 1), true);
    t1 = clock_start();
    Integer val = If(a_idx >= Integer(VAR_SIZE_BIT, 0), Integer(2, 1), Integer(2, 2));
    assign_vector(big_U, a_idx.abs(), val);
}

// 1 BIG_U pop, 1 WL read, 1 VAR_NUM linear scan
void from_U(WatchList &wls, vector<Integer> &big_U, Integer &a_idx)
{
    Bit flag_found = false;
    for (size_t i = 0; i < VAR_NUM + 1; ++i)
    {
        Bit flag_1 = big_U[i] == Integer(2, 1);
        Bit flag_2 = big_U[i] == Integer(2, 2);
        a_idx = If(flag_1, Integer(VAR_SIZE_BIT, i), If(flag_2, Integer(VAR_SIZE_BIT, i), a_idx));
        big_U[i] = If((flag_1 | flag_2) & !flag_found, Integer(2, 0), big_U[i]);
        flag_found = flag_found | flag_1 | flag_2;
    }
    auto wl = wls.get_watchlist_clauses(a_idx);
}

// 3 V write, 1 WL read, 1 scan #VAR_NUM, #VAR_NUM V read
void decision(WatchList &wls, VariableStateOram &big_V, vector<Integer> &activities, Integer &a_idx,
              vector<Integer> &big_U)
{
    Integer max_score(ACTIVITY_SIZE_BIT, 0);
    for (auto &i : activities)
    {
        max_score = If(max_score < i, i, max_score);
        Integer dummy(VAR_SIZE_BIT, 0);
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
    auto wl = wls.get_watchlist_clauses(a_idx);
}

// #VAR_NUM loop: 2#VAR_NUM V read + 1 CL read
// outside loop: #VAR_NUM V write
double conflict(VariableStateOram &big_V, ClauseList &cl, vector<Integer> &activities, Clause &w, int factor = 1,
                int red_num = 1)
{
    Integer dl;
    Integer ante = Integer(ANTE_SIZE_BIT, 0);
    auto t1 = clock_start();
    for (int i = 0; i < (VAR_NUM + 1) / factor; ++i)
    {
        // Fetch the antecedent clause at the start of each iteration
        cl.get_clause(Integer(CLAUSE_IDX_SIZE_BIT, 1), w);
    }

    for (int i = 0; i < MAX_LIT_IN_CLAUSE * (VAR_NUM + 1) / factor; ++i)
    {
        big_V.get_antecedent(1, ante);
        big_V.get_decision_level(1, dl);
    }


    for (int i = 0; i < (VAR_NUM + 1) / factor; ++i)
    {
        big_V.get_decision_level(1, dl);
    }

    for (int i = 0; i < activities.size() / factor; ++i)
    {
        activities[i] = activities[i] + Integer(ACTIVITY_SIZE_BIT, 1);
    }


    Integer a = Integer(VAR_SIZE_BIT, 1);
    Integer tmp = Integer(1 << DL_ORAM_BLOCK_SIZE, 1);
    big_V.clean_up_after_backtrack(factor);

    cl.insert_clause(Integer(CON_CLAUSE_IDX_SIZE_BIT, -2), w);
    double red_time = time_from(t1) * factor / 1000.0 / 1000.0 * red_num;
    return red_time;
}

void test_all_cdcl(int var_num, int clause_num, int max_num_of_con_clause, int max_lit_in_clause, int max_lit_in_con_clause, int max_clause_in_wl,
                   int conflict_factor = 1,
                   int orange_num = 1, int yellow_num = 1, int blue_num = 1, int red_num = 1)
{
    init_constant(var_num, clause_num, max_num_of_con_clause, max_lit_in_clause, max_lit_in_con_clause, max_clause_in_wl);

    WatchList wls(base);
    VariableStateOram big_V(base);
    ClauseList cl(base);
    vector<Integer> activities;
    vector<Integer> unit_clause; // <literal, clause_idx>
    vector<Integer> big_U;       // 0 if not a unit literal, 1 if positive, 2 if negative

    vector<int> lits = {1, 2, 3};
    Clause w(lits);

    Integer a_idx(VAR_SIZE_BIT, 4);        // The literal visited in the current giant step
    Integer w_idx(CLAUSE_IDX_SIZE_BIT, 3); // The clause visited in the current giant step

    for (int i = 0; i < VAR_NUM + 1; ++i)
    {
        activities.push_back(Integer(ACTIVITY_SIZE_BIT, 2, PUBLIC));
        big_U.push_back(Integer(2, 0));
        unit_clause.push_back(Integer(CLAUSE_IDX_SIZE_BIT, 0));
    }

    auto t1 = clock_start();

    decision(wls, big_V, activities, a_idx, big_U);
    double yellow_time = time_from(t1) / 1000.0 / 1000.0 * yellow_num;

    t1 = clock_start();
    from_U(wls, big_U, a_idx);
    double blue_time = time_from(t1) / 1000.0 / 1000.0 * blue_num;

    auto t_orange = clock_start();
    t1 = clock_start();
    cl.phi_oram_time = 0;
    test_check_clause(cl, big_V, w);
    auto check_clause_time = time_from(t1) / 1000.0 / 1000.0 * orange_num;
    t1 = clock_start();
    WL_ORAM_TIME = 0;
    test_update_wl(wls);
    auto update_wl_time = time_from(t1) / 1000.0 / 1000.0 * orange_num;

    t1 = clock_start();
    test_add_implication(big_V, big_U, a_idx);
    auto add_implication_time = time_from(t1) / 1000.0 / 1000.0 * orange_num;

    double orange_time = time_from(t_orange) / 1000.0 / 1000.0 * orange_num;

    double red_time = conflict(big_V, cl, activities, w, conflict_factor, red_num);

    cout << "|total block time: " << yellow_time + blue_time + red_time + orange_time << "s\n";
    cout << "|    |____conflict time: " << red_time << "s    #" << red_num << "\n";
    cout << "|    |____decision time: " << yellow_time << "s    #" << yellow_num << "\n";
    cout << "|    |____from_U time: " << blue_time << "s    #" << blue_num << "\n";
    cout << "|    |____core time: " << orange_time << "s    #" << orange_num << "\n";
    cout << "|    |    |____test_check_clause time: " << check_clause_time << "s\n";
    cout << "|    |    |____test_update_wl time: " << update_wl_time << "s\n";
    cout << "|    |    |____test_add_implication time: " << add_implication_time << "s\n";
    cout << yellow_time + blue_time + red_time + orange_time
         << " " << yellow_time << " " << blue_time << " " << red_time << " " << orange_time
         << " " << check_clause_time << " " << update_wl_time << " " << add_implication_time
         << "\n";
}

int main(int argc, char **argv)
{
    int party = atoi(argv[1]);
    init_backend(party, 24);
    base->switch_to_gt();

    int var_num = atoi(argv[2]);
    int clause_num = atoi(argv[3]);
    int max_num_of_con_clause = atoi(argv[4]);
    int max_lit_in_clause = atoi(argv[5]);
    int max_lit_in_con_clause = atoi(argv[6]);
    int max_clause_in_wl = atoi(argv[7]);
    int conflict_factor = 1;
    int orange_num = atoi(argv[8]);
    int yellow_num = atoi(argv[9]);
    int blue_num = atoi(argv[10]);
    int red_num = atoi(argv[11]);

    test_all_cdcl(var_num, clause_num, max_num_of_con_clause, max_lit_in_clause, max_lit_in_con_clause, max_clause_in_wl,
                   conflict_factor, orange_num, yellow_num, blue_num, red_num);

    delete base;

    return 0;
}
