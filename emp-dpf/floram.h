#ifndef EMP_DPF_FLORAM_H__
#define EMP_DPF_FLORAM_H__

#include "emp-dpf/ccrh.h"
#include "emp-dpf/randomio.h"
#include "emp-dpf/utils.h"
#include "emp-dpf/prg.h"
#include "emp-tool/emp-tool.h"
#include "emp-ot/emp-ot.h"
#include <emp-tool/utils/block.h>
#include "src/lowmc.h"

namespace emp
{

    class GbWire
    {
    public:
        block L;

        GbWire(const block &L = zero_block) : L(L) {}
    };

    template <typename T>
    class HalfGen : public Backend
    {
    public:
        T *io;
        COT<T> *ot;
        MITCCRH<8> mitccrh;

        block constant[2];
        block delta;
        uint64_t ands = 0;

        HalfGen(int party, T *io, COT<T> *ot, block delta) : Backend(party), io(io), ot(ot), delta(delta)
        {
            PRG().random_block(constant, 2);
            this->io->send_block(constant, 2);
            constant[1] = constant[1] ^ delta;
            mitccrh.setS(constant[0]);
        }

        inline void HalfGateGen(const block &LA0, const block &LB0, block *LW0)
        {
            block table[2];
            bool pa = getLSB(LA0);
            bool pb = getLSB(LB0);
            block HLA0, HA1, HLB0, HB1, W0;
            block tmp;
            block H[4];
            H[0] = LA0;
            H[1] = LA0 ^ delta;
            H[2] = LB0;
            H[3] = LB0 ^ delta;
            mitccrh.hash<2, 2>(H);
            HLA0 = H[0];
            HA1 = H[1];
            HLB0 = H[2];
            HB1 = H[3];
            table[0] = HLA0 ^ HA1;
            table[0] = table[0] ^ (select_mask[pb] & delta);
            W0 = HLA0;
            W0 = W0 ^ (select_mask[pa] & table[0]);
            tmp = HLB0 ^ HB1;
            table[1] = tmp ^ LA0;
            W0 = W0 ^ HLB0;
            W0 = W0 ^ (select_mask[pb] & tmp);
            *LW0 = W0;
            io->send_block(table, 2);
        }

        void xor_gate(void *out, const void *left, const void *right) override
        {
            ((GbWire *)out)->L = ((GbWire *)left)->L ^ ((GbWire *)right)->L;
        }

        void not_gate(void *out, const void *in) override
        {
            ((GbWire *)out)->L = ((GbWire *)in)->L ^ constant[1];
        }

        void and_gate(void *out, const void *left, const void *right) override
        {
            ands++;
            HalfGateGen(((GbWire *)left)->L, ((GbWire *)right)->L, &(((GbWire *)out)->L));
        }

        void feed(void *lbls, int party, const bool *b, size_t nel) override
        {
            GbWire *out = (GbWire *)lbls;
            if (party == PUBLIC)
            {
                for (size_t i = 0; i < nel; ++i)
                {
                    ((GbWire *)lbls)[i].L = constant[b[i]];
                }
            }
            else
            {
                block *data = new block[nel];
                if (this->party == ALICE)
                {
                    if (this->party == party)
                    {
                        ot->send_cot(data, nel);
                        for (size_t i = 0; i < nel; i++)
                        {
                            out[i].L = data[i] ^ (b[i] ? delta : zero_block);
                        }
                    }
                    else
                    {
                        ot->send_cot(data, nel);
                        for (size_t i = 0; i < nel; i++)
                        {
                            out[i].L = data[i];
                        }
                    }
                }
            }
        }

        void reveal(bool *out, int party, const void *lbls, size_t nel) override
        {
            const GbWire *in = (const GbWire *)lbls;
            bool shr[nel], ret[nel];
            for (size_t i = 0; i < nel; ++i)
            {
                shr[i] = getLSB(in[i].L);
            }
            if (this->party == ALICE)
            {
                io->send_bool(shr, nel);
                io->recv_bool(ret, nel);
            }
            else
            {
                io->recv_bool(ret, nel);
                io->send_bool(shr, nel);
            }
            for (size_t i = 0; i < nel; ++i)
            {
                out[i] = shr[i] ^ ret[i];
            }
        }

        uint64_t num_and() override
        {
            return ands;
        }
    };

    template <typename T>
    class HalfEva : public Backend
    {
    public:
        T *io;
        COT<T> *ot;
        MITCCRH<8> mitccrh;

        block constant[2];
        uint64_t ands = 0;

        HalfEva(int party, T *io, COT<T> *ot) : Backend(party), io(io), ot(ot)
        {
            this->io->recv_block(constant, 2);
            mitccrh.setS(constant[0]);
        }

        inline void HalfGateEva(const block &A, const block &B, block *W)
        {
            block table[2];
            io->recv_block(table, 2);
            block HA, HB;
            int sa, sb;
            sa = getLSB(A);
            sb = getLSB(B);
            block H[2];
            H[0] = A;
            H[1] = B;
            mitccrh.hash<2, 1>(H);
            HA = H[0];
            HB = H[1];
            *W = HA ^ HB;
            *W = *W ^ (select_mask[sa] & table[0]);
            *W = *W ^ (select_mask[sb] & table[1]);
            *W = *W ^ (select_mask[sb] & A);
        }

        void xor_gate(void *out, const void *left, const void *right) override
        {
            ((GbWire *)out)->L = ((GbWire *)left)->L ^ ((GbWire *)right)->L;
        }

        void not_gate(void *out, const void *in) override
        {
            ((GbWire *)out)->L = ((GbWire *)in)->L ^ constant[1];
        }

        void and_gate(void *out, const void *left, const void *right) override
        {
            ands++;
            HalfGateEva(((GbWire *)left)->L, ((GbWire *)right)->L, &(((GbWire *)out)->L));
        }

        void feed(void *lbls, int party, const bool *b, size_t nel) override
        {
            if (party == PUBLIC)
            {
                for (size_t i = 0; i < nel; ++i)
                {
                    ((GbWire *)lbls)[i].L = constant[b[i]];
                }
            }
            else
            {
                GbWire *out = (GbWire *)lbls;
                if (party == PUBLIC)
                {
                    for (size_t i = 0; i < nel; ++i)
                    {
                        ((GbWire *)lbls)[i].L = constant[b[i]];
                    }
                }
                else
                {
                    block *data = new block[nel];
                    bool *bb = new bool[nel];
                    for (size_t i = 0; i < nel; i++)
                        bb[i] = false;
                    if (this->party == party)
                    {
                        ot->recv_cot(data, b, nel);
                    }
                    else
                    {
                        ot->recv_cot(data, bb, nel);
                    }
                    for (size_t i = 0; i < nel; i++)
                    {
                        out[i].L = data[i];
                    }
                    delete[] bb;
                }
            }
        }

        void reveal(bool *out, int party, const void *lbls, size_t nel) override
        {
            const GbWire *in = (const GbWire *)lbls;
            bool shr[nel], ret[nel];
            for (size_t i = 0; i < nel; ++i)
            {
                shr[i] = getLSB(in[i].L);
            }
            if (this->party == ALICE)
            {
                io->send_bool(shr, nel);
                io->recv_bool(ret, nel);
            }
            else
            {
                io->recv_bool(ret, nel);
                io->send_bool(shr, nel);
            }
            for (size_t i = 0; i < nel; ++i)
            {
                out[i] = shr[i] ^ ret[i];
            }
        }

        uint64_t num_and() override
        {
            return ands;
        }
    };

    class GbStash
    {
    public:
        using Bit = Bit_T<GbWire>;
        using Integer = Integer_T<GbWire>;

        struct GbStashElement
        {
        public:
            Bit valid;
            Integer index, data;
        };

        int stash_size, stash_counter = 0, depth;
        GbStashElement *stash;

        GbStash(int stash_size, int depth) : stash_size(stash_size), depth(depth)
        {
            this->stash = new GbStashElement[stash_size];
            this->stash_counter = 0;
            for (int i = 0; i < stash_size; i++)
            {
                this->stash[i].valid = Bit(false, PUBLIC);
                this->stash[i].index = Integer(depth, 0, PUBLIC);
                this->stash[i].data = Integer(128, 0, PUBLIC);
            }
        }

        void read(const Integer &index, Bit &found, Integer &data, const bool write_back = false)
        {
            // write_back flag: if true, then the element will be deleted (invalid) after read
            for (int i = 0; i < stash_counter; i++)
            {
                Bit valid = this->stash[i].valid & (this->stash[i].index == index);
                found = found | valid;
                data = data.If(valid, this->stash[i].data);
                if (write_back)
                {
                    this->stash[i].valid = this->stash[i].valid ^ valid;
                }
            }
        }

        void write(const Integer &index, const Integer &data, const bool &valid = true)
        {
            this->stash[stash_counter].index = index;
            this->stash[stash_counter].valid = Bit(valid, PUBLIC);
            this->stash[stash_counter].data = data;
            stash_counter++;
        }

