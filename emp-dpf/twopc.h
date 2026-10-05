#ifndef EMP_DPF_TWO_PC_H_
#define EMP_DPF_TWO_PC_H_
#include "emp-dpf/ccrh.h"
#include "emp-dpf/randomio.h"
#include "emp-dpf/utils.h"
#include "emp-tool/emp-tool.h"
#include "emp-ot/emp-ot.h"

namespace emp {

    class AuthBackend : public Backend { public:
        int role;
        AuthBackend(int party, int role = 0) : Backend(party), role(role) { }
    };

    class AuthWire { public:
        block LA0, LB;
        AuthWire(const block & LA0 = zero_block, const block & LB = zero_block) : LA0(LA0), LB(LB) { }
        AuthWire& operator=(const AuthWire& other) {
            if (((AuthBackend*)(emp::backend))->role == 0) {
                this->LA0 = other.LA0;
                this->LB = other.LB;
            } else if (((AuthBackend*)(emp::backend))->role == 1) {
                this->LA0 = other.LA0;
            } else {
                this->LB = other.LB;
            }
            return *this;
        }
    };
    
    template <typename T>
    class AuthHalfGen : public AuthBackend { public:
        T * io;
        COT<T> * ot;
        MITCCRH<8> mitccrh;

        block constant[2];
        block delta;
        uint64_t ands = 0;

        AuthHalfGen(int party, T* io, COT<T>* ot, block delta): AuthBackend(party, 1), io(io), ot(ot), delta(delta) { 
            PRG().random_block(constant, 2);
            this->io->send_block(constant, 2);
            constant[1] = constant[1] ^ delta;
            mitccrh.setS(constant[0]);
        }

        inline void HalfGateGen(const block& LA0, const block& LB0, block *LW0) {
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
            mitccrh.hash<2,2>(H);
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

        void xor_gate(void * out, const void * left, const void * right) override {
            ((AuthWire *)out)->LA0 = ((AuthWire *)left)->LA0 ^ ((AuthWire *)right)->LA0;
        }

        void not_gate(void * out, const void * in) override {
            ((AuthWire *)out)->LA0 = ((AuthWire *)in)->LA0 ^ constant[1];
        }

        void and_gate(void * out, const void * left, const void * right) override {
            ands++;
            HalfGateGen(((AuthWire *)left)->LA0, ((AuthWire *)right)->LA0, &(((AuthWire *)out)->LA0));
        }

        void feed(void * lbls, int party, const bool* b, size_t nel) override {
            if (party == PUBLIC) {
                for (size_t i = 0; i < nel; ++i) {
                    ((AuthWire *)lbls)[i].LA0 = constant[b[i]];
                }
            } else {
                error("do not feed during execution");
            }
        }

        void reveal(bool*out, int party, const void * lbls, size_t nel) override {
            error("do not reveal during execution");
        }

        uint64_t num_and() override {
            return ands;
        }

    }; 
    
    template <typename T>
    class AuthHalfEva : public AuthBackend { public:
        T * io;
        COT<T> * ot;
        MITCCRH<8> mitccrh;

        block constant[2];
        uint64_t ands = 0;

        AuthHalfEva(int party, T* io, COT<T> *ot): AuthBackend(party, 2), io(io), ot(ot) { 
            this->io->recv_block(constant, 2);
            mitccrh.setS(constant[0]);
        }

        inline void HalfGateEva(const block& A, const block& B, block *W) {
            block table[2];
            io->recv_block(table, 2);
            block HA, HB;
            int sa, sb;
            sa = getLSB(A);
            sb = getLSB(B);
            block H[2];
            H[0] = A;
            H[1] = B;
            mitccrh.hash<2,1>(H);
            HA = H[0];
            HB = H[1];
            *W = HA ^ HB;
            *W = *W ^ (select_mask[sa] & table[0]);
            *W = *W ^ (select_mask[sb] & table[1]);
            *W = *W ^ (select_mask[sb] & A);
        }

        void xor_gate(void * out, const void * left, const void * right) override {
            ((AuthWire *)out)->LB = ((AuthWire *)left)->LB ^ ((AuthWire *)right)->LB;
        }

        void not_gate(void * out, const void * in) override {
            ((AuthWire *)out)->LB = ((AuthWire *)in)->LB ^ constant[1];
        }

        void and_gate(void * out, const void * left, const void * right) override {
            ands++;
            HalfGateEva(((AuthWire *)left)->LB, ((AuthWire *)right)->LB, &(((AuthWire *)out)->LB)); 
        }

        void feed(void * lbls, int party, const bool* b, size_t nel) override {
            if (party == PUBLIC) {
                for (size_t i = 0; i < nel; ++i) {
                    ((AuthWire *)lbls)[i].LB = constant[b[i]];
                }
            } else {
                error("do not feed during execution");
            }
        }

        void reveal(bool*out, int party, const void * lbls, size_t nel) override {
            error("do not reveal during execution");
        }

        uint64_t num_and() override {
            return ands;
        }

    }; 

    class Stash { public:

        using Bit = Bit_T<AuthWire>;
        using Integer = Integer_T<AuthWire>;

        struct StashElement { public:
            Bit valid; Integer index, data;
        };

        int stash_size, stash_counter = 0, depth;
        StashElement *stash;

