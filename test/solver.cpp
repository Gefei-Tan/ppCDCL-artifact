#include <iostream>
#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"

using namespace emp;
using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;
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

int main(int argc, char **argv)
{
    int threads = 10;
    int party = atoi(argv[1]);
    int len = atoi(argv[2]);
    int data_size = atoi(argv[3]);
    int idx_len = ceil(log2(len));
    init_backend(party, threads);
    base->switch_to_gt();
    vector<Integer> data;
    data.assign(len, Integer(data_size, 0));
    Integer idx(idx_len, 1, PUBLIC); // Public index; use BOB for a secret index
    Integer res_scan(data_size, 0, PUBLIC);

    auto g0 = base->num_and();
    auto t0 = clock_start();

    for (int i = 0; i < len; ++i)
    {
        Bit b = idx.equal(Integer(idx_len, i, PUBLIC));
        res_scan = If(b, data[i], res_scan);
    }

    auto t1 = time_from(t0);
    auto g1 = base->num_and();
    std::cout << "scan AND: " << (g1 - g0) << " time " << (t1)/1000.0 << "ms \n";

    Integer result(data_size, 0, PUBLIC);
    auto t3 = clock_start();
    read_vector<Integer>(data, Integer(idx_len, 1), result);
    auto t4 = time_from(t3);
    std::cout << "vect AND: " << (base->num_and() - g1) << " time " << t4 / 1000.0 << " ms.\n";

    delete base;
    return 0;
}