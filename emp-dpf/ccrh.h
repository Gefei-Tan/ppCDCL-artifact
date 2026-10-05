#ifndef EMP_DPF_DUAL_CCRH_H_
#define EMP_DPF_DUAL_CCRH_H_
#include "emp-tool/emp-tool.h"

namespace emp {

/*
 * By default, CRH use zero_block as the AES key.
 * Here we model f(x) = AES_{00..0}(x) as a random permutation (and thus in the RPM model)
 */
class DualCCRH: public PRP { public:
    block one = makeBlock(0, 1);
 
	DualCCRH(const block& key = zero_block): PRP(key) {
	}

	block H(block in) {
		block t; t = in = sigma(in);
		permute_block(&t, 1);
		return t ^ in;
	}
    
    inline static block Hp(block x) {
        return x ^ makeBlock(0, getLSB(x));
    }

    void ro_expand_1to2(block *chd, block par) {
        block t; chd[1] = par;
        t = chd[0] = sigma(chd[1]);
        permute_block(&t, 1);
        chd[0] = chd[0] ^ t;
        chd[1] = chd[1] ^ chd[0];
    }
    
    void ro_expand_2to4(block *chd, block *par) {
        block t[2]; chd[3] = par[1]; chd[1] = par[0];
        t[1] = chd[2] = sigma(chd[3]);
        t[0] = chd[0] = sigma(chd[1]);
        ParaEnc<1,2>(t, &aes);
        chd[2] = chd[2] ^ t[1];
        chd[0] = chd[0] ^ t[0];
        chd[3] = chd[3] ^ chd[2];
        chd[1] = chd[1] ^ chd[0];
    }

    void ro_expand_4to8(block *chd, block *par) {
        block t[4]; chd[7] = par[3]; chd[5] = par[2]; 
        chd[3] = par[1]; chd[1] = par[0];
        t[3] = chd[6] = sigma(chd[7]);
        t[2] = chd[4] = sigma(chd[5]);
        t[1] = chd[2] = sigma(chd[3]);
        t[0] = chd[0] = sigma(chd[1]);
        ParaEnc<1,4>(t, &aes);
        chd[6] = chd[6] ^ t[3];
        chd[4] = chd[4] ^ t[2];
        chd[2] = chd[2] ^ t[1];
        chd[0] = chd[0] ^ t[0];
        chd[7] = chd[7] ^ chd[6];
        chd[5] = chd[5] ^ chd[4];
        chd[3] = chd[3] ^ chd[2];
        chd[1] = chd[1] ^ chd[0];
    }

#ifdef __GNUC__
	#ifndef __clang__
		#pragma GCC push_options
		#pragma GCC optimize ("unroll-loops")
	#endif
#endif

	template<int n>
	void H(block out[n], block in[n]) {
		block tmp[n];
		for (int i = 0; i < n; ++i)
			tmp[i] = out[i] = sigma(in[i]);
		permute_block(tmp, n);
		xorBlocks_arr(out, tmp, out, n);
	}
#ifdef __GNUC__
	#ifndef __clang__
		#pragma GCC pop_options
	#endif
#endif

	void Hn(block*out, block* in, int length, block * scratch = nullptr) {
		bool del = false;
		if(scratch == nullptr) {
			del = true;
			scratch = new block[length];
		}

		for (int i = 0; i < length; ++i)
			scratch[i] = out[i] = sigma(in[i]);

		permute_block(scratch, length);
		xorBlocks_arr(out, scratch, out, length);

		if(del) {
			delete[] scratch;
			scratch = nullptr;
		}
	}
};

}//namespace
#endif// EMP_DPF_DUAL_CCRH_H_