        Stash(int stash_size = 1000, int depth = 16) : stash_size(stash_size), depth(depth) {
            this->stash = new StashElement[stash_size];
            this->stash_counter = 0;
            for (int i = 0; i < stash_size; i++) {
                this->stash[i].valid = Bit(false, PUBLIC);
                this->stash[i].index = Integer(depth, 0, PUBLIC);
                this->stash[i].data = Integer(128, 0, PUBLIC);
            }
        }
        
        void read(const Integer & index, Bit & found, Integer & data, const bool write_back = false) {
            // write_back flag: if true, then the element will be deleted (invalid) after read
            for (int i = 0; i < stash_counter; i++) {
                Bit valid = this->stash[i].valid & (this->stash[i].index == index);
                found = found | valid;
                data = data.If(valid, this->stash[i].data);
                if (write_back) {
                    this->stash[i].valid = this->stash[i].valid ^ valid;
                }
            }
        }

        void write(const Integer & index, const Integer & data, const bool & valid = true) {
            this->stash[stash_counter].index = index;
            this->stash[stash_counter].valid = Bit(valid, PUBLIC);
            this->stash[stash_counter].data = data;
            stash_counter++;
        }

        void refresh() {
            this->stash_counter = 0;
            for (int i = 0; i < stash_size; i++) {
                this->stash[i].valid = Bit(false, PUBLIC);
                this->stash[i].index = Integer(depth, 0, PUBLIC);
            }
        }

        ~Stash() {
            delete[] this->stash;
        }
    };

    template<typename T> 
    class MPC : public AuthBackend { public:

        using Bit = Bit_T<AuthWire>;
        using Integer = Integer_T<AuthWire>;

        MPC(int party, int threads, T ** ios, block delta, int max_size=24): 
            AuthBackend(party), ios(ios), threads(threads), delta(delta) {
            // Initialize Ferret OT & DualExecution GC
            this->pool = new ThreadPool(threads-1);
            this->ot_send = new FerretCOT<T>(ALICE, 1, ios, true, false, ferret_b11);
            this->ot_recv = new FerretCOT<T>(BOB, 1, ios, true, false, ferret_b11);
            if (this->party == ALICE) {
                this->ot_send->setup(delta, "./data/pre_ot");
                this->ot_recv->setup("./data/pre_ot");
                this->gate_gen = new AuthHalfGen<T>(party, ios[0], ot_send, delta);
                this->gate_eva = new AuthHalfEva<T>(party, ios[0], ot_recv);
            } else {
                this->ot_recv->setup("./data/pre_ot");
                this->ot_send->setup(delta, "./data/pre_ot");
                this->gate_eva = new AuthHalfEva<T>(party, ios[0], ot_recv);
                this->gate_gen = new AuthHalfGen<T>(party, ios[0], ot_send, delta);
            }
            
            // Initialize DPF
            prp = new TwoKeyPRP*[threads];
            ccrh = new DualCCRH[threads];
            for (int i = 0; i < threads; i++) {
                ccrh[i] = DualCCRH(zero_block);
                prp[i] = new TwoKeyPRP(zero_block, makeBlock(0,1));
            }
            tree = new block[1<<(max_size+1)];
            next = new block[1<<(max_size+1)];
            tree_bit.resize(1<<max_size); 
            DPF_counter = 0;
            this->cf = new BristolFashion("./emp-dpf/files/aes_128.txt");

            // Malicious check
            digest_M = digest_x = zero_block;
            pack = new GaloisFieldPacking();

            block tmp[2];
            ios[0]->hash.digest(tmp);
        }

        void xor_gate(void * out, const void * left, const void * right) override {
            throw std::runtime_error("gates execution should be done by exec function");
        }

        void not_gate(void * out, const void * in) override {
            throw std::runtime_error("gates execution should be done by exec function");
        }

        void and_gate(void * out, const void * left, const void * right) override {
            throw std::runtime_error("gates execution should be done by exec function");
        }

        void feed(void * lbls, int party, const bool* b, size_t nel) override {
            AuthWire *out = (AuthWire *)lbls;
            if (party == PUBLIC) {
                this->gate_gen->feed(lbls, party, b, nel);
                this->gate_eva->feed(lbls, party, b, nel);
            } else {
                block *data = new block[nel];
                if (party == this->party) {
                    ot_recv->recv_cot(data, b, nel);
                    for (size_t i = 0; i < nel; i++) {
                        out[i] = AuthWire(b[i] ? delta : zero_block, data[i]);
                    }
                } else {
                    ot_send->send_cot(data, nel);
                    for (size_t i = 0; i < nel; i++) {
                        out[i] = AuthWire(data[i], zero_block);
                    }
                }
                delete[] data;
            }
        }

        void reveal(bool* out, int party, const void * lbls, size_t nel) override {
            const AuthWire *in = (const AuthWire *)lbls;
            bool shr[nel], ret[nel];
            for (size_t i = 0; i < nel; ++i) {
                shr[i] = getSLSB(in[i].LA0 ^ in[i].LB);
            }
            if (this->party == ALICE) {
                ios[0]->send_bool(shr, nel);
                ios[0]->recv_bool(ret, nel);
            } else {
                ios[0]->recv_bool(ret, nel);
                ios[0]->send_bool(shr, nel);
            }
            block * check = new block[nel];
            for (size_t i = 0; i < nel; ++i) {
                out[i] = shr[i] ^ ret[i];
                check[i] = in[i].LA0 ^ in[i].LB ^ (out[i] ? this->delta : zero_block);
            }
            this->check_buffer_bool(check, nel);
            delete[] check;
        }

