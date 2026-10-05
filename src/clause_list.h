#ifndef PPCDCL_CLAUSE_LIST_H
#define PPCDCL_CLAUSE_LIST_H

#include <cstdint>

#include "clause.h"
#include "util.h"

class ClauseList
{
public:
    // FloRAMs that store the clause bit strings.
    FloRAM<NetIO> *phi_oram = nullptr;               // used for clauses with index >= 0
    FloRAM<NetIO> *conflict_oram = nullptr;          // used for clauses with index <  0
    vector<std::pair<Integer, Integer>> unit_clause; // (literal, clause_idx) pairs; length VAR_NUM + 1
    double phi_oram_time = 0;
    // Logical clause reads and writes per store; the unit tests use these to check which store is accessed.
    std::uint64_t logical_phi_clause_writes = 0;
    std::uint64_t logical_con_clause_writes = 0;
    std::uint64_t logical_phi_clause_reads = 0;
    std::uint64_t logical_con_clause_reads = 0;
    int con_clause_idx = 0; // Index for conflict clauses, used to track the next available index

    // Index sizes in bits. Each covers every block of its ORAM; the number of
    // blocks is (number of clauses) * the number of ORAM blocks per clause.
    int phi_idx_size, conflict_phi_idx_size;
    double t1, t2, t3, t4, t5;

    ClauseList()
    {
    }

    ClauseList(FloramMPC<NetIO> *backend)
    {
        phi_oram = new FloRAM<NetIO>(backend, PHI_ORAM_SIZE_BIT, PHI_CLAUSE_ORAM_UNIT_SIZE, -1, nullptr, -1, -1, false, false);
        conflict_oram = new FloRAM<NetIO>(backend, CON_ORAM_SIZE_BIT, CON_CLAUSE_ORAM_UNIT_SIZE, -1, nullptr, -1, -1, false, false);
        unit_clause.clear();
        unit_clause.reserve(VAR_NUM + 1);

        if (IF_DEBUG)
        {
            std::cout << "#####ClauseList init! "
                      << "\nphi_oram size: " << (PHI_ORAM_SIZE_BIT)
                      << "\nconflict_oram size: " << (CON_ORAM_SIZE_BIT)
                      << "\nphi ORAM block size (bit) : " << (PHI_CLAUSE_ORAM_UNIT_SIZE)
                      << "\ncon ORAM block size (bit) : " << (CON_CLAUSE_ORAM_UNIT_SIZE)
                      << "\n# of blocks for one phi clause: " << PHI_ORAM_BLOCKS_PER_CLAUSE
                      << "\n# of blocks for one conflict clause: " << CON_ORAM_BLOCKS_PER_CLAUSE
                      << "\nphi clause bitstring length: " << PHI_CLAUSE_BITSTRING_SIZE_BIT
                      << "\ncon clause bitstring length: " << CON_CLAUSE_BITSTRING_SIZE_BIT
                      << std::endl;
        }
    }

