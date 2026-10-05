#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include "emp-dpf/floram.h"
#include "src/ppCDCL.h"
#include <iostream>
#include <random>
#include <vector>

using namespace std;
using namespace emp;
using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

const int depth = 20;

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

    bool *key = new bool[128];
    PRG().random_bool(key, 128);
    base->lowmc = new LowMC<GbWire>(key, base);
    delete[] key;
}

void test_bit()
{
    bool b[] = {true, false};
    int p[] = {PUBLIC, ALICE, BOB};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 2; ++k)
                for (int l = 0; l < 3; ++l)
                {
                    Bit b1(b[i], p[j]);
                    Bit b2(b[k], p[l]);
                    Bit b3;
                    base->execute([](Bit &b1, Bit &b2, Bit &b3)
                                  { b3 = b1 & b2; }, b1, b2, b3);
                    bool res = b3.reveal(PUBLIC);
                    if (res != (b[i] and b[k]))
                    {
                        cout << "AND" << i << " " << j << " " << k << " " << l << " " << res << endl;
                        error("test AND error!");
                    }
                    base->execute([](Bit &b1, Bit &b2, Bit &b3)
                                  { b3 = b1 & b1; }, b1, b2, b3);
                    res = b3.reveal(PUBLIC);
                    if (res != b[i])
                    {
                        cout << "AND" << i << " " << j << res << endl;
                        error("test AND error!");
                    }
                    base->execute([](Bit &b1, Bit &b2, Bit &b3)
                                  { b3 = b1 & (!b1); }, b1, b2, b3);
                    res = b3.reveal(PUBLIC);
                    if (res)
                    {
                        cout << "AND" << i << " " << j << res << endl;
                        error("test AND error!");
                    }
                    base->execute([](Bit &b1, Bit &b2, Bit &b3)
                                  { b3 = b1 ^ b2; }, b1, b2, b3);
                    res = b3.reveal(PUBLIC);
                    if (res != (b[i] xor b[k]))
                    {
                        cout << "XOR" << i << " " << j << " " << k << " " << l << " " << res << endl;
                        error("test XOR error!");
                    }
                    base->execute([](Bit &b1, Bit &b2, Bit &b3)
                                  { b3 = b1 ^ b1; }, b1, b2, b3);
                    res = b3.reveal(PUBLIC);
                    if (res)
                    {
                        cout << "XOR" << i << " " << j << res << endl;
                        error("test XOR error!");
                    }
                    base->execute([](Bit &b1, Bit &b2, Bit &b3)
                                  { b3 = b1 ^ (!b1); }, b1, b2, b3);
                    res = b3.reveal(PUBLIC);
                    if (!res)
                    {
                        cout << "XOR" << i << " " << j << res << endl;
                        error("test XOR error!");
                    }
                }
    std::cout << "test bit success!" << std::endl;
}

template <typename Op, typename Op2>
void test_int(int party, int range1 = 1 << 25, int range2 = 1 << 25, bool check = true, int runs = 10)
{
    block seed = makeBlock(0, 1);
    PRG prg(&seed);
    for (int i = 0; i < runs; ++i)
    {
        long long ia, ib;
        prg.random_data(&ia, 8);
        prg.random_data(&ib, 8);
        ia %= range1;
        ib %= range2;
        while (Op()(int(ia), int(ib)) != Op()(ia, ib))
        {
            prg.random_data(&ia, 8);
            prg.random_data(&ib, 8);
            ia %= range1;
            ib %= range2;
        }

        Integer a(32, ia, ALICE);
        Integer b(32, ib, BOB);

        Integer res;

        if (!check)
        {
            base->execute(
                [](Integer &a, Integer &b, Integer &res, int runs)
                {
                    for (int i = 0; i < runs; ++i)
                        res = Op2()(a, b);
                },
                a, b, res, runs);
            break;
        }
        else
        {
            base->execute(
                [](Integer &a, Integer &b, Integer &res)
                { res = Op2()(a, b); },
                a, b, res);
            if (res.reveal<int32_t>(PUBLIC) != Op()(ia, ib))
            {
                std::cout << ia << " " << ib << std::endl;
                cout << a.reveal<int32_t>() << endl;
                cout << b.reveal<int32_t>() << endl;
                cout << ia << "\t" << ib << "\t" << Op()(ia, ib) << "\t" << res.reveal<int32_t>(PUBLIC) << endl;
            }
            assert(res.reveal<int32_t>(PUBLIC) == Op()(ia, ib));
        }
    }
    cout << typeid(Op2).name() << "\t\t\tDONE" << endl;
}

