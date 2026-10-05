#ifndef PPCDCL_VARIABLE_STATE_H
#define PPCDCL_VARIABLE_STATE_H

#include "unordered_set"
#include "vector"

class VariableState
{
private:
    std::vector<int> assignment_value; // to store values of the variables
    std::vector<int> antecedents;
    std::vector<int> dl; // to store decision levels of each variable
    std::vector<int> phase_saving; // to store the phase of the variables
    std::vector<int> trail;        // assignment order (literals)
    std::vector<int> order_ctr;
    int ctr = 0;

    void update_order_ctr(int var)
    {
        this->order_ctr[var] = this->ctr;
        ctr += 1;
    }

public:
    int cur_dl = 0;

    VariableState()
    {
    }

    VariableState(int variable_num)
    {
        if (variable_num > 0)
        {
            assignment_value.reserve(variable_num + 1);
            antecedents.reserve(variable_num + 1);
            dl.reserve(variable_num + 1);
            for (int i = 0; i < variable_num + 1; ++i)
            {
                assignment_value.push_back(-1);
                antecedents.push_back(0);
                dl.push_back(-1);
                phase_saving.push_back(-1);
                order_ctr.push_back(-1);
            }
            trail.reserve(variable_num + 1);

            cur_dl = 0;
        }
    }

    void init(int variable_num)
    {
        if (variable_num > 0)
        {
            assignment_value.reserve(variable_num + 1);
            antecedents.reserve(variable_num + 1);
            dl.reserve(variable_num + 1);
            order_ctr.reserve(variable_num + 1);
            for (int i = 0; i < variable_num + 1; ++i)
            {
                assignment_value.push_back(-1);
                antecedents.push_back(0);
                dl.push_back(-1);
                phase_saving.push_back(-1);
                order_ctr.push_back(-1);
            }
            trail.clear();

            trail.reserve(variable_num + 1);
            cur_dl = 0;
        }
    }

    int num_of_lit_in_cur_dl() const
    {
        int count = 0;
        for (int i = 1; i < assignment_value.size(); ++i)
        {
            if (dl[i] == cur_dl)
            {
                count += 1;
            }
        }
        return count;
    }

    bool is_literal_falsified(int literals) const
    {
        int var = abs(literals);
        int val = assignment_value[var];
        int decision_level = dl[var];
        if (if_variable_unassigned(var))
        {
            return false;
        }
        else if (val == 0 && literals < 0)
        {
            return false;
        }
        else if (val == 0 && literals > 0)
        {
            return true;
        }
        else if (val == 1 && literals < 0)
        {
            return true;
        }
        else if (val == 1 && literals > 0)
        {
            return false;
        }
        else
        {
            return false;
        }
    }

    bool is_literal_satisfied(int literals) const
    {
        int var = abs(literals);
        int val = assignment_value[var];
        int decision_level = dl[var];
        if (if_variable_unassigned(var))
        {
            return false;
        }
        else if (val == 0 && literals < 0)
        {
            return true;
        }
        else if (val == 0 && literals > 0)
        {
            return false;
        }
        else if (val == 1 && literals < 0)
        {
            return false;
        }
        else if (val == 1 && literals > 0)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    int get_assignment(int var) const
    {
        return assignment_value[var];
    }

    int get_antecedent(int var) const
    {
        return antecedents[var];
    }

    int get_decision_level(int var) const
    {
        return dl[var];
    }

    int get_order_ctr(int var) const
    {
        return order_ctr[var];
    }

    // check whether the given unit literal conflicts with the current assignment
    bool is_unit_lit_cause_conflict(int literal) const
    {
        int var = abs(literal);
        int val = assignment_value[var];
        int decision_level = dl[var];
        if (if_variable_unassigned(var))
        {
            return false;
        }
        else if (val == 0 && literal > 0)
        {
            return true;
        }
        else if (val == 1 && literal < 0)
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    void set_unit_literal(int literal, int antecedent, int dl)
    {
        int var = abs(literal);
        this->assignment_value[var] = literal > 0 ? 1 : 0;
        this->phase_saving[var] = literal > 0 ? 1 : 0;
        this->antecedents[var] = antecedent;
        this->dl[var] = dl;
        trail.push_back(literal);
        update_order_ctr(var);
    }

    void clean_up_after_backtrack(int cutoff_dl)
    {
        // Pop any assignment whose stored dl is above the cutoff.
        while (!trail.empty())
        {
            int lit = trail.back();
            int var = abs(lit);
            if (dl[var] <= cutoff_dl)
                break;

            assignment_value[var] = -1;
            antecedents[var] = 0;
            dl[var] = -1;
            order_ctr[var] = -1;
            trail.pop_back();
        }
        ctr = static_cast<int>(trail.size());
    }

    void set_decision(int literal, int dl)
    {
        int var = abs(literal);
        this->assignment_value[var] = literal > 0 ? 1 : 0;
        this->phase_saving[var] = literal > 0 ? 1 : 0;
        this->antecedents[var] = 0;
        this->dl[var] = dl;
        trail.push_back(literal);
        update_order_ctr(var);
    }

    bool if_variable_unassigned(int var) const
    {
        return (this->get_assignment(abs(var)) == -1 || get_decision_level(abs(var)) > cur_dl);
    }

    int const get_phase(int var)
    {
        return phase_saving[var];
    }

    void print_trail()
    {
        std::cout << "printing trail..." << std::endl;
        for (int lit : trail)
        {
            std::cout << lit << " ";
        }
        std::cout << std::endl;
    }

    void print_ctr_based_trail()
    {
        // print the variables ordered by decision level, then by ctr within a level
        std::vector<int> vars;
        for (int i = 1; i < order_ctr.size(); ++i)
        {
            if (order_ctr[i] != -1 && dl[i] != -1)
            {
                vars.push_back(i);
            }
        }
        std::sort(vars.begin(), vars.end(), [&](int a, int b)
                  {
                  if (dl[a] == dl[b])
                  {
                  return order_ctr[a] < order_ctr[b];
                  }
                  return dl[a] < dl[b]; });

        for (int i : vars)
        {
            std::cout << (get_assignment(i) == 1 ? i : -i) << " ";
        }
        std::cout << std::endl;
        for (int i : vars)
        {
            std::cout << (get_assignment(i) == 1 ? i : -i) << " "
                      << "decision level: " << dl[i] << " ctr: " << order_ctr[i] << std::endl;
        }
    }
    void print_single_var_state(int var)
    {
        std::cout << "variable: " << var << " assignment: " << assignment_value[var] << " antecedent: " << antecedents[var]
                  << " decision level: " << dl[var] << std::endl;
    }

    void print_all()
    {
        std::cout << "printing big V..." << std::endl;
        std::cout << "current decision level: " << cur_dl << std::endl;
        for (int i = 1; i < assignment_value.size(); ++i)
        {
            std::cout << "variable: " << i << " assignment: " << assignment_value[i] << " antecedent: "
                      << antecedents[i] << " decision level: " << dl[i] << std::endl;
        }
    }

    const std::vector<int> &get_trail() const
    {
        return trail;
    }
};
#endif // PPCDCL_VARIABLE_STATE_H
