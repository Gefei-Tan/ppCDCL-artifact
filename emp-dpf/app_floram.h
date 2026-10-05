#ifndef EMP_DPF_APP_FLORAM_H__
#define EMP_DPF_APP_FLORAM_H__
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
    template <typename T>
    class AppFloRAM
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
        int append_idx = 0;
        // initialize ORAM part
        AppFloRAM(FloramMPC<T> *base, int size = 24, int unit_size = 5, int stash_size = -1, Integer *in = nullptr,
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
            this->oram_size = std::max(1, size - unit_element);
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

        ~AppFloRAM()
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
            Integer idx(oram_size, 0, PUBLIC);
            for (int i = 0; i < oram_size; i++)
            {
                idx.bits[i] = long_idx.bits[i + unit_element];
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
            read_into_f2k(origin, long_idx, origin_32);
            if (in != nullptr)
            {
                *in = Integer(origin_32);
            }
            base->execute(func, origin_32, value_32);
            base->execute([&]()
                          { value_32 ^= origin_32; });
            write_from_f2k(value_32, long_idx, value);
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

} // namespace emp
#endif // EMP_DPF_APP_FLORAM_H__