void test_dpf(int sz = 16, int a = 3, int b = 2)
{
    std::cout << "DPF testing results:" << std::endl;
    Integer alpha(sz, a, PUBLIC);
    Integer beta(128, b, PUBLIC);
    block *r, beta_value = makeBlock(0, b);
    base->DPF(alpha, beta, sz, &r);
    base->direct_open(r, 1 << (sz));
    for (int i = 0; i < 1 << sz; i++)
    {
        if (i != a && !cmpBlock(&r[i], &zero_block, 1))
        {
            cout << "r[" << i << "]: " << r[i] << endl;
            error("DPF error at zero!");
        }
        if (i == a && !cmpBlock(&r[i], &beta_value, 1))
        {
            error("DPF error at alpha/beta");
        }
        else if (i == a)
        {
            cout << "r[" << i << "]: " << r[i] << endl;
        }
    }
}

void test_oram_refresh(int size, int unit_size, int stash_size)
{
    std::cout << "ORAM refresh testing results (parallel DPF):" << std::endl;

    // Initialize the ORAM. FloRAM's constructor will also perform an initial refresh.
    FloRAM<NetIO> *oram = new FloRAM<NetIO>(base, size, unit_size, stash_size);

    // Fill the ORAM with a known pattern: block i holds the value i.
    int total_blocks = 1 << (oram->oram_size + 1);
    std::cout << "total_blocks: " << total_blocks << std::endl;
    for (int i = 0; i < total_blocks; i++)
    {
        oram->write(Integer(size, i), [&](const Integer &in, Integer &out)
                    { out = Integer(1 << unit_size, i); });
    }

    Integer ka, kb;
    oram->refresh(ka, kb);

    for (int j = 0; j < 1 << size; j++)
    {
        Integer idx = Integer(oram->oram_size, j, PUBLIC);
        Integer read_result(1 << unit_size, 0);
        oram->read(idx, read_result);
        cout << "read " << j << "-th: " << read_result.reveal<int64_t>() << endl;
        if (read_result.reveal<int64_t>() != j)
        {
            cout << "read " << j << "-th: " << read_result.reveal<int64_t>() << endl;
            error("ORAM refresh error!");
        }
    }

    delete oram;
}

void output(FloRAM<NetIO> *arr, string name, int sz)
{
    for (int i = 0; i < sz; i++)
    {
        Integer value;
        arr->read(i, value);
        cout << name << "[" << i << "]: ";
        cout << value.reveal<int64_t>(PUBLIC) << endl;
    }
}

void test_oram(int sz = 10)
{
    std::cout << "ORAM testing results:" << std::endl;
    int bit = 3, bit_z = 6;
    cout << "set x[i]=i, y[i]=2i, z[0]=z[1]=1" << endl;
    Integer *arr_x = new Integer[sz], *arr_y = new Integer[sz], *arr_z = new Integer[sz];
    for (int i = 0; i < sz; i++)
    {
        arr_x[i] = Integer(1 << bit, 1, PUBLIC);
        arr_y[i] = Integer(1 << bit, i << 1, PUBLIC);
        arr_z[i] = Integer(1 << bit_z, 1, PUBLIC);
    }
    FloRAM<NetIO> *x, *y, *z;
    x = new FloRAM<NetIO>(base, depth, bit, -1, arr_x, 0, sz);
    y = new FloRAM<NetIO>(base, depth, bit, -1, arr_y, 0, sz);
    z = new FloRAM<NetIO>(base, depth, bit_z, -1, arr_z, 0, 2);

    cout << "x[i]=3*x[i]" << endl;
    for (int i = 0; i < sz; i++)
    {
        Integer idx(depth, i, PUBLIC);
        x->write(idx,
                 [&](const Integer &in, Integer &out)
                 { out = Integer(1 << bit, 1); });
    }
    output(x, "x", sz);

    cout << "y[i]=x[i]-y[i]" << endl;
    for (int i = 0; i < sz; i++)
    {
        Integer idx(depth, i, PUBLIC);
        Integer x_i;
        x->read(idx, x_i);
        if (x_i.reveal<int64_t>() != 1)
        {
            cout << "x[" << i << "]: " << x_i.reveal<int64_t>() << endl;
            error("x[i] != 1");
        }
        y->write(idx,
                 [&](const Integer &in, Integer &out)
                 { out = x_i - in; });
    }
    y->refresh(y->ka, y->kb);
    output(y, "y", sz);

    cout << "z[i]=z[i-1]+z[i-2]" << endl;
    for (int i = 2; i < sz; i++)
    {
        Integer z1, z2;
        z->read(i - 1, z1);
        z->read(i - 2, z2);
        z->write(i, [&](const Integer &in, Integer &out)
                 { out = z1 + z2; });
    }
    z->refresh(z->ka, z->kb);
    output(z, "z", sz);
}

