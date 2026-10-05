#include <iostream>
#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"
#include "util.h"
#include <random>
#include <map>

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

void test_variable_state_print()
{
    std::cout << "\n=== BASIC VARIABLE STATE TEST ===\n";

    VariableState vs(base);

    std::cout << "\n--- Initial state ---\n";
    vs.print_full_state();

    std::cout << "\n--- Testing integer-indexed operations ---\n";

    std::cout << "Setting var 1 to TRUE_ASSIGNED\n";
    vs.set_assignment(1, vs.true_assigned);

    std::cout << "Setting var 2 to FALSE_ASSIGNED\n";
    vs.set_assignment(2, vs.false_assigned);

    std::cout << "Setting var 3 decision level to 5\n";
    vs.set_decision_level(3, Integer(DL_SIZE_BIT, 5, PUBLIC));

    std::cout << "Setting var 4 antecedent to 10\n";
    vs.set_antecedent(4, Integer(ANTE_SIZE_BIT, 10, PUBLIC));

    vs.print_full_state();

    Integer val_ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
    Integer dl(DL_SIZE_BIT, 0, PUBLIC);
    Integer ante(ANTE_SIZE_BIT, 0, PUBLIC);

    vs.get_assignment(1, val_ass);
    std::cout << "Var 1 assignment: " << reveal_32(val_ass) << "\n";

    vs.get_assignment(2, val_ass);
    std::cout << "Var 2 assignment: " << reveal_32(val_ass) << "\n";

    vs.get_decision_level(3, dl);
    std::cout << "Var 3 decision level: " << reveal_32(dl) << "\n";

    vs.get_antecedent(4, ante);
    std::cout << "Var 4 antecedent: " << reveal_32(ante) << "\n";

    std::cout << "\n--- Testing Integer-indexed operations ---\n";

    Integer var5(VAR_SIZE_BIT, 5, PUBLIC);
    Integer var6(VAR_SIZE_BIT, 6, PUBLIC);
    Integer var7(VAR_SIZE_BIT, 7, PUBLIC);
    Integer var8(VAR_SIZE_BIT, 8, PUBLIC);

    std::cout << "Setting var 5 to TRUE_ASSIGNED\n";
    vs.set_assignment(var5, vs.true_assigned);

    std::cout << "Setting var 6 to FALSE_ASSIGNED\n";
    vs.set_assignment(var6, vs.false_assigned);

    std::cout << "Setting var 7 decision level to 7\n";
    vs.set_decision_level(var7, Integer(DL_SIZE_BIT, 7, PUBLIC));

    std::cout << "Setting var 8 antecedent to 15\n";
    vs.set_antecedent(var8, Integer(ANTE_SIZE_BIT, 15, PUBLIC));

    vs.print_full_state();

    std::cout << "\n--- Testing higher-level operations ---\n";

    Integer lit9(VAR_SIZE_BIT, 9, PUBLIC);
    Integer lit10(VAR_SIZE_BIT, -10, PUBLIC);
    Integer dl1(VAR_SIZE_BIT, 1, PUBLIC);
    Integer dl2(VAR_SIZE_BIT, 2, PUBLIC);
    Integer ante1(ANTE_SIZE_BIT, 20, PUBLIC);
    Integer ante2(ANTE_SIZE_BIT, 25, PUBLIC);

    std::cout << "Setting unit literal 9 with dl=1, ante=20\n";
    vs.set_unit_literal(lit9, ante1, dl1);

    std::cout << "Setting unit literal -10 with dl=2, ante=25\n";
    vs.set_unit_literal(lit10, ante2, dl2);

    vs.print_full_state();

    std::cout << "\n--- Testing query methods ---\n";

    Integer var13(VAR_SIZE_BIT, 13, PUBLIC);
    std::cout << "Is var 13 unassigned? " << vs.is_variable_unassigned(var13).reveal() << "\n";
    std::cout << "Is var 1 unassigned? " << vs.is_variable_unassigned(1).reveal() << "\n";

    Integer lit1(VAR_SIZE_BIT, 1, PUBLIC);
    Integer lit_neg1(VAR_SIZE_BIT, -1, PUBLIC);
    std::cout << "Is literal 1 satisfied? " << vs.is_literal_satisfied(lit1).reveal() << "\n";
    std::cout << "Is literal -1 falsified? " << vs.is_literal_falsified(lit_neg1).reveal() << "\n";

    std::cout << "\n--- Testing clean_up_after_backtrack ---\n";
    std::cout << "Current decision level: " << reveal_32(vs.cur_dl) << "\n";
    std::cout << "Setting current decision level to 2\n";
    vs.cur_dl = Integer(DL_SIZE_BIT, 2, PUBLIC);
    std::cout << "Cleaning up variables above decision level 2\n";
    vs.clean_up_after_backtrack();

    std::cout << "State after cleanup:\n";
    vs.print_full_state();

    std::cout << "\n=== BASIC VARIABLE STATE TEST COMPLETE ===\n";
}