        uint64_t num_and() override {
            return min(this->gate_gen->num_and(), this->gate_eva->num_and());
        }
        
        template<typename Func, typename... Args> 
        void execute(Func&& func, Args&&... args) {
            if (this->party == ALICE) {
                emp::backend = this->gate_gen;
                func(std::forward<Args>(args)...);
                emp::backend = this->gate_eva;
                func(std::forward<Args>(args)...);
                emp::backend = this;
            }
            else {
                emp::backend = this->gate_eva;
                func(std::forward<Args>(args)...);
                emp::backend = this->gate_gen;
                func(std::forward<Args>(args)...);
            }
        }



        ~MPC() {
            delete this->ot_send; delete this->ot_recv;
            delete this->gate_gen; delete this->gate_eva;
            delete[] ccrh;
            for (int i = 0; i < this->threads; i++) delete prp[i];
            delete[] prp;
            delete pack; delete cf;
            delete[] this->tree; delete[] this->next;
            delete this->pool;
            std::remove("./data/pre_ot");
        }

        // Execution & OT
        T ** ios;
        int threads;
        ThreadPool *pool;
        block delta;
        AuthBackend * gate_gen, * gate_eva;
        FerretCOT<T> * ot_send, * ot_recv;
        // DPF
        TwoKeyPRP **prp;
        DualCCRH *ccrh;
        block *tree, *next;
        vector<bool> tree_bit;
        int DPF_counter = 0;
        // ORAM
        BristolFashion *cf;
        // Consistency check
        block digest_M, digest_x;
        GaloisFieldPacking *pack;
        
        inline void io_flush(int seq) {
            ios[seq]->flush();
        }

        inline void all_flush() {
            for (int i = 0; i < this->threads; i++) ios[i]->flush();
        }

        inline void direct_open(block *val, size_t nel = 1, int seq=-1) {
            // each party gets x <- x_0 ^ x_1
            block res[nel];
            if (seq == -1) {
                this->ios[(this->party-1)]->send_block(val,nel);
                io_flush((this->party-1));
                this->ios[(this->party-1)^1]->recv_block(res,nel); 
            } else {
                if (this->party == ALICE) {
                    this->ios[seq]->send_block(val, nel);
                    this->ios[seq]->recv_block(res, nel);
                } else {
                    this->ios[seq]->recv_block(res, nel);
                    this->ios[seq]->send_block(val, nel);
                } 
                io_flush(seq);
            }
            for (size_t i = 0; i < nel; i++) {
                val[i] = res[i] ^ val[i];
            }
        }

        inline void commit_then_open(block &val, int seq=-1) {
            // each party commits x_i then gets x <- x_0 ^ x_1
            block hash_value, commitment, res;
            hash_value = Hash().hash_for_block(&val, sizeof(val));
            if (seq == -1) {
                this->ios[(this->party-1)]->send_block(&hash_value, 1);
                io_flush((this->party-1));
                this->ios[(this->party-1)^1]->recv_block(&commitment, 1);
                this->ios[(this->party-1)]->send_block(&val, 1);
                io_flush((this->party-1));
                this->ios[(this->party-1)^1]->recv_block(&res, 1);
            } else {
                if (this->party == ALICE) {
                    this->ios[seq]->send_block(&hash_value, 1);
                    this->ios[seq]->recv_block(&commitment, 1);
                    this->ios[seq]->send_block(&val, 1);
                    this->ios[seq]->recv_block(&res, 1);
                } else {
                    this->ios[seq]->recv_block(&commitment, 1);
                    this->ios[seq]->send_block(&hash_value, 1);
                    this->ios[seq]->recv_block(&res, 1);
                    this->ios[seq]->send_block(&val, 1);
                } 
                io_flush(seq);
            }
            hash_value = Hash().hash_for_block(&res, sizeof(res));
            if (cmpBlock(&commitment, &hash_value, 1) == false) {
                error("commitment check failed");
            }
            val = res ^ val;
        }

        void SPDZ_to_BDOZ(block *in, Integer &out) {
            // using COT to generate one first, then zero check (put into check buffer)
            out = Integer(128, 0, PUBLIC);
            bool *b = new bool[128]; block *K = new block[128], *M = new block[128];
            for (int i = 0; i < 128; i++) {
                b[i] = block_at_bit(in[0], i);
            }
            if (this->party == ALICE) {
                this->ot_send->send_cot(K, 128);
                this->ot_recv->recv_cot(M, b, 128);
            } else {
                this->ot_recv->recv_cot(M, b, 128);
                this->ot_send->send_cot(K, 128);
            }
            for (int i = 0; i < 128; i++) {
                out.bits[i].bit.LA0 = b[i] ? (K[i] ^ this->delta) : K[i];
                out.bits[i].bit.LB = M[i];
            }
            block check[2]; BDOZ_to_SPDZ(out, check); 
            check[0] = zero_block; check[1] ^= in[1];
            check_buffer(check, 1);

            delete[] b; delete[] K; delete[] M;
        }