void init_constant()
{
}

void test_range_read(int depth, int unit_size, int range, int run = 1, bool if_1024 = true)
{
    cout << "oram size: 2^" << depth << ", element size: 2^" << unit_size << " range: " << range << " runs: " << run << endl;
    FloRAM<NetIO> oram = FloRAM<NetIO>(base, depth, unit_size, 1, nullptr, -1, -1, false, if_1024);
    FloRAM<NetIO> oram1 = FloRAM<NetIO>(base, depth, unit_size, 1, nullptr, -1, -1, false, false);
    vector<Integer> result, result_plain;
    result.resize(range, Integer(1 << unit_size, 0));
    result_plain.resize(range, Integer(1 << unit_size, 0));

    for (int i = 0; i < range + 10; i++)
    {
        Integer idx(depth, i, PUBLIC);
        oram.naive_write(idx, Integer(1 << unit_size, i));
        oram1.naive_write(idx, Integer(1 << unit_size, i));
    }
    // Reset the timing counters
    base->lowmc_1024_ctr = 0;
    base->lowmc_1024_time1 = 0;
    base->lowmc_1024_time = 0;
    base->lowmc_128_ctr = 0;
    base->lowmc_128_time1 = 0;
    base->lowmc_128_time = 0;
    base->aes_ctr = 0;
    base->aes_time1 = 0;
    base->aes_time = 0;

    auto t1 = clock_start();
    for (int i = 0; i < run; i++)
    {
    }
    cout << "range read time: " << time_from(t1) / 1e3 / 1e3 << "s" << endl;
    cout << "       lowmc 1024 ctr: " << base->lowmc_1024_ctr << endl;
    cout << "       lowmc 1024 total time: " << base->lowmc_1024_time / 1e3 / 1e3 << "s" << endl;
    cout << "       lowmc 128 ctr: " << base->lowmc_128_ctr << endl;
    cout << "       lowmc 128 total time: " << base->lowmc_128_time / 1e3 / 1e3 << "s" << endl;

    // Reset the LowMC 128 counters
    base->lowmc_128_ctr = 0;
    base->lowmc_128_time1 = 0;
    base->lowmc_128_time = 0;

    cout << "   DPF: " << oram.range_t1 / 1e3 / 1e3 << "s" << endl;
    cout << "   OROM: " << oram.range_t2 / 1e3 / 1e3 << "s" << endl;
    cout << "   LOWMC 1024: " << oram.range_t3 / 1e3 / 1e3 << "s" << endl;
    cout << "   LOWMC 1024 non enc: " << oram.range_more / 1e3 / 1e3 << "s" << endl;
    cout << "   LOWMC non enc ctr: " << oram.more_ctr << endl;
    cout << "   unpack time: " << oram.range_t4 / 1e3 / 1e3 << "s" << endl;

    t1 = clock_start();
    for (int i = 0; i < run; i++)
    {
        for (int j = 0; j < range; j++)
        {
            oram.read(Integer(depth, j, PUBLIC), result_plain[j]);
        }
    }

    cout << "naive sequential read time: " << time_from(t1) / 1e3 / 1e3 << "s" << endl;
    cout << "       lowmc 128 ctr: " << base->lowmc_128_ctr << endl;
    cout << "       lowmc 128 exe time: " << base->lowmc_128_time1 / 1e3 / 1e3 << "s" << endl;
    cout << "       lowmc 128 total time: " << base->lowmc_128_time / 1e3 / 1e3 << "s" << endl;
    cout << "       AES 128 ctr: " << base->aes_ctr << endl;
    cout << "       AES 128 exe time: " << base->aes_time1 / 1e3 / 1e3 << "s" << endl;
    cout << "       AES 128 total time: " << base->aes_time / 1e3 / 1e3 << "s" << endl;
    // Timing breakdown of the single reads
    cout << "   DPF: " << oram.single_t1 / 1e3 / 1e3 << "s" << endl;
    cout << "   OROM: " << oram.single_t2 / 1e3 / 1e3 << "s" << endl;
    cout << "   LOWMC 128 & unpack: " << oram.single_t3 / 1e3 / 1e3 << "s" << endl;
    cout << "   stash: " << oram.single_t4 / 1e3 / 1e3 << "s" << endl;

    // Reset the LowMC 128 counters
    base->lowmc_128_ctr = 0;
    base->lowmc_128_time1 = 0;
    base->lowmc_128_time = 0;
    // Reset the AES counters
    base->aes_ctr = 0;
    base->aes_time1 = 0;
    base->aes_time = 0;

    t1 = clock_start();
    for (int i = 0; i < run; i++)
    {
        oram1.range_read(Integer(depth, 0, PUBLIC), result, range);
    }
    cout << "small block range read time: " << time_from(t1) / 1e3 / 1e3 << "s" << endl;
    cout << "       lowmc 128 ctr: " << base->lowmc_128_ctr << endl;
    cout << "       lowmc 128 exe time: " << base->lowmc_128_time1 / 1e3 / 1e3 << "s" << endl;
    cout << "       lowmc 128 total time: " << base->lowmc_128_time / 1e3 / 1e3 << "s" << endl;
    cout << "       AES 128 ctr: " << base->aes_ctr << endl;
    cout << "       AES 128 exe time: " << base->aes_time1 / 1e3 / 1e3 << "s" << endl;
    cout << "       AES 128 total time: " << base->aes_time / 1e3 / 1e3 << "s" << endl;

    cout << "   DPF: " << oram1.range_t1 / 1e3 / 1e3 << "s" << endl;
    cout << "   OROM: " << oram1.range_t2 / 1e3 / 1e3 << "s" << endl;
    cout << "   AES 128: " << oram1.range_t3 / 1e3 / 1e3 << "s" << endl;
    cout << "   unpack time: " << oram1.range_t4 / 1e3 / 1e3 << "s" << endl;
}

