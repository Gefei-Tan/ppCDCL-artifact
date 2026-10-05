#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include <random>

#define id(i,j) (i*N+j)
#define log2ceil(x) (int(log2(x-1)+1))

using namespace std;
using namespace emp;
using Bit = Bit_T<AuthWire>;
using Integer = Integer_T<AuthWire>;

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

    base = new MPC<RandomIO>(party, threads, io, delta, log2ceil(N*N));
    emp::backend = base;
}

int bit = 32, logbit = 5;

// N pairs
void matching(int N) {
    // Generate the preference lists
    vector<int> numbers;
    Integer init_partner[N], init_stack[N], top(bit, N-1, PUBLIC);
    for (int i = 0; i < N; i++) {
        init_partner[i] = Integer(bit, N, PUBLIC);
        init_stack[i] = Integer(bit, N-i-1, PUBLIC);
        numbers.push_back(i);
    }
    std::random_device rd;
    std::mt19937 g(rd());
    Integer preference_list_men[N*N], preference_list_women[N*N];
    for (int i = 0; i < N; i++) {
        std::shuffle(numbers.begin(), numbers.end(), g);
        for (int j = 0; j < N; j++) {
            preference_list_men[id(i,j)] = Integer(bit, numbers[j], ALICE);
            preference_list_women[id(i,numbers[j])] = Integer(bit, j, BOB);
        }
    }

    // Initialize the RAMs
    int sN2 = log2ceil(N*N), sN = log2ceil(N+1);
    RAM<RandomIO> *prefer_men, *prefer_women, *women_partner, *stack, *seq;
    prefer_men = new RAM<RandomIO>(base, sN2, logbit, -1, preference_list_men, 0, N*N);
    prefer_women = new RAM<RandomIO>(base, sN2, logbit, -1, preference_list_women, 0, N*N);
    women_partner = new RAM<RandomIO>(base, sN, logbit, -1, init_partner, 0, N);
    stack = new RAM<RandomIO>(base, sN, logbit, -1, init_stack, 0, N);
    seq = new RAM<RandomIO>(base, sN, logbit);
    
    // Gale-Shapley algorithm
    Integer zero(bit, 0, PUBLIC), one(bit, 1, PUBLIC), NN(bit, N, PUBLIC);
    auto start = clock_start();
    for (int count = 0; count < (N*N-N+1); count++) {
        // get the top element from the oblivious stack
        Integer idx; stack->read(top, idx);
        Integer idx_i; seq->write(idx, [&](const Integer &in, Integer &out) { out = in + one; }, &idx_i);
        base->execute([&]() { idx_i = idx * NN + idx_i;});
        // get the favorite one that he has not proposed to
        Integer propose_w; prefer_men->read(idx_i, propose_w);
        // get this woman's current partner
        Integer cmp; women_partner->read(propose_w, cmp); 
        // propose to this woman
        Bit success(false, PUBLIC), no_partner;
        Integer lhs, seq_l, rhs, seq_r;
        base->execute([&]() {
            lhs = propose_w * NN; rhs = lhs + cmp; lhs = lhs + idx; 
            no_partner = (cmp == NN);
            success = success | no_partner;
        });
        // compare with the current partner
        prefer_women->read(lhs, seq_l);
        prefer_women->read(rhs, seq_r);
        base->execute([&]() {
            success = success | (seq_l < seq_r);
            success = success & (top >= zero);
        });
        // update the partner and the stack
        stack->write(top, [&](const Integer &in, Integer &out) { out = in.If(success, cmp); });
        women_partner->write(propose_w, [&](const Integer &in, Integer &out) { out = in.If(success, idx); });
        base->execute([&]() { top = top - zero.If(no_partner, one); }); 
    }

    double time = time_from(start);
    cout << time / 1000 << endl;

    for (int i = 0; i < N; i++) {
        Integer out; women_partner->read(i, out);
        std::cout << out.reveal<int32_t>(PUBLIC) << " ";
    }
    std::cout << std::endl;
}

int main(int argc, char** argv) {
    int party = atoi(argv[1]), threads = 1, N = 8;
    if (argc > 2) threads = atoi(argv[2]);
    if (argc > 3) N = atoi(argv[3]);
    if (argc > 4) {
        init_backend(party, N, threads, argv[4]);
    } else {
        init_backend(party, N, threads);
    }

    matching(N);
    return 0;
}