        void BDOZ_to_SPDZ(const Integer &in, block *out) {
            // Integer (128bits) -> block[2] [x||x*delta]
            block M[128];
            for (int i = 0; i < 128; i++) {
                M[i] = in.bits[i].bit.LA0 ^ in.bits[i].bit.LB;
            }
            out[0] = out[1] = zero_block;
            for (int i = 0 ; i < 64; i++) {
                out[0][0] ^= ((long long)(getSLSB(M[i]))) << i;
                out[0][1] ^= ((long long)(getSLSB(M[i+64]))) << i;
            }
            pack->packing(&out[1], M);
        }
        
        void BDOZ_to_SPDZ(Bit *in, block *out) {
            // Bit[128] -> block[2] [x||x*delta]
            block M[128];
            for (int i = 0; i < 128; i++) {
                M[i] = in[i].bit.LA0 ^ in[i].bit.LB;
            }
            out[0] = out[1] = zero_block;
            for (int i = 0 ; i < 64; i++) {
                out[0][0] ^= ((long long)(getSLSB(M[i]))) << i;
                out[0][1] ^= ((long long)(getSLSB(M[i+64]))) << i;
            }
            pack->packing(&out[1], M);
        }

        void BDOZ_to_SPDZ_with_reverse(Bit *in, block *out) {
            // Bit[128] -> block[2] [x||x*delta]
            block M[128];
            for (int i = 0; i < 128; i++) {
                M[endian_reverse(i)] = in[i].bit.LA0 ^ in[i].bit.LB;
            }
            out[0] = out[1] = zero_block;
            for (int i = 0 ; i < 64; i++) {
                out[0][0] ^= ((long long)(getSLSB(M[i]))) << i;
                out[0][1] ^= ((long long)(getSLSB(M[i+64]))) << i;
            }
            pack->packing(&out[1], M);
        }
        
        void DPF(const Integer &alpha, const Integer &beta, int depth, block **r, block **t=nullptr, vector<bool> **t_bit = nullptr) {
            // input: alpha(bit 0...depth-1), beta(128 bits),
            // &r, &t (address of a pointer, not necessary to initialize)
            // r[alpha<<1] = share of beta, r[alpha<<1|1] = share of beta*delta, t[alpha] = share of delta
            DPF_counter += 1; block W = makeBlock(0, DPF_counter);
            block *tree = this->tree, *next = this->next;
            tree[0] = this->delta ^ W;
            for (int i = 0; i < depth; i++) {
                block cw = this->delta ^ (alpha.bits[depth-i-1].bit.LA0 ^ alpha.bits[depth-i-1].bit.LB);
                if (i == 0) {
                    tree_bit[0] = getSLSB(tree[0]);
                    ccrh[0].ro_expand_1to2(next, tree[0]);
                    cw ^= next[0];
                } else if (i == 1) {
                    tree_bit[0] = getSLSB(tree[0]); tree_bit[1] = getSLSB(tree[1]);
                    ccrh[0].ro_expand_2to4(next, tree);
                    cw ^= next[0] ^ next[2];
                } else {
                    block cw_par[this->threads];
                    memset(cw_par, 0, sizeof(block) * this->threads);
                    this->execute_parallel([&](int seq, size_t start, size_t end) {
                        for (size_t j = start; j < end; j++) {
                            tree_bit[j<<2] = getSLSB(tree[j<<2]); tree_bit[j<<2|1] = getSLSB(tree[j<<2|1]);
                            tree_bit[j<<2|2] = getSLSB(tree[j<<2|2]); tree_bit[j<<2|3] = getSLSB(tree[j<<2|3]);
                            ccrh[seq].ro_expand_4to8(&next[j<<3], &tree[j<<2]);
                            cw_par[seq] ^= next[j<<3] ^ next[j<<3|2] ^ next[j<<3|4] ^ next[j<<3|6];
                        }
                    }, 0, (1<<(i-2)));
                    for (int j = 0; j < this->threads; j++) cw ^= cw_par[j];
                }
                direct_open(&cw);
                block cw_choice[2] = {zero_block, cw};
                this->execute_parallel([&](int seq, size_t start, size_t end) {
                    for (size_t j = start; j < end; j++) {
                        next[j<<1] ^= cw_choice[tree_bit[j]]; 
                        next[j<<1|1] ^= cw_choice[tree_bit[j]];
                    }
                }, 0, 1<<i);
                std::swap(tree, next);
            }
            block sum[2]; BDOZ_to_SPDZ(beta, sum);
            block sum_par[this->threads][2];
            memset(sum_par, 0, sizeof(block) * this->threads<<1);
            if (depth == 0) {
                tree_bit[0] = getSLSB(tree[0]);
                prp[0]->node_expand_1to2(next, tree[0]);
                sum[0] ^= next[0]; sum[1] ^= next[1];
            } else {
                this->execute_parallel([&](int seq, size_t start, size_t end) {
                    for (size_t j = start; j < end; j++) {
                        tree_bit[j<<1] = getSLSB(tree[j<<1]); tree_bit[j<<1|1] = getSLSB(tree[j<<1|1]);
                        prp[seq]->node_expand_2to4(&next[j<<2], &tree[j<<1]);
                        sum_par[seq][0] ^= next[j<<2] ^ next[j<<2|2];
                        sum_par[seq][1] ^= next[j<<2|1] ^ next[j<<2|3];
                    }
                }, 0, 1<<(depth-1));
                for (int i = 0; i < this->threads; i++) {
                    sum[0] ^= sum_par[i][0]; sum[1] ^= sum_par[i][1];
                }
            }
            direct_open(sum, 2);
            block sum_choice[4] = {zero_block, sum[0], zero_block, sum[1]};
            this->execute_parallel([&](int seq, size_t start, size_t end) {
                for (size_t j = start; j < end; j++) {
                    next[j<<1] ^= sum_choice[tree_bit[j]];
                    next[j<<1|1] ^= sum_choice[tree_bit[j]|2];
                } 
            }, 0, 1<<depth);
            *r = next;
            if (t != nullptr) *t = tree;
            if (t_bit != nullptr) *t_bit = &tree_bit;
        }