void add_comm(uint64_t &comm, FloramMPC<NetIO> *base)
{
    comm = 0;
    for (size_t i = 0; i < base->threads; i++)
    {
        comm += base->ios[i]->counter;
    }
    for (size_t i = 0; i < base->threads; i++)
    {
        base->ios[i]->counter = 0;
    }
    base->all_flush();
}

void bench_low_mc(int run = 147456)
{
    bool key[128];
    PRG().random_bool(key, 128);
    LowMC<GbWire> lowmc128 = LowMC<GbWire>(key, base, 11, 128, 128, 25);
    LowMC<GbWire> lowmc256 = LowMC<GbWire>(key, base, 11, 256, 128, 49);
    LowMC<GbWire> lowmc512 = LowMC<GbWire>(key, base, 13, 512, 128, 49);
    LowMC<GbWire> lowmc1024 = LowMC<GbWire>(key, base, 11, 1024, 128, 196);
    Bit input[95232 + 1024];
    Bit output[1024];
    for (int i = 0; i < 95232 + 1024; i++)
    {
        input[i] = Bit(i % 2, PUBLIC);
    }
    uint64_t comm = 0;
    auto t1 = clock_start();
    uint64_t and_n = base->num_and();

    for (int i = 0; i < run; i++)
    {
        base->cf_lowmc_128->compute(output, input);
    }
    add_comm(comm, base);
    cout << "Bristol lowmc 128 \n \t\t#AND: " << ((base->num_and() - and_n) / run)
         << "\n \t\ttime: " << time_from(t1) / 1e3 / run << "ms"
         << endl;

    t1 = clock_start();
    and_n = base->num_and();

    for (int i = 0; i < run; i++)
    {
        base->cf_aes->compute(output, input);
    }
    add_comm(comm, base);
    cout << "Bristol AES 128 \n \t\t#AND: " << ((base->num_and() - and_n) / run)
         << "\n \t\ttime: " << time_from(t1) / 1e3 / run << "ms"
         << endl;

    t1 = clock_start();
    and_n = base->num_and();
    for (int i = 0; i < run; i++)
    {
        base->cf_lowmc_1024->compute(output, input);
    }
    add_comm(comm, base);
    cout << "Bristol lowmc 1024 \n \t\t#AND: " << ((base->num_and() - and_n) / run)
         << "\n \t\ttime: " << time_from(t1) / 1e3 / run << "ms"
         << endl;

    t1 = clock_start();
    and_n = base->num_and();
    for (int i = 0; i < run; i++)
    {
        lowmc128.encrypt(input, output, 1);
    }
    add_comm(comm, base);
    cout << "lowmc 128 \n \t\t#AND: " << ((base->num_and() - and_n) / run)
         << "\n \t\ttime: " << time_from(t1) / 1e3 / run << "ms"
         << endl;

    t1 = clock_start();
    and_n = base->num_and();
    for (int i = 0; i < run; i++)
    {
        lowmc256.encrypt(input, output, 1);
    }
    add_comm(comm, base);
    cout << "lowmc 256 \n \t\t#AND: " << ((base->num_and() - and_n) / run)
         << "\n \t\ttime: " << time_from(t1) / 1e3 / run << "ms"
         << endl;

    t1 = clock_start();
    and_n = base->num_and();
    for (int i = 0; i < run; i++)
    {
        lowmc512.encrypt(input, output, 1);
    }
    add_comm(comm, base);
    cout << "lowmc 512 \n \t\t#AND: " << ((base->num_and() - and_n) / run)
         << "\n \t\ttime: " << time_from(t1) / 1e3 / run << "ms"
         << endl;

    t1 = clock_start();
    and_n = base->num_and();
    for (int i = 0; i < run; i++)
    {
        lowmc1024.encrypt(input, output, 1);
    }
    add_comm(comm, base);
    cout << "lowmc 1024 \n \t\t#AND: " << ((base->num_and() - and_n) / run)
         << "\n \t\ttime: " << time_from(t1) / 1e3 / run << "ms"
         << endl;
}

