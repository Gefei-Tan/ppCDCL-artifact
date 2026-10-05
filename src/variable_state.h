#ifndef PPCDCL_VARIABLE_STATE_H
#define PPCDCL_VARIABLE_STATE_H

#include "unordered_set"
#include "emp-dpf/emp-dpf.h"
#include "util.h"

using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

class VariableState
{
public:
    // Per-variable state, stored in vectors and accessed by linear scan.
    std::vector<Integer> assignment_vec;
    std::vector<Integer> dl_vec;
    std::vector<Integer> ante_vec;
    std::vector<Integer> phase_saving_vec;
    std::vector<Integer> order_ctr_vec;
    Integer cur_dl = Integer(DL_SIZE_BIT, 0, PUBLIC);
    Integer not_assigned = Integer(ASSIGNMENT_SIZE_BIT, NOT_ASSIGNED, PUBLIC);
    Integer true_assigned = Integer(ASSIGNMENT_SIZE_BIT, TRUE_ASSIGNED, PUBLIC);
    Integer false_assigned = Integer(ASSIGNMENT_SIZE_BIT, FALSE_ASSIGNED, PUBLIC);
    Integer zero_ante = Integer(ANTE_SIZE_BIT, 0, PUBLIC);
    Integer zero_dl = Integer(DL_SIZE_BIT, 0, PUBLIC);
    Integer global_order_ctr = Integer(VAR_SIZE_BIT + 4, 0, PUBLIC);

    inline int order_ctr_bitlen() const
    {
        return std::max(bits_required(VAR_NUM + 1) + 1, 3);
    }

    VariableState()
    {
    }

    inline void reset_public_words()
    {
        cur_dl = Integer(DL_SIZE_BIT, 0, PUBLIC);
        not_assigned = Integer(ASSIGNMENT_SIZE_BIT, NOT_ASSIGNED, PUBLIC);
        true_assigned = Integer(ASSIGNMENT_SIZE_BIT, TRUE_ASSIGNED, PUBLIC);
        false_assigned = Integer(ASSIGNMENT_SIZE_BIT, FALSE_ASSIGNED, PUBLIC);
        zero_ante = Integer(ANTE_SIZE_BIT, 0, PUBLIC);
        zero_dl = Integer(DL_SIZE_BIT, 0, PUBLIC);
        global_order_ctr = Integer(order_ctr_bitlen(), 0, PUBLIC);
    }