        void check_buffer(const block * x, const block * M, size_t nel = 1) {
            block chi, cur;
            ios[0]->random_block(chi); cur = makeBlock(0, 1);
            block rnd_M = zero_block, rnd_x = zero_block, tmp_M, tmp_x;
            for (size_t i = 0; i < nel; ++i) {
                gfmul(cur, chi, &cur);
                gfmul(cur, x[i], &tmp_x);
                gfmul(cur, M[i], &tmp_M);
                rnd_x = rnd_x ^ tmp_x;
                rnd_M = rnd_M ^ tmp_M;
            }
            digest_M = digest_M ^ rnd_M;
            digest_x = digest_x ^ rnd_x;
        }

        void check_buffer(const block * M, size_t nel = 1) {
            // for block[] storing x||M[x]
            block chi, cur;
            ios[0]->random_block(chi); cur = makeBlock(0, 1);
            block rnd_M = zero_block, rnd_x = zero_block, tmp_M, tmp_x;
            for (size_t i = 0; i < nel; ++i) {
                gfmul(cur, chi, &cur);
                gfmul(cur, M[i<<1], &tmp_x);
                gfmul(cur, M[i<<1|1], &tmp_M);
                rnd_x = rnd_x ^ tmp_x;
                rnd_M = rnd_M ^ tmp_M;
            }
            digest_M = digest_M ^ rnd_M;
            digest_x = digest_x ^ rnd_x;
        }

        void check_buffer_bool(const block * M, size_t nel = 1) {
            // for boolean x (SLSB of M[x])
            block chi, cur[2] = {zero_block, zero_block};
            ios[0]->random_block(chi); cur[1] = makeBlock(0, 1);
            block rnd_M = zero_block, rnd_x = zero_block, tmp_M, tmp_x;
            for (size_t i = 0; i < nel; ++i) {
                gfmul(cur[1], chi, &cur[1]);
                tmp_x = cur[getSLSB(M[i])];
                gfmul(cur[1], M[i], &tmp_M);
                rnd_x = rnd_x ^ tmp_x;
                rnd_M = rnd_M ^ tmp_M;
            }
            digest_M = digest_M ^ rnd_M;
            digest_x = digest_x ^ rnd_x;
        }

        bool check_digest() {
            block *v = new block[128], *w = new block[128];
            block random_x, random_M;
            bool *u = new bool[128];
            if (this->party == ALICE) {
                this->ot_send->rcot(v, 128);
                this->ot_recv->rcot(w, 128);
            } else {
                this->ot_recv->rcot(w, 128);
                this->ot_send->rcot(v, 128);
            }
            for (int i = 0; i < 128; i++) {
                u[i] = getLSB(w[i]);
                v[i] = v[i] ^ w[i] ^ (u[i] ? this->delta : zero_block);
            } 
            random_x = bool_to_block(u);
            pack->packing(&random_M, v);
            delete [] v; delete [] w; delete [] u;

            block mask_x = random_x ^ digest_x;
            direct_open(&mask_x);
            block mask_M; gfmul(mask_x, this->delta, &mask_M);
            mask_M = mask_M ^ digest_M ^ random_M;
            commit_then_open(mask_M);
            if (cmpBlock(&mask_M, &zero_block, 1) == false) {
                error("consistency check failed");
                return false;
            }
            digest_M = digest_x = zero_block;
            return true;
        }

        void random_sample(Integer &out, int nel = 128) {
            // Sample a random value and send the authenticated shares to each party
            // Samples nel random boolean values
            // Can be considered as both Alice and Bob sample a uniform value, then xor them
            // out -> nel bits authenticated boolean array
            out = Integer(nel, 0, PUBLIC);
            block *K = new block[nel], *M = new block[nel];
            if (this->party == ALICE) {
                this->ot_send->rcot(K, nel);
                this->ot_recv->rcot(M, nel);
            } else {
                this->ot_recv->rcot(M, nel);
                this->ot_send->rcot(K, nel);
            }
            for (int i = 0; i < nel; i++) {
                out.bits[i].bit.LA0 = (getLSB(M[i])) ? (K[i] ^ this->delta) : K[i];
                out.bits[i].bit.LB = M[i];
            }
            delete[] K; delete[] M;
        }

