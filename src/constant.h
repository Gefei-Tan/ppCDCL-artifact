#ifndef PPCDCL_CONSTANT_H
#define PPCDCL_CONSTANT_H

#include "util.h"

// Constants named *_UNIT_SIZE or *_ORAM_BLOCK_SIZE are the base-2 logarithm of an ORAM block size
// in bits (8 bits = 2^3, so 3).
static int VAR_SIZE_BIT;        // bit length of variable
static int CLAUSE_IDX_SIZE_BIT; // bit length of clause index
static int ASSIGNMENT_SIZE_BIT;

static int CON_CLAUSE_IDX_SIZE_BIT; // bit length of conflict clause index
static int CLAUSE_IDX_SIZE_IN_WL;
static int MAX_NUM_OF_CLAUSE_IN_WL; // maximum number of clauses in a watchlist; worst case is the total number of clauses
static int MAX_LIT_IN_CLAUSE;       // maximum number of literals in a clause (input or conflict)
static int MAX_LIT_IN_CON_CLAUSE;   // maximum number of literals in a conflict clause
static int MAX_LIT_IN_PHI_CLAUSE;   // maximum number of literals in an input clause
static int CLAUSE_BITSTRING_SIZE_BIT;
static int CLAUSE_ORAM_UNIT_SIZE;
static int CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE; // ORAM blocks per clause
static int CON_CLAUSE_ORAM_UNIT_SIZE;
static int PHI_CLAUSE_ORAM_UNIT_SIZE;
static int CON_ORAM_BLOCKS_PER_CLAUSE;    // ORAM blocks per conflict clause
static int PHI_ORAM_BLOCKS_PER_CLAUSE;    // ORAM blocks per input clause
static int CON_CLAUSE_BITSTRING_SIZE_BIT; // bits per conflict clause
static int PHI_CLAUSE_BITSTRING_SIZE_BIT; // bits per input clause
static int MAX_NUM_OF_CONFLICT_CLAUSE;    // maximum number of conflict clauses
static int MAX_CLAUSE_IN_WL;              // maximum number of clauses in a watchlist; worst case is the total number of clauses
static int CON_ORAM_SIZE_BIT;
static int PHI_ORAM_SIZE_BIT;
static int WL_ORAM_SIZE_BIT;
// VSIDS activities are unsigned 16-bit values. EMP Integer comparisons are
// signed, but activities start at zero and remain non-negative under the
// public benchmark bounds.
static int ACTIVITY_SIZE_BIT = 16;
static int VAR_NUM;
static int CLAUSE_NUM;
static int WL_BITSTRING_SIZE_BIT; // bits per watchlist
static int WL_ORAM_UNIT_SIZE;     // log2 of the watchlist ORAM block size in bits
static int WL_BLOCK_SIZE;
static int WL_ENTRIES_PER_BLOCK; // whole clause IDs per block; entries never cross blocks
static int WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL;
static int WATCH_POSITION_SIZE_BIT; // 1-based immutable position in the padded clause; 0 is absent
static int WATCH_PAIR_PAYLOAD_BITS;
static int WATCH_PAIR_ORAM_UNIT_SIZE;
static int WATCH_PAIR_ORAM_SIZE_BIT;
static int WATCH_OCC_PAYLOAD_BITS;
static int WATCH_OCC_ORAM_UNIT_SIZE;
static int WATCH_OCC_ORAM_SIZE_BIT;

// The scan cursor holds a signed sentinel (-1) in addition to every public
// watchlist slot. Production code and benchmarks share this encoding.
static inline int watchlist_scan_cursor_bits()
{
    return std::max(bits_required(MAX_CLAUSE_IN_WL + 1) + 1, 3);
}
static int BIG_V_ORAM_SIZE_BIT;
static int DL_ORAM_BLOCK_SIZE;
static int ASS_ORAM_BLOCK_SIZE;
static int ANTE_ORAM_BLOCK_SIZE;
static int ANTE_SIZE_BIT;
static int DL_SIZE_BIT;
static const int NOT_ASSIGNED = 0;
static const int FALSE_ASSIGNED = 1;
static const int TRUE_ASSIGNED = 2;
static const int MIN_VAR_SIZE = 8;
static double PHI_ORAM_READ_TIME = 0;
static double WL_ORAM_TIME = 0;
static const bool IF_DEBUG = false;
static const bool IF_PLAIN_ORAM = false;