    void insert_phi_clause(const Integer &clause_idx_abs, const Clause &clause, Bit flag)
    {
        logical_phi_clause_writes += 1;
        int bits_per_literal = VAR_SIZE_BIT;
        int phi_bits_per_block = 1 << PHI_CLAUSE_ORAM_UNIT_SIZE;

        // Pack all literal bits into one integer.
        int total_bits = PHI_CLAUSE_BITSTRING_SIZE_BIT;
        Integer all_literal_bits(total_bits, 0, PUBLIC);

        for (int i = 0; i < clause.literals.size(); i++)
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

        // Compute in the physical address width. Multiplying at the logical
        // clause-ID width can wrap before phi_oram_write widens the result
        // when a clause occupies multiple blocks.
        Integer physical_clause_idx = clause_idx_abs;
        physical_clause_idx.resize(PHI_ORAM_SIZE_BIT, false);
        Integer phi_base = physical_clause_idx *
                           Integer(PHI_ORAM_SIZE_BIT, PHI_ORAM_BLOCKS_PER_CLAUSE, PUBLIC);

        // Split the packed bits into blocks.
        for (int j = 0; j < PHI_ORAM_BLOCKS_PER_CLAUSE; j++)
        {
            Integer block(phi_bits_per_block, 0, PUBLIC);

            int bit_start = j * phi_bits_per_block;
            int bit_end = std::min((j + 1) * phi_bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < block.size() && b < all_literal_bits.size())
                {
                    block.bits[b - bit_start] = all_literal_bits.bits[b];
                }
            }

            Integer offset(PHI_ORAM_SIZE_BIT, j, PUBLIC);
            Integer phi_index = phi_base + offset;

            phi_oram_write(phi_index, block, flag);

            if (IF_DEBUG)
            {
                std::cout << "writing PHI clause #" << reveal_32(clause_idx_abs) << " " << (j + 1)
                          << "/" << PHI_ORAM_BLOCKS_PER_CLAUSE << " block: "
                          << reveal_bool_string(block, bits_per_literal) << std::endl;
            }
        }
    }

    void insert_con_clause(const Integer &clause_idx_abs, const Clause &clause, Bit flag)
    {
        logical_con_clause_writes += 1;
        int bits_per_literal = VAR_SIZE_BIT;
        int con_bits_per_block = 1 << CON_CLAUSE_ORAM_UNIT_SIZE;

        // Pack all literal bits into one integer.
        int total_bits = CON_CLAUSE_BITSTRING_SIZE_BIT;
        Integer all_literal_bits(total_bits, 0, PUBLIC);

        for (int i = 0; i < clause.literals.size(); i++)
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

        Integer physical_clause_idx = clause_idx_abs;
        physical_clause_idx.resize(CON_ORAM_SIZE_BIT, false);
        Integer con_base = physical_clause_idx *
                           Integer(CON_ORAM_SIZE_BIT, CON_ORAM_BLOCKS_PER_CLAUSE, PUBLIC);

        // Split the packed bits into blocks.
        for (int j = 0; j < CON_ORAM_BLOCKS_PER_CLAUSE; j++)
        {
            Integer block(con_bits_per_block, 0, PUBLIC);

            int bit_start = j * con_bits_per_block;
            int bit_end = std::min((j + 1) * con_bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < block.size() && b < all_literal_bits.size())
                {
                    block.bits[b - bit_start] = all_literal_bits.bits[b];
                }
            }

            Integer offset(CON_ORAM_SIZE_BIT, j, PUBLIC);
            Integer con_index = con_base + offset;

            con_oram_write(con_index, block, flag);

            if (IF_DEBUG)
            {
                std::cout << "writing CON clause #" << reveal_32(clause_idx_abs) << " " << (j + 1)
                          << "/" << CON_ORAM_BLOCKS_PER_CLAUSE << " block: "
                          << reveal_bool_string(block, bits_per_literal) << std::endl;
            }
        }
    }

    void insert_con_clause(const int &clause_idx_abs, const Clause &clause, Bit flag)
    {
        int bits_per_literal = VAR_SIZE_BIT;
        int con_bits_per_block = 1 << CON_CLAUSE_ORAM_UNIT_SIZE;

        // Pack all literal bits into one integer.
        int total_bits = CON_CLAUSE_BITSTRING_SIZE_BIT;
        Integer all_literal_bits(total_bits, 0, PUBLIC);

        for (int i = 0; i < clause.literals.size(); i++)
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
        for (int j = 0; j < CON_ORAM_BLOCKS_PER_CLAUSE; j++)
        {
            Integer block(con_bits_per_block, 0, PUBLIC);

            int bit_start = j * con_bits_per_block;
            int bit_end = std::min((j + 1) * con_bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < block.size() && b < all_literal_bits.size())
                {
                    block.bits[b - bit_start] = all_literal_bits.bits[b];
                }
            }

            int con_index = clause_idx_abs * CON_ORAM_BLOCKS_PER_CLAUSE + j;

            con_oram_write(con_index, block, flag);

            if (IF_DEBUG)
            {
                std::cout << "writing CON clause #" << clause_idx_abs << " " << (j + 1)
                          << "/" << CON_ORAM_BLOCKS_PER_CLAUSE << " block: "
                          << reveal_bool_string(block, bits_per_literal) << std::endl;
            }
        }
    }

    void get_phi_clause(const Integer &clause_idx_abs, Clause &clause)
    {
        logical_phi_clause_reads += 1;
        int bits_per_literal = VAR_SIZE_BIT;
        int phi_bits_per_block = 1 << PHI_CLAUSE_ORAM_UNIT_SIZE;
        int total_bits = PHI_CLAUSE_BITSTRING_SIZE_BIT;

        Integer physical_clause_idx = clause_idx_abs;
        physical_clause_idx.resize(PHI_ORAM_SIZE_BIT, false);
        Integer base_idx = physical_clause_idx *
                           Integer(PHI_ORAM_SIZE_BIT, PHI_ORAM_BLOCKS_PER_CLAUSE, PUBLIC);
        vector<Integer> blocks(PHI_ORAM_BLOCKS_PER_CLAUSE, Integer(phi_bits_per_block, 0, PUBLIC));

        // Read all blocks of the clause with one range_read.
        phi_oram->range_read(base_idx, blocks, PHI_ORAM_BLOCKS_PER_CLAUSE);

        // Reassemble the packed literal bits.
        Integer all_literal_bits(total_bits, 0, PUBLIC);

        for (int j = 0; j < PHI_ORAM_BLOCKS_PER_CLAUSE; j++)
        {
            int bit_start = j * phi_bits_per_block;
            int bit_end = std::min((j + 1) * phi_bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < blocks[j].size() && b < all_literal_bits.size())
                {
                    all_literal_bits.bits[b] = blocks[j].bits[b - bit_start];
                }
            }

            if (IF_DEBUG)
            {
                std::cout << "reading clause idx" << reveal_32(clause_idx_abs)
                          << " using range_read, block " << (j + 1)
                          << "/" << PHI_ORAM_BLOCKS_PER_CLAUSE << " block: "
                          << reveal_bool_string(blocks[j], bits_per_literal) << std::endl;
            }
        }

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

        clause.literals = retrieved_literals;
    }

    void get_con_clause(const Integer &clause_idx_abs, Clause &clause)
    {
        logical_con_clause_reads += 1;

        int bits_per_literal = VAR_SIZE_BIT;
        int con_bits_per_block = 1 << CON_CLAUSE_ORAM_UNIT_SIZE;
        int total_bits = CON_CLAUSE_BITSTRING_SIZE_BIT;

        Integer physical_clause_idx = clause_idx_abs;
        physical_clause_idx.resize(CON_ORAM_SIZE_BIT, false);
        Integer base_idx = physical_clause_idx *
                           Integer(CON_ORAM_SIZE_BIT, CON_ORAM_BLOCKS_PER_CLAUSE, PUBLIC);

        vector<Integer> blocks(CON_ORAM_BLOCKS_PER_CLAUSE, Integer(con_bits_per_block, 0, PUBLIC));

        // Read all blocks of the clause with one range_read.
        conflict_oram->range_read(base_idx, blocks, CON_ORAM_BLOCKS_PER_CLAUSE);

        // Reassemble the packed literal bits.
        Integer all_literal_bits(total_bits, 0, PUBLIC);

        for (int j = 0; j < CON_ORAM_BLOCKS_PER_CLAUSE; j++)
        {
            int bit_start = j * con_bits_per_block;
            int bit_end = std::min((j + 1) * con_bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < blocks[j].size() && b < all_literal_bits.size())
                {
                    all_literal_bits.bits[b] = blocks[j].bits[b - bit_start];
                }
            }

            if (IF_DEBUG)
            {
                std::cout << "reading clause idx" << reveal_32(clause_idx_abs)
                          << " using range_read, block " << (j + 1)
                          << "/" << CON_ORAM_BLOCKS_PER_CLAUSE << " block: "
                          << reveal_bool_string(blocks[j], bits_per_literal) << std::endl;
            }
        }

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

        clause.literals = retrieved_literals;
    }

    void _get_phi_clause(const Integer &clause_idx_abs, Clause &clause)
    {
        int bits_per_literal = VAR_SIZE_BIT;
        int phi_bits_per_block = 1 << PHI_CLAUSE_ORAM_UNIT_SIZE;
        int total_bits = PHI_CLAUSE_BITSTRING_SIZE_BIT;

        Integer all_literal_bits(total_bits, 0, PUBLIC);

        Integer physical_clause_idx = clause_idx_abs;
        physical_clause_idx.resize(PHI_ORAM_SIZE_BIT, false);
        Integer phi_base = physical_clause_idx *
                           Integer(PHI_ORAM_SIZE_BIT, PHI_ORAM_BLOCKS_PER_CLAUSE, PUBLIC);

        // Read all blocks and reassemble the packed literal bits.
        for (int j = 0; j < PHI_ORAM_BLOCKS_PER_CLAUSE; j++)
        {
            Integer offset(PHI_ORAM_SIZE_BIT, j, PUBLIC);
            Integer phi_index = phi_base + offset;

            Integer block(phi_bits_per_block, 0, PUBLIC);
            phi_oram_read(phi_index, block);

            int bit_start = j * phi_bits_per_block;
            int bit_end = std::min((j + 1) * phi_bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < block.size() && b < all_literal_bits.size())
                {
                    all_literal_bits.bits[b] = block.bits[b - bit_start];
                }
            }

            if (IF_DEBUG)
            {
                std::cout << "reading clause idx" << reveal_32(clause_idx_abs) << " "
                          << "reading PHI clause #" << phi_index.reveal<uint>() << " " << (j + 1)
                          << "/" << PHI_ORAM_BLOCKS_PER_CLAUSE << " block: "
                          << reveal_bool_string(block, bits_per_literal) << std::endl;
            }
        }

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

        clause.literals = retrieved_literals;
    }

    void _get_con_clause(const Integer &clause_idx_abs, Clause &clause)
    {
        int bits_per_literal = VAR_SIZE_BIT;
        int con_bits_per_block = 1 << CON_CLAUSE_ORAM_UNIT_SIZE;
        int total_bits = CON_CLAUSE_BITSTRING_SIZE_BIT;

        Integer all_literal_bits(total_bits, 0, PUBLIC);

        Integer physical_clause_idx = clause_idx_abs;
        physical_clause_idx.resize(CON_ORAM_SIZE_BIT, false);
        Integer con_base = physical_clause_idx *
                           Integer(CON_ORAM_SIZE_BIT, CON_ORAM_BLOCKS_PER_CLAUSE, PUBLIC);

        // Read all blocks and reassemble the packed literal bits.
        for (int j = 0; j < CON_ORAM_BLOCKS_PER_CLAUSE; j++)
        {
            Integer offset(CON_ORAM_SIZE_BIT, j, PUBLIC);
            Integer con_index = con_base + offset;

            Integer block(con_bits_per_block, 0, PUBLIC);
            con_oram_read(con_index, block);

            int bit_start = j * con_bits_per_block;
            int bit_end = std::min((j + 1) * con_bits_per_block, total_bits);

            for (int b = bit_start; b < bit_end; b++)
            {
                if (b - bit_start < block.size() && b < all_literal_bits.size())
                {
                    all_literal_bits.bits[b] = block.bits[b - bit_start];
                }
            }

            if (IF_DEBUG)
            {
                std::cout << "reading clause idx" << reveal_32(clause_idx_abs) << " "
                          << "reading CON clause #" << con_index.reveal<uint>() << " " << (j + 1)
                          << "/" << CON_ORAM_BLOCKS_PER_CLAUSE << " block: "
                          << reveal_bool_string(block, bits_per_literal) << std::endl;
            }
        }

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

        clause.literals = retrieved_literals;
    }

    void insert_clause(const Integer &clause_idx, const Clause &clause, Bit flag = true)
    {
        assert(clause_idx.size() == PHI_ORAM_SIZE_BIT || clause_idx.size() == CON_ORAM_SIZE_BIT ||
               clause_idx.size() == CLAUSE_IDX_SIZE_BIT || clause_idx.size() == CLAUSE_IDX_SIZE_IN_WL);

        Integer clause_idx_abs = clause_idx.abs();

        // Non-negative indices belong to the phi ORAM, negative ones to the conflict ORAM.
        Bit if_from_phi = clause_idx.geq(Integer(clause_idx.size(), 0));

        // Both ORAMs are written; the flag selects which write takes effect.
        insert_phi_clause(clause_idx_abs, clause, flag & if_from_phi);
        insert_con_clause(clause_idx_abs, clause, flag & !if_from_phi);
    }

    void get_clause(const Integer &clause_idx, Clause &clause)
    {
        assert(clause_idx.size() == PHI_ORAM_SIZE_BIT || clause_idx.size() == CON_ORAM_SIZE_BIT ||
               clause_idx.size() == CLAUSE_IDX_SIZE_BIT || clause_idx.size() == CLAUSE_IDX_SIZE_IN_WL);
        auto t = clock_start();

        // Non-negative indices belong to the phi ORAM, negative ones to the conflict ORAM.
        Bit if_from_phi = clause_idx.geq(Integer(clause_idx.size(), 0));

        Integer clause_idx_abs = clause_idx.abs();
        t1 += time_from(t);
        t = clock_start();

        Clause phi_clause;
        Clause con_clause;

        // Read from both ORAMs and select the result by sign.
        get_phi_clause(clause_idx_abs, phi_clause);
        get_con_clause(clause_idx_abs, con_clause);
        for (int i = 0; i < MAX_LIT_IN_CLAUSE; i++)
        {
            clause.literals[i] = If(if_from_phi, phi_clause.literals[i], con_clause.literals[i]);
        }

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
        Integer idx(CLAUSE_IDX_SIZE_IN_WL, clause_idx, PUBLIC);
        insert_clause(idx, clause, flag);
    }

    void delete_clause(int clause_idx, Bit flag = true)
    {
        Integer idx(CLAUSE_IDX_SIZE_IN_WL, clause_idx, PUBLIC);
        delete_clause(idx, flag);
    }

    void get_clause(const int clause_idx, Clause &clause)
    {
        Integer idx(CLAUSE_IDX_SIZE_IN_WL, clause_idx, PUBLIC);
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

    void phi_oram_write(const Integer &idx, const Integer &val, Bit flag)
    {
        auto t1 = clock_start();
        Integer phi_idx = idx;
        phi_oram->write(phi_idx.resize(PHI_ORAM_SIZE_BIT), [&](const Integer &in, Integer &out)
                        { out = If(flag, val, in); });

        if (IF_DEBUG)
        {
            std::cout << ("phi ") << "ORAM writing #" << idx.reveal<int>() << " " << " block: " << reveal_bool_string(val, VAR_SIZE_BIT) << std::endl;
        }
        phi_oram_time += time_from(t1);
    }

    void con_oram_write(const Integer &idx, const Integer &val, Bit flag)
    {
        auto t1 = clock_start();
        Integer phi_idx = idx;
        conflict_oram->write(phi_idx.resize(CON_ORAM_SIZE_BIT), [&](const Integer &in, Integer &out)
                             { out = If(flag, val, in); });

        if (IF_DEBUG)
        {
            std::cout << ("conflict ") << "ORAM writing #" << idx.reveal<int>() << " " << " block: " << reveal_bool_string(val, VAR_SIZE_BIT) << std::endl;
        }
        phi_oram_time += time_from(t1);
    }

    void con_oram_write(const int &idx, const Integer &val, Bit flag)
    {
        auto t1 = clock_start();
        conflict_oram->write(idx, [&](const Integer &in, Integer &out)
                             { out = If(flag, val, in); });

        if (IF_DEBUG)
        {
            std::cout << ("conflict ") << "ORAM writing #" << idx << " " << " block: " << reveal_bool_string(val, VAR_SIZE_BIT) << std::endl;
        }
        phi_oram_time += time_from(t1);
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
            phi_oram->write(idx, [&](const Integer &in, Integer &out)
                            { out = If(flag, val, in); });
        }
        else
        {
            conflict_oram->write(idx, [&](const Integer &in, Integer &out)
                                 { out = If(flag, val, in); });
        }
    }

    void con_oram_read(const Integer &idx, Integer &result)
    {
        auto t1 = clock_start();
        Integer phi_idx = idx;
        conflict_oram->read(phi_idx.resize(CON_ORAM_SIZE_BIT), result);
        if (IF_DEBUG)
        {
        }
        phi_oram_time += time_from(t1);
    }

    void phi_oram_read(const Integer &idx, Integer &result)
    {
        auto t1 = clock_start();
        Integer phi_idx = idx;
        phi_oram->read(phi_idx.resize(PHI_ORAM_SIZE_BIT), result);

        if (IF_DEBUG)
        {
        }
        phi_oram_time += time_from(t1);
    }

    void oram_read(bool use_phi, int idx, Integer &result)
    {
        assert(idx <= (1 << PHI_ORAM_SIZE_BIT) - 1);
        assert(idx <= (1 << CON_ORAM_SIZE_BIT) - 1);

        if (use_phi)
        {
            phi_oram->read(idx, result);
        }
        else
        {
            conflict_oram->read(idx, result);
        }
    }

    ~ClauseList()
    {
        delete phi_oram;
        delete conflict_oram;
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

#endif // PPCDCL_CLAUSE_LIST_H
