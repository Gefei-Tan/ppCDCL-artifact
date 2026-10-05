#ifndef EMP_DPF_OT_HKDF_H
#define EMP_DPF_OT_HKDF_H

#include "emp-tool/emp-tool.h"
#include "emp-dpf/emp-dpf.h"
#include <string>

// https://github.com/pag-crypto/zkmbs/blob/main/readable_code/HKDF.java
using namespace std;
using namespace emp;

class HKDF {
public:
    using Bit = Bit_T<AuthWire>;
    using Integer = Integer_T<AuthWire>;

    Bit ZERO = Bit(false, PUBLIC);
    Bit ONE = Bit(true, PUBLIC);
    vector<Bit> IPAD; //0x36
    vector<Bit> OPAD; //0x5c
    vector<Bit> ONE_BYTE; //0x01
    vector<Bit> ZERO_BYTE; //0x00
    string hash_path = "./emp-dpf/files/sha256.txt";
    vector<Bit> IV;

    HKDF() {
        Integer ipad_tmp(8, 0x36, PUBLIC);
        Integer opad_tmp(8, 0x5c, PUBLIC);
        Integer one_byte_tmp(8, 1, PUBLIC);
        Integer zero_byte_tmp(8, 0, PUBLIC);
        IPAD = ipad_tmp.bits;
        OPAD = opad_tmp.bits;
        ZERO_BYTE = zero_byte_tmp.bits;
        ONE_BYTE = one_byte_tmp.bits;
        Integer iv_0(32, 0x6a09e667, PUBLIC);
        Integer iv_1(32, 0xbb67ae85, PUBLIC);
        Integer iv_2(32, 0x3c6ef372, PUBLIC);
        Integer iv_3(32, 0xa54ff53a, PUBLIC);
        Integer iv_4(32, 0x510e527f, PUBLIC);
        Integer iv_5(32, 0x9b05688c, PUBLIC);
        Integer iv_6(32, 0x1f83d9ab, PUBLIC);
        Integer iv_7(32, 0x5be0cd19, PUBLIC);
        IV = iv_7.bits;
        IV.insert(IV.end(), iv_6.bits.begin(), iv_6.bits.end());
        IV.insert(IV.end(), iv_5.bits.begin(), iv_5.bits.end());
        IV.insert(IV.end(), iv_4.bits.begin(), iv_4.bits.end());
        IV.insert(IV.end(), iv_3.bits.begin(), iv_3.bits.end());
        IV.insert(IV.end(), iv_2.bits.begin(), iv_2.bits.end());
        IV.insert(IV.end(), iv_1.bits.begin(), iv_1.bits.end());
        IV.insert(IV.end(), iv_0.bits.begin(), iv_0.bits.end());
    }

    inline int get_block_count(int input_len) {
        return ((input_len + 65) / 512) + 1;
    }

    // rfc6234
    inline vector<Bit> sha256_padding(vector<Bit> input) {
        int input_len = input.size();
        input.insert(input.begin(), ONE); // 4.1.a
        Integer input_len_tmp(64, input_len, PUBLIC);
        vector<Bit> input_len_64_bit = input_len_tmp.bits;
        int block_count = get_block_count(input_len);
        int zero_count = (block_count * 512) - (input_len + 65);
        vector<Bit> zero_padding(zero_count, ZERO);
        input.insert(input.begin(), zero_padding.begin(), zero_padding.end()); // 4.1.b
        input.insert(input.begin(), input_len_64_bit.begin(), input_len_64_bit.end()); // 4.1.c
        return input;
    }

    inline vector<Bit> sha256_compress(vector<Bit> iv, vector<Bit> msg) {
        emp::BristolFashion sha256(hash_path.c_str());
        iv.insert(iv.begin(), msg.begin(), msg.end());
        Integer out_int(256, 0, emp::PUBLIC);
        vector<Bit> out = out_int.bits;
        sha256.compute(out.data(), iv.data());
        return out;
    }

    inline vector<Bit> sha256(string input, bool if_null = false) {
        vector<Bit> bit_vec_input = string_to_bit_vec(input, PUBLIC);
        return sha256(bit_vec_input, if_null);
    }

    inline vector<Bit> sha256(vector<Bit> input, bool if_null = false) {
        Integer iv_t(256, 0, emp::PUBLIC);
        vector<Bit> iv = IV;
        vector<Bit> padded;
        if (!if_null) padded = sha256_padding(input);
        else {
            Integer tmp_zero(512, 0, PUBLIC);
            tmp_zero.bits[511] = Bit(true, PUBLIC);
            padded = tmp_zero.bits;
        }
        int block_count = padded.size() / 512;
        for (int i = block_count - 1; i >= 0; i--) {
            vector<Bit> input_block(padded.begin() + (i * 512), padded.begin() + (i * 512) + 512);
            iv = sha256_compress(iv, input_block);
        }
        return iv;
    }