        void refresh()
        {
            this->stash_counter = 0;
            for (int i = 0; i < stash_size; i++)
            {
                this->stash[i].valid = Bit(false, PUBLIC);
                this->stash[i].index = Integer(depth, 0, PUBLIC);
            }
        }

        ~GbStash()
        {
            delete[] this->stash;
        }
    };

    template <typename T>
    class FloramMPC : public Backend
    {
    public:
        using Bit = Bit_T<GbWire>;
        using Integer = Integer_T<GbWire>;

        FloramMPC(int party, int threads, T **ios, block delta, int max_size = 24) : Backend(party), ios(ios), threads(threads), delta(delta)
        {
            this->og_thread = threads;
            this->max_size = max_size;
            // Initialize Ferret OT & DualExecution GC
            this->pool = new ThreadPool(threads - 1);
            this->ot_send = new FerretCOT<T>(ALICE, 1, ios, false, false, ferret_b11);
            this->ot_recv = new FerretCOT<T>(BOB, 1, ios, false, false, ferret_b11);
            if (this->party == ALICE)
            {
                this->ot_send->setup(delta, "./data/pre_ot");
                this->ot_recv->setup("./data/pre_ot");
                this->gate_gen = new HalfGen<T>(party, ios[0], ot_send, delta);
            }
            else
            {
                this->ot_recv->setup("./data/pre_ot");
                this->ot_send->setup(delta, "./data/pre_ot");
                this->gate_eva = new HalfEva<T>(party, ios[0], ot_recv);
            }

            // Initialize DPF
            prp = new DPFPRG *[threads];
            ccrh = new DualCCRH[threads];
            for (int i = 0; i < threads; i++)
            {
                ccrh[i] = DualCCRH(zero_block);
                prp[i] = new DPFPRG(zero_block, makeBlock(0, 1));
            }
            tree = new block[1 << (max_size + 1)];
            next = new block[1 << (max_size + 1)];
            tree_bit = new bool[1 << max_size];
            for (int i = 0; i < (1 << max_size); i++)
            {
                tree_bit[i] = false;
            }
            DPF_counter = 0;
            this->cf_lowmc_128 = new BristolFashion("./emp-dpf/files/low_mc_128.txt");

            this->cf_lowmc_1024 = new BristolFashion("./emp-dpf/files/low_mc_256.txt");
            this->cf_aes = new BristolFashion("./emp-dpf/files/aes_128.txt");
        }

        void switch_to_gt()
        {
            if (this->party == ALICE)
            {
                emp::backend = this->gate_gen;
            }
            else
            {
                emp::backend = this->gate_eva;
            }
        }

        void switch_back()
        {
            emp::backend = this;
        }

        void xor_gate(void *out, const void *left, const void *right) override
        {
            throw std::runtime_error("gates execution should be done by exec function");
        }

        void not_gate(void *out, const void *in) override
        {
            throw std::runtime_error("gates execution should be done by exec function");
        }

        void and_gate(void *out, const void *left, const void *right) override
        {
            throw std::runtime_error("gates execution should be done by exec function");
        }

        void feed(void *lbls, int party, const bool *b, size_t nel) override
        {
            GbWire *out = (GbWire *)lbls;
            if (party == PUBLIC)
            {
                if (this->party == ALICE)
                {
                    this->gate_gen->feed(lbls, party, b, nel);
                }
                else
                {
                    this->gate_eva->feed(lbls, party, b, nel);
                }
            }
            else
            {
                block *data = new block[nel];
                if (this->party == ALICE)
                {
                    if (this->party == party)
                    {
                        ot_send->send_cot(data, nel);
                        for (size_t i = 0; i < nel; i++)
                        {
                            out[i].L = data[i] ^ (b[i] ? delta : zero_block);
                        }
                    }
                    else
                    {
                        ot_send->send_cot(data, nel);
                        for (size_t i = 0; i < nel; i++)
                        {
                            out[i].L = data[i];
                        }
                    }
                }
                else
                {
                    bool *bb = new bool[nel];
                    for (size_t i = 0; i < nel; i++)
                        bb[i] = false;
                    if (this->party == party)
                    {
                        ot_recv->recv_cot(data, b, nel);
                    }
                    else
                    {
                        ot_recv->recv_cot(data, bb, nel);
                    }
                    for (size_t i = 0; i < nel; i++)
                    {
                        out[i].L = data[i];
                    }
                    delete[] bb;
                }
                delete[] data;
            }
        }

        void reveal(bool *out, int party, const void *lbls, size_t nel) override
        {
            const GbWire *in = (const GbWire *)lbls;
            bool shr[nel], ret[nel];
            for (size_t i = 0; i < nel; ++i)
            {
                shr[i] = getLSB(in[i].L);
            }
            if (this->party == ALICE)
            {
                ios[0]->send_bool(shr, nel);
                ios[0]->recv_bool(ret, nel);
            }
            else
            {
                ios[0]->recv_bool(ret, nel);
                ios[0]->send_bool(shr, nel);
            }
            for (size_t i = 0; i < nel; ++i)
            {
                out[i] = shr[i] ^ ret[i];
            }
        }

        uint64_t num_and() override
        {
            if (this->party == ALICE)
            {
                return this->gate_gen->num_and();
            }
            else
            {
                return this->gate_eva->num_and();
            }
        }

        template <typename Func, typename... Args>
        void execute(Func &&func, Args &&...args)
        {
            if (this->party == ALICE)
            {
                emp::backend = this->gate_gen;
                func(std::forward<Args>(args)...);
            }
            else
            {
                emp::backend = this->gate_eva;
                func(std::forward<Args>(args)...);
            }
        }

        ~FloramMPC()
        {
            delete this->ot_send;
            delete this->ot_recv;
            delete this->gate_gen;
            delete this->gate_eva;
            delete[] ccrh;
            for (int i = 0; i < this->threads; i++)
                delete prp[i];
            delete[] prp;
            delete cf_lowmc_128;
            delete cf_lowmc_1024;
            delete cf_aes;
            delete this->lowmc;
            delete[] this->tree;
            delete[] this->next;
            delete[] this->tree_bit;
            delete this->pool;

            std::remove("./data/pre_ot");
        }

        // Execution & OT
        T **ios;
        int threads;
        int og_thread;
        ThreadPool *pool;
        block delta;
        Backend *gate_gen, *gate_eva;
        FerretCOT<T> *ot_send, *ot_recv;
        // DPF
        DPFPRG **prp;
        DualCCRH *ccrh;
        block *tree, *next;
        bool *tree_bit;
        int DPF_counter = 0;
        int max_size = 0;
        vector<Bit> round_key;
        // ORAM
        BristolFashion *cf_lowmc_128;
        BristolFashion *cf_lowmc_1024;
        BristolFashion *cf_aes;
        LowMC<GbWire> *lowmc;
        int lowmc_128_ctr = 0;
        int lowmc_1024_ctr = 0;
        int aes_ctr = 0;
        double lowmc_128_time = 0;
        double lowmc_1024_time = 0;
        double lowmc_128_time1 = 0;
        double lowmc_1024_time1 = 0;
        double aes_time = 0;
        double aes_time1 = 0;

        inline void io_flush(int seq)
        {
            ios[seq]->flush();
        }

        inline void all_flush()
        {
            for (int i = 0; i < this->threads; i++)
                ios[i]->flush();
        }

        inline void direct_open(block *val, size_t nel = 1, int seq = -1)
        {
            // each party gets x <- x_0 ^ x_1
            block res[nel];
            if (seq == -1)
            {
                this->ios[(this->party - 1)]->send_block(val, nel);
                io_flush((this->party - 1));
                this->ios[(this->party - 1) ^ 1]->recv_block(res, nel);
            }
            else
            {
                if (this->party == ALICE)
                {
                    this->ios[seq]->send_block(val, nel);
                    this->ios[seq]->recv_block(res, nel);
                }
                else
                {
                    this->ios[seq]->recv_block(res, nel);
                    this->ios[seq]->send_block(val, nel);
                }
                io_flush(seq);
            }
            for (size_t i = 0; i < nel; i++)
            {
                val[i] = res[i] ^ val[i];
            }
        }

        inline void commit_then_open(block &val, int seq = -1)
        {
            // each party commits x_i then gets x <- x_0 ^ x_1
            block hash_value, commitment, res;
            hash_value = Hash().hash_for_block(&val, sizeof(val));
            if (seq == -1)
            {
                this->ios[(this->party - 1)]->send_block(&hash_value, 1);
                io_flush((this->party - 1));
                this->ios[(this->party - 1) ^ 1]->recv_block(&commitment, 1);
                this->ios[(this->party - 1)]->send_block(&val, 1);
                io_flush((this->party - 1));
                this->ios[(this->party - 1) ^ 1]->recv_block(&res, 1);
            }
            else
            {
                if (this->party == ALICE)
                {
                    this->ios[seq]->send_block(&hash_value, 1);
                    this->ios[seq]->recv_block(&commitment, 1);
                    this->ios[seq]->send_block(&val, 1);
                    this->ios[seq]->recv_block(&res, 1);
                }
                else
                {
                    this->ios[seq]->recv_block(&commitment, 1);
                    this->ios[seq]->send_block(&hash_value, 1);
                    this->ios[seq]->recv_block(&res, 1);
                    this->ios[seq]->send_block(&val, 1);
                }
                io_flush(seq);
            }
            hash_value = Hash().hash_for_block(&res, sizeof(res));
            if (cmpBlock(&commitment, &hash_value, 1) == false)
            {
                error("commitment check failed");
            }
            val = res ^ val;
        }