        // AES and put MAC into checking buffer
        void AES(const Integer &msg, const Integer &key, block *out) {
            // AES128: output f(k,msg) & f(k^1,msg)
            // extend msg (from any bits to 128)
            Bit key_msg[256], res_0[128], res_1[128];
            for (int i = 0; i < 128; i++) {
                key_msg[endian_reverse(i)] = key.bits[i];
            }
            // padding msg to 128 bits
            for (size_t i = 0; i < msg.size(); i++) {
                key_msg[endian_reverse(i)+128] = msg.bits[i];
            }
            for (size_t i = msg.size(); i < 128; i++) {
                key_msg[endian_reverse(i)+128] = Bit(false, PUBLIC);
            }
            this->execute(
                [this, &key_msg, &res_0, &res_1]() {
                    this->cf->compute(res_0, key_msg);
                    // endian_reverse(0) -> 120
                    key_msg[120] ^= Bit(true, PUBLIC);
                    this->cf->compute(res_1, key_msg);
                }
            );
            // res_0 = f(k,msg), res_1 = f(k^1,msg)
            block res[4]; 
            BDOZ_to_SPDZ_with_reverse(res_0, res);
            BDOZ_to_SPDZ_with_reverse(res_1, &res[2]);
            check_buffer(res, 2);
            out[0] = res[0]; out[1] = res[2];
        }

        template<typename Func> // parameter: int seq, size_t start, size_t end
        inline void execute_parallel(Func&& func, size_t left, size_t right) {
            if (threads == 1) { func(0, left, right); return; }
            std::vector<std::future<void>> fut;
            size_t width = (right - left) / (this->threads);
            size_t start = left + width, end = min(right, start + width);
            if (width > 0) {
                for (int i = 0; i < threads-2; i++) {
                    fut.push_back(this->pool->enqueue(func, i, start, end));
                    start = end; end += width;
                } 
            }
            end = right;
            fut.push_back(this->pool->enqueue(func, threads-2, start, end));
            func(threads-1, left, left + width);
            for (auto &f: fut) f.get();
        }

    };

    template<typename T>
    class RAM { public:

        using Bit = Bit_T<AuthWire>;
        using Integer = Integer_T<AuthWire>;
        
        #ifdef BREAKDOWN
        double ind_read[4] = {0, 0, 0, 0};
        double access[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        #endif
        int oram_size, unit_size, unit_element;
        Integer ka, kb;
        Stash *stash;
        block *ORAM_R, *ORAM_W;
        MPC<T> *base;

        // initialize ORAM part
        RAM(MPC<T> *base, int size = 24, int unit_size = 5, int stash_size = -1, Integer *in = nullptr, int st=-1, int ed=-1) {
            // Each block contains 16 bytes -> 2^{7-unit_size} elements
            this->base = base;
            this->unit_size = unit_size;
            this->unit_element = 7-unit_size;
            this->oram_size = std::max(1, size-unit_element);
            // Initialize stash
            if (stash_size == -1) stash_size = std::max(1, int(sqrt(1<<(size))/(20*sqrt(base->threads))));
            this->stash = new Stash(stash_size, oram_size);
            // Initialize ORAM
            ORAM_R = new block[1<<(oram_size+1)];
            ORAM_W = new block[1<<(oram_size+1)];
            memset(ORAM_W, 0, sizeof(block) * (1<<(oram_size+1)));
            // Put the input array into ORAM
            if (in != nullptr) {
                if (st == -1 && ed == -1) {
                    st = 0; ed = (1<<size);
                }
                Integer *compact = new Integer[1<<oram_size];
                for (int i = (st>>unit_element); i <= ((ed-1)>>unit_element); i++) {
                    compact[i] = Integer(128, 0, PUBLIC);
                }
                for (int i = st; i < ed; i++) {
                    for (int j = 0; j < (1<<unit_size); j++) {
                        compact[i>>unit_element].bits[(i%(1<<unit_element))*(1<<unit_size)+j] = in[i].bits[j];
                    }
                }
                for (int i = (st>>unit_element); i <= ((ed-1)>>unit_element); i++) {
                    base->BDOZ_to_SPDZ(compact[i], &ORAM_W[i<<1]);
                }
                delete[] compact;
            }
            // Initial refresh
            this->refresh(this->ka, this->kb);
        }
        
        ~RAM() {
            delete[] this->ORAM_R;
            delete[] this->ORAM_W;
            delete this->stash;
        }
        
        void read(const Integer &long_idx, Integer &out, bool outside = true, vector<bool> *t_bit = nullptr) {
            // idx: 0-1 4bytes read, 2+ 16bytes read
            Integer idx = long_idx;
            if (outside) {
                idx = Integer(oram_size, 0, PUBLIC);
                for (int i = 0; i < oram_size; i++) {
                    idx.bits[i] = long_idx.bits[i+unit_element];
                }
            }

            // Part A: compute DPF
            block *t, v_shr[2] = {zero_block, zero_block};
            static const int log_size = 7;
            #ifdef BREAKDOWN
            block dummy = zero_block;
            auto start = clock_start();
            #endif 
            if (outside) {
                Integer short_idx = Integer(std::max(0,oram_size-log_size), 0, PUBLIC);
                for (int i = 0; i < oram_size-log_size; i++) {
                    short_idx.bits[i] = idx.bits[i+log_size];
                }
                // calculate 2^(idx % 128)
                Integer mux = Integer(128, 1, PUBLIC);
                base->execute([&](Integer &out) {
                    for (int cnt = 0; cnt < std::min(log_size, oram_size); cnt++) {
                        int width = 1<<cnt;
                        for (int i = 0; i < width; i++) {
                            out.bits[i|width] = out.bits[i|width].If(idx.bits[cnt], out.bits[i]);
                            out.bits[i] ^= out.bits[i|width];
                        } 
                    }
                }, mux);
                base->DPF(short_idx, mux, std::max(0,oram_size-log_size), &t);
                #ifdef BREAKDOWN
                base->commit_then_open(dummy);
                ind_read[0] += time_from(start);
                start = clock_start();
                #endif
            } 

            // Part B: OROM access
            block v_shr_par[base->threads][2]; 
            memset(v_shr_par, 0, sizeof(v_shr_par));
            // Using DPF (t value) to retrieve [(R[i]||R[i]*delta) ^ f(k_0,i) ^ f(k_1,i)]
            if (outside) {
                base->execute_parallel([&](int seq, size_t start, size_t end) {
                    for (size_t i = start; i < end; i++) {
                        bool bit = block_at_bit(t[i>>(log_size)<<1], i%(1<<log_size));
                        block value[4] = {zero_block, ORAM_R[i<<1], zero_block, ORAM_R[i<<1|1]};
                        v_shr_par[seq][0] ^= value[bit];
                        v_shr_par[seq][1] ^= value[bit|2];
                    }
                }, 0, (1<<oram_size));
            } else {
                base->execute_parallel([&](int seq, size_t start, size_t end) {
                    for (size_t i = start; i < end; i++) {
                        block value[4] = {zero_block, ORAM_R[i<<1], zero_block, ORAM_R[i<<1|1]};
                        v_shr_par[seq][0] ^= value[(*t_bit)[i]];
                        v_shr_par[seq][1] ^= value[(*t_bit)[i] | 2];
                    }
                }, 0, (1<<oram_size));
            }
            for (int i = 0; i < base->threads; i++) {
                v_shr[0] ^= v_shr_par[i][0]; v_shr[1] ^= v_shr_par[i][1];
            }
            #ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside) {
                ind_read[1] += time_from(start);
            } else {
                access[1] += time_from(start);
            }
            start = clock_start();
            #endif

            // Part C: AES-MPC
            block AES_result[2];
            base->AES(idx, ka, AES_result);
            v_shr[0] ^= AES_result[0]; v_shr[1] ^= AES_result[1];
            base->AES(idx, kb, AES_result);
            v_shr[0] ^= AES_result[0]; v_shr[1] ^= AES_result[1];
            #ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside) {
                ind_read[2] += time_from(start);
            } else {
                access[2] += time_from(start);
            }
            start = clock_start();
            #endif

