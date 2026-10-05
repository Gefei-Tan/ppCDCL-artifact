#include <iostream>
#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"

FloramMPC<NetIO> *base;

void init_backend(int party, int threads = 2, const char *addr = "127.0.0.1")
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

    base = new FloramMPC<NetIO>(party, threads, io, delta);
    emp::backend = base;
}

void bench_wl_heuristic(int var_num, int clause_num, string cnf_name)
{
    int max_possible_occ_bit_width = log2(clause_num) + 1;
    Integer *alice_var_occurrences = new Integer[2 * var_num];
    Integer *bob_var_occurrences = new Integer[2 * var_num];
    auto t1 = clock_start();
    for (int i = 0; i < 2 * var_num; ++i)
    {
        alice_var_occurrences[i] = Integer(max_possible_occ_bit_width, 1, ALICE);
        bob_var_occurrences[i] = Integer(max_possible_occ_bit_width, 2, BOB);
    }

    for (int i = 0; i < 2 * var_num; ++i)
    {
        alice_var_occurrences[i] + bob_var_occurrences[i];
    }

    emp::sort(alice_var_occurrences, 2 * var_num);

    auto time_used = time_from(t1) / 1e6;
    std::cout << cnf_name << ", " << var_num << ", " << clause_num << ", " << time_used << "s \n";
}

int main(int argc, char **argv)
{
    int threads = 10;
    int party = atoi(argv[1]);
    init_backend(party, threads);
    base->switch_to_gt();
    vector<string> test_var_nums = {"QG", "ais", "bf", "blocksworld", "flat100-239", "flat200-479", "logistics", "n10", "n4", "n5", "n6", "n7", "n8", "parity", "pret", "ssa"};
    vector<int> var_nums = {993, 178, 1698, 1644, 300, 600, 1881, 1349, 348, 475, 618, 777, 947, 1044, 105, 2351};
    vector<int> clause_nums = {56239, 2451, 5158, 29544, 1117, 2237, 11729, 11938, 1828, 2877, 4301, 5684, 7360, 3611, 280, 6716};
    for (size_t i = 0; i < var_nums.size(); ++i)
    {
        bench_wl_heuristic(var_nums[i], clause_nums[i], test_var_nums[i]);
        
    }

    delete base;
    return 0;
}