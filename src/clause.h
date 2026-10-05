#ifndef CLAUSE_H
#define CLAUSE_H

#include "iostream"
#include "emp-dpf/emp-dpf.h"
#include "constant.h"
#include "unordered_set"

using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

class Clause
{
public:
    vector<Integer> literals;

    Clause()
    {
        for (int i = 0; i < MAX_LIT_IN_CLAUSE; ++i)
        {
            literals.push_back(Integer(VAR_SIZE_BIT, 0, PUBLIC));
        }
    }

    // Builds an all-zero clause; the argument is ignored.
    Clause(const Integer lits)
    {
        for (int i = 0; i < MAX_LIT_IN_CLAUSE; ++i)
        {
            literals.push_back(Integer(VAR_SIZE_BIT, 0, PUBLIC));
        }
    }

    Clause(std::vector<Integer> &input_literals)
    {
        if (input_literals.size() > MAX_LIT_IN_CLAUSE)
        {
            throw std::runtime_error("Too many literals in clause");
        }

        literals = input_literals;

        // Pad with zeros if needed
        while (literals.size() < MAX_LIT_IN_CLAUSE)
        {
            literals.push_back(Integer(VAR_SIZE_BIT, 0, PUBLIC));
        }
    }

    Clause(std::vector<int> input_literals, int party = PUBLIC)
    {
        if (input_literals.size() > MAX_LIT_IN_CLAUSE)
        {
            throw std::runtime_error("Too many literals in clause");
        }

        for (int i = 0; i < input_literals.size(); ++i)
        {
            literals.push_back(Integer(VAR_SIZE_BIT, input_literals[i], party));
        }

        // Pad with zeros if needed
        while (literals.size() < MAX_LIT_IN_CLAUSE)
        {
            literals.push_back(Integer(VAR_SIZE_BIT, 0, PUBLIC));
        }
    }

    Clause(std::vector<std::string> literals_str, int party = PUBLIC)
    {
        if (literals_str.size() > MAX_LIT_IN_CLAUSE)
        {
            throw std::runtime_error("Too many literals in clause");
        }

        for (int i = 0; i < literals_str.size(); ++i)
        {
            int lit = std::stoi(literals_str[i]);
            literals.push_back(Integer(VAR_SIZE_BIT, lit, party));
        }

        while (literals.size() < MAX_LIT_IN_CLAUSE)
        {
            literals.push_back(Integer(VAR_SIZE_BIT, 0, PUBLIC));
        }
    }

    void set_literals_from_vec(std::vector<Integer> &input_literals)
    {
        if (input_literals.size() > MAX_LIT_IN_CLAUSE)
        {
            throw std::runtime_error("Too many literals in clause");
        }

        literals.clear();
        literals = input_literals;

        while (literals.size() < MAX_LIT_IN_CLAUSE)
        {
            literals.push_back(Integer(VAR_SIZE_BIT, 0, PUBLIC));
        }
    }

    ~Clause() {}

    int lit_to_bit_idx(int literal)
    {
        if (literal == 0)
            return 0;
        else if (literal > 0)
            return literal;
        else
            return -literal + VAR_NUM;
    }

    static int bit_idx_to_lit(int bit_idx)
    {
        if (bit_idx == 0)
            return 0;
        else if (bit_idx <= VAR_NUM)
            return bit_idx;
        else
            return -bit_idx + VAR_NUM;
    }

    void parse_clause(std::vector<Integer> &out_literals)
    {
        out_literals = literals;
    }

    void print_int_as_bit_string(Integer in)
    {
        std::cout << "printing " << in.reveal<int>() << " as bit string\n";
        for (int i = in.size() - 1; i >= 0; --i)
        {
            std::cout << in.bits[i].reveal() << " ";
        }
        std::cout << "\n";
    }

    // Prints every literal, including zero padding.
    void print_raw()
    {
        for (auto &lit : literals)
        {
            std::cout << lit.reveal<int>() << " ";
        }
        std::cout << "\n";
    }

    // Prints the non-zero literals.
    void print() const
    {
        for (auto &lit : literals)
        {
            int value = reveal_32(lit);
            if (value != 0)
            {
                std::cout << value << " ";
            }
        }
        std::cout << "\n";
    }
};

#endif // CLAUSE_H