        void DPF(const Integer &alpha, const Integer &beta, int depth, block **r, block **t = nullptr,
                 bool **t_bit = nullptr)
        {
            // input: alpha(bit 0...depth-1), beta(128 bits),
            // &r, &t (address of a pointer, not necessary to initialize)
            // r[alpha<<1] = share of beta, r[alpha<<1|1] = share of beta*delta, t[alpha] = share of delta
            DPF_counter += 1;
            block W = makeBlock(0, DPF_counter);
            block *tree = this->tree, *next = this->next;
            tree[0] = this->delta ^ W;

            block init_cw[depth];
            {
                block K[depth], M[depth];
                bool b[depth];
                for (int i = 0; i < depth; i++)
                {
                    b[i] = getLSB(alpha.bits[depth - i - 1].bit.L);
                    init_cw[i] = this->delta;
                }
                if (this->party == ALICE)
                {
                    this->ot_send->send_cot(K, depth);
                    this->ot_recv->recv_cot(M, b, depth);
                }
                else
                {
                    this->ot_recv->recv_cot(M, b, depth);
                    this->ot_send->send_cot(K, depth);
                }
                for (int i = 0; i < depth; i++)
                {
                    init_cw[i] ^= K[i] ^ M[i] ^ (b[i] ? this->delta : zero_block);
                }
            }

            for (int i = 0; i < depth; i++)
            {
                block cw = init_cw[i];
                if (i == 0)
                {
                    tree_bit[0] = getSLSB(tree[0]);
                    ccrh[0].ro_expand_1to2(next, tree[0]);
                    cw ^= next[0];
                }
                else if (i == 1)
                {
                    tree_bit[0] = getSLSB(tree[0]);
                    tree_bit[1] = getSLSB(tree[1]);
                    ccrh[0].ro_expand_2to4(next, tree);
                    cw ^= next[0] ^ next[2];
                }
                else
                {
                    block cw_par[this->threads];
                    memset(cw_par, 0, sizeof(block) * this->threads);
                    this->execute_parallel([&](int seq, size_t start, size_t end)
                                           {
                        for (size_t j = start; j < end; j++) {
                            tree_bit[j << 2] = getSLSB(tree[j << 2]);
                            tree_bit[j << 2 | 1] = getSLSB(tree[j << 2 | 1]);
                            tree_bit[j << 2 | 2] = getSLSB(tree[j << 2 | 2]);
                            tree_bit[j << 2 | 3] = getSLSB(tree[j << 2 | 3]);
                            ccrh[seq].ro_expand_4to8(&next[j << 3], &tree[j << 2]);
                            cw_par[seq] ^= next[j << 3] ^ next[j << 3 | 2] ^ next[j << 3 | 4] ^ next[j << 3 | 6];
                        } }, 0, (1 << (i - 2)));
                    for (int j = 0; j < this->threads; j++)
                        cw ^= cw_par[j];
                }
                direct_open(&cw);
                block cw_choice[2] = {zero_block, cw};
                this->execute_parallel([&](int seq, size_t start, size_t end)
                                       {
                    for (size_t j = start; j < end; j++) {
                        next[j << 1] ^= cw_choice[tree_bit[j]];
                        next[j << 1 | 1] ^= cw_choice[tree_bit[j]];
                    } }, 0, 1 << i);
                std::swap(tree, next);
            }
            block sum;
            pack_value(beta, sum);
            block sum_par[this->threads];
            memset(sum_par, 0, sizeof(block) * this->threads);
            if (depth == 0)
            {
                tree_bit[0] = getSLSB(tree[0]);
                prp[0]->node_expand_one(&next[0], &tree[0]);
                sum ^= next[0];
            }
            else if (depth == 1)
            {
                tree_bit[0] = getSLSB(tree[0]);
                tree_bit[1] = getSLSB(tree[1]);
                prp[0]->node_expand_two(&next[0], &tree[0]);
                sum ^= next[0] ^ next[1];
            }
            else
            {
                this->execute_parallel([&](int seq, size_t start, size_t end)
                                       {
                    for (size_t j = start; j < end; j++) {
                        tree_bit[j << 2] = getSLSB(tree[j << 2]);
                        tree_bit[j << 2 | 1] = getSLSB(tree[j << 2 | 1]);
                        tree_bit[j << 2 | 2] = getSLSB(tree[j << 2 | 2]);
                        tree_bit[j << 2 | 3] = getSLSB(tree[j << 2 | 3]);
                        prp[seq]->node_expand_four(&next[j << 2], &tree[j << 2]);
                        sum_par[seq] ^= next[j << 2] ^ next[j << 2 | 2] ^ next[j << 2 | 1] ^ next[j << 2 | 3];
                    } }, 0, 1 << (depth - 2));
                for (int i = 0; i < this->threads; i++)
                {
                    sum ^= sum_par[i];
                }
            }
            direct_open(&sum);
            block sum_choice[2] = {zero_block, sum};
            this->execute_parallel([&](int seq, size_t start, size_t end)
                                   {
                for (size_t j = start; j < end; j++) {
                    next[j] ^= sum_choice[tree_bit[j]];
                } }, 0, 1 << depth);
            *r = next;
            if (t != nullptr)
                *t = tree;
            if (t_bit != nullptr)
                *t_bit = tree_bit;
        }

        void pack_value(const Integer &in, block &out)
        {
            out = zero_block;
            for (int i = 0; i < 64; i++)
            {
                out[0] ^= ((long long)getLSB(in.bits[i].bit.L)) << i;
                out[1] ^= ((long long)getLSB(in.bits[i + 64].bit.L)) << i;
            }
        }

        void unpack_value(block in, Integer &out)
        {
            // using COT to generate one first, then zero check (put into check buffer)
            out = Integer(128, 0, PUBLIC);
            bool *b = new bool[128];
            block *M = new block[128];
            for (int i = 0; i < 128; i++)
            {
                b[i] = block_at_bit(in, i);
            }
            if (this->party == ALICE)
            {
                this->ot_send->send_cot(M, 128);
            }
            else
            {
                this->ot_recv->recv_cot(M, b, 128);
            }
            for (int i = 0; i < 128; i++)
            {
                if (this->party == ALICE)
                {
                    out.bits[i].bit.L = b[i] ? (M[i] ^ this->delta) : M[i];
                }
                else
                {
                    out.bits[i].bit.L = M[i];
                }
            }
            delete[] b;
            delete[] M;
        }

        void batch_unpack_value(block *in, Integer *out, int len)
        {
            // using COT to generate one first, then zero check (put into check buffer)

            Integer zero = Integer(128, 0, PUBLIC);
            int cot_size = 128 * len;
            bool *b = new bool[cot_size];
            block *M = new block[cot_size];
            for (int j = 0; j < len; j++)
            {
                for (int i = 0; i < 128; i++)
                {
                    b[j * 128 + i] = block_at_bit(in[j], i);
                }
            }
            if (this->party == ALICE)
            {
                this->ot_send->send_cot(M, cot_size);
            }
            else
            {
                this->ot_recv->recv_cot(M, b, cot_size);
            }
            for (int j = 0; j < len; ++j)
            {
                out[j] = zero;
                for (int i = 0; i < 128; i++)
                {
                    if (this->party == ALICE)
                    {
                        out[j].bits[i].bit.L = b[j * 128 + i] ? (M[j * 128 + i] ^ this->delta) : M[j * 128 + i];
                    }
                    else
                    {
                        out[j].bits[i].bit.L = M[j * 128 + i];
                    }
                }
            }
            delete[] b;
            delete[] M;
        }

        void random_sample(Integer &out, int nel = 128)
        {
            // Sample a random value and send the authenticated shares to each party
            // Samples nel random boolean values
            // Can be considered as both Alice and Bob sample a uniform value, then xor them
            // out -> nel bits authenticated boolean array
            out = Integer(nel, 0, PUBLIC);
            block *M = new block[nel];
            if (this->party == ALICE)
            {
                this->ot_send->rcot(M, nel);
            }
            else
            {
                this->ot_recv->rcot(M, nel);
            }
            for (int i = 0; i < nel; i++)
            {
                out.bits[i].bit.L = M[i];
            }
            delete[] M;
        }