    // inputs are vectors of Bit
    inline vector<Bit> hmac(vector<Bit> key, vector<Bit> msg) {
        int key_len = key.size(); // in bit
        if (key_len < 512) { // right padding key to 16 * 32 = 512 bits, the block size of sha-256.
            vector<Bit> key_pad(512 - key_len, ZERO);
            key.insert(key.begin(), key_pad.begin(), key_pad.end());
        }
        vector<Bit> key_ipad(512, ZERO);
        vector<Bit> key_opad(512, ZERO);
        for (int i = 0; i < 512; ++i) {
            key_ipad[i] = key[i] ^ IPAD[i % 8];
            key_opad[i] = key[i] ^ OPAD[i % 8];
        }
        key_ipad.insert(key_ipad.begin(), msg.begin(), msg.end()); // concat with msg
        vector<Bit> in_digest = sha256(key_ipad);
        key_opad.insert(key_opad.begin(), in_digest.begin(), in_digest.end()); // concat key_opad with in_digest
        vector<Bit> out_digest = sha256(key_opad);
        return out_digest;
    }

    inline vector<Bit> kdf_extract(vector<Bit> x, vector<Bit> y) {
        return hmac(x, y);
    }

    inline vector<Bit> kdf_expand(vector<Bit> key, vector<Bit> msg) {
        msg.insert(msg.begin(), ONE_BYTE.begin(), ONE_BYTE.end());
        return hmac(key, msg);
    }

    // length is in byte
    inline vector<Bit> get_label(int output_len, string lbl, vector<Bit> context_digest) {
        string tls13_str = "tls13 ";
        lbl = tls13_str.append(lbl);
        int lbl_len = lbl.length();
        int context_digest_len_in_byte = context_digest.size() / 8;
        Integer tmp(16, output_len, PUBLIC);
        Integer tmp1(8, lbl_len, PUBLIC);
        Integer tmp2(8, context_digest_len_in_byte, PUBLIC);
        vector<Bit> output_len_bit = tmp.bits;
        vector<Bit> lbl_len_bit = tmp1.bits;
        vector<Bit> context_digest_len_bit = tmp2.bits;
        vector<Bit> lbl_bit = string_to_bit_vec(lbl);

        // output_len || lbl_len || lbl_bit || context_digest_len_in_byte || context_digest
        output_len_bit.insert(output_len_bit.begin(), lbl_len_bit.begin(), lbl_len_bit.end());
        output_len_bit.insert(output_len_bit.begin(), lbl_bit.begin(), lbl_bit.end());
        output_len_bit.insert(output_len_bit.begin(), context_digest_len_bit.begin(), context_digest_len_bit.end());
        output_len_bit.insert(output_len_bit.begin(), context_digest.begin(), context_digest.end());
        return output_len_bit;
    }

    inline vector<Bit> hkdf_derive_tk(vector<Bit> secret, int len = 16) {
        vector<Bit> hkdf_lbl = get_label(len, "key", ZERO_BYTE);
        vector<Bit> expansion = kdf_expand(secret, hkdf_lbl);
        vector<Bit> ret(expansion.begin(), expansion.begin() + len * 8);
        return ret;
    }

    inline vector<Bit> hkdf_derive_iv(vector<Bit> secret, int len = 16) {
        vector<Bit> hkdf_lbl = get_label(len, "iv", ZERO_BYTE);
        vector<Bit> expansion = kdf_expand(secret, hkdf_lbl);
        vector<Bit> ret(expansion.begin(), expansion.begin() + len * 8);
        return ret;
    }

    inline vector<Bit> hkdf_derive_secret(vector<Bit> secret, string lbl, vector<Bit> context_digest) {
        vector<Bit> hkdf_lbl = get_label(32, lbl, context_digest);
        vector<Bit> expansion = kdf_expand(secret, hkdf_lbl);
        return expansion;
    }

    template<class T>
    inline T bit_vec_to_T(vector<Bit> vec) {
        Integer tmp(vec.size(), 0, PUBLIC);
        tmp.bits = vec;
        return tmp.reveal<T>(PUBLIC);
    }

    string bit_vec_to_binary_str(vector<Bit> vec, int party = PUBLIC) {
        string ret = "";
        for (int i = 0; i < vec.size(); ++i) {
            string tmp = vec[i].reveal(party) == true ? "1" : "0";
            ret = tmp + ret;
        }
        return ret;
    }


    inline vector<Bit> string_to_bit_vec(string input, int party = PUBLIC) {
        int str_len = input.size();
        vector<Bit> result;
        for (int i = 0; i < str_len; ++i) {
            Integer tmp(8, (int) input[i], party);
            result.insert(result.begin(), tmp.bits.begin(), tmp.bits.end());
        }
        return result;
    }


};

#endif 
