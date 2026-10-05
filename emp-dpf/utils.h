#ifndef EMP_DPF_UTILS_H__
#define EMP_DPF_UTILS_H__

#include "emp-tool/emp-tool.h"

namespace emp {

    inline static bool getSLSB(const block & x) { return (x[0] & 2) == 2; }

    inline static int endian_reverse(int x) { return (15-(x/8))*8 + (x%8); }

    inline static bool block_at_bit(const block &x, int pos) {
        if (pos < 64) {
            return ((1ll << pos) & x[0]) ? true : false;
        } else {
            return ((1ll << (pos - 64)) & x[1]) ? true : false;
        }
    }

}

#endif