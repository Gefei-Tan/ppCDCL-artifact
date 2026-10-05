#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include <iostream>

using namespace emp;
using Bit = Bit_T<AuthWire>;
using Integer = Integer_T<AuthWire>;

MPC<RandomIO> * mpc;

int depth = 10;

void init_backend(int party, int threads=2, const char* addr="127.0.0.1") {
    RandomIO ** io = new RandomIO *[threads];
    for (int i = 0; i < std::max(2,threads); i++) {
        io[i] = new RandomIO((party-1) ? addr : nullptr, 12345+i, true);
    }
    block delta; PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE) delta ^= makeBlock(0, 1);
    else delta ^= makeBlock(0, 3);

    mpc = new MPC<RandomIO>(party, threads, io, delta);
    emp::backend = mpc;
}

void benchmark_circuit(int runs=10000) {
    std::cout << "Pure circuit testing results (g/s):" << std::endl;
    BristolFashion cf("./emp-dpf/files/aes_128.txt");
    Bit out[128], msg[256];
    auto start = clock_start();
    auto total = mpc->num_and();
    mpc->execute(
        [&]() {
            for (int i = 0; i < runs; i++) {
                cf.compute(out, msg);
            }
        }
    );
    long long t = time_from(start);
    total = mpc->num_and() - total;
    std::cout << "total gates" << total << std::endl;
    std::cout << "gates/s:" << (total * 1e6 / t) << std::endl;
}

int main(int argc, char** argv) {
    int party = atoi(argv[1]);
    if (argc > 2) { init_backend(party, 1, argv[2]);
    } else { init_backend(party, 1); }
    benchmark_circuit();
    return 0;
}