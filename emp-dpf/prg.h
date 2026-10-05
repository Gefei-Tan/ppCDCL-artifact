#ifndef EMP_DPF_PRG_H__
#define EMP_DPF_PRG_H__

#include "emp-tool/emp-tool.h"
using namespace emp;

//kappa->2kappa PRG, implemented as G(k) = PRF_seed0(k)\xor k || PRF_seed1(k)\xor k
class DPFPRG { public:
	emp::AES_KEY aes_key[2];

	DPFPRG(block seed0, block seed1) {
		AES_set_encrypt_key((const block)seed0, aes_key);
	}

	void node_expand_one(block *children, block *parent) {
		block tmp;
		tmp = children[0] = parent[0];
		ParaEnc<1,1>(&tmp, aes_key);
		children[0] = children[0] ^ tmp;
	}

	void node_expand_two(block *children, block *parent) {
		//p[0],           p[1]
		//c[0], c[1]      c[2], c[3]
		//t[0]  t[2]      t[1]  t[3]
		block tmp[2];
		tmp[1] = children[1] = parent[1];
		tmp[0] = children[0] = parent[0];
		ParaEnc<1,2>(tmp, aes_key);
		children[1] = children[1] ^ tmp[1];
		children[0] = children[0] ^ tmp[0];
	}
	void node_expand_four(block *children, block *parent) {
		//p[0],           p[1]                    p2             p3
		//c[0], c[1]      c[2], c[3],       4      5           6     7
		//t[0]  t[4]      t[1]  t[5]       2       6           3     7
		block tmp[4];
		tmp[3] = children[3] = parent[3];
		tmp[2] = children[2] = parent[2];
		tmp[1] = children[1] = parent[1];
		tmp[0] = children[0] = parent[0];

		ParaEnc<1,4>(tmp, aes_key);
		children[3] = children[3] ^ tmp[3];
		children[2] = children[2] ^ tmp[2];
		children[1] = children[1] ^ tmp[1];
		children[0] = children[0] ^ tmp[0];
	}

};
#endif