            // Part D: Stash access
            Integer val; base->SPDZ_to_BDOZ(v_shr, val);
            base->execute(
                [this, idx, &val, outside]() {
                    Bit found(false, PUBLIC);
                    Integer v_in_stash(128, 0, PUBLIC);
                    this->stash->read(idx, found, v_in_stash, !outside);
                    val = val.If(found, v_in_stash);
                }
            );

            if (outside) { read_into_f2k(val, long_idx, out); } 
            else { out = val; }
            #ifdef BREAKDOWN
            base->commit_then_open(dummy);
            if (outside) {
                ind_read[3] += time_from(start);
            } else {
                access[3] += time_from(start);
            }
            #endif
        }

        void read(const int long_idx, Integer &out, bool outside = true) {
            int32_t idx = long_idx >> unit_element;
            block v_shr[2] = {ORAM_W[idx<<1], ORAM_W[idx<<1|1]};
            Integer val; base->SPDZ_to_BDOZ(v_shr, val);
            if (!outside) { out = val; return; }
            out = Integer(1<<unit_size, 0, PUBLIC);
            for (int i = 0; i < (1<<unit_size); i++) {
                out.bits[i] = val.bits[((long_idx%(1<<unit_element))<<unit_size)+i];
            }
        }

        template<typename Func>
        void write(const Integer &long_idx, Func &&func, Integer *in = nullptr) {
            Integer idx(oram_size, 0, PUBLIC); 
            for (int i = 0; i < oram_size; i++) {
                idx.bits[i] = long_idx.bits[i+unit_element];
            }

            #ifdef BREAKDOWN
            auto start = clock_start();
            block dummy = zero_block;
            #endif
            // Part A: Mal-DPF
            Integer beta; base->random_sample(beta, 128);
            block *r, *t; vector<bool> *tbit;
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
            read_into_f2k(origin, long_idx, origin_32);
            if (in != nullptr) {
                *in = Integer(origin_32);
            }
            base->execute(func, origin_32, value_32);
            base->execute([&]() {value_32 ^= origin_32;});
            write_from_f2k(value_32, long_idx, value);
            base->execute( [&]() { value ^= origin; });
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
            base->execute( [&]() { value ^= origin ^ beta; });
            block variation[2]; base->BDOZ_to_SPDZ(value, variation);
            base->check_buffer(&variation[0], &variation[1], 1);
            base->commit_then_open(variation[0]);
            variation[1] = zero_block;
            base->execute_parallel([&](int seq, size_t start, size_t end) {
                for (size_t i = start; i < end; i++) {
                    block eps; gfmul(t[i], variation[0], &eps);
                    ORAM_W[i<<1] ^= r[i<<1] ^ variation[!getSLSB(t[i])];
                    ORAM_W[i<<1|1] ^= r[i<<1|1] ^ eps;
                }
            }, 0, (1<<oram_size));
            #ifdef BREAKDOWN
            base->commit_then_open(dummy);
            access[6] += time_from(start);
            start = clock_start();
            #endif
            
            // Part F: refresh
            if (this->stash->stash_counter == this->stash->stash_size) {
                this->refresh(this->ka, this->kb);
            }
            #ifdef BREAKDOWN
            base->commit_then_open(dummy);
            access[7] += time_from(start);
            #endif
        }
        
