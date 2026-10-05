#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include <emp-tool/utils/block.h>
#include <random>

#define log2ceil(x) (int(log2(x-1)+1))

using namespace std;
using namespace emp;
using Bit = Bit_T<AuthWire>;
using Integer = Integer_T<AuthWire>;

MPC<RandomIO> * base;

int bit = 64, logbit = 6;
const int N = 1<<25;

void init_backend(int party, int threads=2, const char* addr="127.0.0.1") {
    RandomIO ** io = new RandomIO *[threads];
    for (int i = 0; i < max(2,threads); i++) {
        io[i] = new RandomIO((party-1) ? addr : nullptr, 12345+i, true);
    }
    block delta; PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE) delta ^= makeBlock(0, 1);
    else delta ^= makeBlock(0, 3);

    base = new MPC<RandomIO>(party, threads, io, delta, log2ceil(N));
    emp::backend = base;
}

void search(int cnt = 32) {
    Integer *numbers = new Integer[N];
    for (int i = 0; i < N; i++) {
        numbers[i] = Integer(bit, i, PUBLIC);
    }

    block dummy = zero_block;
    base->commit_then_open(dummy);

    auto start = clock_start();
    // Initialize the RAMs
    RAM<RandomIO> *array = new RAM<RandomIO>(base, log2ceil(N), logbit, -1, numbers, 0, N);
    base->commit_then_open(dummy);

    double time = time_from(start);
    cout << time / 1000 << endl;
    start = clock_start();
    
    // Binary search for cnt times
    // R -> lower_bound;
    Integer one(32, 1, PUBLIC);
    while(cnt--) {
        Integer L(32, 0, PUBLIC), R(32,N-1, PUBLIC), target(bit, cnt, PUBLIC);
        int diff = N-1;
        while (diff > 0) {
            diff = diff >> 1;
            Integer mid, val;
            base->execute([&](){mid = L+((R-L)>>1);});
            array->read(mid, val);
            base->execute([&]() {
                Bit cmp = val < target;
                L = L.If(cmp, mid+one);
                R = R.If(!cmp, mid);
            });
        }
    }
    base->commit_then_open(dummy);
    time = time_from(start);
    cout << time / 1000 << endl;
    delete[] numbers;
}

int main(int argc, char** argv) {
    int party = atoi(argv[1]), threads = 1, cnt = 32;
    if (argc > 2) threads = atoi(argv[2]);
    if (argc > 3) cnt = atoi(argv[3]);
    if (argc > 4) {
        init_backend(party, threads, argv[4]);
    } else {
        init_backend(party, threads);
    }
    search(cnt);
    return 0;
}