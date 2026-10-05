#ifndef PPCDCL_VARIABLE_STATE_BACKEND_H
#define PPCDCL_VARIABLE_STATE_BACKEND_H

#include "variable_state.h"
#include "variable_state_oram.h"

class VariableStateBackend
{
public:
    class WordProxy
    {
    public:
        enum class Kind
        {
            CurDl,
            NotAssigned,
            TrueAssigned,
            FalseAssigned
        };

        WordProxy(VariableStateBackend *owner, Kind kind)
            : owner_(owner), kind_(kind)
        {
        }

        Integer get() const
        {
            return owner_->get_word(kind_);
        }

        Integer operator()() const
        {
            return get();
        }

        operator Integer() const
        {
            return get();
        }

        WordProxy &operator=(const Integer &value)
        {
            owner_->set_word(kind_, value);
            return *this;
        }

        WordProxy &operator=(const WordProxy &value)
        {
            owner_->set_word(kind_, value.get());
            return *this;
        }

    private:
        VariableStateBackend *owner_;
        Kind kind_;
    };

    WordProxy cur_dl;
    WordProxy not_assigned;
    WordProxy true_assigned;
    WordProxy false_assigned;

    explicit VariableStateBackend(FloramMPC<NetIO> *backend, bool use_oram = true)
        : cur_dl(this, WordProxy::Kind::CurDl),
          not_assigned(this, WordProxy::Kind::NotAssigned),
          true_assigned(this, WordProxy::Kind::TrueAssigned),
          false_assigned(this, WordProxy::Kind::FalseAssigned),
          use_oram_(use_oram)
    {
        if (use_oram_)
        {
            oram_ = new VariableStateOram(backend);
        }
        else
        {
            linear_ = new VariableState(backend);
        }
    }

    ~VariableStateBackend()
    {
        delete oram_;
        delete linear_;
    }

    bool uses_oram() const
    {
        return use_oram_;
    }

    void set_cur_dl(const Integer &value)
    {
        set_word(WordProxy::Kind::CurDl, value);
    }

    int order_ctr_bitlen() const
    {
        return use_oram_ ? oram_->order_ctr_bitlen() : linear_->order_ctr_bitlen();
    }

    Integer live_assignment_count() const
    {
        return use_oram_ ? oram_->global_order_ctr : linear_->global_order_ctr;
    }

    void print_full_state()
    {
        if (use_oram_)
        {
            oram_->print_full_state();
        }
        else
        {
            linear_->print_full_state();
        }
    }

    Bit is_literal_falsified(const Integer &literal)
    {
        return use_oram_ ? oram_->is_literal_falsified(literal) : linear_->is_literal_falsified(literal);
    }

    Bit is_literal_falsified(int literal)
    {
        return use_oram_ ? oram_->is_literal_falsified(literal) : linear_->is_literal_falsified(literal);
    }

    Bit is_literal_satisfied(const Integer &literal)
    {
        return use_oram_ ? oram_->is_literal_satisfied(literal) : linear_->is_literal_satisfied(literal);
    }

    Bit is_literal_satisfied(int literal)
    {
        return use_oram_ ? oram_->is_literal_satisfied(literal) : linear_->is_literal_satisfied(literal);
    }

    Bit is_variable_unassigned(const Integer &literal)
    {
        return use_oram_ ? oram_->is_variable_unassigned(literal) : linear_->is_variable_unassigned(literal);
    }

    Bit is_variable_unassigned(int literal)
    {
        return use_oram_ ? oram_->is_variable_unassigned(literal) : linear_->is_variable_unassigned(literal);
    }

    void set_unit_literal(const Integer &literal, const Integer &antecedent, const Integer &dl, Bit flag = true)
    {
        if (use_oram_)
        {
            oram_->set_unit_literal(literal, antecedent, dl, flag);
        }
        else
        {
            linear_->set_unit_literal(literal, antecedent, dl, flag);
        }
    }