        // Evaluates AES-128 under the key (real_LowMC_128 evaluates LowMC).
        // Key, message and output use the byte order of a block in memory, so
        // out holds the parties' shares of AES_ecb_encrypt_blks applied to the
        // block whose low bits are msg. refresh() computes the same value
        // locally to mask ORAM_R.
        void LowMC_128(const Integer &msg, const Integer &key, block &out)
        {
            aes_ctr++;
            auto t1 = clock_start();
            // AES128: output f(k,msg)
            // extend msg (from any bits to 128)
            Bit key_msg[256], res[128];
            for (int i = 0; i < 128; i++)
            {
                key_msg[endian_reverse(i)] = key.bits[i];
            }
            // padding msg to 128 bits
            for (size_t i = 0; i < msg.size(); i++)
            {
                key_msg[endian_reverse(i) + 128] = msg.bits[i];
            }
            for (size_t i = msg.size(); i < 128; i++)
            {
                key_msg[endian_reverse(i) + 128] = Bit(false, PUBLIC);
            }
            auto t2 = clock_start();
            this->execute(
                [this, &key_msg, &res]()
                {
                    this->cf_aes->compute(res, key_msg);
                });
            aes_time1 += time_from(t2);
            out = zero_block;
            for (int i = 0; i < 64; i++)
            {
                out[0] ^= ((long long)getLSB(res[endian_reverse(i)].bit.L)) << i;
                out[1] ^= ((long long)getLSB(res[endian_reverse(i + 64)].bit.L)) << i;
            }
            aes_time += time_from(t1);
        }

        // LowMC 128bit
        void real_LowMC_128(const Integer &msg, const Integer &key, block &out)
        {
            auto t1 = clock_start();
            lowmc_128_ctr++;
            Bit round_key_and_msg[32384 + 128], res[128];
            int round_key_and_key_size = 32384;
            // first put in the key
            for (size_t i = 0; i < 128; i++)
            {
                round_key_and_msg[i] = key.bits[i];
            }
            // then put in the round key
            for (size_t i = 0; i < round_key_and_key_size; i++)
            {
                round_key_and_msg[i + 128] = this->round_key[i];
            }

            // put in the msg
            for (size_t i = 0; i < msg.size(); i++)
            {
                round_key_and_msg[round_key_and_key_size + i] = msg.bits[i];
            }
            for (size_t i = msg.size(); i < 128; i++)
            {
                round_key_and_msg[i + round_key_and_key_size] = Bit(false, PUBLIC);
            }
            auto t2 = clock_start();
            this->execute(
                [this, &round_key_and_msg, &res]()
                {
                    this->cf_lowmc_128->compute(res, round_key_and_msg);
                });
            lowmc_128_time1 += time_from(t2);
            out = zero_block;
            for (int i = 0; i < 64; i++)
            {
                out[0] ^= ((long long)getLSB(res[i].bit.L)) << i;
                out[1] ^= ((long long)getLSB(res[i + 64].bit.L)) << i;
            }
            lowmc_128_time += time_from(t1);
        }

        // LowMC
        void LowMC_1024(const Integer &msg, const Integer &key, block *out)
        {
            int round_key_and_key_size = 3840;
            int block_size = 256;

            auto t1 = clock_start();
            lowmc_1024_ctr++;
            Bit round_key_and_msg[round_key_and_key_size + block_size], res[block_size];
            // first put in the key
            for (size_t i = 0; i < key.size(); i++)
            {
                round_key_and_msg[i] = key.bits[i];
            }
            // zero-pad the key to block_size bits
            for (size_t i = key.size(); i < block_size; i++)
            {
                round_key_and_msg[i] = Bit(false, PUBLIC);
            }
            // then put in the round key
            for (size_t i = 0; i < round_key_and_key_size; i++)
            {
                round_key_and_msg[i + block_size] = this->round_key[i];
            }

            // put in the msg
            for (size_t i = 0; i < msg.size(); i++)
            {
                round_key_and_msg[i + round_key_and_key_size] = msg.bits[i];
            }
            for (size_t i = msg.size(); i < block_size; i++)
            {
                round_key_and_msg[i + round_key_and_key_size] = Bit(false, PUBLIC);
            }
            auto t2 = clock_start();
            this->execute(
                [this, &round_key_and_msg, &res]()
                {
                    this->cf_lowmc_1024->compute(res, round_key_and_msg);
                });
            lowmc_1024_time1 += time_from(t2);

            for (int j = 0; j < 2; j++)
            {
                for (int i = 0; i < 64; i++)
                {
                    out[j][0] ^= ((long long)getLSB(res[i + j * 128].bit.L)) << i;
                    out[j][1] ^= ((long long)getLSB(res[i + j * 128 + 64].bit.L)) << i;
                }
            }
            lowmc_1024_time += time_from(t1);
        }

        template <typename Func>
        // parameter: int seq, size_t start, size_t end
        inline void execute_parallel(Func &&func, size_t left, size_t right)
        {
            if (threads == 1)
            {
                func(0, left, right);
                return;
            }
            std::vector<std::future<void>> fut;
            size_t width = (right - left) / (this->threads);
            size_t start = left + width, end = min(right, start + width);
            if (width > 0)
            {
                for (int i = 0; i < threads - 2; i++)
                {
                    fut.push_back(this->pool->enqueue(func, i, start, end));
                    start = end;
                    end += width;
                }
            }
            end = right;
            fut.push_back(this->pool->enqueue(func, threads - 2, start, end));
            func(threads - 1, left, left + width);
            for (auto &f : fut)
                f.get();
        }
    };

    template <typename T>
    class FloRAM
    {
    public:
        using Bit = Bit_T<GbWire>;
        using Integer = Integer_T<GbWire>;

#ifdef BREAKDOWN
        double ind_read[4] = {0, 0, 0, 0};
        double access[8] = {0, 0, 0, 0, 0, 0, 0, 0};
#endif
        int oram_size, unit_size, unit_element, size;
        Integer ka, kb;
        GbStash *stash;
        block *ORAM_R, *ORAM_W;
        FloramMPC<T> *base;
        bool read_only = false, if_large_block = false;
        double range_t1 = 0, range_t2 = 0, range_t3 = 0, range_t4 = 0, range_more = 0;
        double single_t1 = 0, single_t2 = 0, single_t3 = 0, single_t4 = 0;
        int more_ctr = 0;

        // initialize ORAM part
        FloRAM(FloramMPC<T> *base, int size = 24, int unit_size = 5, int stash_size = -1, Integer *in = nullptr,
               int st = -1, int ed = -1, bool read_only = false, bool if_large_block = false)
        {
            if (size > base->max_size)
                throw std::runtime_error("ORAM size bigger than base max size");
            // range_read removes the mask of a large block with LowMC_1024,
            // which does not match the AES mask that refresh() applies.
            if (if_large_block)
                throw std::runtime_error("large-block range reads are not supported");
            // Each block contains 16 bytes -> 2^{7-unit_size} elements
            this->read_only = read_only;
            this->size = size;
            this->base = base;
            this->unit_size = unit_size;
            this->unit_element = 7 - unit_size;
            // refresh() processes physical blocks four at a time, so keep at
            // least two physical address bits.  Tiny logical domains
            // leave the additional blocks unreachable.
            this->oram_size = std::max(2, size - unit_element);
            this->if_large_block = if_large_block;

            // Initialize stash
            if (stash_size == -1)
                stash_size = std::max(1, int(sqrt(1 << (size)) / (20 * sqrt(base->threads))));
            this->stash = new GbStash(stash_size, oram_size);
            // Initialize ORAM
            ORAM_R = new block[1 << (oram_size + 1)];
            ORAM_W = new block[1 << (oram_size + 1)];
            memset(ORAM_W, 0, sizeof(block) * (1 << (oram_size + 1)));
            // Put the input array into ORAM
            if (in != nullptr)
            {
                if (st == -1 && ed == -1)
                {
                    st = 0;
                    ed = (1 << size);
                }
                Integer *compact = new Integer[1 << oram_size];
                for (int i = (st >> unit_element); i <= ((ed - 1) >> unit_element); i++)
                {
                    compact[i] = Integer(128, 0, PUBLIC);
                }
                for (int i = st; i < ed; i++)
                {
                    for (int j = 0; j < (1 << unit_size); j++)
                    {
                        compact[i >> unit_element].bits[(i % (1 << unit_element)) * (1 << unit_size) +
                                                        j] = in[i].bits[j];
                    }
                }
                for (int i = (st >> unit_element); i <= ((ed - 1) >> unit_element); i++)
                {
                    base->pack_value(compact[i], ORAM_W[i]);
                }
                delete[] compact;
            }
            // Initial refresh
            this->refresh(this->ka, this->kb);
        }

        ~FloRAM()
        {
            delete[] this->ORAM_R;
            delete[] this->ORAM_W;
            delete this->stash;
        }

