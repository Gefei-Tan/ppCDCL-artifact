#ifndef PPCDCL_CLAUSE_LIST_VEC_H
#define PPCDCL_CLAUSE_LIST_VEC_H

#include "clause.h"
#include "util.h"

class ClauseListVec
{
public:
    // Arrays of clause blocks, accessed by linear scan.
    vector<Integer> phi_oram;                        // used for clauses with index >= 0
    vector<Integer> conflict_oram;                   // used for clauses with index <  0
    vector<std::pair<Integer, Integer>> unit_clause; // (literal, clause_idx) pairs; length VAR_NUM + 1

    // Index sizes in bits. Each covers every block of its array; the number of
    // blocks is (number of clauses) * CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE.
    int phi_idx_size, conflict_phi_idx_size;
    double t1, t2, t3, t4, t5;

    ClauseListVec()
    {
    }

    ClauseListVec(FloramMPC<NetIO> *backend)
    {
        // Phi and conflict clauses use the same block size.
        phi_oram.resize(1 << PHI_ORAM_SIZE_BIT, Integer(1 << CLAUSE_ORAM_UNIT_SIZE, 0));
        conflict_oram.resize(1 << CON_ORAM_SIZE_BIT, Integer(1 << CLAUSE_ORAM_UNIT_SIZE, 0));
        phi_idx_size = PHI_ORAM_SIZE_BIT;
        conflict_phi_idx_size = CON_ORAM_SIZE_BIT;
        unit_clause.resize(VAR_NUM + 1, {Integer(VAR_SIZE_BIT, 0), Integer(CLAUSE_IDX_SIZE_BIT, 0)});

        std::cout << "#####ClauseList init! "
                  << "\nphi_oram size: " << (PHI_ORAM_SIZE_BIT)
                  << "\nconflict_oram size: " << (CON_ORAM_SIZE_BIT)
                  << "\nORAM block size (bit) : " << (CLAUSE_ORAM_UNIT_SIZE)
                  << "\n# of blocks for one clause: " << CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE
                  << "\nsingle clause bitstring length: " << CLAUSE_BITSTRING_SIZE_BIT
                  << std::endl;
    }

    void init(FloramMPC<NetIO> *backend)
    {
        // Phi and conflict clauses use the same block size.
        phi_oram.resize(1 << PHI_ORAM_SIZE_BIT, Integer(1 << CLAUSE_ORAM_UNIT_SIZE, 0));
        conflict_oram.resize(1 << CON_ORAM_SIZE_BIT, Integer(1 << CLAUSE_ORAM_UNIT_SIZE, 0));
        phi_idx_size = PHI_ORAM_SIZE_BIT;
        conflict_phi_idx_size = CON_ORAM_SIZE_BIT;
        unit_clause.resize(VAR_NUM + 1, {Integer(VAR_SIZE_BIT, 0), Integer(CLAUSE_IDX_SIZE_BIT, 0)});

        std::cout << "#####ClauseList init! "
                  << "\nphi_oram size: " << (PHI_ORAM_SIZE_BIT)
                  << "\nconflict_oram size: " << (CON_ORAM_SIZE_BIT)
                  << "\nORAM block size (bit) : " << (CLAUSE_ORAM_UNIT_SIZE)
                  << "\n# of blocks for one clause: " << CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE
                  << "\nsingle clause bitstring length: " << CLAUSE_BITSTRING_SIZE_BIT
                  << std::endl;
    }

