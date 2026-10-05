#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include <iostream>

using namespace std;
using namespace emp;
using Bit = Bit_T<AuthWire>;
using Integer = Integer_T<AuthWire>;

const int size_of_element = 6; // 64-bit element
int depth = 25, threads = 1;

MPC<RandomIO> * base;

void init_backend(int party, int N, int threads=2, const char* addr="127.0.0.1") {
    RandomIO ** io = new RandomIO *[threads];
    for (int i = 0; i < max(2,threads); i++) {
        io[i] = new RandomIO((party-1) ? addr : nullptr, 12345+i, true);
    }
    block delta; PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE) delta ^= makeBlock(0, 1);
    else delta ^= makeBlock(0, 3);

    base = new MPC<RandomIO>(party, threads, io, delta, N+size_of_element-7);
    emp::backend = base;
}

void benchmark_oram(int size, int total_runs = 64) {
    RAM<RandomIO> *ram = new RAM<RandomIO>(base, depth, size_of_element, size);
    double read = 0, write = 0;
    int cnt_runs = 0;
    while (cnt_runs < total_runs) {
        int runs = min(size, total_runs - cnt_runs);
        // put something into the stash
        for (int i = 0; i < (size-runs)>>1; i++) {
            ram->write(0, [&](const Integer &in, Integer &out){out = in;});
        }
        // occupy half capacity of the stash
        auto start = clock_start();
        for (int i = (size-runs)>>1; i < (size>>1); i++) {
            Integer idx(depth, i, PUBLIC), value(1<<size_of_element, i, PUBLIC);
            ram->write(idx, [&](const Integer &in, Integer &out){out = value;});
        }
        write += time_from(start);

        // test read time when the stash half-occupied
        start = clock_start();
        for (int i = 0; i < runs; i++) {
            Integer idx(depth, i, PUBLIC), value;
            ram->read(idx, value);
        }
        read += time_from(start);

        // occupy the whole stash and refresh it
        start = clock_start();
        for (int i = (size>>1); i < ((size-runs)>>1)+runs; i++) {
            Integer idx(depth, i, PUBLIC), value(1<<size_of_element, i, PUBLIC);
            ram->write(idx, [&](const Integer &in, Integer &out){out = value;});
        }
        write += time_from(start);
        if (runs != size) {
            start = clock_start();
            ram->refresh(ram->ka, ram->kb);
            write += time_from(start) * runs / size;
        }
        cnt_runs += runs;
    }

    std::cout << read / 1000 / total_runs << "\t" << write / 1000 / total_runs << std::endl;
    delete ram;
}

int main(int argc, char** argv) {
    int party = atoi(argv[1]); bool wan = 0;
    if (argc > 2) depth = atoi(argv[2]);
    if (argc > 3) threads = atoi(argv[3]);
    if (argc > 4) wan = (bool)atoi(argv[4]);
    int stash_size = max(1, int(sqrt(1<<depth)/(20*sqrt(threads))));
    if (wan) stash_size = max(1, int(sqrt(1<<depth)/16));
    if (argc > 5) {
        init_backend(party, depth, threads, argv[5]);
    } else {
        init_backend(party, depth, threads);
    }
    benchmark_oram(stash_size, 128); 
    return 0;
    
}