#include <iostream>
#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"
#include "src/watchlist.h"

FloramMPC<NetIO> *base;

void init_backend(int party, int depth, int size_of_element, int threads = 2, int round_key_size = 94208, const char *addr = "127.0.0.1")
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

void test_clause_list_print()
{
    ClauseList cl(base);
    vector<int> clause = {1, 2, -3, 4, -5};
    vector<int> clause2 = {-1, 2, -3, 4, 5};
    vector<int> clause3 = {-2, -3, 4};
    vector<int> clause4 = {0};
    vector<int> clause5 = {1};
    vector<int> con_clause = {1, 3, 4};
    vector<int> con_clause1 = {-3, -5, 4};

    Clause c(clause);
    Clause c2(clause2);
    Clause c3(clause3);
    Clause c4(clause4);
    Clause c5(clause5);
    Clause con_c(con_clause);
    Clause con_c1(con_clause1);

    Clause retrieved_c, retrieved_c2, retrieved_c5, retrieved_con_c, retrieved_con_c1;

    std::cout << "\n=== TESTING VECTOR-BASED CLAUSE IMPLEMENTATION ===\n";
    cl.print("Before insert:\n");

    std::cout << "\n--- Inserting clauses ---\n";
    cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, 1), c);
    cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, 2), c2);
    cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, 3), c3);
    cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, 4), c4);
    cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, 5), c5);
    cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, -1), con_c);
    cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, -2), con_c1);
    cl.print("After insert:\n");

    std::cout << "\n--- Retrieving and verifying clauses ---\n";
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, 1), retrieved_c);
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, 2), retrieved_c2);
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, 5), retrieved_c5);
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, -1), retrieved_con_c);
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, -2), retrieved_con_c1);

    std::cout << "Original clause 1: ";
    c.print();
    std::cout << "Retrieved clause 1: ";
    retrieved_c.print();

    std::cout << "Original clause 2: ";
    c2.print();
    std::cout << "Retrieved clause 2: ";
    retrieved_c2.print();

    std::cout << "Original clause 5: ";
    c5.print();
    std::cout << "Retrieved clause 5: ";
    retrieved_c5.print();

    std::cout << "Original conflict clause -1: ";
    con_c.print();
    std::cout << "Retrieved conflict clause -1: ";
    retrieved_con_c.print();

    std::cout << "Original conflict clause -2: ";
    con_c1.print();
    std::cout << "Retrieved conflict clause -2: ";
    retrieved_con_c1.print();

    std::cout << "\n--- Deleting clauses ---\n";
    cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, 1), Bit(true));
    cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, 2), Bit(false));
    cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, 3), Bit(true));
    cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, -1), Bit(true));
    cl.print("After delete:\n");

    std::cout << "\n--- Verifying deletion ---\n";
    Clause empty_c1, empty_c2, empty_c3, empty_con_c;
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, 1), empty_c1);
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, 2), empty_c2);
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, 3), empty_c3);
    cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, -1), empty_con_c);

    std::cout << "Deleted clause 1 should be empty: ";
    empty_c1.print();
    std::cout << "Deleted clause 2 should still exist (flag=false): ";
    empty_c2.print();
    std::cout << "Deleted clause 3 should be empty: ";
    empty_c3.print();
    std::cout << "Deleted conflict clause -1 should be empty: ";
    empty_con_c.print();
}

