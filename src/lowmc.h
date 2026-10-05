#ifndef _EMP_LOWMC_H__
#define _EMP_LOWMC_H__

#include "emp-tool/emp-tool.h"
#include "emp-dpf/emp-dpf.h"

template <typename WireType>
class LowMC
{
public:
    using Bit = Bit_T<WireType>;
    LowMC(bool *key_in, Backend *base, int rounds = 49, int blocksize = 1024, int keysize = 128, int numofboxes = 20)

    {
        this->base = base;
        this->rounds = rounds;
        this->blocksize = blocksize;
        this->keysize = keysize;
        this->numofboxes = numofboxes;
        identitysize = blocksize - 3 * numofboxes;
        init(key_in);
        keyschedule();
    }
    LowMC()
    {
    }

    ~LowMC()
    {
        if (zero_bit_vec != nullptr)
            delete[] zero_bit_vec;
        if (LinMatrices_loc != nullptr)
            delete[] LinMatrices_loc;
        if (roundconstants_loc != nullptr)
            delete[] roundconstants_loc;
        if (key_loc != nullptr)
            delete[] key_loc;
        if (KeyMatrices_loc != nullptr)
            delete[] KeyMatrices_loc;
        if (roundkeys_loc != nullptr)
            delete[] roundkeys_loc;
        if (roundconstants != nullptr)
            delete[] roundconstants;
        if (key != nullptr)
            delete[] key;
        if (roundkeys != nullptr)
            delete[] roundkeys;
    }

    void encrypt(Bit *ctx, const Bit *msg, int nblocks)
    {
        for (int i = 0; i < nblocks; ++i)
            encrypt_block(ctx + i * blocksize, msg + i * blocksize);
    }

    void encrypt(bool *ctx, const bool *msg, int nblocks)
    {
        for (int i = 0; i < nblocks; ++i)
            encrypt_block(ctx + i * blocksize, msg + i * blocksize);
    }

    void encrypt_block(Bit *ctx, const Bit *msg)
    {
        ZKBitVecXor(ctx, msg, roundkeys, blocksize);
        for (unsigned r = 1; r <= rounds; ++r)
        {
            Substitution(ctx, ctx);
            MultiplyWithGF2Matrix(
                ctx, LinMatrices_loc + (r - 1) * blocksize * blocksize, ctx);
            ZKBitVecXor(ctx, ctx, roundconstants + (r - 1) * blocksize, blocksize);
            ZKBitVecXor(ctx, ctx, roundkeys + r * blocksize, blocksize);
        }
    }

    void encrypt_block(bool *ctx, const bool *msg)
    {
        ZKBitVecXor(ctx, msg, roundkeys_loc, blocksize);
        for (unsigned r = 1; r <= rounds; ++r)
        {
            Substitution(ctx, ctx);
            MultiplyWithGF2Matrix(
                ctx, LinMatrices_loc + (r - 1) * blocksize * blocksize, ctx);
            ZKBitVecXor(ctx, ctx, roundconstants_loc + (r - 1) * blocksize,
                        blocksize);
            ZKBitVecXor(ctx, ctx, roundkeys_loc + r * blocksize, blocksize);
        }
    }

    void set_key(const Bit *key_in)
    {
        delete[] key;
        delete[] roundkeys;

        key = new Bit[keysize];
        key_loc = new bool[keysize];
        roundkeys = new Bit[(rounds + 1) * blocksize];
        roundkeys_loc = new bool[(rounds + 1) * blocksize];

        std::memcpy(key, key_in, keysize * sizeof(Bit));

        for (unsigned r = 0; r <= rounds; ++r)
        {
            MultiplyWithGF2Matrix_Key(roundkeys + r * blocksize,
                                      KeyMatrices_loc + r * blocksize * keysize, key);
        }
    }

private:
    const std::vector<unsigned> Sbox = {0x00, 0x01, 0x03, 0x06,
                                        0x07, 0x04, 0x05, 0x02};

    PRG prg;
    Backend *base;

    block choice[2];
    bool *zero_bit_vec = nullptr;

    bool *LinMatrices_loc = nullptr;
    bool *roundconstants_loc = nullptr;
    bool *key_loc = nullptr;
    bool *KeyMatrices_loc = nullptr;
    bool *roundkeys_loc = nullptr;

    Bit *roundconstants = nullptr; // r*n
    Bit *key = nullptr;            // k
    Bit *roundkeys = nullptr; // r*n
    unsigned numofboxes;      // Number of Sboxes
    unsigned blocksize;       // Block size in bits
    unsigned keysize;         // Key size in bits
    unsigned rounds;          // Number of rounds
    unsigned identitysize;    // Bits left unchanged by the Sbox layer

    template <typename T>
    void ZKBitVecXor(T *out, const T *a, const T *b, int len)
    {
        for (int i = 0; i < len; ++i)
            out[i] = a[i] ^ b[i];
    }