        template<typename Func>
        void write(const int long_idx, Func &&func) {
            int32_t idx = long_idx >> unit_element;
            Integer origin; this->read(long_idx, origin, false);
            Integer origin_32, value_32, value; 
            origin_32 = Integer(1<<unit_size, 0, PUBLIC);
            for (int i = 0; i < (1<<unit_size); i++) {
                origin_32.bits[i] = origin.bits[((long_idx%(1<<unit_element))<<unit_size)+i];
            }
            base->execute(func, origin_32, value_32);
            base->execute([&]() {value_32 ^= origin_32;});
            value = Integer(128, 0, PUBLIC);
            for (int i = 0; i < (1<<unit_size); i++) {
                value.bits[((long_idx%(1<<unit_element))<<unit_size)+i] = value_32.bits[i];
            }
            base->execute( [&]() { value ^= origin; });
            this->stash->write(Integer(oram_size, idx, PUBLIC), value);
            base->BDOZ_to_SPDZ(value, &ORAM_W[idx<<1]);
            if (this->stash->stash_counter == this->stash->stash_size) {
                this->refresh(this->ka, this->kb);
            }
        }
        
        void refresh(Integer &ka, Integer &kb) {
            this->stash->refresh();
            block key; PRG().random_block(&key, 1);
            // expand aes key
            AES_KEY aes_key[2]; 
            AES_set_encrypt_key((const block)key, aes_key);
            AES_set_encrypt_key((const block)(key^makeBlock(0,1)), &aes_key[1]);
            // authenticate key
            ka = Integer(&key, ALICE);
            kb = Integer(&key, BOB);

            base->execute_parallel([&](int seq, size_t start, size_t end) {
                for (size_t i = start; i < end; i++) {
                    ORAM_R[i<<2] = ORAM_R[(i<<2)|1] = makeBlock(0,i<<1);
                    ORAM_R[(i<<2)|2] = ORAM_R[(i<<2)|3] = makeBlock(0,i<<1|1);
                    ParaEnc<2,1>(&ORAM_R[i<<2], aes_key);
                    ParaEnc<2,1>(&ORAM_R[(i<<2)|2], aes_key);
                    ORAM_R[i<<2] ^= ORAM_W[i<<2]; ORAM_R[(i<<2)|1] ^= ORAM_W[(i<<2)|1];
                    ORAM_R[(i<<2)|2] ^= ORAM_W[(i<<2)|2]; ORAM_R[(i<<2)|3] ^= ORAM_W[(i<<2)|3];
                }
            }, 0, 1<<(oram_size-1));

            size_t left = 0;
            base->all_flush();
            do {
                size_t right = min((size_t)(1<<(oram_size+1)), left + (1<<16));
                base->execute_parallel([&](int seq, size_t start, size_t end) {
                    base->direct_open(&ORAM_R[start], end-start, seq);
                }, left, right);
                left = right;
            } while(left < 1u<<(oram_size+1));
            base->all_flush();
        }
    
        void read_into_f2k(Integer val, Integer pos, Integer &out) {
            // val: 128 bits, pos: unit_element bits, out: 2^unit_size bits
            out = Integer(1<<unit_size, 0, PUBLIC);
            for (int i = 0; i < (1<<unit_size); i++) {
                out.bits[i] = val.bits[i];
            }
            base->execute([&]() {
                Bit mux[(1<<unit_element)-1];
                for (int i = 1; i < (1<<unit_element); i++) {
                    mux[i-1] = Bit(true, PUBLIC);
                    for (int j = 0; j < unit_element; j++) {
                        mux[i-1] = mux[i-1] & (pos.bits[j] ^ !(Bit((i>>j) & 1, PUBLIC)));
                    }
                }
                for (int i = 0; i < 1<<unit_size; i++) {
                    for (int j = 1; j < (1<<unit_element); j++) {
                        out.bits[i] = out.bits[i].If(mux[j-1], val.bits[i|(j<<unit_size)]);
                    }
                }
            });
        }

        void write_from_f2k(const Integer& val, const Integer& pos, Integer &out) {
            // val: 2^unit_size bits, pos: unit_element bits, out: 128 bits
            out = Integer(128, 0, PUBLIC);
            for (int i = 0; i < (1<<unit_size); i++) {
                out.bits[i] = val.bits[i];
            }
            base->execute([&]() {
                for (int cnt = 0; cnt < unit_element; cnt++) {
                    int width = 1<<(cnt+unit_size);
                    for (int i = 0; i < width; i++) {
                        out.bits[i|width] = out.bits[i|width].If(pos.bits[cnt], out.bits[i]);
                        out.bits[i] ^= out.bits[i|width];
                    } 
                }
            });
        }

    };

}

#endif