    Bit try_set_unit_literal(const Integer &literal,
                             const Integer &antecedent,
                             const Integer &dl,
                             Bit flag = Bit(true, PUBLIC))
    {
        return use_oram_
                   ? oram_->try_set_unit_literal(literal, antecedent, dl, flag)
                   : linear_->try_set_unit_literal(literal, antecedent, dl, flag);
    }

    void set_decision(const Integer &literal, const Integer &dl, Bit flag = true)
    {
        if (use_oram_)
        {
            oram_->set_decision(literal, dl, flag);
        }
        else
        {
            linear_->set_decision(literal, dl, flag);
        }
    }

    void get_assignment(const Integer &var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_assignment(var, out);
        }
        else
        {
            linear_->get_assignment(var, out);
        }
    }

    void get_assignment(int var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_assignment(var, out);
        }
        else
        {
            linear_->get_assignment(var, out);
        }
    }

    void get_antecedent(const Integer &var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_antecedent(var, out);
        }
        else
        {
            linear_->get_antecedent(var, out);
        }
    }

    void get_antecedent(int var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_antecedent(var, out);
        }
        else
        {
            linear_->get_antecedent(var, out);
        }
    }

    void get_decision_level(const Integer &var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_decision_level(var, out);
        }
        else
        {
            linear_->get_decision_level(var, out);
        }
    }

    void get_decision_level(int var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_decision_level(var, out);
        }
        else
        {
            linear_->get_decision_level(var, out);
        }
    }

    void get_order_counter(const Integer &var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_order_counter(var, out);
        }
        else
        {
            linear_->get_order_counter(var, out);
        }
    }

    void get_order_counter(int var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_order_counter(var, out);
        }
        else
        {
            linear_->get_order_counter(var, out);
        }
    }

    void get_phase(const Integer &var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_phase(var, out);
        }
        else
        {
            linear_->get_phase(var, out);
        }
    }

    void get_phase(int var, Integer &out)
    {
        if (use_oram_)
        {
            oram_->get_phase(var, out);
        }
        else
        {
            linear_->get_phase(var, out);
        }
    }

    void clean_up_after_backtrack(const Bit &flag = Bit(true, PUBLIC), int factor = 1)
    {
        if (use_oram_)
        {
            oram_->clean_up_after_backtrack(flag, factor);
        }
        else
        {
            linear_->clean_up_after_backtrack(flag, factor);
        }
    }

private:
    Integer get_word(WordProxy::Kind kind) const
    {
        switch (kind)
        {
        case WordProxy::Kind::CurDl:
            return use_oram_ ? oram_->cur_dl : linear_->cur_dl;
        case WordProxy::Kind::NotAssigned:
            return use_oram_ ? oram_->not_assigned : linear_->not_assigned;
        case WordProxy::Kind::TrueAssigned:
            return use_oram_ ? oram_->true_assigned : linear_->true_assigned;
        case WordProxy::Kind::FalseAssigned:
            return use_oram_ ? oram_->false_assigned : linear_->false_assigned;
        }
        return Integer(1, 0, PUBLIC);
    }

    void set_word(WordProxy::Kind kind, const Integer &value)
    {
        if (use_oram_)
        {
            switch (kind)
            {
            case WordProxy::Kind::CurDl:
                oram_->cur_dl = value;
                break;
            case WordProxy::Kind::NotAssigned:
                oram_->not_assigned = value;
                break;
            case WordProxy::Kind::TrueAssigned:
                oram_->true_assigned = value;
                break;
            case WordProxy::Kind::FalseAssigned:
                oram_->false_assigned = value;
                break;
            }
        }
        else
        {
            switch (kind)
            {
            case WordProxy::Kind::CurDl:
                linear_->cur_dl = value;
                break;
            case WordProxy::Kind::NotAssigned:
                linear_->not_assigned = value;
                break;
            case WordProxy::Kind::TrueAssigned:
                linear_->true_assigned = value;
                break;
            case WordProxy::Kind::FalseAssigned:
                linear_->false_assigned = value;
                break;
            }
        }
    }

    bool use_oram_;
    VariableStateOram *oram_ = nullptr;
    VariableState *linear_ = nullptr;
};

#endif // PPCDCL_VARIABLE_STATE_BACKEND_H