void test_oram(int oram_size = 8, int unit_size = 7, int stash_size = -1, bool pub_write = false, bool pub_read = false,
               int limit = -1)
{
    if (limit == -1)
        limit = (1 << oram_size);
    FloRAM<NetIO> *phi_oram; // used for clauses with index >= 0
    phi_oram = new FloRAM<NetIO>(base, oram_size, unit_size, stash_size);
    Integer one(1 << unit_size, 1, PUBLIC);
    for (int i = 0; i < limit; ++i)
    {
        std::cout << "write: " << i << std::endl;
        if (pub_write)
        {
            phi_oram->write(i, [&](const Integer &in, Integer &out)
                            { out = one; });
        }
        else
        {
            phi_oram->write(Integer(oram_size, i), [&](const Integer &in, Integer &out)
                            { out = one; });
        }
    }

    std::cout << "finish writing\n";

    for (int i = 0; i < limit; ++i)
    {
        Integer block_phi(1 << unit_size, 0);
        std::cout << "read: " << i << std::endl;
        if (pub_read)
        {
            phi_oram->read(i, block_phi);
        }
        else
        {
            phi_oram->read(Integer(oram_size, i), block_phi);
        }
        if (1 != block_phi.reveal<int64_t>())
        {
            std::cout << "block " << i << " : " << block_phi.reveal<int64_t>() << "\n";
            throw std::runtime_error("ORAM consistency error");
        }
    }

    delete phi_oram;
}

void test_clause_oram_plain(int run = 1)
{
    for (int j = 0; j < run; ++j)
    {
        ClauseList cl(base);
        for (int i = 0; i < CLAUSE_NUM + 1; ++i)
        {
            Clause c(vector<int>{1, -2, 3});
            Clause c_ret;
            Integer idx = Integer(PHI_ORAM_SIZE_BIT, i);
            cl.phi_oram->write(idx,
                               [&](const Integer &in, Integer &out)
                               {
                                   out = Integer(1 << CLAUSE_ORAM_UNIT_SIZE, 1);
                               });
            Integer ret;
            cl.phi_oram->read(Integer(PHI_ORAM_SIZE_BIT, i), ret);
            std::cout << "Clause " << i << ": ";
            std::cout << ret.reveal<int64_t>() << "\n";
        }
        for (int i = 0; i < CLAUSE_NUM; ++i)
        {
            Clause c(vector<int>{1, -2, 3});
            Clause c_ret;
            cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, -(i + 1)), c, Bit(true));
            cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, -(i + 1)), c_ret);
            std::cout << "Clause " << i << ": ";
            c_ret.print();
        }
    }
}