void test_variable_state_randomized()
{
    std::cout << "\n=== RANDOMIZED VARIABLE STATE TEST ===\n";

    // Use smaller test sizes for faster testing, but respect VAR_NUM limit
    const int max_test_vars = std::min(VAR_NUM, 50);
    const int max_test_clauses = std::min(CLAUSE_NUM, 50);
    const int max_test_conflicts = std::min(MAX_NUM_OF_CONFLICT_CLAUSE, 20);

    VariableStateOram vs(base);

    std::map<int, int> expected_assignments;
    std::map<int, int> expected_decision_levels;
    std::map<int, int> expected_antecedents;

    std::mt19937 gen(42 + VAR_NUM + CLAUSE_NUM + MAX_NUM_OF_CONFLICT_CLAUSE); // Fixed seed for reproducibility
    std::uniform_int_distribution<> var_dist(1, VAR_NUM);
    std::uniform_int_distribution<> assignment_dist(NOT_ASSIGNED, TRUE_ASSIGNED);
    std::uniform_int_distribution<> dl_dist(0, VAR_NUM);
    // Use clause limits for antecedent range
    std::uniform_int_distribution<> ante_dist(-MAX_NUM_OF_CONFLICT_CLAUSE, CLAUSE_NUM);
    std::uniform_int_distribution<> sign_dist(0, 1);

    std::cout << "\n--- Testing random variable assignments ---\n";

    // Scale operations based on available variables
    int num_operations = std::min(100, max_test_vars * 3);

    for (int i = 0; i < num_operations; i++)
    {
        int var = var_dist(gen);
        int assignment = assignment_dist(gen);
        int dl = dl_dist(gen);
        int ante = ante_dist(gen);

        // Randomly index by int or by Integer
        if (sign_dist(gen))
        {
            // Index by int
            vs.set_assignment(var, Integer(ASSIGNMENT_SIZE_BIT, assignment, PUBLIC));
            vs.set_decision_level(var, Integer(DL_SIZE_BIT, dl, PUBLIC));
            vs.set_antecedent(var, Integer(ANTE_SIZE_BIT, ante, PUBLIC));
        }
        else
        {
            // Index by Integer
            Integer var_int(VAR_SIZE_BIT, var, PUBLIC);
            vs.set_assignment(var_int, Integer(ASSIGNMENT_SIZE_BIT, assignment, PUBLIC));
            vs.set_decision_level(var_int, Integer(DL_SIZE_BIT, dl, PUBLIC));
            vs.set_antecedent(var_int, Integer(ANTE_SIZE_BIT, ante, PUBLIC));
        }

        expected_assignments[var] = assignment;
        expected_decision_levels[var] = dl;
        expected_antecedents[var] = ante;
    }

    std::cout << "\n--- Verifying variable states ---\n";

    bool all_correct = true;
    int errors = 0;

    for (const auto &pair : expected_assignments)
    {
        int var = pair.first;
        int expected_assignment = pair.second;
        int expected_dl = expected_decision_levels[var];
        int expected_ante = expected_antecedents[var];

        // Check both with int and Integer indices
        Integer var_int(VAR_SIZE_BIT, var, PUBLIC);

        // Get values using int index
        Integer ass_int(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        Integer dl_int(DL_SIZE_BIT, 0, PUBLIC);
        Integer ante_int(ANTE_SIZE_BIT, 0, PUBLIC);

        vs.get_assignment(var, ass_int);
        vs.get_decision_level(var, dl_int);
        vs.get_antecedent(var, ante_int);

        int retrieved_ass_int = reveal_32(ass_int, true);
        int retrieved_dl_int = reveal_32(dl_int, true);
        int retrieved_ante_int = reveal_32(ante_int);

        // Get values using Integer index
        Integer ass_Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        Integer dl_Integer(DL_SIZE_BIT, 0, PUBLIC);
        Integer ante_Integer(ANTE_SIZE_BIT, 0, PUBLIC);

        vs.get_assignment(var_int, ass_Integer);
        vs.get_decision_level(var_int, dl_Integer);
        vs.get_antecedent(var_int, ante_Integer);

        int retrieved_ass_Integer = reveal_32(ass_Integer, true);
        int retrieved_dl_Integer = reveal_32(dl_Integer, true);
        int retrieved_ante_Integer = reveal_32(ante_Integer);

        string retrieved_ass_str = reveal_int_details(ass_Integer);
        string retrieved_dl_str = reveal_int_details(dl_Integer);
        string retrieved_ante_str = reveal_int_details(ante_Integer);

        bool correct =
            (retrieved_ass_int == expected_assignment) &&
            (retrieved_dl_int == expected_dl) &&
            (retrieved_ante_int == expected_ante) &&
            (retrieved_ass_Integer == expected_assignment) &&
            (retrieved_dl_Integer == expected_dl) &&
            (retrieved_ante_Integer == expected_ante);

        std::cout << "Var " << var << ": ";
        if (correct)
        {
            std::cout << "OK\n";
        }
        else
        {
            std::cout << "ERROR\n";
            std::cout << "  Expected: ass=" << expected_assignment << ", dl=" << expected_dl << ", ante=" << expected_ante << "\n";
            std::cout << "  Retrieved (int): ass=" << retrieved_ass_int << ", dl=" << retrieved_dl_int << ", ante=" << retrieved_ante_int << "\n";
            std::cout << "  Retrieved (Integer): ass=" << retrieved_ass_Integer << ", dl=" << retrieved_dl_Integer << ", ante=" << retrieved_ante_Integer << "\n";
            std::cout << "ass str" << retrieved_ass_str << "\n";
            std::cout << "dl str" << retrieved_dl_str << "\n";
            std::cout << "ante str" << retrieved_ante_str << "\n";
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Testing unit literals and decisions ---\n";

    int unit_test_count = std::min(10, max_test_vars / 3);

    // Set unit literals with both positive and negative polarities
    for (int i = 1; i <= unit_test_count; i++)
    {
        // Use variables from 1 to unit_test_count to avoid exceeding limits
        int var_idx = i;
        int lit = (i % 2 == 0) ? var_idx : -var_idx;
        int dl = i % max_test_vars;
        int ante = i % max_test_clauses;

        std::cout << "Setting unit literal " << lit << " with dl=" << dl << ", ante=" << ante << "\n";

        Integer lit_int(VAR_SIZE_BIT, lit, PUBLIC);
        Integer dl_int(DL_SIZE_BIT, dl, PUBLIC);
        Integer ante_int(ANTE_SIZE_BIT, ante, PUBLIC);

        vs.set_unit_literal(lit_int, ante_int, dl_int);

        Integer var_int(VAR_SIZE_BIT, abs(lit), PUBLIC);
        Integer ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        Integer var_dl(DL_SIZE_BIT, 0, PUBLIC);
        Integer var_ante(ANTE_SIZE_BIT, 0, PUBLIC);

        vs.get_assignment(var_int, ass);
        vs.get_decision_level(var_int, var_dl);
        vs.get_antecedent(var_int, var_ante);

        int expected_assignment = (lit > 0) ? TRUE_ASSIGNED : FALSE_ASSIGNED;
        int retrieved_assignment = reveal_32(ass, true);
        int retrieved_dl = reveal_32(var_dl, true);
        int retrieved_ante = reveal_32(var_ante);

        bool correct =
            (retrieved_assignment == expected_assignment) &&
            (retrieved_dl == dl) &&
            (retrieved_ante == ante);

        std::cout << "  Verification: ";
        if (correct)
        {
            std::cout << "OK\n";
        }
        else
        {
            std::cout << "ERROR\n";
            std::cout << "    Expected: ass=" << expected_assignment << ", dl=" << dl << ", ante=" << ante << "\n";
            std::cout << "    Retrieved: ass=" << retrieved_assignment << ", dl=" << retrieved_dl << ", ante=" << retrieved_ante << "\n";
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Testing backtracking ---\n";

    int backtrack_dl = std::max(2, max_test_vars / 4);

    std::cout << "Setting variables with different decision levels\n";
    int backtrack_test_count = std::min(10, max_test_vars / 2);
    vector<pair<int, int>> assigned_backtrack_var;

    for (int i = 1; i <= backtrack_test_count; i++)
    {
        int var = var_dist(gen);
        // Ensure some will be above and some below backtrack_dl
        int dl = (i % 2) ? i : i + backtrack_dl;

        std::cout << "  Setting var " << var << " at decision level " << dl << "\n";
        vs.set_assignment(var, vs.true_assigned);
        vs.set_decision_level(var, Integer(DL_SIZE_BIT, dl, PUBLIC));
        assigned_backtrack_var.push_back({var, dl});
    }

    std::cout << "Backtracking to decision level " << backtrack_dl << "\n";
    vs.cur_dl = Integer(DL_SIZE_BIT, backtrack_dl, PUBLIC);
    vs.clean_up_after_backtrack();

    // Verify that the right variables were reset
    for (auto p : assigned_backtrack_var)
    {
        int var = p.first;
        int dl = p.second;
        bool should_be_reset = (dl >= backtrack_dl);
        Integer ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        vs.get_assignment(var, ass);
        int retrieved_ass = reveal_32(ass, true);

        bool is_reset = (retrieved_ass == NOT_ASSIGNED);
        bool correct = (should_be_reset == is_reset);

        std::cout << "  Var " << var << " (dl=" << dl << "): ";
        if (correct)
        {
            std::cout << "OK" << (should_be_reset ? " (was reset)" : " (kept)") << "\n";
        }
        else
        {
            std::cout << "ERROR\n";
            std::cout << "    Should be reset: " << should_be_reset << "\n";
            std::cout << "    Is reset: " << is_reset << "\n";
            std::cout << "   ass: " << reveal_int_details(ass) << "\n";
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Testing literal status predicates ---\n";

    int status_var1 = var_dist(gen);
    int status_var2 = var_dist(gen);
    int status_var3 = var_dist(gen);

    vs.set_assignment(status_var1, vs.true_assigned);
    vs.set_assignment(status_var2, vs.false_assigned);
    vs.set_assignment(status_var3, vs.not_assigned);

    // Test positive literals
    Integer pos_var1(VAR_SIZE_BIT, status_var1, PUBLIC);
    Integer pos_var2(VAR_SIZE_BIT, status_var2, PUBLIC);
    Integer pos_var3(VAR_SIZE_BIT, status_var3, PUBLIC);

    // Test negative literals
    Integer neg_var1(VAR_SIZE_BIT, -status_var1, PUBLIC);
    Integer neg_var2(VAR_SIZE_BIT, -status_var2, PUBLIC);
    Integer neg_var3(VAR_SIZE_BIT, -status_var3, PUBLIC);

    std::cout << "  Testing satisfied literals:\n";
    std::cout << "    Literal " << status_var1 << " satisfied: " << vs.is_literal_satisfied(pos_var1).reveal() << " (should be 1)\n";
    std::cout << "    Literal -" << status_var2 << " satisfied: " << vs.is_literal_satisfied(neg_var2).reveal() << " (should be 1)\n";
    std::cout << "    Literal " << status_var2 << " satisfied: " << vs.is_literal_satisfied(pos_var2).reveal() << " (should be 0)\n";
    std::cout << "    Literal -" << status_var1 << " satisfied: " << vs.is_literal_satisfied(neg_var1).reveal() << " (should be 0)\n";

    std::cout << "  Testing falsified literals:\n";
    std::cout << "    Literal " << status_var2 << " falsified: " << vs.is_literal_falsified(pos_var2).reveal() << " (should be 1)\n";
    std::cout << "    Literal -" << status_var1 << " falsified: " << vs.is_literal_falsified(neg_var1).reveal() << " (should be 1)\n";
    std::cout << "    Literal " << status_var1 << " falsified: " << vs.is_literal_falsified(pos_var1).reveal() << " (should be 0)\n";
    std::cout << "    Literal -" << status_var2 << " falsified: " << vs.is_literal_falsified(neg_var2).reveal() << " (should be 0)\n";

    std::cout << "  Testing unassigned literals:\n";
    std::cout << "    Var " << status_var3 << " unassigned: " << vs.is_variable_unassigned(pos_var3).reveal() << " (should be 1)\n";
    std::cout << "    Var " << status_var1 << " unassigned: " << vs.is_variable_unassigned(pos_var1).reveal() << " (should be 0)\n";

    if (all_correct)
    {
        std::cout << "\n=== VARIABLE STATE TEST PASSED! ===\n";
    }
    else
    {
        std::cout << "\n=== VARIABLE STATE TEST FAILED WITH " << errors << " ERRORS ===\n";
    }
}

void test_vector_read(int num_elements = 100)
{
    vector<Integer> vec;
    for (int i = 0; i < num_elements; i++)
    {
        vec.push_back(Integer(32, i, PUBLIC));
    }
    for (size_t i = 0; i < num_elements; i++)
    {
        Integer idx = Integer(32, i, PUBLIC);
        Integer val = Integer(32, 0, PUBLIC);
        read_vector(vec, idx, val);
        if (val.reveal<int>() != i)
        {
            throw std::runtime_error("Error in vector read");
        }
    }
}

void test_vector_write(int num_elements = 100)
{
    vector<Integer> vec;
    vec.resize(num_elements, Integer(32, 0, PUBLIC));

    for (size_t i = 0; i < num_elements; i++)
    {
        Integer idx = Integer(32, i, PUBLIC);
        Integer val = Integer(32, i, PUBLIC);
        assign_vector(vec, idx, val, Bit(i % 2));
    }

    for (size_t i = 0; i < num_elements; i++)
    {
        if (vec[i].reveal<int>() != ((i % 2) ? i : 0))
        {
            throw std::runtime_error("Error in vector write");
        }
    }
}

int main(int argc, char **argv)
{
    int party = atoi(argv[1]);
    int depth = 12;
    int unit_size = 6;
    int threads = 1;
    int var_num = 2000;
    int clause_num = 1000;
    int con_num = 200;
    int max_literals = 10;
    int max_con_literals = 10;
    int max_wl_size = 5;

    init_backend(party, depth, unit_size, threads);
    init_constant(var_num, clause_num, con_num, max_literals, max_con_literals, max_wl_size);
    base->switch_to_gt();

    test_variable_state_randomized();

    delete base;
    return 0;
}