void bench_add(int run = 100)
{
    auto t1 = clock_start();
    int range = 20;
    for (int i = 0; i < run; i++)
    {
        Integer idx = Integer(18, 0, ALICE);

        vector<Integer> all_idx(range);
        for (int i = 0; i < range; ++i)
        {
            all_idx[i] = idx + Integer(idx.size(), i, PUBLIC);
        }
    }
    cout << "add time: " << time_from(t1) / 1e3 / 1e3 << "s" << endl;
}

void test_editing()
{
    auto t1 = clock_start();
    int size = 100;
    size_t matrix_size = 768 * 768;
    vector<Integer> database(size);
    for (int i = 0; i < size; ++i)
    {
        database[i] = Integer(700, i, BOB);
    }

    Integer idx(7, 5, ALICE);
    Integer res;
    read_vector(database, idx, res);
    cout << "read time: " << time_from(t1) / 1e3 / 1e3 << "s" << endl;
    t1 = clock_start();
    Integer ptx = res;
    bool key[128];
    PRG().random_bool(key, 128);
    LowMC<GbWire> lowmc128 = LowMC<GbWire>(key, base, 11, 128, 128, 25);
    Integer input(128, 0, PUBLIC);
    Integer output(128, 0, PUBLIC);
    for (size_t i = 0; i < matrix_size * 32 / 128; i++)
    {
        lowmc128.encrypt(input.bits.data(), output.bits.data(), 1);
    }
    cout << "encrypt time: " << time_from(t1) / 1e3 / 1e3 << "s" << endl;
}

// Return the lowest size bits of input, least significant bit first
vector<bool> bit_decomp(int input, int size)
{
    vector<bool> bits(size);
    for (int i = 0; i < size; ++i)
    {
        bits[i] = (input >> i) & 1;
    }

    cout << "bits: ";
    for (int i = 0; i < size; ++i)
    {
        cout << bits[i] << " ";
    }
    cout << endl;

    return bits;
}
// Builds a secret value of the given width. The low 64 bits are Alice's input
// and the remaining bits are Bob's.
Integer make_secret_value(int width, uint64_t low, uint64_t high)
{
    Integer low_bits(64, static_cast<int64_t>(low), ALICE);
    Integer high_bits(64, static_cast<int64_t>(high), BOB);
    Integer value(width, 0, PUBLIC);
    for (int i = 0; i < width; i++)
    {
        value.bits[i] = (i < 64) ? low_bits.bits[i] : high_bits.bits[i - 64];
    }
    return value;
}