void test_clause_list_randomized()
{
    int max_var_num = VAR_NUM;
    int phi_max_literals = MAX_LIT_IN_PHI_CLAUSE;
    int con_max_literals = MAX_LIT_IN_CON_CLAUSE;
    int test_phi_clauses = std::min(CLAUSE_NUM, 200);
    int test_con_clauses = std::min(MAX_NUM_OF_CONFLICT_CLAUSE, 200);

    std::cout << "\n=== RANDOMIZED CLAUSE LIST TEST ===\n";
    std::cout << "Testing with " << CLAUSE_NUM << " clauses, " << max_var_num
              << " variables, max " << phi_max_literals << " literals per phi clause"
              << ", max " << con_max_literals << " literals per conflict clause\n";

    ClauseList cl(base);

    std::map<int, std::vector<int>> original_clauses;
    std::map<int, std::vector<int>> original_conflict_clauses;

    // Seed with a fixed value to ensure determinism
    std::mt19937 gen(42 + CLAUSE_NUM + max_var_num + phi_max_literals + con_max_literals);
    std::uniform_int_distribution<> var_dist(1, max_var_num);
    std::uniform_int_distribution<> sign_dist(0, 1);
    std::uniform_int_distribution<> phi_size_dist(1, phi_max_literals);
    std::uniform_int_distribution<> con_size_dist(1, con_max_literals);

    std::cout << "\n--- Generating and inserting random clauses ---\n";

    // Generate and insert regular clauses (positive indices)
    for (int i = 1; i <= test_phi_clauses; i++)
    {
        int clause_size = phi_size_dist(gen);

        std::vector<int> literals;
        std::set<int> used_vars; // To avoid duplicate variables with different signs

        while (literals.size() < clause_size)
        {
            int var = var_dist(gen);
            if (used_vars.find(var) == used_vars.end())
            {
                int literal = sign_dist(gen) ? var : -var;
                literals.push_back(literal);
                used_vars.insert(var);
            }
        }

        original_clauses[i] = literals;

        Clause c(literals);
        std::cout << "inserted clause " << i << ": ";
        c.print();
        std::cout << "plain clause: ";
        for (auto lit : literals)
            std::cout << lit << " ";
        std::cout << "\n";

        cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, i), c);

        std::cout << "Inserted clause " << i << ": ";
        for (auto lit : literals)
            std::cout << lit << " ";
        std::cout << "\n";
    }

    // Generate and insert conflict clauses (negative indices)
    for (int i = 1; i <= test_con_clauses; i++)
    {
        int clause_size = con_size_dist(gen);

        std::vector<int> literals;
        std::set<int> used_vars;

        while (literals.size() < clause_size)
        {
            int var = var_dist(gen);
            if (used_vars.find(var) == used_vars.end())
            {
                int literal = sign_dist(gen) ? var : -var;
                literals.push_back(literal);
                used_vars.insert(var);
            }
        }

        original_conflict_clauses[i] = literals;

        Clause c(literals);
        cl.insert_clause(Integer(CON_ORAM_SIZE_BIT, -i), c);

        std::cout << "Inserted conflict clause -" << i << ": ";
        for (auto lit : literals)
            std::cout << lit << " ";
        std::cout << "\n";
    }

    std::cout << "\n--- Retrieving and verifying clauses ---\n";
    bool all_correct = true;
    int errors = 0;

    // Verify regular clauses
    for (int i = 1; i <= test_phi_clauses; i++)
    {
        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, i), retrieved);

        std::vector<int> retrieved_literals;
        for (const auto &lit : retrieved.literals)
        {
            int value = reveal_32(lit);
            if (value != 0)
            {
                retrieved_literals.push_back(value);
            }
        }

        std::sort(retrieved_literals.begin(), retrieved_literals.end());
        auto original = original_clauses[i];
        std::sort(original.begin(), original.end());

        bool matches = (retrieved_literals == original);
        if (!matches)
        {
            std::cout << "ERROR: Clause " << i << " does not match original!\n";
            std::cout << "  Original: ";
            for (auto lit : original)
                std::cout << lit << " ";
            std::cout << "\n  Retrieved: ";
            for (auto lit : retrieved_literals)
                std::cout << lit << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
            return;
        }
    }

    // Verify conflict clauses
    for (int i = 1; i <= test_con_clauses; i++)
    {
        Clause retrieved;
        cl.get_clause(Integer(CON_ORAM_SIZE_BIT, -i), retrieved);

        std::vector<int> retrieved_literals;
        for (const auto &lit : retrieved.literals)
        {
            int value = reveal_32(lit);
            if (value != 0)
            {
                retrieved_literals.push_back(value);
            }
        }

        std::sort(retrieved_literals.begin(), retrieved_literals.end());
        auto original = original_conflict_clauses[i];
        std::sort(original.begin(), original.end());

        bool matches = (retrieved_literals == original);
        if (!matches)
        {
            std::cout << "ERROR: Conflict clause -" << i << " does not match original!\n";
            std::cout << "  Original: ";
            for (auto lit : original)
                std::cout << lit << " ";
            std::cout << "\n  Retrieved: ";
            for (auto lit : retrieved_literals)
                std::cout << lit << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Deleting clauses ---\n";
    std::set<int> deleted_clauses;
    std::set<int> deleted_conflict_clauses;

    // Delete about half the regular clauses
    for (int i = 1; i <= test_phi_clauses; i += 2)
    {
        cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, i), Bit(true));
        deleted_clauses.insert(i);
        std::cout << "Deleted clause " << i << "\n";
    }

    // Delete about half the conflict clauses
    for (int i = 1; i <= test_con_clauses; i += 2)
    {
        cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, -i), Bit(true));
        deleted_conflict_clauses.insert(i);
        std::cout << "Deleted conflict clause -" << i << "\n";
    }

    std::cout << "\n--- Verifying deletion ---\n";

    // Check deleted regular clauses
    for (int idx : deleted_clauses)
    {
        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, idx), retrieved);

        bool is_empty = true;
        for (const auto &lit : retrieved.literals)
        {
            if (reveal_32(lit) != 0)
            {
                is_empty = false;
                break;
            }
        }

        if (!is_empty)
        {
            std::cout << "ERROR: Clause " << idx << " was not properly deleted!\n";
            std::cout << "  Retrieved: ";
            retrieved.print();
            all_correct = false;
            errors++;
        }
    }

    // Check deleted conflict clauses
    for (int idx : deleted_conflict_clauses)
    {
        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, -idx), retrieved);

        bool is_empty = true;
        for (const auto &lit : retrieved.literals)
        {
            if (reveal_32(lit) != 0)
            {
                is_empty = false;
                break;
            }
        }

        if (!is_empty)
        {
            std::cout << "ERROR: Conflict clause -" << idx << " was not properly deleted!\n";
            std::cout << "  Retrieved: ";
            retrieved.print();
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Verifying non-deleted clauses ---\n";

    // Check non-deleted regular clauses
    for (int i = 1; i <= test_phi_clauses; i++)
    {
        if (deleted_clauses.find(i) != deleted_clauses.end())
            continue;

        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, i), retrieved);

        std::vector<int> retrieved_literals;
        for (const auto &lit : retrieved.literals)
        {
            int value = reveal_32(lit);
            if (value != 0)
            {
                retrieved_literals.push_back(value);
            }
        }

        std::sort(retrieved_literals.begin(), retrieved_literals.end());
        auto original = original_clauses[i];
        std::sort(original.begin(), original.end());

        bool matches = (retrieved_literals == original);
        if (!matches)
        {
            std::cout << "ERROR: Clause " << i << " was modified after deletion operations!\n";
            std::cout << "  Original: ";
            for (auto lit : original)
                std::cout << lit << " ";
            std::cout << "\n  Retrieved: ";
            for (auto lit : retrieved_literals)
                std::cout << lit << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
        }
    }

    if (all_correct)
    {
        std::cout << "\n=== CLAUSE LIST TEST PASSED! ===\n";
    }
    else
    {
        std::cout << "\n=== CLAUSE LIST TEST FAILED WITH " << errors << " ERRORS ===\n";
    }
}