    void insert_clause(const Integer &clause_idx, const Clause &clause, Bit flag = true)
    {
        Integer clause_idx_abs = clause_idx.abs();

        // Non-negative indices go to the phi array, negative ones to the conflict array.
        Bit if_greater = clause_idx.geq(Integer(clause_idx.size(), 0));

        int bits_per_literal = VAR_SIZE_BIT;
        int bits_per_block = 1 << CLAUSE_ORAM_UNIT_SIZE;
        int blocks_per_clause = CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE;

        // Pack all literal bits into one integer.
        int total_bits = MAX_LIT_IN_CLAUSE * bits_per_literal;
        Integer all_literal_bits(total_bits, 0, PUBLIC);

        for (int i = 0; i < clause.literals.size() && i < MAX_LIT_IN_CLAUSE; i++)
        {
            int bit_start = i * bits_per_literal;
            for (int b = 0; b < bits_per_literal && b < clause.literals[i].size(); b++)
            {
                if (bit_start + b < all_literal_bits.size())
                {
                    all_literal_bits.bits[bit_start + b] = clause.literals[i].bits[b];
                }
            }
        }

        // Split the packed bits into blocks.
        for (int j = 0; j < blocks_per_clause; j++)
        {
            Integer block(bits_per_block, 0, PUBLIC);

            int bit_start = j * bits_per_block;
            int bit_end = std::min((j + 1) * bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < block.size() && b < all_literal_bits.size())
                {
                    block.bits[b - bit_start] = all_literal_bits.bits[b];
                }
            }

            // Block index: |clause_idx| * blocks per clause + j.
            Integer multiplier = Integer(clause_idx_abs.size(), CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE, PUBLIC);
            Integer base = clause_idx_abs * multiplier;
            Integer offset = Integer(clause_idx_abs.size(), j, PUBLIC);
            Integer index = base + offset;

            if (IF_DEBUG)
            {
                std::cout << "writing clause #" << reveal_32(clause_idx) << " " << (j + 1) << "/" << CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE << " block: "
                          << reveal_bool_string(block, bits_per_literal) << std::endl;
            }

            oram_write(if_greater, index, block, flag);
        }
    }

    void get_clause(const Integer &clause_idx, Clause &clause)
    {
        auto t = clock_start();
        // Non-negative indices are read from the phi array, negative ones from the conflict array.
        Bit if_greater = clause_idx.geq(Integer(clause_idx.size(), 0));

        int bits_per_literal = VAR_SIZE_BIT;
        int bits_per_block = 1 << CLAUSE_ORAM_UNIT_SIZE;
        int total_bits = MAX_LIT_IN_CLAUSE * bits_per_literal;

        Integer clause_idx_abs = clause_idx.abs();
        t1 += time_from(t);
        t = clock_start();

        // Reassemble the packed literal bits from all blocks of the clause.
        Integer all_literal_bits(total_bits, 0, PUBLIC);

        for (int j = 0; j < CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE; j++)
        {
            Integer multiplier = Integer(clause_idx_abs.size(), CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE, PUBLIC);
            Integer base = clause_idx_abs * multiplier;
            Integer offset = Integer(clause_idx_abs.size(), j, PUBLIC);
            Integer index = base + offset;

            Integer block;
            oram_read(if_greater, index, block);

            int bit_start = j * bits_per_block;
            int bit_end = std::min((j + 1) * bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < block.size() && b < all_literal_bits.size())
                {
                    all_literal_bits.bits[b] = block.bits[b - bit_start];
                }
            }

            if (IF_DEBUG)
            {
                std::cout << "reading clause #" << index.reveal<int>() << " " << (j + 1) << "/" << CLAUSE_ORAM_NUM_OF_BLOCK_FOR_ONE_CLAUSE << " block: "
                          << reveal_bool_string(block, bits_per_literal) << std::endl;
            }
        }
        t2 += time_from(t);
        t = clock_start();
        // Unpack the literals.
        std::vector<Integer> retrieved_literals;
        for (int i = 0; i < MAX_LIT_IN_CLAUSE; i++)
        {
            Integer literal(bits_per_literal, 0, PUBLIC);
            int bit_start = i * bits_per_literal;

            for (int b = 0; b < bits_per_literal; b++)
            {
                if (bit_start + b < all_literal_bits.size())
                {
                    literal.bits[b] = all_literal_bits.bits[bit_start + b];
                }
            }

            retrieved_literals.push_back(literal);
        }
        t3 += time_from(t);
        t = clock_start();

        clause.set_literals_from_vec(retrieved_literals);
        t4 += time_from(t);
    }

    void delete_clause(Integer clause_idx, Bit flag = true)
    {
        // Overwrite the clause with an empty one.
        Clause zero_clause;
        insert_clause(clause_idx, zero_clause, flag);
    }

    // Overloads taking a public int clause index.
    void insert_clause(const int clause_idx, const Clause &clause, Bit flag = true)
    {
        Integer idx(phi_idx_size, clause_idx, PUBLIC);
        insert_clause(idx, clause, flag);
    }

    void delete_clause(int clause_idx, Bit flag = true)
    {
        Integer idx(phi_idx_size, clause_idx, PUBLIC);
        delete_clause(idx, flag);
    }

    void get_clause(const int clause_idx, Clause &clause)
    {
        Integer idx(phi_idx_size, clause_idx, PUBLIC);
        get_clause(idx, clause);
    }

    void print(string msg = "\n==============clause list=================\n")
    {
        std::cout << msg;
        for (int i = 0; i < CLAUSE_NUM + 1; ++i)
        {
            Clause c;
            get_clause(Integer(PHI_ORAM_SIZE_BIT, i), c);
            std::cout << "Clause " << i << ": ";
            c.print();
        }
        for (int i = 0; i < MAX_NUM_OF_CONFLICT_CLAUSE + 1; ++i)
        {
            Clause c;
            get_clause(Integer(PHI_ORAM_SIZE_BIT, -i), c);
            std::cout << "Conflict Clause -" << i << ": ";
            c.print();
        }
        std::cout << "unit clause: \n";
        for (auto &i : unit_clause)
        {
            std::cout << "literal: " << reveal_32(i.first) << " clause_idx: " << reveal_32(i.second) << std::endl;
        }
    }

    void oram_write(Bit if_greater, const Integer &idx, const Integer &val, Bit flag)
    {
        assert(idx.reveal<int32_t>() <= (1 << PHI_ORAM_SIZE_BIT) - 1);
        assert(idx.reveal<int32_t>() <= (1 << CON_ORAM_SIZE_BIT) - 1);
        Integer phi_idx = idx;
        assign_vector(phi_oram, phi_idx, val, flag & if_greater);
        assign_vector(conflict_oram, phi_idx, val, flag & !if_greater);

        if (IF_DEBUG)
        {
            std::cout << (if_greater.reveal() ? "phi " : "conflict ") << "ORAM writing #" << idx.reveal<int>() << " " << " block: " << reveal_bool_string(val, VAR_SIZE_BIT) << std::endl;
        }
    }

    void oram_write(bool use_phi, int idx, const Integer &val, Bit flag)
    {
        assert(idx <= (1 << PHI_ORAM_SIZE_BIT) - 1);
        assert(idx <= (1 << CON_ORAM_SIZE_BIT) - 1);

        if (val.size() != (1 << CLAUSE_ORAM_UNIT_SIZE))
        {
            // Block size is not validated.
        }

        if (use_phi)
        {
            phi_oram[idx] = val;
        }
        else
        {
            conflict_oram[idx] = val;
        }
    }

    void oram_read(Bit if_greater, const Integer &idx, Integer &result)
    {
        assert(idx.reveal<int32_t>() <= (1 << PHI_ORAM_SIZE_BIT) - 1);
        assert(idx.reveal<int32_t>() <= (1 << CON_ORAM_SIZE_BIT) - 1);

        if (result.size() != (1 << CLAUSE_ORAM_UNIT_SIZE))
        {
            // Block size is not validated.
        }
        Integer tmp;
        Integer phi_idx = idx;
        read_vector(phi_oram, phi_idx, result);
        read_vector(conflict_oram, phi_idx, tmp);
        result = If(if_greater, result, tmp);
        if (IF_DEBUG)
        {
            std::cout << (if_greater.reveal() ? "phi " : "conflict ") << "ORAM reading #" << idx.reveal<int>() << " " << " block: " << reveal_bool_string(result, VAR_SIZE_BIT) << std::endl;
        }
    }

    void oram_read(bool use_phi, int idx, Integer &result)
    {
        assert(idx <= (1 << PHI_ORAM_SIZE_BIT) - 1);
        assert(idx <= (1 << CON_ORAM_SIZE_BIT) - 1);

        if (use_phi)
        {
            result = phi_oram[idx];
        }
        else
        {
            result = conflict_oram[idx];
        }
    }

    ~ClauseListVec()
    {
    }

    void print_time()
    {
        std::cout << "t1: " << t1 / 1000.0 / 1000.0 << "s" << std::endl;
        std::cout << "t2: " << t2 / 1000.0 / 1000.0 << "s" << std::endl;
        std::cout << "t3: " << t3 / 1000.0 / 1000.0 << "s" << std::endl;
        std::cout << "t4: " << t4 / 1000.0 / 1000.0 << "s" << std::endl;
        std::cout << "t5: " << t5 / 1000.0 / 1000.0 << "s" << std::endl;
    }
};

#endif // PPCDCL_CLAUSE_LIST_VEC_H