// Opens a value of at most 128 bits and compares it with the expected halves.
bool value_matches(const Integer &value, uint64_t low, uint64_t high)
{
    uint64_t opened[2] = {0, 0};
    value.reveal(opened, PUBLIC);
    return opened[0] == low && opened[1] == high;
}

// Writes random values through a FloRAM and reads them back with scalar reads
// and, for 128-bit elements, range reads. The stash is small, so values are
// read both from the stash and, after a refresh, from the masked memory.
// Returns the number of mismatches.
int test_oram_roundtrip(int size, int unit_size, int stash_size, int writes)
{
    const int cells = 1 << size;
    const int width = 1 << unit_size;
    const int range = 3;
    const uint64_t low_mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    const uint64_t high_mask = (width > 64) ? ~0ULL : 0ULL;

    FloRAM<NetIO> oram(base, size, unit_size, stash_size);
    std::vector<uint64_t> low(cells, 0), high(cells, 0);
    // Both parties draw the same sequence of cells and values.
    std::mt19937_64 rng(20261005);
    int mismatches = 0;

    auto check_scalar = [&](int cell, const char *when)
    {
        Integer out;
        oram.read(Integer(size, cell, ALICE), out);
        if (!value_matches(out, low[cell], high[cell]))
        {
            cout << "ORAM scalar read mismatch at cell " << cell << " (" << when << ")" << endl;
            mismatches++;
        }
    };
    auto check_range = [&](int start)
    {
        std::vector<Integer> out(range, Integer(width, 0, PUBLIC));
        oram.range_read(Integer(size, start, BOB), out, range);
        for (int off = 0; off < range; off++)
        {
            if (!value_matches(out[off], low[start + off], high[start + off]))
            {
                cout << "ORAM range read mismatch at cell " << (start + off) << endl;
                mismatches++;
            }
        }
    };

    check_scalar(static_cast<int>(rng() % cells), "before any write");
    for (int w = 0; w < writes; w++)
    {
        const int cell = static_cast<int>(rng() % cells);
        low[cell] = rng() & low_mask;
        high[cell] = rng() & high_mask;
        Integer value = make_secret_value(width, low[cell], high[cell]);
        oram.write(Integer(size, cell, ALICE), [&](const Integer &in, Integer &out)
                   { out = value; });

        check_scalar(cell, "cell just written");
        check_scalar(static_cast<int>(rng() % cells), "other cell");
        if (unit_size == 7 && w % 2 == 1)
        {
            check_range(static_cast<int>(rng() % (cells - range + 1)));
        }
    }
    return mismatches;
}

// Loads a read-only FloRAM from an input array and reads every cell back.
// Returns the number of mismatches.
int test_oram_read_only(int size, int unit_size)
{
    const int cells = 1 << size;
    const int width = 1 << unit_size;
    const uint64_t low_mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    const uint64_t high_mask = (width > 64) ? ~0ULL : 0ULL;

    std::vector<uint64_t> low(cells), high(cells);
    std::vector<Integer> input(cells);
    std::mt19937_64 rng(5);
    for (int i = 0; i < cells; i++)
    {
        low[i] = rng() & low_mask;
        high[i] = rng() & high_mask;
        input[i] = make_secret_value(width, low[i], high[i]);
    }

    FloRAM<NetIO> oram(base, size, unit_size, -1, input.data(), -1, -1, true);
    int mismatches = 0;
    for (int cell = 0; cell < cells; cell++)
    {
        Integer out;
        oram.read(Integer(size, cell, BOB), out);
        if (!value_matches(out, low[cell], high[cell]))
        {
            cout << "read-only ORAM mismatch at cell " << cell << endl;
            mismatches++;
        }
    }
    return mismatches;
}

int main(int argc, char **argv)
{
    int party = atoi(argv[1]);
    int depth = 18;
    int unit_size = 7;
    int threads = 4;

    init_backend(party, depth, unit_size, threads);

    int mismatches = 0;
    mismatches += test_oram_roundtrip(6, 7, 4, 24);
    mismatches += test_oram_roundtrip(8, 5, 3, 16);
    mismatches += test_oram_read_only(5, 7);
    mismatches += test_oram_read_only(6, 4);

    base->switch_to_gt();

    delete emp::backend;
    if (mismatches != 0)
    {
        cout << "ORAM round-trip test FAILED with " << mismatches << " mismatch(es)" << endl;
        return 1;
    }
    cout << "ORAM round-trip test passed" << endl;
    return 0;
}
