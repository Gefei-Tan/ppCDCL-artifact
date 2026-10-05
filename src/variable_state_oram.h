#ifndef VARIABLE_STATE_ORAM_H
#define VARIABLE_STATE_ORAM_H

#include <algorithm>
#include <cstdlib>
#include "unordered_set"
#include "emp-dpf/emp-dpf.h"
#include "constant.h"

using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

class VariableStateOram
{
public:
    FloRAM<NetIO> *assignment_vec = nullptr;
    FloRAM<NetIO> *dl_vec = nullptr;
    FloRAM<NetIO> *ante_vec = nullptr;
    FloRAM<NetIO> *order_ctr_vec = nullptr;
    FloRAM<NetIO> *phase = nullptr;
    Integer cur_dl = Integer(VAR_SIZE_BIT, 0, PUBLIC);
    Integer not_assigned = Integer(ASSIGNMENT_SIZE_BIT, NOT_ASSIGNED, PUBLIC);
    Integer true_assigned = Integer(ASSIGNMENT_SIZE_BIT, TRUE_ASSIGNED, PUBLIC);
    Integer false_assigned = Integer(ASSIGNMENT_SIZE_BIT, FALSE_ASSIGNED, PUBLIC);
    Integer zero_ante = Integer(ANTE_SIZE_BIT, 0, PUBLIC);
    Integer zero_dl = Integer(DL_SIZE_BIT, 0, PUBLIC);
    Integer zero_phase = Integer(3, 0, PUBLIC);
    Integer global_order_ctr = Integer(VAR_SIZE_BIT + 4, 0, PUBLIC);

    inline int logical_order_ctr_bitlen() const
    {
        return std::max(bits_required(VAR_NUM + 1) + 1, 3);
    }

    inline int order_ctr_oram_block_size() const
    {
        return std::max(bits_required(logical_order_ctr_bitlen()), 3);
    }

    inline int order_ctr_bitlen() const
    {
        return 1 << order_ctr_oram_block_size();
    }

    VariableStateOram()
    {
    }