        void read(const Integer &long_idx, Integer &out, bool outside = true, bool *t_bit = nullptr)
        {
            if (long_idx.size() > size)
                throw std::runtime_error("index out of range");
            // idx: 0-1 4bytes read, 2+ 16bytes read
            Integer idx = long_idx;
            Integer normalized_long_idx = long_idx;
            if (outside)
            {
                // Packed FloRAMs can have more physical address bits plus
                // in-block selector bits than a tiny logical index exposes.
                // Those missing high bits are public zero, not array entries
                // beyond the end of long_idx.bits.
                normalized_long_idx.resize(unit_element + oram_size, false);
                idx = Integer(oram_size, 0, PUBLIC);
                for (int i = 0; i < oram_size; i++)
                {
                    idx.bits[i] = normalized_long_idx.bits[i + unit_element];
                }
            }

            // Part A: compute DPF
            auto t1 = clock_start();
            block *t, v_shr = zero_block;
            static const int log_size = 7;
#ifdef BREAKDOWN
            block dummy = zero_block;
            auto start = clock_start();
#endif
            if (outside)
            {
                Integer short_idx = Integer(std::max(0, oram_size - log_size), 0, PUBLIC);
                for (int i = 0; i < oram_size - log_size; i++)
                {
                    short_idx.bits[i] = idx.bits[i + log_size];
                }
                // calculate 2^(idx % 128)
                Integer mux = Integer(128, 1, PUBLIC);
                base->execute([&](Integer &out)
                              {
                    for (int cnt = 0; cnt < std::min(log_size, oram_size); cnt++) {
                        int width = 1 << cnt;
                        for (int i = 0; i < width; i++) {
                            out.bits[i | width] = out.bits[i | width].If(idx.bits[cnt], out.bits[i]);
                            out.bits[i] ^= out.bits[i | width];
                        }
                    } }, mux);
                base->DPF(short_idx, mux, std::max(0, oram_size - log_size), &t);
#ifdef BREAKDOWN
                base->commit_then_open(dummy);
                ind_read[0] += time_from(start);
                start = clock_start();
#endif
            }
            single_t1 += time_from(t1);
            // Part B: OROM access
            t1 = clock_start();
            block v_shr_par[base->threads];
            memset(v_shr_par, 0, sizeof(v_shr_par));
            // Using DPF (t value) to retrieve [(R[i]||R[i]*delta) ^ f(k_0,i) ^ f(k_1,i)]
            if (outside)
            {
                base->execute_parallel([&](int seq, size_t start, size_t end)
                                       {
                    for (size_t i = start; i < end; i++) {
                        bool bit = block_at_bit(t[i >> (log_size)], i % (1 << log_size));
                        block value[2] = {zero_block, ORAM_R[i]};
                        v_shr_par[seq] ^= value[bit];
                    } }, 0, (1 << oram_size));
            }
            else
            {
                base->execute_parallel([&](int seq, size_t start, size_t end)
                                       {
                    for (size_t i = start; i < end; i++) {
                        block value[2] = {zero_block, ORAM_R[i]};
                        v_shr_par[seq] ^= value[(t_bit)[i]];
                    } }, 0, (1 << oram_size));
            }
            for (int i = 0; i < base->threads; i++)
            {
                v_shr ^= v_shr_par[i];
            }
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside)
            {
                ind_read[1] += time_from(start);
            }
            else
            {
                access[1] += time_from(start);
            }
            start = clock_start();
#endif
            single_t2 += time_from(t1);
            t1 = clock_start();
            // Part C: AES-MPC
            // refresh() masks ORAM_R under both keys in every mode.
            block AES_result;
            base->LowMC_128(idx, ka, AES_result);
            v_shr ^= AES_result;
            base->LowMC_128(idx, kb, AES_result);
            v_shr ^= AES_result;

#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside)
            {
                ind_read[2] += time_from(start);
            }
            else
            {
                access[2] += time_from(start);
            }
            start = clock_start();
#endif
            // Part D: Stash access
            Integer val;
            base->unpack_value(v_shr, val);
            single_t3 += time_from(t1);
            t1 = clock_start();
            if (!read_only)
            {
                base->execute(
                    [this, idx, &val, outside]()
                    {
                        Bit found(false, PUBLIC);
                        Integer v_in_stash(128, 0, PUBLIC);
                        this->stash->read(idx, found, v_in_stash, !outside);
                        val = val.If(found, v_in_stash);
                    });
            }
            if (outside)
            {
                read_into_f2k(val, normalized_long_idx, out);
            }
            else
            {
                out = val;
            }
            single_t4 += time_from(t1);
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside)
            {
                ind_read[3] += time_from(start);
            }
            else
            {
                access[3] += time_from(start);
            }
#endif
        }

        void range_read(const Integer &long_idx, vector<Integer> &out, int range)
        {
            if (range <= 0)
                throw std::runtime_error("range must be positive");
            if (long_idx.size() > size)
                throw std::runtime_error("index out of range");

            // Phase 0: split block/element indices
            Integer normalized_long_idx = long_idx;
            normalized_long_idx.resize(unit_element + oram_size, false);
            Integer idx = normalized_long_idx; // block index inside ORAM

            idx = Integer(oram_size, 0, PUBLIC);
            for (int i = 0; i < oram_size; ++i)
                idx.bits[i] = normalized_long_idx.bits[i + unit_element];

            // Phase 1: evaluate one DPF
            auto t1 = clock_start();
            static const int LOG = 7; // 2^7 bits per block in t[]
            block *t;                 // one-hot share for idx
            {
                Integer short_idx = Integer(std::max(0, oram_size - LOG), 0, PUBLIC);
                for (int i = 0; i < oram_size - LOG; ++i)
                    short_idx.bits[i] = idx.bits[i + LOG];

                // mux = 2^{idx mod 128}, as in the scalar read
                Integer mux = Integer(128, 1, PUBLIC);
                base->execute([&](Integer &m)
                              {
        for (int b = 0; b < std::min(LOG, oram_size); ++b) {
            int w = 1 << b;
            for (int j = 0; j < w; ++j) {
                m.bits[j | w] = m.bits[j | w].If(idx.bits[b], m.bits[j]);
                m.bits[j]    ^= m.bits[j | w];
            }
        } }, mux);

                base->DPF(short_idx, mux,
                          std::max(0, oram_size - LOG), /*depth*/
                          &t);
            }
            range_t1 += time_from(t1);
            t1 = clock_start();
            // Phase 2: single ORAM scan

            std::vector<std::vector<block>> v_par(base->threads,
                                                  std::vector<block>(range, zero_block));
            base->execute_parallel([&](int seq, size_t begin, size_t end)
                                   {
                                       std::vector<block> local(range, zero_block);
                                       for (size_t i = begin; i < end; ++i)
                                       {
                                           block val = ORAM_R[i]; // read once
                                           for (int off = 0; off < range; ++off)
                                           {
                                               // i-th block in ORAM_R corresponds to off-th element in the range if t[i-off] = 1
                                               if (i < static_cast<size_t>(off))
                                                   continue;

                                               size_t src = i - off; // position in t
                                               bool bit = block_at_bit(t[src >> LOG],
                                                                       src & ((1 << LOG) - 1));
                                               // Use array indexing like regular read to avoid branch prediction issues
                                               block value[2] = {zero_block, val};
                                               local[off] ^= value[bit];
                                           }
                                       }
                                       v_par[seq].swap(local); // move into thread slot
                                   },
                                   0, 1u << oram_size);

            /* XOR the per-thread accumulators */
            std::vector<block> v_acc(range, zero_block);
            for (int th = 0; th < base->threads; ++th)
            {
                for (int off = 0; off < range; ++off)
                {
                    v_acc[off] ^= v_par[th][off];
                }
            }
            vector<Integer> all_idx(range);
            for (size_t i = 0; i < range; i++)
            {
                all_idx[i] = Integer(idx.size(), i) + idx;
            }

            range_t2 += time_from(t1);
            t1 = clock_start();
            // Phase 3: unmask each accumulator and decode
            // Process in batches of 2 blocks (256 bits)
            if (if_large_block)
            {
                for (int batch_start = 0; batch_start < range; batch_start += 2)
                {
                    int batch_size = std::min(2, range - batch_start);

                    // 256-bit message holding up to 2 indices
                    Integer batch_msg(256, 0, PUBLIC);

                    // Pack up to 2 indices into the message, using precomputed all_idx
                    for (int i = 0; i < batch_size; i++)
                    {
                        int off = batch_start + i;
                        Integer idx_of = all_idx[off];

                        // Copy idx_of bits into the appropriate position in batch_msg (128 bits per index)
                        for (int j = 0; j < std::min(idx_of.size(), size_t(128)); j++)
                        {
                            batch_msg.bits[i * 128 + j] = idx_of.bits[j];
                        }
                    }

                    // Process the batch with a single LowMC_1024 call for each key
                    block batch_results[2] = {zero_block};

                    if (!read_only)
                    {
                        base->LowMC_1024(batch_msg, ka, batch_results);
                    }
                    base->LowMC_1024(batch_msg, kb, batch_results);

                    // Apply results to the appropriate accumulators
                    for (int i = 0; i < batch_size; i++)
                    {
                        int off = batch_start + i;
                        v_acc[off] ^= batch_results[i];
                        v_acc[off] ^= batch_results[i];
                    }
                }
            }
            else
            {
                for (int off = 0; off < range; ++off)
                {
                    block tmp;
                    base->LowMC_128(all_idx[off], ka, tmp);
                    v_acc[off] ^= tmp;
                    base->LowMC_128(all_idx[off], kb, tmp);
                    v_acc[off] ^= tmp;
                }
            }
            range_t3 += time_from(t1);
            t1 = clock_start();
            // Process each element to extract the needed word bits
            for (int off = 0; off < range; ++off)
            {
                /* unpack the 128-bit block, then select the wanted word bits */
                Integer val128;
                base->unpack_value(v_acc[off], val128);

                // Writes remain in the stash until the next refresh.  A
                // scalar read overlays the newest matching stash entry on the
                // value recovered from ORAM_R; range_read does the same.
                if (!read_only)
                {
                    base->execute(
                        [this, &all_idx, off, &val128]()
                        {
                            Bit found(false, PUBLIC);
                            Integer value_in_stash(128, 0, PUBLIC);
                            this->stash->read(all_idx[off], found, value_in_stash);
                            val128 = val128.If(found, value_in_stash);
                        });
                }

                /* long_idx_off = long_idx + off  (unit-element bits needed only) */
                Integer pos = normalized_long_idx;
                if (off)
                {
                    Integer add = Integer(pos.size(), off, PUBLIC);
                    base->execute([&]()
                                  { pos = pos + add; });
                }

                read_into_f2k(val128, pos, out[off]); // final logical word
            }
            range_t4 += time_from(t1);
        }

        void read(const int long_idx, Integer &out, bool outside = true)
        {
            int32_t idx = long_idx >> unit_element;
            block v_shr = ORAM_W[idx];
            Integer val;
            base->unpack_value(v_shr, val);
            if (!outside)
            {
                out = val;
                return;
            }
            out = Integer(1 << unit_size, 0, PUBLIC);
            for (int i = 0; i < (1 << unit_size); i++)
            {
                out.bits[i] = val.bits[((long_idx % (1 << unit_element)) << unit_size) + i];
            }
        }

        void read_batch(Integer *out, const int long_idx_start = -1, const int long_idx_end = -1)
        {
            int32_t start_idx = 0;
            int32_t end_idx = 1 << oram_size;

            if (long_idx_start != -1 && long_idx_end != -1)
            {
                start_idx = long_idx_start >> unit_element;
                end_idx = long_idx_end >> unit_element;
            }
            else if (long_idx_start == -1 && long_idx_end == -1)
            {
                // read all
                start_idx = 0;
                end_idx = 1 << oram_size;
            }
            else if (long_idx_start < 0 || long_idx_end > (1 << size))
            {
                throw std::runtime_error("batch read index out of range");
            }

            int n_block = end_idx - start_idx;
            block *v_shrs = new block[n_block];
            // Copy ORAM_W blocks into v_shrs with proper offset
            for (int j = start_idx; j < end_idx; ++j)
            {
                v_shrs[j - start_idx] = ORAM_W[j];
            }
            Integer *val = new Integer[n_block];
            base->batch_unpack_value(v_shrs, val, n_block);

            // For each long index, use the correct block and bit offset.
            for (int i = long_idx_start; i < long_idx_end; i++)
            {
                out[i] = Integer(1 << unit_size, 0, PUBLIC);
                int block_index = (i >> unit_element) - start_idx;
                int bit_offset = (i % (1 << unit_element)) << unit_size;
                for (int j = 0; j < (1 << unit_size); j++)
                {
                    out[i].bits[j] = val[block_index].bits[bit_offset + j];
                }
            }
            delete[] v_shrs;
            delete[] val;
        }

        template <typename Func>
        void write(const Integer &long_idx, Func &&func, Integer *in = nullptr)
        {
            if (long_idx.size() > size)
                throw std::runtime_error("index out of range");
            Integer normalized_long_idx = long_idx;
            normalized_long_idx.resize(unit_element + oram_size, false);
            Integer idx(oram_size, 0, PUBLIC);
            for (int i = 0; i < oram_size; i++)
            {
                idx.bits[i] = normalized_long_idx.bits[i + unit_element];
            }

#ifdef BREAKDOWN
            auto start = clock_start();
            block dummy = zero_block;
#endif
            // Part A: Mal-DPF
            Integer beta;
            base->random_sample(beta, 128);
            block *r, *t;
            bool *tbit;
            base->DPF(idx, beta, oram_size, &r, &t, &tbit);
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            access[0] += time_from(start);
#endif

            // Part B: get D[alpha] value and apply new value
            Integer origin = Integer(128, 0, PUBLIC);
            this->read(idx, origin, false, tbit);

#ifdef BREAKDOWN
            start = clock_start();
#endif
            // Part C: apply new function
            Integer origin_32, value_32, value;
            read_into_f2k(origin, normalized_long_idx, origin_32);
            if (in != nullptr)
            {
                *in = Integer(origin_32);
            }
            base->execute(func, origin_32, value_32);
            base->execute([&]()
                          { value_32 ^= origin_32; });
            write_from_f2k(value_32, normalized_long_idx, value);
            base->execute([&]()
                          { value ^= origin; });
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            access[4] += time_from(start);
            start = clock_start();
#endif

            // Part D: write into stash
            this->stash->write(idx, value);
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            access[5] += time_from(start);
            start = clock_start();
#endif

            // Part E: write into OWOM
            base->execute([&]()
                          { value ^= origin ^ beta; });
            block variation[2];
            base->pack_value(value, variation[0]);
            base->direct_open(variation, 1);
            variation[1] = zero_block;
            base->execute_parallel([&](int seq, size_t start, size_t end)
                                   {
                for (size_t i = start; i < end; i++) {
                    ORAM_W[i] ^= r[i] ^ variation[!getSLSB(t[i])];
                } }, 0, (1 << oram_size));
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            access[6] += time_from(start);
            start = clock_start();
#endif

            // Part F: refresh
            if (this->stash->stash_counter == this->stash->stash_size)
            {
                this->refresh(this->ka, this->kb);
            }
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            access[7] += time_from(start);
#endif
        }

        template <typename Func>
        void write(const int long_idx, Func &&func)
        {
            int32_t idx = long_idx >> unit_element;
            Integer origin;
            this->read(long_idx, origin, false);
            Integer origin_32, value_32, value;
            origin_32 = Integer(1 << unit_size, 0, PUBLIC);
            for (int i = 0; i < (1 << unit_size); i++)
            {
                origin_32.bits[i] = origin.bits[((long_idx % (1 << unit_element)) << unit_size) + i];
            }
            base->execute(func, origin_32, value_32);
            base->execute([&]()
                          { value_32 ^= origin_32; });
            value = Integer(128, 0, PUBLIC);
            for (int i = 0; i < (1 << unit_size); i++)
            {
                value.bits[((long_idx % (1 << unit_element)) << unit_size) + i] = value_32.bits[i];
            }
            base->execute([&]()
                          { value ^= origin; });
            this->stash->write(Integer(oram_size, idx, PUBLIC), value);
            base->pack_value(value, ORAM_W[idx]);
            if (this->stash->stash_counter == this->stash->stash_size)
            {
                this->refresh(this->ka, this->kb);
            }
        }

        void refresh(Integer &ka, Integer &kb)
        {
            this->stash->refresh();
            // Each party masks its share of the memory with AES under a fresh
            // key of its own. Reads remove both masks inside the circuit.
            block key;
            PRG().random_block(&key, 1);
            // expand aes key
            AES_KEY aes_key[2];
            AES_set_encrypt_key((const block)key, aes_key);
            // authenticate key
            ka = Integer(&key, ALICE);
            kb = Integer(&key, BOB);

            base->execute_parallel([&](int seq, size_t start, size_t end)
                                   {
                for (size_t i = start; i < end; i++) {
                    ORAM_R[i << 2] = makeBlock(0, i << 2);
                    ORAM_R[(i << 2) | 1] = makeBlock(0, (i << 2) | 1);
                    ORAM_R[(i << 2) | 2] = makeBlock(0, (i << 2) | 2);
                    ORAM_R[(i << 2) | 3] = makeBlock(0, (i << 2) | 3);
                    ParaEnc<1, 4>(&ORAM_R[i << 2], aes_key);
                    ORAM_R[i << 2] ^= ORAM_W[i << 2];
                    ORAM_R[(i << 2) | 1] ^= ORAM_W[(i << 2) | 1];
                    ORAM_R[(i << 2) | 2] ^= ORAM_W[(i << 2) | 2];
                    ORAM_R[(i << 2) | 3] ^= ORAM_W[(i << 2) | 3];
                } }, 0, 1 << (oram_size - 2));

            size_t left = 0;
            base->all_flush();
            do
            {
                size_t right = min((size_t)(1 << (oram_size)), left + (1 << 16));
                base->execute_parallel([&](int seq, size_t start, size_t end)
                                       { base->direct_open(&ORAM_R[start], end - start, seq); }, left, right);
                left = right;
            } while (left < 1u << (oram_size));
            base->all_flush();
        }

        void read_into_f2k(Integer val, Integer pos, Integer &out)
        {
            // val: 128 bits, pos: unit_element bits, out: 2^unit_size bits
            out = Integer(1 << unit_size, 0, PUBLIC);
            for (int i = 0; i < (1 << unit_size); i++)
            {
                out.bits[i] = val.bits[i];
            }
            base->execute([&]()
                          {
                Bit mux[(1 << unit_element) - 1];
                for (int i = 1; i < (1 << unit_element); i++) {
                    mux[i - 1] = Bit(true, PUBLIC);
                    for (int j = 0; j < unit_element; j++) {
                        mux[i - 1] = mux[i - 1] & (pos.bits[j] ^ !(Bit((i >> j) & 1, PUBLIC)));
                    }
                }
                for (int i = 0; i < 1 << unit_size; i++) {
                    for (int j = 1; j < (1 << unit_element); j++) {
                        out.bits[i] = out.bits[i].If(mux[j - 1], val.bits[i | (j << unit_size)]);
                    }
                } });
        }

        void write_from_f2k(const Integer &val, const Integer &pos, Integer &out)
        {
            // val: 2^unit_size bits, pos: unit_element bits, out: 128 bits
            out = Integer(128, 0, PUBLIC);
            for (int i = 0; i < (1 << unit_size); i++)
            {
                out.bits[i] = val.bits[i];
            }
            base->execute([&]()
                          {
                for (int cnt = 0; cnt < unit_element; cnt++) {
                    int width = 1 << (cnt + unit_size);
                    for (int i = 0; i < width; i++) {
                        out.bits[i | width] = out.bits[i | width].If(pos.bits[cnt], out.bits[i]);
                        out.bits[i] ^= out.bits[i | width];
                    }
                } });
        }

        void naive_write(const Integer &idx, const Integer &value)
        {
            write(idx, [&](const Integer &in, Integer &out)
                  { out = value; });
        }
    };

    template <typename T>
    class RFloRAM
    {
    public:
        using Bit = Bit_T<GbWire>;
        using Integer = Integer_T<GbWire>;

#ifdef BREAKDOWN
        double ind_read[4] = {0, 0, 0, 0};
        double access[8] = {0, 0, 0, 0, 0, 0, 0, 0};
#endif
        int oram_size, unit_size, unit_element, size;
        Integer ka, kb;
        GbStash *stash;
        block *ORAM_R, *ORAM_W;
        FloramMPC<T> *base;
        bool read_only = false;
        double range_t1 = 0, range_t2 = 0, range_t3 = 0, range_t4 = 0;
        double single_t1 = 0, single_t2 = 0, single_t3 = 0, single_t4 = 0;

        // initialize ORAM part
        RFloRAM(FloramMPC<T> *base, int size = 24, int unit_size = 5, int stash_size = -1, Integer *in = nullptr,
                int st = -1, int ed = -1, bool read_only = false)
        {
            if (size > base->max_size)
                throw std::runtime_error("ORAM size bigger than base max size");
            // Each block contains 16 bytes -> 2^{7-unit_size} elements
            this->read_only = read_only;
            this->size = size;
            this->base = base;
            this->unit_size = unit_size;
            this->unit_element = 7 - unit_size;
            this->oram_size = std::max(1, size - unit_element);
            // Initialize ORAM
            ORAM_R = new block[1 << (oram_size + 1)];
            block *ORAM_W = new block[1 << (oram_size + 1)];
            memset(ORAM_W, 0, sizeof(block) * (1 << (oram_size + 1)));
            // Put the input array into ORAM
            if (in != nullptr)
            {
                if (st == -1 && ed == -1)
                {
                    st = 0;
                    ed = (1 << size);
                }
                Integer *compact = new Integer[1 << oram_size];
                for (int i = (st >> unit_element); i <= ((ed - 1) >> unit_element); i++)
                {
                    compact[i] = Integer(128, 0, PUBLIC);
                }
                for (int i = st; i < ed; i++)
                {
                    for (int j = 0; j < (1 << unit_size); j++)
                    {
                        compact[i >> unit_element].bits[(i % (1 << unit_element)) * (1 << unit_size) +
                                                        j] = in[i].bits[j];
                    }
                }
                for (int i = (st >> unit_element); i <= ((ed - 1) >> unit_element); i++)
                {
                    base->pack_value(compact[i], ORAM_W[i]);
                }
                delete[] compact;
            }
            // Initial refresh
            this->_refresh(this->ka, this->kb);
            delete[] ORAM_W;
        }

        ~RFloRAM()
        {
            delete[] this->ORAM_R;
            delete[] this->ORAM_W;
            delete this->stash;
        }

        void read(const Integer &long_idx, Integer &out, bool outside = true, bool *t_bit = nullptr)
        {
            if (long_idx.size() > size)
                throw std::runtime_error("index out of range");
            // idx: 0-1 4bytes read, 2+ 16bytes read
            Integer idx = long_idx;
            if (outside)
            {
                idx = Integer(oram_size, 0, PUBLIC);
                for (int i = 0; i < oram_size; i++)
                {
                    idx.bits[i] = long_idx.bits[i + unit_element];
                }
            }

            // Part A: compute DPF
            auto t1 = clock_start();
            block *t, v_shr = zero_block;
            static const int log_size = 7;
#ifdef BREAKDOWN
            block dummy = zero_block;
            auto start = clock_start();
#endif
            if (outside)
            {
                Integer short_idx = Integer(std::max(0, oram_size - log_size), 0, PUBLIC);
                for (int i = 0; i < oram_size - log_size; i++)
                {
                    short_idx.bits[i] = idx.bits[i + log_size];
                }
                // calculate 2^(idx % 128)
                Integer mux = Integer(128, 1, PUBLIC);
                base->execute([&](Integer &out)
                              {
                    for (int cnt = 0; cnt < std::min(log_size, oram_size); cnt++) {
                        int width = 1 << cnt;
                        for (int i = 0; i < width; i++) {
                            out.bits[i | width] = out.bits[i | width].If(idx.bits[cnt], out.bits[i]);
                            out.bits[i] ^= out.bits[i | width];
                        }
                    } }, mux);
                base->DPF(short_idx, mux, std::max(0, oram_size - log_size), &t);
#ifdef BREAKDOWN
                base->commit_then_open(dummy);
                ind_read[0] += time_from(start);
                start = clock_start();
#endif
            }
            single_t1 += time_from(t1);
            // Part B: OROM access
            t1 = clock_start();
            block v_shr_par[base->threads];
            memset(v_shr_par, 0, sizeof(v_shr_par));
            // Using DPF (t value) to retrieve [(R[i]||R[i]*delta) ^ f(k_0,i) ^ f(k_1,i)]
            if (outside)
            {
                base->execute_parallel([&](int seq, size_t start, size_t end)
                                       {
                    for (size_t i = start; i < end; i++) {
                        bool bit = block_at_bit(t[i >> (log_size)], i % (1 << log_size));
                        block value[2] = {zero_block, ORAM_R[i]};
                        v_shr_par[seq] ^= value[bit];
                    } }, 0, (1 << oram_size));
            }
            else
            {
                base->execute_parallel([&](int seq, size_t start, size_t end)
                                       {
                    for (size_t i = start; i < end; i++) {
                        block value[2] = {zero_block, ORAM_R[i]};
                        v_shr_par[seq] ^= value[(t_bit)[i]];
                    } }, 0, (1 << oram_size));
            }
            for (int i = 0; i < base->threads; i++)
            {
                v_shr ^= v_shr_par[i];
            }
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside)
            {
                ind_read[1] += time_from(start);
            }
            else
            {
                access[1] += time_from(start);
            }
            start = clock_start();
#endif
            single_t2 += time_from(t1);
            t1 = clock_start();
            // Part C: AES-MPC
            block AES_result;
            base->LowMC_128(idx, ka, AES_result);
            v_shr ^= AES_result;
            base->LowMC_128(idx, kb, AES_result);
            v_shr ^= AES_result;
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside)
            {
                ind_read[2] += time_from(start);
            }
            else
            {
                access[2] += time_from(start);
            }
            start = clock_start();