inline static int ceil_log2_public_count(int value_count)
{
        if (value_count <= 0)
        {
                throw std::logic_error("Public storage count must be positive");
        }
        return bits_required(value_count - 1);
}

inline static int ceil_div_public(int numerator, int denominator)
{
        if (numerator <= 0 || denominator <= 0)
        {
                throw std::logic_error("Public storage dimensions must be positive");
        }
        return numerator / denominator + (numerator % denominator != 0);
}

inline static void init_constant(int var_num, int clause_num, int max_num_of_con_clause, int max_lit_in_clause, int max_lit_in_con_clause, int max_clause_in_wl)
{
        if (max_lit_in_clause > var_num || max_lit_in_con_clause > var_num)
        {
                throw std::logic_error("Max literals in clause exceed variable number");
        }
        // three assignment states, so 2 bits
        ASSIGNMENT_SIZE_BIT = 2;

        VAR_NUM = var_num;
        CLAUSE_NUM = clause_num;

        // variable and clause index sizes
        VAR_SIZE_BIT = std::max(bits_required(VAR_NUM + 1) + 1, 3);
        CLAUSE_IDX_SIZE_BIT = std::max(bits_required(CLAUSE_NUM + 1) + 1, 3); // includes a sign bit; minimum 3 bits
        MAX_NUM_OF_CONFLICT_CLAUSE = max_num_of_con_clause;
        CON_CLAUSE_IDX_SIZE_BIT = std::max(bits_required(max_num_of_con_clause + 1) + 1, 3); // includes a sign bit; minimum 3 bits

        // clause ORAM sizes
        MAX_LIT_IN_PHI_CLAUSE = max_lit_in_clause;
        MAX_LIT_IN_CON_CLAUSE = max_lit_in_con_clause;
        MAX_LIT_IN_CLAUSE = std::max(MAX_LIT_IN_PHI_CLAUSE, MAX_LIT_IN_CON_CLAUSE);

        const int phi_payload_bits = MAX_LIT_IN_PHI_CLAUSE * VAR_SIZE_BIT;
        const int con_payload_bits = MAX_LIT_IN_CON_CLAUSE * VAR_SIZE_BIT;
        PHI_CLAUSE_ORAM_UNIT_SIZE = std::min(ceil_log2_public_count(phi_payload_bits), 7);
        CON_CLAUSE_ORAM_UNIT_SIZE = std::min(ceil_log2_public_count(con_payload_bits), 7);
        CLAUSE_ORAM_UNIT_SIZE = std::max(PHI_CLAUSE_ORAM_UNIT_SIZE, CON_CLAUSE_ORAM_UNIT_SIZE);

        PHI_ORAM_BLOCKS_PER_CLAUSE = ceil_div_public(phi_payload_bits, 1 << PHI_CLAUSE_ORAM_UNIT_SIZE);
        CON_ORAM_BLOCKS_PER_CLAUSE = ceil_div_public(con_payload_bits, 1 << CON_CLAUSE_ORAM_UNIT_SIZE);
        CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE = std::max(PHI_ORAM_BLOCKS_PER_CLAUSE, CON_ORAM_BLOCKS_PER_CLAUSE);

        PHI_CLAUSE_BITSTRING_SIZE_BIT = phi_payload_bits;
        CON_CLAUSE_BITSTRING_SIZE_BIT = con_payload_bits;
        CLAUSE_BITSTRING_SIZE_BIT = std::max(PHI_CLAUSE_BITSTRING_SIZE_BIT, CON_CLAUSE_BITSTRING_SIZE_BIT);

        PHI_ORAM_SIZE_BIT = std::max(ceil_log2_public_count((CLAUSE_NUM + 1) * PHI_ORAM_BLOCKS_PER_CLAUSE), 8);
        CON_ORAM_SIZE_BIT = std::max(ceil_log2_public_count((MAX_NUM_OF_CONFLICT_CLAUSE + 1) * CON_ORAM_BLOCKS_PER_CLAUSE), 8);

        // watchlist ORAM sizes
        CLAUSE_IDX_SIZE_IN_WL = std::max(CLAUSE_IDX_SIZE_BIT, CON_CLAUSE_IDX_SIZE_BIT);
        MAX_CLAUSE_IN_WL = max_clause_in_wl;
        const int wl_payload_bits = MAX_CLAUSE_IN_WL * CLAUSE_IDX_SIZE_IN_WL;
        WL_ORAM_UNIT_SIZE = std::min(ceil_log2_public_count(wl_payload_bits), 7);
        WL_BLOCK_SIZE = 1 << WL_ORAM_UNIT_SIZE;
        WL_ENTRIES_PER_BLOCK = std::max(1, WL_BLOCK_SIZE / CLAUSE_IDX_SIZE_IN_WL);
        WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL =
                ceil_div_public(MAX_CLAUSE_IN_WL, WL_ENTRIES_PER_BLOCK);
        WL_BITSTRING_SIZE_BIT = wl_payload_bits;
        WL_ORAM_SIZE_BIT = ceil_log2_public_count(WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL * (2 * VAR_NUM + 1));

        // ORAMs for indexed watch state. The pair ORAM stores, for each clause,
        // two compact 1-based positions in the padded clause; the occurrence
        // ORAM stores one private bitmap record per literal. Indexed mode is
        // selected only when the bitmap fits in one FloRAM word of at most 128
        // bits.
        WATCH_POSITION_SIZE_BIT = std::max(bits_required(MAX_LIT_IN_CLAUSE), 1);
        WATCH_PAIR_PAYLOAD_BITS = 2 * WATCH_POSITION_SIZE_BIT;
        WATCH_PAIR_ORAM_UNIT_SIZE =
                std::min(ceil_log2_public_count(WATCH_PAIR_PAYLOAD_BITS), 7);
        WATCH_PAIR_ORAM_SIZE_BIT =
                ceil_log2_public_count(CLAUSE_NUM + MAX_NUM_OF_CONFLICT_CLAUSE + 1);
        WATCH_OCC_PAYLOAD_BITS = MAX_CLAUSE_IN_WL;
        WATCH_OCC_ORAM_UNIT_SIZE =
                std::min(ceil_log2_public_count(WATCH_OCC_PAYLOAD_BITS), 7);
        WATCH_OCC_ORAM_SIZE_BIT = ceil_log2_public_count(2 * VAR_NUM + 1);

        // variable state ORAM size
        BIG_V_ORAM_SIZE_BIT = bits_required(VAR_NUM + 1);

        // EMP Integer comparisons are signed, so decision levels reserve a sign bit.
        DL_SIZE_BIT = std::max(bits_required(VAR_NUM + 1) + 1, 3);
        ASSIGNMENT_SIZE_BIT = 2;
        ANTE_SIZE_BIT = std::max(CLAUSE_IDX_SIZE_BIT, CON_CLAUSE_IDX_SIZE_BIT);
        DL_ORAM_BLOCK_SIZE = std::max(bits_required(DL_SIZE_BIT), 3);
        ASS_ORAM_BLOCK_SIZE = std::max(bits_required(ASSIGNMENT_SIZE_BIT), 3);
        ANTE_ORAM_BLOCK_SIZE = std::max(bits_required(ANTE_SIZE_BIT), 3);

        int max_wl_size = max_num_of_con_clause + clause_num + 1;
        MAX_NUM_OF_CLAUSE_IN_WL = max_wl_size;

        if (IF_DEBUG)
        {
                std::cout << "#var: " << VAR_NUM << ", #clause: " << CLAUSE_NUM << ", max_num_of_con_clause: "
                          << MAX_NUM_OF_CONFLICT_CLAUSE << " max literal in clause:" << MAX_LIT_IN_PHI_CLAUSE << " max literal in con clause:" << MAX_LIT_IN_CON_CLAUSE << " max clause in wl: " << MAX_CLAUSE_IN_WL << std::endl;
        }
}

#endif // PPCDCL_CONSTANT_H