    VariableState(FloramMPC<NetIO> *backend)
    {
        reset_public_words();
        assignment_vec.resize(VAR_NUM + 1, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        dl_vec.resize(VAR_NUM + 1, Integer(DL_SIZE_BIT, 0, PUBLIC));
        ante_vec.resize(VAR_NUM + 1, Integer(ANTE_SIZE_BIT, 0, PUBLIC));
        phase_saving_vec.resize(VAR_NUM + 1, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        order_ctr_vec.resize(VAR_NUM + 1, Integer(order_ctr_bitlen(), 0, PUBLIC));

        if (IF_DEBUG)
        {
            std::cout << "#####Big V init! "
                      << "\nvector size: " << (VAR_NUM + 1)
                      << "\nassignment bit size: " << ASSIGNMENT_SIZE_BIT
                      << "\ndl bit size: " << DL_SIZE_BIT
                      << "\nante bit size: " << ANTE_SIZE_BIT
                      << "\nphase saving bit size: " << ASSIGNMENT_SIZE_BIT
                      << std::endl;
        }
    }

    ~VariableState()
    {
    }

    void init(FloramMPC<NetIO> *backend)
    {
        reset_public_words();
        assignment_vec.clear();
        dl_vec.clear();
        ante_vec.clear();
        phase_saving_vec.clear();
        order_ctr_vec.clear();
        assignment_vec.resize(VAR_NUM + 1, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        dl_vec.resize(VAR_NUM + 1, Integer(DL_SIZE_BIT, 0, PUBLIC));
        ante_vec.resize(VAR_NUM + 1, Integer(ANTE_SIZE_BIT, 0, PUBLIC));
        phase_saving_vec.resize(VAR_NUM + 1, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        order_ctr_vec.resize(VAR_NUM + 1, Integer(order_ctr_bitlen(), 0, PUBLIC));

        if (IF_DEBUG)
        {
            std::cout << "#####Big V init! "
                      << "\nvector size: " << (VAR_NUM + 1)
                      << "\nassignment bit size: " << ASSIGNMENT_SIZE_BIT
                      << "\ndl bit size: " << DL_SIZE_BIT
                      << "\nante bit size: " << ANTE_SIZE_BIT
                      << std::endl;
        }
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

            // Clear the record of every variable whose decision level exceeds cur_dl.
            assignment_vec[i] = If(if_reset, not_assigned, assignment_vec[i]);

            ante_vec[i] = If(if_reset, zero_ante, ante_vec[i]);

            dl_vec[i] = If(if_reset, zero_dl, dl_vec[i]);

            order_ctr_vec[i] = If(if_reset, Integer(order_ctr_bitlen(), 0, PUBLIC), order_ctr_vec[i]);
        }
        // Assignment order is the live trail position, not a lifetime event
        // counter. Keeping it bounded by n prevents wrap after many restarts.
        global_order_ctr = global_order_ctr - removed;
    }

    void assign(const Integer &literal, const Integer &val)
    {
        set_assignment(literal.abs(), val);
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
        return ((val_ass == false_assigned) & ((literal > 0))) | ((val_ass == true_assigned) & ((literal < 0)));
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

    void update_record_range(const std::vector<Bit> &idx_bits,
                             const Integer &assignment,
                             const Integer &decision_level,
                             const Integer &antecedent,
                             const Integer &phase_value,
                             const Integer &order_value,
                             int left, int right, int level,
                             const Bit &active)
    {
        if (right - left == 1)
        {
            assignment_vec[left] = If(active, assignment, assignment_vec[left]);
            dl_vec[left] = If(active, decision_level, dl_vec[left]);
            ante_vec[left] = If(active, antecedent, ante_vec[left]);
            phase_saving_vec[left] = If(active, phase_value, phase_saving_vec[left]);
            order_ctr_vec[left] = If(active, order_value, order_ctr_vec[left]);
            return;
        }

        const int middle = left + (right - left) / 2;
        Bit active_right = active & idx_bits[level];
        Bit active_left = active & !idx_bits[level];
        update_record_range(idx_bits, assignment, decision_level, antecedent,
                            phase_value, order_value, left, middle,
                            level + 1, active_left);
        update_record_range(idx_bits, assignment, decision_level, antecedent,
                            phase_value, order_value, middle, right,
                            level + 1, active_right);
    }

    Bit try_set_unit_literal_range(const std::vector<Bit> &idx_bits,
                                   const Integer &assignment,
                                   const Integer &decision_level,
                                   const Integer &antecedent,
                                   const Integer &order_value,
                                   int original_size,
                                   int left, int right, int level,
                                   const Bit &active)
    {
        if (right - left == 1)
        {
            // Slot zero is the public sentinel; only the public domain 1..n
            // may accept an implication.
            Bit is_real_leaf(left > 0 && left < original_size, PUBLIC);
            Bit did_assign = active & is_real_leaf &
                             (assignment_vec[left] == not_assigned);
            assignment_vec[left] = If(did_assign, assignment, assignment_vec[left]);
            dl_vec[left] = If(did_assign, decision_level, dl_vec[left]);
            ante_vec[left] = If(did_assign, antecedent, ante_vec[left]);
            phase_saving_vec[left] = If(did_assign, assignment, phase_saving_vec[left]);
            order_ctr_vec[left] = If(did_assign, order_value, order_ctr_vec[left]);
            return did_assign;
        }

        const int middle = left + (right - left) / 2;
        Bit active_right = active & idx_bits[level];
        Bit active_left = active & !idx_bits[level];
        Bit assigned_left = try_set_unit_literal_range(
            idx_bits, assignment, decision_level, antecedent, order_value,
            original_size, left, middle, level + 1, active_left);
        Bit assigned_right = try_set_unit_literal_range(
            idx_bits, assignment, decision_level, antecedent, order_value,
            original_size, middle, right, level + 1, active_right);
        // The routed selectors are one-hot, so the two subtrees cannot both
        // commit. XOR is therefore equivalent to OR and is free in the
        // underlying garbled circuit.
        return assigned_left ^ assigned_right;
    }

    void set_variable_record(const Integer &literal,
                             const Integer &assignment,
                             const Integer &decision_level,
                             const Integer &antecedent,
                             const Bit &flag)
    {
        const int original_size = static_cast<int>(assignment_vec.size());
        int index_bits = 0;
        for (int size = 1; size < original_size; size <<= 1)
        {
            index_bits += 1;
        }
        const int routed_size = 1 << index_bits;

        assignment_vec.resize(routed_size, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        dl_vec.resize(routed_size, Integer(DL_SIZE_BIT, 0, PUBLIC));
        ante_vec.resize(routed_size, Integer(ANTE_SIZE_BIT, 0, PUBLIC));
        phase_saving_vec.resize(routed_size, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        order_ctr_vec.resize(routed_size, Integer(order_ctr_bitlen(), 0, PUBLIC));

        Integer compact_idx = literal.abs();
        compact_idx.resize(index_bits, true);
        std::vector<Bit> route_bits = compact_idx.bits;
        std::reverse(route_bits.begin(), route_bits.end());
        update_record_range(route_bits, assignment, decision_level, antecedent,
                            assignment, global_order_ctr, 0, routed_size, 0, flag);

        assignment_vec.resize(original_size);
        dl_vec.resize(original_size);
        ante_vec.resize(original_size);
        phase_saving_vec.resize(original_size);
        order_ctr_vec.resize(original_size);
    }

    void set_unit_literal(const Integer &literal, const Integer &antecedent, const Integer &dl,
                          const Bit &flag = true)
    {
        Bit sign = literal > Integer(literal.size(), 0, PUBLIC);
        Integer value = If(sign, true_assigned, false_assigned); // 1 if variable is assigned false, 2 if true
        set_variable_record(literal, value, dl, antecedent, flag);
        global_order_ctr = If(flag, global_order_ctr + Integer(order_ctr_bitlen(), 1, PUBLIC), global_order_ctr);
    }

    Bit try_set_unit_literal(const Integer &literal,
                             const Integer &antecedent,
                             const Integer &dl,
                             const Bit &flag = Bit(true, PUBLIC))
    {
        const int original_size = static_cast<int>(assignment_vec.size());
        int index_bits = 0;
        for (int size = 1; size < original_size; size <<= 1)
        {
            index_bits += 1;
        }
        const int routed_size = 1 << index_bits;

        assignment_vec.resize(routed_size, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        dl_vec.resize(routed_size, Integer(DL_SIZE_BIT, 0, PUBLIC));
        ante_vec.resize(routed_size, Integer(ANTE_SIZE_BIT, 0, PUBLIC));
        phase_saving_vec.resize(routed_size, Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC));
        order_ctr_vec.resize(routed_size, Integer(order_ctr_bitlen(), 0, PUBLIC));

        Integer absolute_literal = literal.abs();
        // The routing tree uses only ceil(log2(n + 1)) low bits. Reject any
        // representable value with a secret high bit before truncation so a
        // malformed out-of-domain literal cannot alias a real leaf when
        // n + 1 is a power of two.
        Bit fits_route(true, PUBLIC);
        for (int bit = index_bits; bit < absolute_literal.size(); ++bit)
        {
            fits_route = fits_route & !absolute_literal.bits[bit];
        }
        Integer compact_idx = absolute_literal;
        compact_idx.resize(index_bits, true);
        std::vector<Bit> route_bits = compact_idx.bits;
        std::reverse(route_bits.begin(), route_bits.end());

        Bit sign = literal > Integer(literal.size(), 0, PUBLIC);
        Integer assignment = If(sign, true_assigned, false_assigned);
        Bit did_assign = try_set_unit_literal_range(
            route_bits, assignment, dl, antecedent, global_order_ctr,
            original_size, 0, routed_size, 0, flag & fits_route);

        assignment_vec.resize(original_size);
        dl_vec.resize(original_size);
        ante_vec.resize(original_size);
        phase_saving_vec.resize(original_size);
        order_ctr_vec.resize(original_size);

        global_order_ctr = If(
            did_assign,
            global_order_ctr + Integer(order_ctr_bitlen(), 1, PUBLIC),
            global_order_ctr);
        return did_assign;
    }

    void set_decision(const Integer &literal, const Integer &dl, const Bit &flag = true)
    {
        Bit sign = literal > Integer(literal.size(), 0, PUBLIC);
        Integer value = If(sign, true_assigned, false_assigned); // 1 if variable is assigned false, 2 if true
        set_variable_record(literal, value, dl,
                            Integer(ANTE_SIZE_BIT, 0, PUBLIC), flag);
        global_order_ctr = If(flag, global_order_ctr + Integer(order_ctr_bitlen(), 1, PUBLIC), global_order_ctr);
    }

    inline const void get_assignment(const Integer &var, Integer &out)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_assignment");
        }
        out = Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        read_vector(assignment_vec, var.abs(), out);
    }

    inline const void get_assignment(int var, Integer &out)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_assignment");
        }
        out = assignment_vec[std::abs(var)];
    }

    inline const void get_antecedent(const Integer &var, Integer &out)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_antecedent");
        }
        out = Integer(ANTE_SIZE_BIT, 0, PUBLIC);
        read_vector(ante_vec, var.abs(), out);
    }

