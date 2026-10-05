#include <iostream>
#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"
#include <random>
#include <map>
#include <set>
#include <algorithm>

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

void test_watchlist_print()
{
    std::cout << "\n=== TESTING WATCHLIST BASIC OPERATIONS ===\n";
    WatchList wl(base);

    Integer lit1(VAR_SIZE_BIT, 1, PUBLIC);
    Integer lit2(VAR_SIZE_BIT, 2, PUBLIC);
    Integer lit3(VAR_SIZE_BIT, 3, PUBLIC);
    Integer neg_lit1(VAR_SIZE_BIT, -1, PUBLIC);
    Integer neg_lit2(VAR_SIZE_BIT, -2, PUBLIC);
    Integer neg_lit3(VAR_SIZE_BIT, -3, PUBLIC);

    Integer clause1(CLAUSE_IDX_SIZE_BIT, 1, PUBLIC);
    Integer clause2(CLAUSE_IDX_SIZE_BIT, 2, PUBLIC);
    Integer clause3(CLAUSE_IDX_SIZE_BIT, 3, PUBLIC);
    Integer clause4(CLAUSE_IDX_SIZE_BIT, 4, PUBLIC);
    Integer clause5(CLAUSE_IDX_SIZE_BIT, 5, PUBLIC);
    Integer conflict1(CLAUSE_IDX_SIZE_BIT, -1, PUBLIC);
    Integer conflict2(CLAUSE_IDX_SIZE_BIT, -2, PUBLIC);

    std::cout << "\n--- Print empty watchlist ---\n";
    wl.print_human();

    std::cout << "\n--- Adding clauses to watchlists ---\n";

    wl.add_clause(lit1, clause1);
    wl.add_clause(lit1, clause3);
    wl.add_clause(lit1, conflict1);

    wl.add_clause(lit2, clause2);
    wl.add_clause(lit2, clause4);

    wl.add_clause(lit3, clause3);
    wl.add_clause(lit3, clause5);
    wl.add_clause(lit3, conflict2);

    wl.add_clause(neg_lit1, clause2);
    wl.add_clause(neg_lit1, conflict2);

    wl.add_clause(neg_lit2, clause1);
    wl.add_clause(neg_lit2, conflict1);

    wl.add_clause(neg_lit3, clause4);
    wl.add_clause(neg_lit3, clause5);

    std::cout << "\n--- Print populated watchlist ---\n";
    wl.print_human();

    std::cout << "\n--- Print specific literals' watchlists ---\n";
    std::cout << "Literal 1: ";
    wl.print_clause_in_wl(lit1);
    std::cout << "Literal -2: ";
    wl.print_clause_in_wl(neg_lit2);
    std::cout << "Literal 3: ";
    wl.print_clause_in_wl(lit3);

    std::cout << "\n--- Removing clauses from watchlists ---\n";

    wl.remove_clause(lit1, clause3);
    wl.remove_clause(lit3, conflict2);
    wl.remove_clause(neg_lit2, conflict1);

    std::cout << "After removal:\n";
    std::cout << "Literal 1: ";
    wl.print_clause_in_wl(lit1);
    std::cout << "Literal -2: ";
    wl.print_clause_in_wl(neg_lit2);
    std::cout << "Literal 3: ";
    wl.print_clause_in_wl(lit3);

    std::cout << "\n=== WATCHLIST BASIC TEST COMPLETE ===\n";
}