#endif
            // Part D: Stash access
            Integer val;
            base->unpack_value(v_shr, val);
            single_t3 += time_from(t1);
            t1 = clock_start();
            if (outside)
            {
                read_into_f2k(val, long_idx, out);
            }
            else
            {
                out = val;
            }
            single_t4 += time_from(t1);
#ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside)
            {
                ind_read[3] += time_from(start);
            }
            else
            {
                access[3] += time_from(start);
            }
#endif
        }

        void range_read(const Integer &long_idx, vector<Integer> &out, int range)
        {
            if (range <= 0)
                throw std::runtime_error("range must be positive");
            if (long_idx.size() > size)
                throw std::runtime_error("index out of range");

            // Phase 0: split block/element indices
            Integer idx = long_idx; // block index inside ORAM

            idx = Integer(oram_size, 0, PUBLIC);
            for (int i = 0; i < oram_size; ++i)
                idx.bits[i] = long_idx.bits[i + unit_element];

            // Phase 1: evaluate one DPF
            auto t1 = clock_start();
            static const int LOG = 7; // 2^7 bits per block in t[]
            block *t;                 // one-hot share for idx
            {
                Integer short_idx = Integer(std::max(0, oram_size - LOG), 0, PUBLIC);
                for (int i = 0; i < oram_size - LOG; ++i)
                    short_idx.bits[i] = idx.bits[i + LOG];

                // mux = 2^{idx mod 128}, as in the scalar read
                Integer mux = Integer(128, 1, PUBLIC);
                base->execute([&](Integer &m)
                              {
        for (int b = 0; b < std::min(LOG, oram_size); ++b) {
            int w = 1 << b;
            for (int j = 0; j < w; ++j) {
                m.bits[j | w] = m.bits[j | w].If(idx.bits[b], m.bits[j]);
                m.bits[j]    ^= m.bits[j | w];
            }
        } }, mux);

                base->DPF(short_idx, mux,
                          std::max(0, oram_size - LOG), /*depth*/
                          &t);
            }
            range_t1 += time_from(t1);
            t1 = clock_start();
            // Phase 2: single ORAM scan

            std::vector<std::vector<block>> v_par(base->threads,
                                                  std::vector<block>(range, zero_block));
            base->execute_parallel([&](int seq, size_t begin, size_t end)
                                   {
                                       std::vector<block> local(range, zero_block);
                                       for (size_t i = begin; i < end; ++i)
                                       {
                                           block val = ORAM_R[i]; // read once
                                           for (int off = 0; off < range; ++off)
                                           {
                                               // i-th block in ORAM_R corresponds to off-th element in the range if t[i-off] = 1
                                               if (i < static_cast<size_t>(off))
                                                   continue;

                                               size_t src = i - off; // position in t
                                               bool bit = block_at_bit(t[src >> LOG],
                                                                       src & ((1 << LOG) - 1));
                                               // Use array indexing like regular read to avoid branch prediction issues
                                               block value[2] = {zero_block, val};
                                               local[off] ^= value[bit];
                                           }
                                       }
                                       v_par[seq].swap(local); // move into thread slot
                                   },
                                   0, 1u << oram_size);

            /* XOR the per-thread accumulators */
            std::vector<block> v_acc(range, zero_block);
            for (int th = 0; th < base->threads; ++th)
            {
                for (int off = 0; off < range; ++off)
                {
                    v_acc[off] ^= v_par[th][off];
                }
            }
            range_t2 += time_from(t1);
            t1 = clock_start();
            // Phase 3: unmask each accumulator and decode
            for (int off = 0; off < range; ++off)
            {

                /* idx_off = idx + off   (still secret) */
                Integer delta = Integer(idx.size(), off, PUBLIC);
                Integer idx_of;
                base->execute([&]()
                              { idx_of = idx + delta; });

                block tmp;
                base->LowMC_128(idx_of, ka, tmp);
                v_acc[off] ^= tmp;
                base->LowMC_128(idx_of, kb, tmp);
                v_acc[off] ^= tmp;

                /* unpack the 128-bit block, then select the wanted word bits */
                Integer val128;
                base->unpack_value(v_acc[off], val128);

                /* long_idx_off = long_idx + off  (unit-element bits needed only) */
                Integer pos = long_idx;
                if (off)
                {
                    Integer add = Integer(pos.size(), off, PUBLIC);
                    base->execute([&]()
                                  { pos = pos + add; });
                }

                read_into_f2k(val128, pos, out[off]); // final logical word
            }

            range_t3 += time_from(t1);
        }

        void read(const int long_idx, Integer &out, bool outside = true)
        {
            int32_t idx = long_idx >> unit_element;
            block v_shr = ORAM_W[idx];
            Integer val;
            base->unpack_value(v_shr, val);
            if (!outside)
            {
                out = val;
                return;
            }
            out = Integer(1 << unit_size, 0, PUBLIC);
            for (int i = 0; i < (1 << unit_size); i++)
            {
                out.bits[i] = val.bits[((long_idx % (1 << unit_element)) << unit_size) + i];
            }
        }

        void read_batch(Integer *out, const int long_idx_start = -1, const int long_idx_end = -1)
        {
            int32_t start_idx = 0;
            int32_t end_idx = 1 << oram_size;

            if (long_idx_start != -1 && long_idx_end != -1)
            {
                start_idx = long_idx_start >> unit_element;
                end_idx = long_idx_end >> unit_element;
            }
            else if (long_idx_start == -1 && long_idx_end == -1)
            {
                // read all
                start_idx = 0;
                end_idx = 1 << oram_size;
            }
            else if (long_idx_start < 0 || long_idx_end > (1 << size))
            {
                throw std::runtime_error("batch read index out of range");
            }

            int n_block = end_idx - start_idx;
            block *v_shrs = new block[n_block];
            // Copy ORAM_W blocks into v_shrs with proper offset
            for (int j = start_idx; j < end_idx; ++j)
            {
                v_shrs[j - start_idx] = ORAM_W[j];
            }
            Integer *val = new Integer[n_block];
            base->batch_unpack_value(v_shrs, val, n_block);

            // For each long index, use the correct block and bit offset.
            for (int i = long_idx_start; i < long_idx_end; i++)
            {
                out[i] = Integer(1 << unit_size, 0, PUBLIC);
                int block_index = (i >> unit_element) - start_idx;
                int bit_offset = (i % (1 << unit_element)) << unit_size;
                for (int j = 0; j < (1 << unit_size); j++)
                {
                    out[i].bits[j] = val[block_index].bits[bit_offset + j];
                }
            }
            delete[] v_shrs;
            delete[] val;
        }

        void _refresh(Integer &ka, Integer &kb)
        {
            block key;
            PRG().random_block(&key, 1);
            // expand aes key
            AES_KEY aes_key[2];
            AES_set_encrypt_key((const block)key, aes_key);
            // authenticate key
            ka = Integer(&key, ALICE);
            kb = Integer(&key, BOB);

            base->execute_parallel([&](int seq, size_t start, size_t end)
                                   {
                for (size_t i = start; i < end; i++) {
                    ORAM_R[i << 2] = makeBlock(0, i << 2);
                    ORAM_R[(i << 2) | 1] = makeBlock(0, (i << 2) | 1);
                    ORAM_R[(i << 2) | 2] = makeBlock(0, (i << 2) | 2);
                    ORAM_R[(i << 2) | 3] = makeBlock(0, (i << 2) | 3);
                    ParaEnc<1, 4>(&ORAM_R[i << 2], aes_key);
                    ORAM_R[i << 2] ^= ORAM_W[i << 2];
                    ORAM_R[(i << 2) | 1] ^= ORAM_W[(i << 2) | 1];
                    ORAM_R[(i << 2) | 2] ^= ORAM_W[(i << 2) | 2];
                    ORAM_R[(i << 2) | 3] ^= ORAM_W[(i << 2) | 3];
                } }, 0, 1 << (oram_size - 2));

            size_t left = 0;
            base->all_flush();
            do
            {
                size_t right = min((size_t)(1 << (oram_size)), left + (1 << 16));
                base->execute_parallel([&](int seq, size_t start, size_t end)
                                       { base->direct_open(&ORAM_R[start], end - start, seq); }, left, right);
                left = right;
            } while (left < 1u << (oram_size));
            base->all_flush();
        }

        void read_into_f2k(Integer val, Integer pos, Integer &out)
        {
            // val: 128 bits, pos: unit_element bits, out: 2^unit_size bits
            out = Integer(1 << unit_size, 0, PUBLIC);
            for (int i = 0; i < (1 << unit_size); i++)
            {
                out.bits[i] = val.bits[i];
            }
            base->execute([&]()
                          {
                Bit mux[(1 << unit_element) - 1];
                for (int i = 1; i < (1 << unit_element); i++) {
                    mux[i - 1] = Bit(true, PUBLIC);
                    for (int j = 0; j < unit_element; j++) {
                        mux[i - 1] = mux[i - 1] & (pos.bits[j] ^ !(Bit((i >> j) & 1, PUBLIC)));
                    }
                }
                for (int i = 0; i < 1 << unit_size; i++) {
                    for (int j = 1; j < (1 << unit_element); j++) {
                        out.bits[i] = out.bits[i].If(mux[j - 1], val.bits[i | (j << unit_size)]);
                    }
                } });
        }
    };

}

#endif