    VariableStateOram(FloramMPC<NetIO> *backend)
    {
        use_oram_word_sizes();
        assignment_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, ASS_ORAM_BLOCK_SIZE);
        dl_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, DL_ORAM_BLOCK_SIZE);
        ante_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, ANTE_ORAM_BLOCK_SIZE);
        order_ctr_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, order_ctr_oram_block_size());
        phase = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, 3);
        reset_public_words();

        std::cout << "#####Big V ORAM init! "
                  << "\noram size: " << (1 << VAR_SIZE_BIT)
                  << "\nassignment unit size: " << (1 << ASS_ORAM_BLOCK_SIZE)
                  << "\ndl unit size: " << (1 << DL_ORAM_BLOCK_SIZE)
                  << "\nante unit size: " << (1 << ANTE_ORAM_BLOCK_SIZE)
                  << "\norder counter unit size: " << order_ctr_bitlen()
                  << std::endl;
    }

    ~VariableStateOram()
    {
        delete assignment_vec;
        delete dl_vec;
        delete ante_vec;
        delete order_ctr_vec;
        delete phase;
    }

    void init(FloramMPC<NetIO> *backend)
    {
        use_oram_word_sizes();
        assignment_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, ASS_ORAM_BLOCK_SIZE);
        dl_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, DL_ORAM_BLOCK_SIZE);
        ante_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, ANTE_ORAM_BLOCK_SIZE);
        order_ctr_vec = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, order_ctr_oram_block_size());
        phase = new FloRAM<NetIO>(backend, VAR_SIZE_BIT, 3);
        reset_public_words();

        std::cout << "#####Big V ORAM init! "
                  << "\noram size: " << (1 << VAR_SIZE_BIT)
                  << "\nassignment unit size: " << (1 << ASS_ORAM_BLOCK_SIZE)
                  << "\ndl unit size: " << (1 << DL_ORAM_BLOCK_SIZE)
                  << "\nante unit size: " << (1 << ANTE_ORAM_BLOCK_SIZE)
                  << "\norder counter unit size: " << order_ctr_bitlen()
                  << std::endl;
    }

    void assign(Integer var, const Integer val)
    {
        set_assignment(var, val);
    }

    inline void use_oram_word_sizes()
    {
        DL_SIZE_BIT = 1 << DL_ORAM_BLOCK_SIZE;
        ASSIGNMENT_SIZE_BIT = 1 << ASS_ORAM_BLOCK_SIZE;
        ANTE_SIZE_BIT = 1 << ANTE_ORAM_BLOCK_SIZE;
    }

    inline void reset_public_words()
    {
        cur_dl = Integer(DL_SIZE_BIT, 0, PUBLIC);
        not_assigned = Integer(ASSIGNMENT_SIZE_BIT, NOT_ASSIGNED, PUBLIC);
        true_assigned = Integer(ASSIGNMENT_SIZE_BIT, TRUE_ASSIGNED, PUBLIC);
        false_assigned = Integer(ASSIGNMENT_SIZE_BIT, FALSE_ASSIGNED, PUBLIC);
        zero_ante = Integer(ANTE_SIZE_BIT, 0, PUBLIC);
        zero_dl = Integer(DL_SIZE_BIT, 0, PUBLIC);
        zero_phase = Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        global_order_ctr = Integer(order_ctr_bitlen(), 0, PUBLIC);
    }

    inline Integer normalize_index(const Integer &var) const
    {
        Integer idx = var.abs();
        idx.resize(VAR_SIZE_BIT);
        return idx;
    }

    inline int normalize_index(int var) const
    {
        return std::abs(var);
    }

    Bit is_literal_falsified(const Integer &literal)
    {
        Integer val_ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        get_assignment(literal, val_ass);
        return ((val_ass == false_assigned) & (literal > Integer(VAR_SIZE_BIT, 0))) | ((
                                                                                          val_ass == (true_assigned) & (literal < Integer(VAR_SIZE_BIT, 0))));
    }

    Bit is_literal_falsified(const int literal)
    {
        Integer val_ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        get_assignment(literal, val_ass);
        return ((val_ass == false_assigned) & (literal > 0)) | ((val_ass == true_assigned) & (literal < 0));
    }

    Bit is_literal_satisfied(const Integer &literal)
    {
        Integer val_ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        get_assignment(literal, val_ass);
        return ((val_ass == false_assigned) & (literal < Integer(VAR_SIZE_BIT, 0))) | ((
                                                                                          val_ass == (true_assigned) & (literal > Integer(VAR_SIZE_BIT, 0))));
    }

    Bit is_literal_satisfied(const int literal)
    {
        Integer val_ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        get_assignment(literal, val_ass);
        return ((val_ass == false_assigned) & (literal < 0)) | ((val_ass == true_assigned) & (literal > 0));
    }

    Bit is_variable_unassigned(const Integer &literal)
    {
        Integer val_ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        get_assignment(literal, val_ass);
        return val_ass == (not_assigned);
    }

    Bit is_variable_unassigned(int literal)
    {
        Integer val_ass(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        get_assignment(literal, val_ass);
        return val_ass == (not_assigned);
    }

    void set_unit_literal(const Integer &literal, const Integer &antecedent, const Integer &dl, Bit flag = true)
    {
        Integer var = literal.abs();
        Integer zero(literal.size(), 0, PUBLIC);
        Integer one(literal.size(), 1, PUBLIC);
        Bit val = literal > zero;
        Integer value(ASSIGNMENT_SIZE_BIT, 1, PUBLIC);
        one.resize(value.size());
        var.resize(VAR_SIZE_BIT);
        value = If(val, value + one, value); // 1 if variable is assigned false, 2 if true
        set_assignment(var, value, flag);
        set_decision_level(var, dl, flag);
        set_antecedent(var, antecedent, flag);
        set_order_counter(var, global_order_ctr, flag);
        global_order_ctr = If(flag, global_order_ctr + Integer(order_ctr_bitlen(), 1, PUBLIC), global_order_ctr);
        set_phase(var, value, flag);
    }

    Bit try_set_unit_literal(const Integer &literal,
                             const Integer &antecedent,
                             const Integer &dl,
                             Bit flag = Bit(true, PUBLIC))
    {
        Integer assignment(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        get_assignment(literal, assignment);
        Integer var = literal.abs();
        Bit valid_literal =
            (var > Integer(var.size(), 0, PUBLIC)) &
            (var <= Integer(var.size(), VAR_NUM, PUBLIC));
        Bit did_assign = flag & valid_literal & (assignment == not_assigned);
        set_unit_literal(literal, antecedent, dl, did_assign);
        return did_assign;
    }

    void set_decision(const Integer &literal, const Integer &dl, Bit flag = true)
    {
        Integer var;
        Integer value(ASSIGNMENT_SIZE_BIT, 1, PUBLIC);
        Integer zero(1 << ANTE_ORAM_BLOCK_SIZE, 0, PUBLIC);
        Integer one(value.size(), 1, PUBLIC);
        Bit val;
        var = literal.abs();
        var.resize(VAR_SIZE_BIT);
        val = literal > Integer(literal.size(), 0);
        one.resize(value.size());
        value = If(val, value + one, value); // 1 if variable is assigned false, 2 if true
        set_assignment(var, value, flag);
        set_decision_level(var, dl, flag);
        set_antecedent(var, zero, flag);
        set_order_counter(var, global_order_ctr, flag);
        global_order_ctr = If(flag, global_order_ctr + Integer(order_ctr_bitlen(), 1, PUBLIC), global_order_ctr);
        set_phase(var, value, flag);
    }

    inline const void get_assignment(const Integer &var, Integer &out)
    {
        assignment_vec->read(normalize_index(var), out);
    }

    inline const void get_assignment(int var, Integer &out)
    {
        assignment_vec->read(normalize_index(var), out);
    }

    inline const void get_antecedent(const Integer &var, Integer &out)
    {
        ante_vec->read(normalize_index(var), out);
    }

    inline const void get_antecedent(int var, Integer &out)
    {
        ante_vec->read(normalize_index(var), out);
    }

    void get_decision_level(const Integer &var, Integer &out)
    {
        dl_vec->read(normalize_index(var), out);
    }

    void get_decision_level(int var, Integer &out)
    {
        dl_vec->read(normalize_index(var), out);
    }

    void get_order_counter(const Integer &var, Integer &out)
    {
        order_ctr_vec->read(normalize_index(var), out);
    }
    void get_order_counter(int var, Integer &out)
    {
        order_ctr_vec->read(normalize_index(var), out);
    }
    void get_phase(const Integer &var, Integer &out)
    {
        phase->read(normalize_index(var), out);
    }
    void get_phase(int var, Integer &out)
    {
        phase->read(normalize_index(var), out);
    }

    inline void set_assignment(const Integer &var, Integer val, Bit flag = true)
    {
        assignment_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                              { out = If(flag, val, in); });
    }

    inline void set_assignment(int var, Integer val, Bit flag = true)
    {
        assignment_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                              { out = If(flag, val, in); });
    }

    inline void set_antecedent(const Integer &var, Integer val, Bit flag = true)
    {
        ante_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                        { out = If(flag, val, in); });
    }

    inline void set_antecedent(const int var, Integer val, Bit flag = true)
    {
        ante_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                        { out = If(flag, val, in); });
    }

    inline void set_decision_level(const Integer &var, Integer val, Bit flag = true)
    {
        dl_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                      { out = If(flag, val, in); });
    }

    inline void set_decision_level(const int var, Integer val, Bit flag = true)
    {
        dl_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                      { out = If(flag, val, in); });
    }

    inline void set_order_counter(const Integer &var, Integer val, Bit flag = true)
    {
        order_ctr_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                             { out = If(flag, val, in); });
    }

    inline void set_order_counter(const int var, Integer val, Bit flag = true)
    {
        order_ctr_vec->write(normalize_index(var), [&](const Integer &in, Integer &out)
                             { out = If(flag, val, in); });
    }

    inline void set_phase(const Integer &var, Integer val, Bit flag = true)
    {
        phase->write(normalize_index(var), [&](const Integer &in, Integer &out)
                     { out = If(flag, val, in); });
    }
    inline void set_phase(const int var, Integer val, Bit flag = true)
    {
        phase->write(normalize_index(var), [&](const Integer &in, Integer &out)
                     { out = If(flag, val, in); });
    }

    void clean_up_after_backtrack(const Bit &flag = Bit(true, PUBLIC), int factor = 1)
    {
        Integer removed(order_ctr_bitlen(), 0, PUBLIC);
        for (int i = 0; i < (VAR_NUM + 1) / factor; ++i)
        {
            Integer dl_lvl = Integer(DL_SIZE_BIT, 0, PUBLIC);
            get_decision_level(i, dl_lvl);
            Bit if_reset = flag & (dl_lvl > cur_dl);
            removed = removed + If(if_reset,
                                   Integer(order_ctr_bitlen(), 1, PUBLIC),
                                   Integer(order_ctr_bitlen(), 0, PUBLIC));
            set_assignment(i, not_assigned, if_reset);
            set_decision_level(i, zero_dl, if_reset);
            set_antecedent(i, zero_ante, if_reset);
            set_order_counter(i, Integer(order_ctr_bitlen(), 0, PUBLIC), if_reset);
        }
        global_order_ctr = global_order_ctr - removed;
    }

    void print_full_state()
    {
        for (int i = 0; i < VAR_NUM + 1; ++i)
        {
            Integer assignment;
            Integer dl;
            Integer ante;
            Integer var = Integer(VAR_SIZE_BIT, i, PUBLIC);
            get_assignment(var, assignment);
            get_decision_level(var, dl);
            get_antecedent(var, ante);
            std::cout << "Variable " << i << " assignment: " << assignment.reveal<uint32_t>() << " dl: "
                      << dl.reveal<uint32_t>() << " ante: " << ante.reveal<uint32_t>() << "\n";
        }
    }
};

#endif // VARIABLE_STATE_ORAM_H