    inline const void get_antecedent(int var, Integer &out)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_antecedent");
        }
        out = ante_vec[std::abs(var)];
    }

    void get_decision_level(const Integer &var, Integer &out)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_decision_level");
        }
        out = Integer(DL_SIZE_BIT, 0, PUBLIC);
        read_vector(dl_vec, var.abs(), out);
    }

    void get_order_counter(const Integer &var, Integer &out)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_order_counter");
        }
        out = Integer(order_ctr_bitlen(), 0, PUBLIC);
        read_vector(order_ctr_vec, var.abs(), out);
    }

    void get_order_counter(int var, Integer &out)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_order_counter");
        }
        out = order_ctr_vec[std::abs(var)];
    }

    void get_decision_level(int var, Integer &out)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_decision_level");
        }
        out = dl_vec[std::abs(var)];
    }

    inline void set_assignment(const Integer &var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_assignment");
        }

        assign_vector(assignment_vec, var.abs(), val, flag);
    }

    inline void set_assignment(int var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_assignment");
        }

        int idx = std::abs(var);
        assignment_vec[idx] = If(flag, val, assignment_vec[idx]);
    }

    inline void set_antecedent(const Integer &var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_antecedent");
        }
        assign_vector(ante_vec, var.abs(), val, flag);
    }

    inline void set_antecedent(const int var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_antecedent");
        }
        int idx = std::abs(var);
        ante_vec[idx] = If(flag, val, ante_vec[idx]);
    }

    inline void set_order_counter(const int var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_order_counter");
        }
        order_ctr_vec[std::abs(var)] = If(flag, val, order_ctr_vec[std::abs(var)]);
    }

    inline void set_decision_level(const Integer &var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_decision_level");
        }
        assign_vector(dl_vec, var.abs(), val, flag);
    }

    inline void set_order_counter(const Integer &var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_order_counter");
        }
        assign_vector(order_ctr_vec, var.abs(), val, flag);
    }

    inline void set_decision_level(const int var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_decision_level");
        }
        int idx = std::abs(var);
        dl_vec[idx] = If(flag, val, dl_vec[idx]);
    }

    inline void set_phase(const Integer &var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_phase");
        }
        assign_vector(phase_saving_vec, var.abs(), val, flag);
    }

    inline void set_phase(int var, const Integer &val, Bit flag = true)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in set_phase");
        }
        int idx = std::abs(var);
        phase_saving_vec[idx] = If(flag, val, phase_saving_vec[idx]);
    }

    inline void get_phase(const Integer &var, Integer &out)
    {
        if (IF_DEBUG && reveal_32(var) > VAR_NUM)
        {
            std::cout << "Error: var " << reveal_32(var) << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_phase");
        }
        out = Integer(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
        read_vector(phase_saving_vec, var.abs(), out);
    }

    inline void get_phase(int var, Integer &out)
    {
        if (IF_DEBUG && std::abs(var) > VAR_NUM)
        {
            std::cout << "Error: var " << var << " is out of bounds\n";
            throw std::out_of_range("Variable index out of bounds in get_phase");
        }
        out = phase_saving_vec[std::abs(var)];
    }

    void print_full_state()
    {
        std::cout << "\n==============big v=============\n";
        for (int i = 0; i < VAR_NUM + 1; ++i)
        {
            Integer assignment = assignment_vec[i];
            Integer dl = dl_vec[i];
            Integer ante = ante_vec[i];
            std::cout << "Variable " << i << " assignment: " << reveal_32(assignment, true) << " dl: "
                      << reveal_32(dl, true) << " ante: " << reveal_32(ante) << "\n";
        }
    }
};

#endif // PPCDCL_VARIABLE_STATE_H