void test_watchlist_randomized()
{
    // Use smaller test sizes regardless of the actual constants
    const int max_test_vars = std::min(VAR_NUM, 50);
    const int max_test_clauses = std::min(CLAUSE_NUM, 100);
    const int max_test_conflicts = std::min(MAX_NUM_OF_CONFLICT_CLAUSE, 20);

    std::cout << "\n=== RANDOMIZED WATCHLIST TEST ===\n";
    std::cout << "Testing with a subset of " << max_test_vars << " variables, "
              << max_test_clauses << " clauses, " << max_test_conflicts
              << " conflict clauses (from full sizes: " << VAR_NUM << ", "
              << CLAUSE_NUM << ", " << MAX_NUM_OF_CONFLICT_CLAUSE << ")\n";

    WatchList wl(base);

    std::map<int, std::set<int>> original_watchlists;

    // Deterministic random number generator for reproducibility
    std::mt19937 gen(42 + VAR_NUM + CLAUSE_NUM + MAX_NUM_OF_CONFLICT_CLAUSE);
    std::uniform_int_distribution<> var_dist(1, VAR_NUM);
    std::uniform_int_distribution<> clause_dist(1, CLAUSE_NUM);
    std::uniform_int_distribution<> sign_dist(0, 1);
    std::uniform_int_distribution<> conflict_dist(1, MAX_NUM_OF_CONFLICT_CLAUSE);

    std::cout << "\n--- Generating and populating watchlists ---\n";

    // Generate random watchlist entries, capping the total regardless of variable count
    int total_entries = std::min(max_test_vars * 2, 100);
    std::cout << "Creating " << total_entries << " watchlist entries\n";

    for (int i = 0; i < total_entries; i++)
    {
        int var = var_dist(gen);
        int lit = sign_dist(gen) ? var : -var;

        // Generate a random clause idx (regular or conflict)
        int clause_idx;
        if (sign_dist(gen))
        {
            clause_idx = clause_dist(gen);
        }
        else
        {
            clause_idx = -conflict_dist(gen);
        }

        original_watchlists[lit].insert(clause_idx);

        std::cout << "Adding clause " << clause_idx << " to literal " << lit << "'s watchlist\n";
        wl.add_clause(Integer(VAR_SIZE_BIT, lit, PUBLIC), Integer(CLAUSE_IDX_SIZE_BIT, clause_idx, PUBLIC));
    }

    std::cout << "\n--- Verifying watchlist contents ---\n";
    bool all_correct = true;
    int errors = 0;

    // Verify all literals have the correct clauses
    for (const auto &pair : original_watchlists)
    {
        int lit = pair.first;
        const auto &expected_clauses = pair.second;

        std::vector<Integer> retrieved = wl.get_watchlist_clauses(Integer(VAR_SIZE_BIT, lit, PUBLIC));

        // Extract non-zero clause indices
        std::set<int> retrieved_clauses;
        for (const auto &clause_idx : retrieved)
        {
            int value = reveal_32(clause_idx);
            if (value != 0)
            {
                retrieved_clauses.insert(value);
            }
        }

        bool matches = (retrieved_clauses == expected_clauses);
        std::cout << "Literal " << lit << ": ";
        if (matches)
        {
            std::cout << "OK\n";
        }
        else
        {
            std::cout << "ERROR\n";
            std::cout << "  Expected: ";
            for (auto c : expected_clauses)
                std::cout << c << " ";
            std::cout << "\n  Retrieved: ";
            for (auto c : retrieved_clauses)
                std::cout << c << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
        }
    }

    std::cout << "\n--- Removing entries from watchlists ---\n";
    std::map<int, std::set<int>> deletions;

    for (auto &pair : original_watchlists)
    {
        int lit = pair.first;
        auto &clauses = pair.second;

        // Delete approximately half of the entries for each literal
        std::vector<int> to_delete;
        int count = 0;
        for (int clause_idx : clauses)
        {
            if (count % 2 == 0)
            {
                to_delete.push_back(clause_idx);
                deletions[lit].insert(clause_idx);
            }
            count++;
        }

        for (int clause_idx : to_delete)
        {
            std::cout << "Removing clause " << clause_idx << " from literal " << lit << "'s watchlist\n";
            wl.remove_clause(Integer(VAR_SIZE_BIT, lit, PUBLIC), Integer(CLAUSE_IDX_SIZE_BIT, clause_idx, PUBLIC));
            clauses.erase(clause_idx);
        }
    }

    std::cout << "\n--- Verifying watchlist after deletions ---\n";

    // Verify all literals have the correct clauses after deletion
    for (const auto &pair : original_watchlists)
    {
        int lit = pair.first;
        const auto &expected_clauses = pair.second;

        std::vector<Integer> retrieved = wl.get_watchlist_clauses(Integer(VAR_SIZE_BIT, lit, PUBLIC));

        // Extract non-zero clause indices
        std::set<int> retrieved_clauses;
        for (const auto &clause_idx : retrieved)
        {
            int value = reveal_32(clause_idx);
            if (value != 0)
            {
                retrieved_clauses.insert(value);
            }
        }

        bool matches = (retrieved_clauses == expected_clauses);
        std::cout << "Literal " << lit << " after deletion: ";
        if (matches)
        {
            std::cout << "OK\n";
        }
        else
        {
            std::cout << "ERROR\n";
            std::cout << "  Expected: ";
            for (auto c : expected_clauses)
                std::cout << c << " ";
            std::cout << "\n  Retrieved: ";
            for (auto c : retrieved_clauses)
                std::cout << c << " ";
            std::cout << "\n";
            all_correct = false;
            errors++;
        }
    }

    if (all_correct)
    {
        std::cout << "\n=== WATCHLIST TEST PASSED! ===\n";
    }
    else
    {
        std::cout << "\n=== WATCHLIST TEST FAILED WITH " << errors << " ERRORS ===\n";
    }
}

int main(int argc, char **argv)
{
    int party = atoi(argv[1]);
    int depth = 25;
    int unit_size = 6;
    int threads = 10;
    int var_num = 1000;
    int clause_num = 1000;
    int con_num = 1000;
    int max_literals = 10;
    int max_con_literals = 10;
    int max_wl_size = 120;

    init_backend(party, depth, unit_size, threads);
    init_constant(var_num, clause_num, con_num, max_literals, max_con_literals, max_wl_size);
    base->switch_to_gt();

    test_watchlist_randomized();

    delete base;
    return 0;
}