void test_clause_list_deterministic(int num_clauses = 10, int max_var_num = 20, int max_literals = 5)
{
    int var_num = max_var_num;
    int clause_num = num_clauses;
    int con_num = num_clauses; // Same number for conflict clauses

    init_constant(var_num, clause_num, con_num, max_literals, max_literals, clause_num);

    std::cout << "\n=== DETERMINISTIC CLAUSE LIST TEST ===\n";
    std::cout << "Testing with " << num_clauses << " clauses, " << max_var_num
              << " variables, max " << max_literals << " literals per clause\n";

    ClauseList cl(base);

    std::map<int, std::vector<int>> original_clauses;
    std::map<int, std::vector<int>> original_conflict_clauses;

    std::cout << "\n--- Generating and inserting deterministic clauses ---\n";

    const std::vector<std::vector<int>> deterministic_clauses = {
        {1, 2, -3},
        {-1, 2, 4, -5},
        {2, -3, 5},
        {-2, 3, -4},
        {1, -5},
        {1, 2, 3, 4, -5},
        {-1, -2, -3, -4, 5},
        {1, 3, 5},
        {-1, -3, -5},
        {2, 4}};

    const std::vector<std::vector<int>> deterministic_conflict_clauses = {
        {1, -2, 3},
        {-1, 2, -3},
        {3, 4, 5},
        {-3, -4, -5},
        {1, 2}};

    // Generate and insert regular clauses (positive indices)
    for (int i = 1; i <= num_clauses && i <= deterministic_clauses.size(); i++)
    {
        std::vector<int> literals = deterministic_clauses[i - 1];

        // Make sure literals use valid variables
        for (auto &lit : literals)
        {
            int var = std::abs(lit);
            if (var > max_var_num)
            {
                lit = (lit > 0) ? max_var_num : -max_var_num;
            }
        }

        original_clauses[i] = literals;

        Clause c(literals);
        std::cout << "Inserting clause " << i << ": ";
        for (auto lit : literals)
            std::cout << lit << " ";
        std::cout << "\n";

        cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, i), c);
    }

    // Generate and insert conflict clauses (negative indices)
    for (int i = 1; i <= num_clauses / 2 && i <= deterministic_conflict_clauses.size(); i++)
    {
        std::vector<int> literals = deterministic_conflict_clauses[i - 1];

        // Make sure literals use valid variables
        for (auto &lit : literals)
        {
            int var = std::abs(lit);
            if (var > max_var_num)
            {
                lit = (lit > 0) ? max_var_num : -max_var_num;
            }
        }

        original_conflict_clauses[i] = literals;

        Clause c(literals);
        std::cout << "Inserting conflict clause -" << i << ": ";
        for (auto lit : literals)
            std::cout << lit << " ";
        std::cout << "\n";

        cl.insert_clause(Integer(PHI_ORAM_SIZE_BIT, -i), c);
    }

    std::cout << "\n--- Retrieving and verifying clauses ---\n";
    bool all_correct = true;
    int errors = 0;

    // Verify regular clauses
    for (int i = 1; i <= num_clauses && i <= deterministic_clauses.size(); i++)
    {
        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, i), retrieved);

        std::vector<int> retrieved_literals;
        for (const auto &lit : retrieved.literals)
        {
            int value = reveal_32(lit);
            if (value != 0)
            {
                retrieved_literals.push_back(value);
            }
        }

        std::sort(retrieved_literals.begin(), retrieved_literals.end());
        auto original = original_clauses[i];
        std::sort(original.begin(), original.end());

        bool matches = (retrieved_literals == original);
        std::cout << "Clause " << i << ": ";
        if (matches)
        {
            std::cout << "OK\n";
        }
        else
        {
            std::cout << "ERROR\n";
            std::cout << "  Original: ";
            for (auto lit : original)
                std::cout << lit << " ";
            std::cout << "\n  Retrieved: ";
            for (auto lit : retrieved_literals)
                std::cout << lit << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
        }
    }

    // Verify conflict clauses
    for (int i = 1; i <= num_clauses / 2 && i <= deterministic_conflict_clauses.size(); i++)
    {
        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, -i), retrieved);

        std::vector<int> retrieved_literals;
        for (const auto &lit : retrieved.literals)
        {
            int value = reveal_32(lit);
            if (value != 0)
            {
                retrieved_literals.push_back(value);
            }
        }

        std::sort(retrieved_literals.begin(), retrieved_literals.end());
        auto original = original_conflict_clauses[i];
        std::sort(original.begin(), original.end());

        bool matches = (retrieved_literals == original);
        std::cout << "Conflict clause -" << i << ": ";
        if (matches)
        {
            std::cout << "OK\n";
        }
        else
        {
            std::cout << "ERROR\n";
            std::cout << "  Original: ";
            for (auto lit : original)
                std::cout << lit << " ";
            std::cout << "\n  Retrieved: ";
            for (auto lit : retrieved_literals)
                std::cout << lit << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Deleting clauses ---\n";
    std::set<int> deleted_clauses;
    std::set<int> deleted_conflict_clauses;

    // Delete even-numbered regular clauses
    for (int i = 2; i <= num_clauses && i <= deterministic_clauses.size(); i += 2)
    {
        cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, i), Bit(true));
        deleted_clauses.insert(i);
        std::cout << "Deleted clause " << i << "\n";
    }

    // Delete even-numbered conflict clauses
    for (int i = 2; i <= num_clauses / 2 && i <= deterministic_conflict_clauses.size(); i += 2)
    {
        cl.delete_clause(Integer(PHI_ORAM_SIZE_BIT, -i), Bit(true));
        deleted_conflict_clauses.insert(i);
        std::cout << "Deleted conflict clause -" << i << "\n";
    }

    std::cout << "\n--- Verifying deletion ---\n";

    // Check deleted regular clauses
    for (int idx : deleted_clauses)
    {
        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, idx), retrieved);

        bool is_empty = true;
        for (const auto &lit : retrieved.literals)
        {
            if (reveal_32(lit) != 0)
            {
                is_empty = false;
                break;
            }
        }

        std::cout << "Deleted clause " << idx << ": ";
        if (is_empty)
        {
            std::cout << "CORRECTLY EMPTY\n";
        }
        else
        {
            std::cout << "ERROR - NOT EMPTY\n";
            std::cout << "  Retrieved: ";
            retrieved.print();
            all_correct = false;
            errors++;
        }
    }

    // Check deleted conflict clauses
    for (int idx : deleted_conflict_clauses)
    {
        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, -idx), retrieved);

        bool is_empty = true;
        for (const auto &lit : retrieved.literals)
        {
            if (reveal_32(lit) != 0)
            {
                is_empty = false;
                break;
            }
        }

        std::cout << "Deleted conflict clause -" << idx << ": ";
        if (is_empty)
        {
            std::cout << "CORRECTLY EMPTY\n";
        }
        else
        {
            std::cout << "ERROR - NOT EMPTY\n";
            std::cout << "  Retrieved: ";
            retrieved.print();
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Verifying non-deleted clauses ---\n";

    // Check non-deleted regular clauses
    for (int i = 1; i <= num_clauses && i <= deterministic_clauses.size(); i++)
    {
        if (deleted_clauses.find(i) != deleted_clauses.end())
            continue;

        Clause retrieved;
        cl.get_clause(Integer(PHI_ORAM_SIZE_BIT, i), retrieved);

        std::vector<int> retrieved_literals;
        for (const auto &lit : retrieved.literals)
        {
            int value = reveal_32(lit);
            if (value != 0)
            {
                retrieved_literals.push_back(value);
            }
        }

        std::sort(retrieved_literals.begin(), retrieved_literals.end());
        auto original = original_clauses[i];
        std::sort(original.begin(), original.end());

        bool matches = (retrieved_literals == original);
        std::cout << "Non-deleted clause " << i << ": ";
        if (matches)
        {
            std::cout << "OK - PRESERVED\n";
        }
        else
        {
            std::cout << "ERROR - MODIFIED\n";
            std::cout << "  Original: ";
            for (auto lit : original)
                std::cout << lit << " ";
            std::cout << "\n  Retrieved: ";
            for (auto lit : retrieved_literals)
                std::cout << lit << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
        }
    }

    if (all_correct)
    {
        std::cout << "\n=== CLAUSE LIST TEST PASSED! ===\n";
    }
    else
    {
        std::cout << "\n=== CLAUSE LIST TEST FAILED WITH " << errors << " ERRORS ===\n";
    }
}

void bench_clause_list_get(int run = 1e2)
{

    ClauseList cl(base);
    auto t1 = clock_start();
    for (int i = 0; i < run; ++i)
    {
        Clause c;
        cl.get_clause(Integer(CLAUSE_IDX_SIZE_BIT, 1), c);
    }
    std::cout << "TOTAL: " << time_from(t1) / 1000.0 / 1000.0 << " s\n";
    cl.print_time();
}

int main(int argc, char **argv)
{
    int party = atoi(argv[1]);
    int depth = 12;
    int unit_size = 6;
    int threads = 10;
    int stash = 1;
    int var_num = 200;
    int clause_num = 120;
    int con_num = 10;
    int max_literals = 3;       // Maximum literals per clause
    int max_con_literals = 100; // Maximum literals per conflict clause
    int max_wl_size = 13;       // Maximum clauses per watchlist
    bool pub_read = false, pub_write = false;
    init_backend(party, depth, unit_size, threads);
    init_constant(var_num, clause_num, con_num, max_literals, max_con_literals, max_wl_size);

    base->switch_to_gt();

    test_clause_list_randomized();

    delete base;
    return 0;
}