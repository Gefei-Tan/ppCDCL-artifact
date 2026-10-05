#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include <iostream>

using namespace std;
using namespace emp;
using Bit = Bit_T<AuthWire>;
using Integer = Integer_T<AuthWire>;
#define log2ceil(x) (int(log2(x-1)+1))

int depth = 5, threads = 1;

const int size_of_element = 7;
MPC<RandomIO> * base;

void init_backend(int party, int threads=2, int stash_size=-1, const char* addr="127.0.0.1") {
    RandomIO ** io = new RandomIO *[threads];
    for (int i = 0; i < max(2,threads); i++) {
        io[i] = new RandomIO((party-1) ? addr : nullptr, 12345+i, true);
    }
    block delta; PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE) delta ^= makeBlock(0, 1);
    else delta ^= makeBlock(0, 3);

    base = new MPC<RandomIO>(party, threads, io, delta, 24);
    emp::backend = base;
}

void bits_to_int(const vector<Bit> &in, Integer &out) {
    out = new Integer(in.size(), 0, PUBLIC);
    out.bits = in;
}

void scrypt(int N=(1<<10), int r=8) {
    int block_size = 8*r;
    Integer X[block_size];
    for (int i = 0; i < block_size; i++) {
        X[i] = Integer(128, 0, PUBLIC);
    }
    // Measures the ORAM accesses of scrypt only; the PRF and BlockMix steps are omitted.
    
    auto start = clock_start();
    Integer V[N*block_size];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 8*r; j++) {
            V[i*block_size+j] = X[j]; 
        }
    }
    RAM<RandomIO> *v = new RAM<RandomIO>(base, log2ceil(N*block_size), size_of_element, -1, V, 0, N*block_size);
    for (int i = 0; i < N; i++) {
        Integer idx(32, 0, PUBLIC), one(32, 1, PUBLIC);
        base->execute([&]() { idx = idx << 3;});
        Integer v_idx[block_size];
        for (int j = 0; j < block_size; j++) {
            v->read(idx, v_idx[j]);
            base->execute([&]() { idx = idx + one;});
        }
    }
    std::cout << "Time: " << time_from(start) / 1000 << std::endl;
    delete v;
}

int main(int argc, char** argv) {
    int party = atoi(argv[1]), N=1<<10, r=8;
    if (argc > 2) threads = atoi(argv[2]);
    if (argc > 3) N = atoi(argv[3]);
    if (argc > 4) r = atoi(argv[4]);
    if (argc > 5) {
        init_backend(party, threads, -1, argv[5]);
    } else {
        init_backend(party, threads, -1);
    }
    scrypt(N, r); 
    return 0;
}