    template <typename T>
    void Substitution(T *out, const T *in)
    {
        const T *pt = in;
        T *pto = out;
        T a, b, c;
        for (unsigned i = 0; i < numofboxes; ++i, pt += 3, pto += 3)
        {
            a = pt[0] ^ (pt[1] & pt[2]);
            b = pt[0] ^ (pt[1] ^ (pt[0] & pt[2]));
            c = pt[0] ^ (pt[1] ^ (pt[2] ^ (pt[0] & pt[1])));
            pto[0] = a;
            pto[1] = b;
            pto[2] = c;
        }
        memcpy(pto, pt, identitysize * sizeof(T));
    }

    void MultiplyWithGF2Matrix(bool *out, const bool *matrix, const bool *in)
    {
        const bool *pt = matrix;
        bool *buf = new bool[blocksize];
        for (unsigned i = 0; i < blocksize; ++i)
            buf[i] = pt[i] & in[0];
        pt += blocksize;
        for (unsigned i = 1; i < blocksize; ++i, pt += blocksize)
        {
            for (unsigned j = 0; j < blocksize; ++j)
                buf[j] = buf[j] ^ (pt[j] & in[i]);
        }
        memcpy(out, buf, blocksize * sizeof(bool));
        delete[] buf;
    }

    void MultiplyWithGF2Matrix(Bit *out, const bool *matrix, const Bit *in)
    {
        const bool *pt = matrix;
        Bit *buf = new Bit[blocksize];
        choice[1] = in[0].bit.L;
        for (unsigned i = 0; i < blocksize; ++i)
            buf[i].bit.L = choice[pt[i]];
        pt += blocksize;
        for (unsigned i = 1; i < blocksize; ++i, pt += blocksize)
        {
            choice[1] = in[i].bit.L;
            for (unsigned j = 0; j < blocksize; ++j)
            {
                buf[j].bit.L = buf[j].bit.L ^ choice[pt[j]];
            }
        }
        memcpy(out, buf, blocksize * sizeof(Bit));
        delete[] buf;
    }

    void MultiplyWithGF2Matrix_Key(bool *out, const bool *matrix, const bool *k)
    {
        const bool *pt = matrix;
        for (unsigned i = 0; i < blocksize; ++i, pt += keysize)
        {
            out[i] = pt[0] & k[0];
            for (unsigned j = 1; j < keysize; ++j)
                out[i] = out[i] ^ (pt[j] & k[j]);
        }
    }

    void MultiplyWithGF2Matrix_Key(Bit *out, const bool *matrix, const Bit *k)
    {
        const bool *pt = matrix;
        for (unsigned i = 0; i < blocksize; ++i, pt += keysize)
        {
            choice[1] = k[0].bit.L;
            out[i].bit.L = choice[pt[0]];
            for (unsigned j = 1; j < keysize; ++j)
            {
                choice[1] = k[j].bit.L;
                out[i].bit.L = out[i].bit.L ^ choice[pt[j]];
            }
        }
    }

    void keyschedule()
    {
        roundkeys = new Bit[(rounds + 1) * blocksize];
        roundkeys_loc = new bool[(rounds + 1) * blocksize];
        for (unsigned r = 0; r <= rounds; ++r)
        {
            MultiplyWithGF2Matrix_Key(roundkeys + r * blocksize,
                                      KeyMatrices_loc + r * blocksize * keysize, key);
            MultiplyWithGF2Matrix_Key(roundkeys_loc + r * blocksize,
                                      KeyMatrices_loc + r * blocksize * keysize,
                                      key_loc);
        }
    }

    void init(bool *key_b)
    {
        prg.reseed(&zero_block);
        unsigned len = 0;

        // key: k
        key = new Bit[keysize];
        key_loc = new bool[keysize];
        memcpy(key_loc, key_b, keysize * sizeof(bool));
        base->feed((block *)key, ALICE, key_loc, keysize);

        // LinMatrices: r * n * n
        len = rounds * blocksize * blocksize;
        LinMatrices_loc = new bool[len];
        prg.random_bool(LinMatrices_loc, len);

        // roundconstants: r * n
        len = rounds * blocksize;
        roundconstants = new Bit[len];
        roundconstants_loc = new bool[len];
        prg.random_bool(roundconstants_loc, len);
        base->feed((block *)roundconstants, PUBLIC,
                   roundconstants_loc, len);

        // KeyMatrices: (r + 1) * n * k
        len = (rounds + 1) * blocksize * keysize;
        KeyMatrices_loc = new bool[len];
        prg.random_bool(KeyMatrices_loc, len);

        zero_bit_vec = new bool[blocksize];
        memset(zero_bit_vec, 0, blocksize * sizeof(bool));
        choice[0] = zero_block;
    }

    void random_bit_vec_gen(Bit *out, bool *out_loc, int len)
    {
        out_loc = new bool[len];
        out = new Bit[len];
        prg.random_bool(out_loc, len);
        base->feed((block *)out, PUBLIC, out_loc, len);
    }
};

#endif