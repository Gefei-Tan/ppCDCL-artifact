#ifndef CDCL_H
#define CDCL_H

#include <fstream>
#include <sstream>
#include <iomanip>
#include "clause.h"
#include "variable_state.h"
#include "watchlist.h"
#include "blk_watchlist.h"
#include "constant.h"
#include "src/dimacs.h"
#include "src/public_schedule.h"
#include <algorithm>
#include <cassert>
#include <random>
#include "emp-tool/emp-tool.h"
#include "map"
#include "math.h"
#include <unordered_set>

class CDCL
{
public:
    /* heuristics config flags */
    bool static const if_restart_by_step = false;     // restart after a fixed number of steps
    bool static const if_restart_by_conflict = false; // restart after a fixed number of conflicts
    bool static const if_restart_by_ruby = true;      // restart on the Luby schedule
    bool static const if_delete_clause = false;       // delete learned clauses
    bool static const if_step_decay = false;          // decay activities every decay_step steps
    bool static const if_phase_saving = true;         // reuse the saved phase when deciding
    bool static const if_artificial_decision_level = false;
    bool use_shortest_watchlist = false;                  // pick the two shortest-watchlist literals, not the first two found
    static const int restart_by_conflict_threshold = 20;  // conflicts between restarts
    int ruby_restart_unit = 256;                          // base unit of the Luby restart sequence
    static const int by_step_restart_threshold = 3;       // steps between restarts
    bool static const if_delete_old_clause = false;       // delete the oldest learned clause
    static const int delete_old_clause_threshold = 10000; // max number of learned clauses kept
    int last_restart_step_idx = 0;                        // step index at the last restart
    int num_pnc_non_zero = 0;                             // number of steps with a non-zero pnc_state
    int uip_loop_cap = 0;                                 // cap on UIP loop iterations (0: no cap)
    /* decision config flags */
    static const bool if_mVSIDS = true;
    static const bool if_eVSIDS = false;
    float decay_factor = 0.95;
    static const int decay_step = 50;

    string file_name = "";

    /* debug flags */
    static const bool if_verify = false;
    static const bool if_debug = false;
    static const bool if_detail_debug = false;
    static const bool if_lazy_conflict_debug = false;
    static const bool if_double_check_with_cadical = false;
    bool if_mute = false;

    // Cap on learned clause length (0 or negative: no limit).
    enum ClauseCapPolicy
    {
        CLAUSE_CAP_RESTART = 0,
        CLAUSE_CAP_CHRONO_BACKTRACK = 1,
        CLAUSE_CAP_NON_CHRONO_BACKTRACK = 2
    };
    int learned_clause_len_cap = -1;
    ClauseCapPolicy clause_cap_policy = CLAUSE_CAP_RESTART;

    /* CDCL data structures */
    int a_idx = -1;            // literal we are visiting in current giant step
    int w_idx = 0;             // clause index in current giant step
    int ctr = -1;              // counter for the watch list of a_idx
    int var_num = 0;           // number of variables
    int conflict_ctr = 1;      // conflict counter; used as negative index for conflict clause
    int asserting_literal = 0; // the asserting literal of the learned UIP clause
    int restart_ctr = 0;
    int previous_restart_conflict_ctr = 1;
    std::uint64_t conflict_schedule_counter = 0;
    std::uint64_t luby_restart_index = 1;
    std::uint64_t luby_interval_progress = 0;
    int pnc_state;
    bool if_sat = false;
    bool if_unsat = false;
    long long step_idx;
    float cutoff_percentage = 0.5;
    int root_level_decision_literal = 0;
    int step_state = -1;
    bool pending_decision = false;
    bool pending_conflict = false;
    bool dummy_step = false;
    int force_decision = 0;

    // Parameters for oblivious mode simulation:
    int oblivious_decision_delay = 1;
    int oblivious_conflict_delay = 1;
    int emp_max_wl_size = 0;
    int emp_max_conflict_phi_size = 50;

    long long naive_oblivious_extra_steps = 0;
    long long oblivious_extra_steps = 0;
    int last_decision_event_step = 0;
    int last_conflict_event_step = 0;
    long long conflict_avoided = 0;
    long long decision_avoided = 0;

    int max_lit_in_conflict_clause = 0; // length of the longest learned clause
    int max_lit_in_clause = 0;          // length of the longest original clause

    // Block counters for instrumentation:
    long long orange_count = 0; // Core steps (includes dummy steps for padding/delays)
    long long blue_count = 0;   // get_w_from_big_U calls
    long long green_count = 0;  // get_w_from_cur_wl calls
    long long red_count = 0;    // handle_conflict_w calls
    long long yellow_count = 0; // get_w_from_new_decision calls

    // Other data structures
    std::unordered_map<int, int> unit_clause;     // unit clause map: literal -> clause_idx
    std::vector<int> cur_wl;                      // current watch list, padded with zeros to emp_max_wl_size
    std::unordered_set<int> big_U;                // all unit literals to be propagated
    VariableState big_V;                          // variable state for assignments, antecedents, decision levels
    std::unordered_map<int, Clause *> phi;        // clauses
    std::unordered_map<int, Clause> conflict_phi; // conflict clauses
    std::unordered_map<int, WatchList> wls;       // watchlist of each literal
    std::unordered_map<int, int> activities;      // integer VSIDS scores

    /* stats */
    int total_learned_clause = 0;
    int total_conflict_clause = 0;
    int total_step = 0;
    int total_restart = 0;
    int total_conflict = 0;
    int max_wl_size = 0;
    long long mock_step_idx = 0;
    long long lucky_mock_step_idx = 0;
    long long total_wl_size_sum = 0;
    std::unordered_map<string, int> procedure_log; // step counts per pnc_state and step_state
    vector<int> wls_size;
    vector<int> conflict_clause_size;
    vector<int> uip_loop_size;
    vector<int> uip_bound_size;      // upper bound on UIP loop iterations (implications at the decision level + 1)
    vector<int> uip_bound_real_diff; // bound minus actual UIP loop iterations
    vector<int> conflict_log;
    vector<int> decision_log;

    struct ConflictAnalysisResult
    {
        int beta = 0;
        bool over_cap = false;
        bool over_uip_cap = false;
        bool over_count_cap = false;
        int learned_clause_size = 0;
    };

    CDCL(int variable_num) : big_V(variable_num)
    {
        var_num = variable_num;
        if (if_restart_by_conflict && if_restart_by_ruby)
        {
            throw std::runtime_error("Cannot use both conflict and ruby restart");
        }
    }

    ~CDCL()
    {
        for (auto i : phi)
        {
            delete i.second;
        }
    }

    void install_initial_watchers(int clause_idx,
                                  const std::vector<int> &literals)
    {
        std::vector<int> watched_literals;
        for (int literal : literals)
        {
            if (literal == 0 ||
                std::find(watched_literals.begin(), watched_literals.end(),
                          literal) != watched_literals.end())
            {
                continue;
            }

            auto current = wls.find(literal);
            if (current == wls.end())
            {
                WatchList watchlist;
                watchlist.add_clause(clause_idx);
                wls.insert({literal, watchlist});
            }
            else
            {
                current->second.add_clause(clause_idx);
            }

            watched_literals.push_back(literal);
            if (watched_literals.size() == 2)
                break;
        }
    }

    // parse cnf file and initialize all data structures
    CDCL(std::string file_str)
    {
        file_name = file_str;
        const ppcdcl::DimacsCnf cnf = ppcdcl::parse_dimacs_file(file_str);
        const int variable_num = cnf.variable_count;
        const int clause_num = cnf.declared_clause_count;
        if (variable_num <= 0)
        {
            throw std::runtime_error("variable number is 0");
        }
        this->var_num = variable_num;
        big_V.init(variable_num);
        phi.reserve(clause_num + 1);
        int clause_idx = 1;
        Clause *empty_clause = new Clause;
        phi.insert({0, empty_clause});
        // initialize watch list for each literal
        WatchList zero_wl;
        wls.insert({0, zero_wl});
        for (int i = 1; i < variable_num + 1; ++i)
        {
            WatchList wl;
            wls.insert({i, wl});
            WatchList wl_neg;
            wls.insert({-i, wl_neg});
            activities.insert({abs(i), 0});
        }
        for (const std::vector<int> &literals : cnf.clauses)
        {
            Clause *cur_clause = new Clause(literals);
            if (cur_clause->literals.size() == 1)
            {
                unit_clause.insert({literals[0], clause_idx});
            }
            if (cur_clause->literals.empty())
                if_unsat = true;
            phi.insert({clause_idx, cur_clause});
            install_initial_watchers(clause_idx, cur_clause->literals);
            clause_idx += 1;
        }
    }

    CDCL(std::string file_str, int party)
    {
        // parse cnf file
        std::ifstream file(file_str);
        // throw exception if file doesn't exist
        if (!file)
        {
            throw std::runtime_error("file doesn't exist");
        }
        std::string line;
        // get meta info
        std::vector<std::string> meta_info;
        int variable_num = 0, clause_num = 0, alice_cutoff = 0;
        // ignore comments
        while (std::getline(file, line))
        {
            auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] == 'c')
            {
                continue;
            }
            else
            {
                meta_info.clear();
                std::istringstream meta_iss(line);
                std::string meta_str;
                while (meta_iss >> meta_str)
                {
                    meta_info.push_back(meta_str);
                }
                if (meta_info.size() != 4 || meta_info[0] != "p" || meta_info[1] != "cnf")
                {
                    throw std::runtime_error("invalid or missing DIMACS 'p cnf' header");
                }
                variable_num = atoi(meta_info[2].c_str());
                clause_num = atoi(meta_info[3].c_str());
                alice_cutoff = cutoff_percentage * clause_num;
                this->var_num = variable_num;
                big_V.init(variable_num);
                phi.reserve(clause_num);
                break;
            }
        }
        if (variable_num <= 0)
        {
            throw std::runtime_error("variable number is 0");
        }
        int clause_idx = 1;
        Clause *empty_clause = new Clause;
        phi.insert({0, empty_clause});
        // initialize watch list for each literal
        WatchList zero_wl;
        wls.insert({0, zero_wl});
        for (int i = 1; i < variable_num + 1; ++i)
        {
            WatchList wl;
            wls.insert({i, wl});
            WatchList wl_neg;
            wls.insert({-i, wl_neg});
            activities.insert({abs(i), 0});
        }
        // parse each clause
        while (std::getline(file, line))
        {
            if (party == 2)
            {
                // only parse the second half of the clauses
                if (clause_idx < alice_cutoff)
                {
                    clause_idx += 1;
                    continue;
                }
            }
            else if (party == 1)
            {
                // only parse the first half
                if (clause_idx > alice_cutoff)
                {
                    break;
                }
            }
            // ignore irregular endings
            if (line.size() <= 1)
            {
                continue;
            }
            std::vector<std::string> literals;
            std::istringstream clause_iss(line);
            std::string lit_in_clause;
            while (clause_iss >> lit_in_clause)
            {
                literals.push_back(lit_in_clause);
            }
            literals.pop_back();
            Clause *cur_clause = new Clause(literals);
            // handle unit clause
            if (cur_clause->literals.size() == 1)
            {
                unit_clause.insert({atoi(literals[0].c_str()), clause_idx});
            }
            phi.insert({clause_idx, cur_clause});
            install_initial_watchers(clause_idx, cur_clause->literals);
            clause_idx += 1;
        }
    }

    CDCL(std::string file_str, std::vector<Clause> conflict_phi)
    {
        // parse cnf file
        std::ifstream file(file_str);
        // throw exception if file doesn't exist
        if (!file)
        {
            throw std::runtime_error("file doesn't exist");
        }
        std::string line;
        // get meta info
        std::vector<std::string> meta_info;
        int variable_num = 0, clause_num = 0;

        // ignore comments
        while (std::getline(file, line))
        {
            if (line[0] == 'c')
            {
                continue;
            }
            else
            {
                std::istringstream meta_iss(line);
                std::string meta_str;
                while (meta_iss >> meta_str)
                {
                    meta_info.push_back(meta_str);
                }
                variable_num = atoi(meta_info[2].c_str());
                clause_num = atoi(meta_info[3].c_str());
                this->var_num = variable_num;
                big_V.init(variable_num);
                phi.reserve(clause_num);
                break;
            }
        }
        if (variable_num <= 0)
        {
            throw std::runtime_error("variable number is 0");
        }
        int clause_idx = 1;
        Clause *empty_clause = new Clause;
        phi.insert({0, empty_clause});
        // initialize watch list for each literal
        WatchList zero_wl;
        wls.insert({0, zero_wl});
        for (int i = 1; i < variable_num + 1; ++i)
        {
            WatchList wl;
            wls.insert({i, wl});
            WatchList wl_neg;
            wls.insert({-i, wl_neg});
            activities.insert({abs(i), 0});
        }
        // parse each clause
        while (std::getline(file, line))
        {
            // ignore irregular endings
            if (line.size() <= 1)
            {
                continue;
            }
            std::vector<std::string> literals;
            std::istringstream clause_iss(line);
            std::string lit_in_clause;
            while (clause_iss >> lit_in_clause)
            {
                literals.push_back(lit_in_clause);
            }
            literals.pop_back();
            Clause *cur_clause = new Clause(literals);
            // handle unit clause
            if (cur_clause->literals.size() == 1)
            {
                unit_clause.insert({atoi(literals[0].c_str()), clause_idx});
            }
            phi.insert({clause_idx, cur_clause});
            install_initial_watchers(clause_idx, cur_clause->literals);
            clause_idx += 1;
        }
        for (auto c : conflict_phi)
        {
            this->conflict_phi.insert({-conflict_ctr, c});
            conflict_ctr += 1;
            construct_wl(0 - (conflict_ctr - 1));
        }
    }

    inline int epoch_from_step(long long step) const
    {
        // epoch 0 reserved for unit/root assignments
        if (!if_artificial_decision_level)
            return big_V.cur_dl;
        if (oblivious_decision_delay <= 0)
            return big_V.cur_dl;
        // step=1..dec_dly -> epoch 1, step=dec_dly+1..2*dec_dly -> epoch 2, ...
        return static_cast<int>((step - 1) / oblivious_decision_delay) + 1;
    }

    inline bool is_public_decision_slot(long long one_based_step) const
    {
        return one_based_step > 0 && ppcdcl::policy::is_modulo_one_slot(
                                         static_cast<std::uint64_t>(one_based_step),
                                         static_cast<std::uint64_t>(std::max(1, oblivious_decision_delay)));
    }

    inline bool is_public_conflict_slot(long long one_based_step) const
    {
        return one_based_step > 0 && ppcdcl::policy::is_modulo_one_slot(
                                         static_cast<std::uint64_t>(one_based_step),
                                         static_cast<std::uint64_t>(std::max(1, oblivious_conflict_delay)));
    }

    void finish_public_conflict_slot(bool conflict_slot)
    {
        if (!conflict_slot)
            return;
        conflict_schedule_counter += 1;
        luby_interval_progress += 1;
        const std::uint64_t interval = ppcdcl::policy::luby_interval(
            luby_restart_index, static_cast<std::uint64_t>(ruby_restart_unit));
        if (luby_interval_progress >= interval)
        {
            luby_interval_progress = 0;
            luby_restart_index += 1;
            if (!if_sat && !if_unsat)
                restart();
        }
    }

    struct PublicConflictSlotFinalizer
    {
        CDCL *solver;
        bool conflict_slot;
        PublicConflictSlotFinalizer(CDCL *solver, bool conflict_slot)
            : solver(solver), conflict_slot(conflict_slot) {}
        ~PublicConflictSlotFinalizer()
        {
            solver->finish_public_conflict_slot(conflict_slot);
        }
    };

    bool if_all_assigned()
    {
        for (int i = 1; i < var_num + 1; ++i)
        {
            if (big_V.if_variable_unassigned(i))
            {
                return false;
            }
        }
        return true;
    }

    void handle_unit_clause()
    {
        // Detect contradictory unit clauses before making any assignments.
        check_unit_clause_conflict();
        if (if_unsat)
        {
            return;
        }
        for (auto i : unit_clause)
        {
            if (if_debug)
                std::cout << "unit clause: ";
            add_implication(i.first, i.second, 0);
        }
    }

    void push_unit_clause_to_big_U()
    {
        // if a literal has decision level 0 and is assigned, push it into big_U
        for (int i = 1; i < var_num + 1; ++i)
        {
            if (big_V.get_decision_level(i) == 0 && big_V.get_assignment(i) != -1)
            {
                int sign = big_V.get_assignment(i) == 1 ? 1 : -1;
                big_U.insert(sign * i);
            }
        }
    }

    void check_unit_clause_conflict()
    {
        // the formula is UNSAT if both i and -i are unit clauses
        for (auto i : unit_clause)
        {
            if (unit_clause.find(-i.first) != unit_clause.end())
            {
                if_unsat = true;
                if (if_debug)
                    std::cout << "Conflict detected in unit clauses: " << i.first << " and " << -i.first << "\n";
                return;
            }
        }
    }

    inline static void print_histogram(const vector<int> &data, const std::string &name)
    {
        if (!data.empty())
        {
            // Sort a copy for percentile calculation
            const size_t n = data.size();
            std::vector<int> sorted_sizes(data.begin(), data.end());
            std::sort(sorted_sizes.begin(), sorted_sizes.end());

            // Basic statistics
            int min_size = sorted_sizes.front();
            int max_size = sorted_sizes.back();
            double avg_size = 0;
            for (int size : data)
            {
                avg_size += size;
            }
            avg_size /= static_cast<double>(n);

            // Percentiles
            auto percentile = [&](double fraction)
            {
                size_t idx = static_cast<size_t>(n * fraction);
                if (idx >= n)
                    idx = n - 1;
                return sorted_sizes[idx];
            };
            int p10 = percentile(0.10);
            int p25 = percentile(0.25);
            int p50 = percentile(0.50); // median
            int p75 = percentile(0.75);
            int p90 = percentile(0.90);

            std::cout << "   Total: " << data.size() << std::endl;
            std::cout << "   Min size: " << min_size << std::endl;
            std::cout << "   Max size: " << max_size << std::endl;
            std::cout << "   Avg size: " << avg_size << std::endl;
            std::cout << "   Percentiles:" << std::endl;
            std::cout << "     10th: " << p10 << std::endl;
            std::cout << "     25th: " << p25 << std::endl;
            std::cout << "     50th (median): " << p50 << std::endl;
            std::cout << "     75th: " << p75 << std::endl;
            std::cout << "     90th: " << p90 << std::endl;

            // Simple histogram (10 buckets)
            std::cout << "   Size distribution:" << std::endl;
            int bucket_size = (max_size - min_size + 10) / 10; // ensure at least size 1
            bucket_size = std::max(1, bucket_size);
            std::vector<size_t> histogram(10, 0);

            for (int size : data)
            {
                int bucket = std::min(9, (size - min_size) / bucket_size);
                histogram[bucket]++;
            }

            for (int i = 0; i < 10; i++)
            {
                int lower = min_size + i * bucket_size;
                int upper = min_size + (i + 1) * bucket_size - 1;
                if (i == 9)
                    upper = max_size; // last bucket includes max

                // Calculate percentage and draw a simple bar
                double percent = 100.0 * static_cast<double>(histogram[i]) / static_cast<double>(n);
                int bar_width = static_cast<int>(percent / 2.0);
                bar_width = std::max(0, std::min(bar_width, 50));

                std::cout << "     " << std::setw(4) << lower << "-" << std::setw(4) << upper
                          << ": " << std::setw(5) << std::fixed << std::setprecision(1) << percent
                          << "% |" << std::string(bar_width, '#') << std::endl;
            }
        }
    }

    void handle_conflict_w(std::vector<int> non_falsified_literals)
    {
        // conflict handling plus simulated delay (red block)
        int conflict_delay = oblivious_conflict_delay - (step_idx - last_conflict_event_step);
        if (conflict_delay > 0)
        {
            oblivious_extra_steps += conflict_delay;
        }
        last_conflict_event_step = step_idx;

        ConflictAnalysisResult analysis = conflict_analysis(w_idx);
        if (if_unsat)
        {
            return;
        }

        if (analysis.over_cap)
        {
            if (clause_cap_policy == CLAUSE_CAP_RESTART)
            {
                restart();
            }
            else if (clause_cap_policy == CLAUSE_CAP_CHRONO_BACKTRACK)
            {
                int target_level = std::max(0, big_V.cur_dl - 1);
                backtrack(target_level);
            }
            else if (clause_cap_policy == CLAUSE_CAP_NON_CHRONO_BACKTRACK)
            {
                backtrack(analysis.beta);
            }
            return;
        }
        if (analysis.over_uip_cap || analysis.over_count_cap)
        {
            restart();
            return;
        }
        backtrack(analysis.beta);
        backtrack_aftermath();
    }

    void handle_unit_w(std::vector<int> non_falsified_literals)
    {
        int unit_lit = non_falsified_literals[0];
        add_implication(unit_lit, w_idx, big_V.cur_dl);
        ctr -= 1;
    }

    void handle_not_unit_w(std::vector<int> non_falsified_literals)
    {
        update_wl(w_idx, -a_idx, non_falsified_literals);
        ctr -= 1;
    }

    void print_stats_for_log(const vector<int> &event_log, const string &event_name, int his_size = 10)
    {
        if (event_log.size() <= 1)
        {
            std::cout << "No " << event_name << " events recorded." << std::endl;
            return;
        }

        // Calculate intervals between events
        std::vector<int> intervals;
        for (size_t i = 1; i < event_log.size(); i++)
        {
            intervals.push_back(event_log[i] - event_log[i - 1]);
        }

        // Basic statistics
        std::sort(intervals.begin(), intervals.end());
        int total_events = event_log.size();
        int min_interval = intervals.front();
        int max_interval = intervals.back();
        double avg_interval = 0;
        for (int interval : intervals)
        {
            avg_interval += interval;
        }
        avg_interval /= intervals.size();

        // Percentiles
        int p10 = intervals[intervals.size() * 0.10];
        int p25 = intervals[intervals.size() * 0.25];
        int p50 = intervals[intervals.size() * 0.50]; // median
        int p75 = intervals[intervals.size() * 0.75];
        int p90 = intervals[intervals.size() * 0.90];

        std::cout << "==============" << event_name << " EVENT STATS==============" << std::endl;
        std::cout << "   Total events: " << total_events << std::endl;
        std::cout << "   First event at step: " << event_log.front() << std::endl;
        std::cout << "   Last event at step: " << event_log.back() << std::endl;
        std::cout << "   Frequency: " << (double)total_events / step_idx * 100.0 << "% of steps" << std::endl;

        if (intervals.size() > 0)
        {
            std::cout << "   Intervals between events:" << std::endl;
            std::cout << "     Min: " << min_interval << " steps" << std::endl;
            std::cout << "     Max: " << max_interval << " steps" << std::endl;
            std::cout << "     Avg: " << avg_interval << " steps" << std::endl;
            std::cout << "     Median: " << p50 << " steps" << std::endl;
            std::cout << "     10th percentile: " << p10 << " steps" << std::endl;
            std::cout << "     25th percentile: " << p25 << " steps" << std::endl;
            std::cout << "     75th percentile: " << p75 << " steps" << std::endl;
            std::cout << "     90th percentile: " << p90 << " steps" << std::endl;

            // Histogram of intervals
            int bucket_size = (max_interval - min_interval + 10) / 10;
            bucket_size = std::max(1, bucket_size);
            std::vector<size_t> histogram(10, 0);

            for (int interval : intervals)
            {
                int bucket = std::min(9, (interval - min_interval) / bucket_size);
                histogram[bucket]++;
            }

            std::cout << "   Interval distribution:" << std::endl;
            for (int i = 0; i < 10; i++)
            {
                int lower = min_interval + i * bucket_size;
                int upper = min_interval + (i + 1) * bucket_size - 1;
                if (i == 9)
                    upper = max_interval;

                double percent = 100.0 * static_cast<double>(histogram[i]) / intervals.size();
                int bar_width = static_cast<int>(percent / 2.0);
                bar_width = std::max(0, std::min(bar_width, 50));

                std::cout << "     " << std::setw(5) << lower << "-" << std::setw(5) << upper
                          << " steps: " << std::setw(5) << histogram[i] << " ("
                          << std::setw(5) << std::fixed << std::setprecision(1) << percent
                          << "%) |" << std::string(bar_width, '#') << std::endl;
            }
        }

        // Distribution of events across total execution time
        std::cout << "   Event distribution across solver execution:" << std::endl;
        int execution_bucket_size = step_idx / 10;
        if (execution_bucket_size < 1)
            execution_bucket_size = 1;
        std::vector<size_t> execution_histogram(10, 0);

        for (int event_step : event_log)
        {
            int bucket = std::min(9, event_step / execution_bucket_size);
            execution_histogram[bucket]++;
        }

        for (int i = 0; i < 10; i++)
        {
            int lower = i * execution_bucket_size;
            int upper = (i + 1) * execution_bucket_size - 1;
            if (i == 9)
                upper = step_idx;

            double percent = 100.0 * static_cast<double>(execution_histogram[i]) / event_log.size();
            int bar_width = static_cast<int>(percent / 2.0);
            bar_width = std::max(0, std::min(bar_width, 50));

            std::cout << "     Step " << std::setw(8) << lower << "-" << std::setw(8) << upper
                      << ": " << std::setw(5) << execution_histogram[i] << " events ("
                      << std::setw(5) << std::fixed << std::setprecision(1) << percent
                      << "%) |" << std::string(bar_width, '#') << std::endl;
        }
    }

    void finalize_stats()
    {
        max_lit_in_conflict_clause = 0;
        max_lit_in_clause = 0;
        for (auto i : conflict_phi)
        {
            if (i.second.literals.size() > max_lit_in_conflict_clause)
            {
                max_lit_in_conflict_clause = i.second.literals.size();
            }
        }
        for (auto i : phi)
        {
            if (i.second->literals.size() > max_lit_in_clause)
            {
                max_lit_in_clause = i.second->literals.size();
            }
        }
        if (learned_clause_len_cap > 0)
        {
            int capped = std::max(learned_clause_len_cap, max_lit_in_clause);
            if (max_lit_in_conflict_clause < capped)
            {
                max_lit_in_conflict_clause = capped;
            }
        }
        total_learned_clause = conflict_phi.size();
    }

    static const bool cadical(string file_name, string cadical_path = "./cadical")
    {
        if (!file_name.empty())
        {
            string cmd = cadical_path + " " + file_name + " > /dev/null 2>&1";
            int rc = std::system(cmd.c_str());
            if (rc != -1)
            {
                int code = (rc >> 8) & 0xff;
                bool cadical_sat = (code == 10);
                bool cadical_unsat = (code == 20);
                if (cadical_sat)
                {
                    return true;
                }
                else if (cadical_unsat)
                {
                    return false;
                }
                else
                {
                    throw std::runtime_error("Cadical returned unknown result code");
                }
            }
            else
            {
                throw std::runtime_error("Cadical execution failed");
            }
        }
        else
        {
            throw std::runtime_error("Cadical file name is empty");
        }
    }

    void terminate(bool if_sat)
    {
        finalize_stats();

        if (if_mute)
        {
            return;
        }

        if (if_sat)
        {
            std::cout << "SAT!!!" << std::endl;
            if (if_verify)
            {
                if (verifiy_sat())
                {
                    std::cout << "SAT VERIFIED!!!" << std::endl;
                }
                else
                {
                    throw std::runtime_error("SAT NOT VERIFIED!!!");
                }
            }
        }
        else
        {
            std::cout << "UNSAT!!!" << std::endl;
        }

        if (emp_max_wl_size < max_wl_size)
        {
            std::cout << "WARNING!! max_wl_size " << max_wl_size << " exceeds the fixed size " << emp_max_wl_size
                      << std::endl;
        }
        std::cout << "=============WL STATS===============" << std::endl;
        print_histogram(wls_size, "Watch List Size");
        std::cout << "=============CONFLICT CLAUSE STATS===============" << std::endl;
        print_histogram(conflict_clause_size, "Watch List Size");
        std::cout << "=============UIP STATS===============" << std::endl;
        print_histogram(uip_loop_size, "Watch List Size");

        std::cout << "=============UIP BOUND STATS===============" << std::endl;
        print_histogram(uip_bound_size, "UIP Bound Size");

        std::cout << "=============UIP BOUND VS REAL STATS===============" << std::endl;
        print_histogram(uip_bound_real_diff, "UIP Bound vs Real Difference");

        std::cout << "=============EVENT ANALYSIS===============" << std::endl;
        print_stats_for_log(decision_log, "DECISION");
        print_stats_for_log(conflict_log, "CONFLICT");

        std::cout << "=============APPLIED EMP SETTING:===============" << std::endl;
        std::cout << "   MAX_WL_SIZE: " << emp_max_wl_size << std::endl;
        std::cout << "   DECISION_DLY " << oblivious_decision_delay << std::endl;
        std::cout << "   CONFLICT_DLY " << oblivious_conflict_delay << std::endl;
        std::cout << "==============STATS===============" << std::endl;
        std::cout << "   #var " << var_num << std::endl;
        std::cout << "   #clause " << phi.size() << std::endl;
        std::cout << "   max_wl_size " << max_wl_size << std::endl;
        std::cout << "   #learned_clause " << total_learned_clause << std::endl;
        std::cout << "   #restart: " << restart_ctr << std::endl;
        std::cout << "   #conflict: " << total_conflict << std::endl;
        std::cout << "   #decision: " << decision_log.size() << std::endl;

        std::cout << "=============BLOCKs===============" << std::endl;
        std::cout << "   Orange blocks (core): " << orange_count << std::endl;
        std::cout << "   Blue blocks (get_w_from_big_U): " << blue_count << std::endl;
        std::cout << "   Green blocks (get_w_from_cur_wl): " << green_count << std::endl;
        std::cout << "   Red blocks (handle_conflict_w): " << red_count << std::endl;
        std::cout << "   Yellow blocks (get_w_from_new_decision): " << yellow_count << std::endl;
    }

    int begin_giant_step(long long step_num = 0)
    {
        if (learned_clause_len_cap == 0)
        {
            // a cap of 0 is replaced by var_num, which never rejects a clause
            learned_clause_len_cap = var_num;
            uip_loop_cap = 0; // no cap
            std::cout << "use varnum!!" << std::endl;
        }
        step_idx = 0;
        init_log();
        int prev_conflict_step_idx = 0;
        int total_conflict_step = 0;
        handle_unit_clause();
        if (if_unsat)
        {
            terminate(false);
            return 0;
        }
        // Main giant step loop
        for (step_idx = 1; step_idx < step_num; ++step_idx)
        {
            const bool conflict_slot = is_public_conflict_slot(step_idx);
            const bool decision_slot = is_public_decision_slot(step_idx);
            PublicConflictSlotFinalizer conflict_slot_finalizer(this, conflict_slot);
            if (if_artificial_decision_level)
            {
                big_V.cur_dl = epoch_from_step(step_idx);
            }
            orange_count++; // count each giant step, including dummy steps
            if (conflict_slot)
            {
                red_count++; // count conflict handling steps
            }
            if (decision_slot)
            {
                yellow_count++; // count decision making steps
            }
            dummy_step = false;
            if (if_detail_debug)
                std::cout << "\n### step_idx: " << step_idx << std::endl;
            // Freeze when waiting for a scheduled decision or conflict slot.
            if (pending_conflict && !conflict_slot)
            {
                dummy_step = true;
            }
            if (pending_decision && !decision_slot)
            {
                dummy_step = true;
            }
            if (dummy_step)
            {
                continue;
            }
            pick_new_clause();
            if (dummy_step)
            {
                continue;
            }
            if (if_sat)
            {
                break;
            }
            if (if_detail_debug)
                std::cout << "### search two non falsified" << std::endl;
            std::vector<int> non_falsified_literals;
            if (use_shortest_watchlist)
            {
                non_falsified_literals = search_two_shortest_non_falsified(w_idx, -a_idx);
            }
            else
            {
                non_falsified_literals = search_two_non_falsified(w_idx, -a_idx);
            }
            // Determine step state: 3 = no watch list; 0 = conflict; 1 = unit; 2 = non-unit clause.
            step_state = (w_idx == 0) ? 3 : ((non_falsified_literals[0] == 0) ? 0 : ((non_falsified_literals[0] == non_falsified_literals[1]) ? 1 : 2));
            // log each conflict once, when it is first detected
            if (step_state == 0 && !pending_conflict)
            {
                conflict_log.push_back(step_idx);
            }
            if (if_detail_debug)
                std::cout << "### step_state: " << step_state << std::endl;
            // step_state 3: cur_wl is empty, so the picked literal has no effect
            log_current_step();
            if (step_state == 0)
            {
                pending_conflict = true;
                if (if_detail_debug)
                    std::cout << "### conflict!" << w_idx << std::endl;
                if (conflict_slot)
                {
                    total_conflict += 1;
                    handle_conflict_w(non_falsified_literals);
                    pending_conflict = false;
                    if (if_unsat)
                    {
                        break;
                    }
                    continue;
                }
                else
                {
                    dummy_step = true;
                    continue;
                }
            }
            if (step_state == 3)
            {
                ctr -= 1;
                continue;
            }
            else if (step_state == 1)
            {
                if (if_detail_debug)
                    std::cout << "### unit clause! w_idx: " << w_idx << std::endl;
                handle_unit_w(non_falsified_literals);
            }
            else if (step_state == 2)
            {
                if (if_detail_debug)
                    std::cout << "### not unit clause! w_idx: " << w_idx << std::endl;
                handle_not_unit_w(non_falsified_literals);
            }
            if (if_unsat)
            {
                break;
            }
        }

        lucky_mock_step_idx = step_idx - total_wl_size_sum + num_pnc_non_zero * max_wl_size;
        // statistics on the extra steps incurred by oblivious execution
        conflict_avoided = (step_idx + naive_oblivious_extra_steps) -
                           (step_idx + oblivious_extra_steps) / oblivious_conflict_delay;
        decision_avoided = (step_idx + naive_oblivious_extra_steps) -
                           (step_idx + oblivious_extra_steps) / oblivious_decision_delay;
        if (if_unsat)
        {
            terminate(false);
            return 0;
        }
        else if (if_sat)
        {
            terminate(true);
            return 1;
        }
        else
        {
            if (!if_mute)
            {
                std::cout << "avg step per conflict " << total_conflict_step / total_conflict << std::endl;
                std::cout << "total steps " << step_idx << std::endl;
                std::cout << "restart times: " << restart_ctr << std::endl;
                std::cout << "TIMEOUT!" << std::endl;
            }
            finalize_stats();
            return -1;
        }
    }

    // pick the unassigned variable with the highest activity; the sign comes from the saved phase
    int pick_variable_VSIDS()
    {
        std::vector<int> unassigned;
        for (int i = 1; i < var_num + 1; ++i)
        {
            if (big_V.if_variable_unassigned(i))
            {
                unassigned.push_back(i);
            }
        }
        // pick the variable with the highest activity score in all unassigned variables
        int var_with_max_activity = 0;
        int max_activity = -1;
        for (auto i : unassigned)
        {
            if (activities.find(i)->second > max_activity)
            {
                max_activity = activities.find(abs(i))->second;
                var_with_max_activity = i;
            }
        }
        if (if_debug && big_V.cur_dl == 1)
        {
            std::cout << "making a decision at root...\n";
            std::cout << "tried_both_way_at_root: ";
            std::cout << std::endl;
            std::cout << "root_level_decision_literal: " << root_level_decision_literal << std::endl;
            std::cout << "var_with_max_activity: " << var_with_max_activity << std::endl;
        }

        // Use the saved phase; a variable that was never assigned defaults to
        // positive, as in the secure solver.
        int sign = -1;
        if (if_phase_saving)
            sign = big_V.get_phase(var_with_max_activity);
        if (sign == -1)
        {
            return var_with_max_activity;
        }
        if (sign == 0)
        {
            return -var_with_max_activity;
        }
        else
        {
            return var_with_max_activity;
        }
    }

    std::vector<int> get_unit_literals_dl0() const
    {
        std::unordered_set<int> out;
        for (const auto &kv : unit_clause)
        {
            out.insert(kv.first);
        }
        for (int v = 1; v <= var_num; ++v)
        {
            int assign = big_V.get_assignment(v);
            int dl = big_V.get_decision_level(v);
            if (assign != -1 && dl == 0)
            {
                out.insert(assign == 1 ? v : -v);
            }
        }
        return std::vector<int>(out.begin(), out.end());
    }

    int compute_lbd(const Clause &c) const
    {
        std::unordered_set<int> levels;
        levels.reserve(c.literals.size());
        for (int lit : c.literals)
        {
            int dl = big_V.get_decision_level(abs(lit));
            if (dl >= 0)
            {
                levels.insert(dl);
            }
        }
        return static_cast<int>(levels.size());
    }

    std::vector<std::vector<int>> get_learned_clauses() const
    {
        std::vector<std::vector<int>> out;
        out.reserve(conflict_phi.size());
        for (const auto &kv : conflict_phi)
        {
            out.push_back(kv.second.literals);
        }
        return out;
    }

    std::vector<Clause> get_learned_clause_entries() const
    {
        std::vector<Clause> out;
        out.reserve(conflict_phi.size());
        for (const auto &kv : conflict_phi)
        {
            out.push_back(kv.second);
        }
        return out;
    }

    struct CdclStats
    {
        int var_num;
        int clause_count;
        int learned_clause_count;
        int max_wl_size;
        int max_lit_in_clause;
        int max_lit_in_conflict_clause;
        int max_uip_loop;
        long long orange_blocks;
        long long blue_blocks;
        long long green_blocks;
        long long red_blocks;
        long long yellow_blocks;
        long long step_count;
        int conflict_count;
        int decision_count;
        int restart_count;
    };

    CdclStats export_stats() const
    {
        CdclStats s{0};
        s.var_num = var_num;
        s.clause_count = static_cast<int>(phi.size());
        s.learned_clause_count = static_cast<int>(conflict_phi.size());
        s.max_wl_size = max_wl_size;
        s.max_lit_in_clause = max_lit_in_clause;
        s.max_lit_in_conflict_clause = max_lit_in_conflict_clause;
        int max_uip = 0;
        for (int loops : uip_loop_size)
        {
            if (loops > max_uip)
            {
                max_uip = loops;
            }
        }
        s.max_uip_loop = max_uip;
        s.orange_blocks = orange_count;
        s.blue_blocks = blue_count;
        s.green_blocks = green_count;
        s.red_blocks = red_count;
        s.yellow_blocks = yellow_count;
        s.step_count = step_idx;
        s.conflict_count = total_conflict;
        s.decision_count = static_cast<int>(decision_log.size());
        s.restart_count = restart_ctr;
        return s;
    }

    void serialize(const std::string &path, bool include_conflict_clauses = true, bool include_watchlists = true, bool include_activities = true)
    {
        std::ofstream out(path);
        if (!out)
        {
            throw std::runtime_error("Failed to open snapshot file for write: " + path);
        }
        out << std::setprecision(17);
        out << "# CDCL_SNAPSHOT v1\n";
        out << "[meta]\n";
        out << "var_num " << var_num << "\n";
        out << "file_name " << file_name << "\n";
        out << "end\n\n";

        out << "[core]\n";
        out << "a_idx " << a_idx << "\n";
        out << "w_idx " << w_idx << "\n";
        out << "ctr " << ctr << "\n";
        out << "conflict_ctr " << conflict_ctr << "\n";
        out << "asserting_literal " << asserting_literal << "\n";
        out << "restart_ctr " << restart_ctr << "\n";
        out << "previous_restart_conflict_ctr " << previous_restart_conflict_ctr << "\n";
        out << "last_restart_step_idx " << last_restart_step_idx << "\n";
        out << "num_pnc_non_zero " << num_pnc_non_zero << "\n";
        out << "uip_loop_cap " << uip_loop_cap << "\n";
        out << "pnc_state " << pnc_state << "\n";
        out << "if_sat " << (if_sat ? 1 : 0) << "\n";
        out << "if_unsat " << (if_unsat ? 1 : 0) << "\n";
        out << "if_mute " << (if_mute ? 1 : 0) << "\n";
        out << "step_idx " << step_idx << "\n";
        out << "cutoff_percentage " << cutoff_percentage << "\n";
        out << "root_level_decision_literal " << root_level_decision_literal << "\n";
        out << "step_state " << step_state << "\n";
        out << "pending_decision " << (pending_decision ? 1 : 0) << "\n";
        out << "pending_conflict " << (pending_conflict ? 1 : 0) << "\n";
        out << "dummy_step " << (dummy_step ? 1 : 0) << "\n";
        out << "force_decision " << force_decision << "\n";
        out << "decay_factor " << decay_factor << "\n";
        out << "learned_clause_len_cap " << learned_clause_len_cap << "\n";
        out << "clause_cap_policy " << static_cast<int>(clause_cap_policy) << "\n";
        out << "oblivious_decision_delay " << oblivious_decision_delay << "\n";
        out << "oblivious_conflict_delay " << oblivious_conflict_delay << "\n";
        out << "emp_max_wl_size " << emp_max_wl_size << "\n";
        out << "emp_max_conflict_phi_size " << emp_max_conflict_phi_size << "\n";
        out << "naive_oblivious_extra_steps " << naive_oblivious_extra_steps << "\n";
        out << "oblivious_extra_steps " << oblivious_extra_steps << "\n";
        out << "last_decision_event_step " << last_decision_event_step << "\n";
        out << "last_conflict_event_step " << last_conflict_event_step << "\n";
        out << "conflict_avoided " << conflict_avoided << "\n";
        out << "decision_avoided " << decision_avoided << "\n";
        out << "max_lit_in_conflict_clause " << max_lit_in_conflict_clause << "\n";
        out << "max_lit_in_clause " << max_lit_in_clause << "\n";
        out << "orange_count " << orange_count << "\n";
        out << "blue_count " << blue_count << "\n";
        out << "green_count " << green_count << "\n";
        out << "red_count " << red_count << "\n";
        out << "yellow_count " << yellow_count << "\n";
        out << "total_learned_clause " << total_learned_clause << "\n";
        out << "total_conflict_clause " << total_conflict_clause << "\n";
        out << "total_step " << total_step << "\n";
        out << "total_restart " << total_restart << "\n";
        out << "total_conflict " << total_conflict << "\n";
        out << "max_wl_size " << max_wl_size << "\n";
        out << "mock_step_idx " << mock_step_idx << "\n";
        out << "lucky_mock_step_idx " << lucky_mock_step_idx << "\n";
        out << "total_wl_size_sum " << total_wl_size_sum << "\n";
        out << "end\n\n";

        const int kListWrap = 16;
        auto write_list = [&](const std::string &label, const std::vector<int> &vals)
        {
            size_t idx = 0;
            while (idx < vals.size())
            {
                out << label;
                for (int col = 0; col < kListWrap && idx < vals.size(); ++col, ++idx)
                {
                    out << " " << vals[idx];
                }
                out << "\n";
            }
        };

        out << "[big_v]\n";
        out << "cur_dl " << big_V.cur_dl << "\n";
        out << "vars " << var_num << "\n";
        for (int i = 1; i < var_num + 1; ++i)
        {
            out << "var " << i << " " << big_V.get_assignment(i) << " " << big_V.get_antecedent(i) << " "
                << big_V.get_decision_level(i) << " " << big_V.get_phase(i) << "\n";
        }
        const auto &trail = big_V.get_trail();
        out << "trail_count " << static_cast<int>(trail.size()) << "\n";
        write_list("trail", trail);
        out << "end\n\n";

        out << "[unit_clause]\n";
        {
            std::vector<int> keys;
            keys.reserve(unit_clause.size());
            for (const auto &kv : unit_clause)
            {
                keys.push_back(kv.first);
            }
            std::sort(keys.begin(), keys.end());
            out << "count " << static_cast<int>(keys.size()) << "\n";
            for (int key : keys)
            {
                out << key << " " << unit_clause.find(key)->second << "\n";
            }
        }
        out << "end\n\n";

        out << "[big_u]\n";
        {
            std::vector<int> lits(big_U.begin(), big_U.end());
            std::sort(lits.begin(), lits.end());
            out << "count " << static_cast<int>(lits.size()) << "\n";
            write_list("lits", lits);
        }
        out << "end\n\n";

        out << "[cur_wl]\n";
        {
            out << "count " << static_cast<int>(cur_wl.size()) << "\n";
            write_list("entries", cur_wl);
        }
        out << "end\n\n";

        if (include_activities)
        {
            out << "[activities]\n";
            out << "count " << var_num << "\n";
            for (int i = 1; i < var_num + 1; ++i)
            {
                auto it = activities.find(i);
                int value = (it == activities.end()) ? 0 : it->second;
                out << i << " " << value << "\n";
            }
            out << "end\n\n";
        }

        out << "[clauses]\n";
        {
            std::vector<int> keys;
            keys.reserve(phi.size());
            for (const auto &kv : phi)
            {
                keys.push_back(kv.first);
            }
            std::sort(keys.begin(), keys.end());
            out << "count " << static_cast<int>(keys.size()) << "\n";
            for (int key : keys)
            {
                const Clause *c = phi.find(key)->second;
                out << key << " " << static_cast<int>(c->literals.size());
                for (int lit : c->literals)
                {
                    out << " " << lit;
                }
                out << "\n";
            }
        }
        out << "end\n\n";

        if (include_conflict_clauses)
        {
            out << "[conflict_clauses]\n";
            std::vector<int> keys;
            keys.reserve(conflict_phi.size());
            for (const auto &kv : conflict_phi)
            {
                keys.push_back(kv.first);
            }
            std::sort(keys.begin(), keys.end());
            out << "count " << static_cast<int>(keys.size()) << "\n";
            for (int key : keys)
            {
                const Clause &c = conflict_phi.find(key)->second;
                out << key << " " << static_cast<int>(c.literals.size());
                for (int lit : c.literals)
                {
                    out << " " << lit;
                }
                out << "\n";
            }
            out << "end\n\n";
        }

        if (include_watchlists)
        {
            out << "[watchlists]\n";
            std::vector<int> keys;
            keys.reserve(wls.size());
            for (const auto &kv : wls)
            {
                if (!kv.second.empty() || kv.first == 0)
                {
                    keys.push_back(kv.first);
                }
            }
            std::sort(keys.begin(), keys.end());
            out << "count " << static_cast<int>(keys.size()) << "\n";
            for (int key : keys)
            {
                const auto &wl = wls.find(key)->second.wl;
                std::vector<int> clause_ids(wl.begin(), wl.end());
                out << key << " " << static_cast<int>(clause_ids.size());
                for (int cid : clause_ids)
                {
                    out << " " << cid;
                }
                out << "\n";
            }
            out << "end\n\n";
        }
    }

    void deserialize(const std::string &path, bool include_conflict_clauses = true, bool include_watchlists = true, bool include_activities = true)
    {
        std::ifstream in(path);
        if (!in)
        {
            throw std::runtime_error("Failed to open snapshot file for read: " + path);
        }

        auto trim = [](const std::string &s) -> std::string
        {
            size_t start = s.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
            {
                return "";
            }
            size_t end = s.find_last_not_of(" \t\r\n");
            return s.substr(start, end - start + 1);
        };

        std::unordered_map<std::string, std::vector<std::string>> sections;
        std::string section;
        std::string line;
        while (std::getline(in, line))
        {
            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#')
            {
                continue;
            }
            if (trimmed.front() == '[' && trimmed.back() == ']')
            {
                section = trimmed.substr(1, trimmed.size() - 2);
                continue;
            }
            if (trimmed == "end")
            {
                section.clear();
                continue;
            }
            if (!section.empty())
            {
                sections[section].push_back(trimmed);
            }
        }

        auto meta_it = sections.find("meta");
        if (meta_it == sections.end())
        {
            throw std::runtime_error("Snapshot missing [meta] section");
        }

        int parsed_var_num = 0;
        std::string parsed_file_name;
        for (const auto &ln : meta_it->second)
        {
            std::istringstream iss(ln);
            std::string key;
            if (!(iss >> key))
            {
                continue;
            }
            if (key == "var_num")
            {
                iss >> parsed_var_num;
            }
            else if (key == "file_name")
            {
                std::string rest;
                std::getline(iss, rest);
                parsed_file_name = trim(rest);
            }
        }
        if (parsed_var_num <= 0)
        {
            throw std::runtime_error("Snapshot has invalid var_num");
        }

        auto reset_state = [&](int new_var_num)
        {
            for (auto &kv : phi)
            {
                delete kv.second;
            }
            phi.clear();
            conflict_phi.clear();
            unit_clause.clear();
            cur_wl.clear();
            big_U.clear();
            wls.clear();
            activities.clear();
            procedure_log.clear();
            wls_size.clear();
            conflict_clause_size.clear();
            uip_loop_size.clear();
            uip_bound_size.clear();
            uip_bound_real_diff.clear();
            conflict_log.clear();
            decision_log.clear();

            var_num = new_var_num;
            big_V = VariableState(new_var_num);
            file_name.clear();

            a_idx = -1;
            w_idx = 0;
            ctr = -1;
            conflict_ctr = 1;
            asserting_literal = 0;
            restart_ctr = 0;
            previous_restart_conflict_ctr = 1;
            last_restart_step_idx = 0;
            num_pnc_non_zero = 0;
            uip_loop_cap = 0;
            pnc_state = 0;
            if_sat = false;
            if_unsat = false;
            step_idx = 0;
            cutoff_percentage = 0.5f;
            root_level_decision_literal = 0;
            step_state = -1;
            pending_decision = false;
            pending_conflict = false;
            dummy_step = false;
            force_decision = 0;
            decay_factor = 0.95f;
            learned_clause_len_cap = 0;
            clause_cap_policy = CLAUSE_CAP_NON_CHRONO_BACKTRACK;
            oblivious_decision_delay = 1;
            oblivious_conflict_delay = 1;
            emp_max_wl_size = 0;
            emp_max_conflict_phi_size = 50;
            naive_oblivious_extra_steps = 0;
            oblivious_extra_steps = 0;
            last_decision_event_step = 0;
            last_conflict_event_step = 0;
            conflict_avoided = 0;
            decision_avoided = 0;
            max_lit_in_conflict_clause = 0;
            max_lit_in_clause = 0;
            orange_count = 0;
            blue_count = 0;
            green_count = 0;
            red_count = 0;
            yellow_count = 0;
            total_learned_clause = 0;
            total_conflict_clause = 0;
            total_step = 0;
            total_restart = 0;
            total_conflict = 0;
            max_wl_size = 0;
            mock_step_idx = 0;
            lucky_mock_step_idx = 0;
            total_wl_size_sum = 0;
            if_mute = false;
        };

        reset_state(parsed_var_num);
        file_name = parsed_file_name;

        auto core_it = sections.find("core");
        if (core_it != sections.end())
        {
            for (const auto &ln : core_it->second)
            {
                std::istringstream iss(ln);
                std::string key;
                if (!(iss >> key))
                {
                    continue;
                }
                if (key == "a_idx")
                    iss >> a_idx;
                else if (key == "w_idx")
                    iss >> w_idx;
                else if (key == "ctr")
                    iss >> ctr;
                else if (key == "conflict_ctr")
                    iss >> conflict_ctr;
                else if (key == "asserting_literal")
                    iss >> asserting_literal;
                else if (key == "restart_ctr")
                    iss >> restart_ctr;
                else if (key == "previous_restart_conflict_ctr")
                    iss >> previous_restart_conflict_ctr;
                else if (key == "last_restart_step_idx")
                    iss >> last_restart_step_idx;
                else if (key == "num_pnc_non_zero")
                    iss >> num_pnc_non_zero;
                else if (key == "uip_loop_cap")
                    iss >> uip_loop_cap;
                else if (key == "pnc_state")
                    iss >> pnc_state;
                else if (key == "if_sat")
                {
                    int v = 0;
                    iss >> v;
                    if_sat = (v != 0);
                }
                else if (key == "if_unsat")
                {
                    int v = 0;
                    iss >> v;
                    if_unsat = (v != 0);
                }
                else if (key == "if_mute")
                {
                    int v = 0;
                    iss >> v;
                    if_mute = (v != 0);
                }
                else if (key == "step_idx")
                    iss >> step_idx;
                else if (key == "cutoff_percentage")
                    iss >> cutoff_percentage;
                else if (key == "root_level_decision_literal")
                    iss >> root_level_decision_literal;
                else if (key == "step_state")
                    iss >> step_state;
                else if (key == "pending_decision")
                {
                    int v = 0;
                    iss >> v;
                    pending_decision = (v != 0);
                }
                else if (key == "pending_conflict")
                {
                    int v = 0;
                    iss >> v;
                    pending_conflict = (v != 0);
                }
                else if (key == "dummy_step")
                {
                    int v = 0;
                    iss >> v;
                    dummy_step = (v != 0);
                }
                else if (key == "force_decision")
                    iss >> force_decision;
                else if (key == "decay_factor")
                    iss >> decay_factor;
                else if (key == "learned_clause_len_cap")
                    iss >> learned_clause_len_cap;
                else if (key == "clause_cap_policy")
                {
                    int v = 0;
                    iss >> v;
                    if (v == 0)
                        clause_cap_policy = CLAUSE_CAP_RESTART;
                    else if (v == 1)
                        clause_cap_policy = CLAUSE_CAP_CHRONO_BACKTRACK;
                    else
                        clause_cap_policy = CLAUSE_CAP_NON_CHRONO_BACKTRACK;
                }
                else if (key == "oblivious_decision_delay")
                    iss >> oblivious_decision_delay;
                else if (key == "oblivious_conflict_delay")
                    iss >> oblivious_conflict_delay;
                else if (key == "emp_max_wl_size")
                    iss >> emp_max_wl_size;
                else if (key == "emp_max_conflict_phi_size")
                    iss >> emp_max_conflict_phi_size;
                else if (key == "naive_oblivious_extra_steps")
                    iss >> naive_oblivious_extra_steps;
                else if (key == "oblivious_extra_steps")
                    iss >> oblivious_extra_steps;
                else if (key == "last_decision_event_step")
                    iss >> last_decision_event_step;
                else if (key == "last_conflict_event_step")
                    iss >> last_conflict_event_step;
                else if (key == "conflict_avoided")
                    iss >> conflict_avoided;
                else if (key == "decision_avoided")
                    iss >> decision_avoided;
                else if (key == "max_lit_in_conflict_clause")
                    iss >> max_lit_in_conflict_clause;
                else if (key == "max_lit_in_clause")
                    iss >> max_lit_in_clause;
                else if (key == "orange_count")
                    iss >> orange_count;
                else if (key == "blue_count")
                    iss >> blue_count;
                else if (key == "green_count")
                    iss >> green_count;
                else if (key == "red_count")
                    iss >> red_count;
                else if (key == "yellow_count")
                    iss >> yellow_count;
                else if (key == "total_learned_clause")
                    iss >> total_learned_clause;
                else if (key == "total_conflict_clause")
                    iss >> total_conflict_clause;
                else if (key == "total_step")
                    iss >> total_step;
                else if (key == "total_restart")
                    iss >> total_restart;
                else if (key == "total_conflict")
                    iss >> total_conflict;
                else if (key == "max_wl_size")
                    iss >> max_wl_size;
                else if (key == "mock_step_idx")
                    iss >> mock_step_idx;
                else if (key == "lucky_mock_step_idx")
                    iss >> lucky_mock_step_idx;
                else if (key == "total_wl_size_sum")
                    iss >> total_wl_size_sum;
            }
        }

        auto clauses_it = sections.find("clauses");
        if (clauses_it == sections.end())
        {
            throw std::runtime_error("Snapshot missing [clauses] section");
        }
        {
            int expected = -1;
            int seen = 0;
            for (const auto &ln : clauses_it->second)
            {
                std::istringstream iss(ln);
                std::string first;
                if (!(iss >> first))
                {
                    continue;
                }
                if (first == "count")
                {
                    iss >> expected;
                    continue;
                }
                int clause_id = std::stoi(first);
                int size = 0;
                iss >> size;
                std::vector<int> lits;
                lits.reserve(size);
                for (int i = 0; i < size; ++i)
                {
                    int lit = 0;
                    if (!(iss >> lit))
                    {
                        throw std::runtime_error("Clause entry missing literals");
                    }
                    lits.push_back(lit);
                }
                Clause *c = new Clause;
                c->literals = lits;
                phi.insert({clause_id, c});
                seen += 1;
            }
            if (expected >= 0 && seen != expected)
            {
                throw std::runtime_error("Clause count mismatch while loading snapshot");
            }
        }

        if (include_conflict_clauses)
        {
            auto conflict_it = sections.find("conflict_clauses");
            if (conflict_it != sections.end())
            {
                int expected = -1;
                int seen = 0;
                for (const auto &ln : conflict_it->second)
                {
                    std::istringstream iss(ln);
                    std::string first;
                    if (!(iss >> first))
                    {
                        continue;
                    }
                    if (first == "count")
                    {
                        iss >> expected;
                        continue;
                    }
                    int clause_id = std::stoi(first);
                    int size = 0;
                    iss >> size;
                    Clause c;
                    c.literals.reserve(size);
                    for (int i = 0; i < size; ++i)
                    {
                        int lit = 0;
                        if (!(iss >> lit))
                        {
                            throw std::runtime_error("Conflict clause entry missing literals");
                        }
                        c.literals.push_back(lit);
                    }
                    conflict_phi.insert({clause_id, c});
                    seen += 1;
                }
                if (expected >= 0 && seen != expected)
                {
                    throw std::runtime_error("Conflict clause count mismatch while loading snapshot");
                }
            }
        }

        auto big_v_it = sections.find("big_v");
        if (big_v_it == sections.end())
        {
            throw std::runtime_error("Snapshot missing [big_v] section");
        }
        {
            int cur_dl = 0;
            int trail_expected = -1;
            std::vector<int> trail;
            std::vector<int> assignment(var_num + 1, -1);
            std::vector<int> antecedent(var_num + 1, 0);
            std::vector<int> dl(var_num + 1, -1);
            std::vector<int> phase(var_num + 1, -1);

            for (const auto &ln : big_v_it->second)
            {
                std::istringstream iss(ln);
                std::string key;
                if (!(iss >> key))
                {
                    continue;
                }
                if (key == "cur_dl")
                {
                    iss >> cur_dl;
                }
                else if (key == "vars")
                {
                    int vars = 0;
                    iss >> vars;
                    if (vars != var_num)
                    {
                        throw std::runtime_error("Snapshot var count mismatch in [big_v]");
                    }
                }
                else if (key == "var")
                {
                    int id = 0;
                    int assign = -1;
                    int ante = 0;
                    int level = -1;
                    int ph = -1;
                    iss >> id >> assign >> ante >> level >> ph;
                    if (id <= 0 || id > var_num)
                    {
                        throw std::runtime_error("Invalid variable id in [big_v]");
                    }
                    assignment[id] = assign;
                    antecedent[id] = ante;
                    dl[id] = level;
                    phase[id] = ph;
                }
                else if (key == "trail_count")
                {
                    iss >> trail_expected;
                }
                else if (key == "trail")
                {
                    int lit = 0;
                    while (iss >> lit)
                    {
                        trail.push_back(lit);
                    }
                }
            }

            if (trail_expected >= 0 && static_cast<int>(trail.size()) != trail_expected)
            {
                throw std::runtime_error("Trail count mismatch while loading snapshot");
            }

            big_V = VariableState(var_num);
            big_V.cur_dl = cur_dl;
            for (int lit : trail)
            {
                int var = abs(lit);
                int ante = antecedent[var];
                int level = dl[var];
                if (ante == 0)
                {
                    big_V.set_decision(lit, level);
                }
                else
                {
                    big_V.set_unit_literal(lit, ante, level);
                }
            }

            bool has_phase = false;
            int temp_dl = cur_dl + 1;
            for (int i = 1; i < var_num + 1; ++i)
            {
                if (assignment[i] == -1 && phase[i] != -1)
                {
                    int lit = (phase[i] == 1) ? i : -i;
                    big_V.set_decision(lit, temp_dl);
                    has_phase = true;
                }
            }
            if (has_phase)
            {
                big_V.clean_up_after_backtrack(cur_dl);
            }
            big_V.cur_dl = cur_dl;
        }

        bool have_unit_clause = false;
        auto unit_it = sections.find("unit_clause");
        if (unit_it != sections.end())
        {
            int expected = -1;
            int seen = 0;
            for (const auto &ln : unit_it->second)
            {
                std::istringstream iss(ln);
                std::string first;
                if (!(iss >> first))
                {
                    continue;
                }
                if (first == "count")
                {
                    iss >> expected;
                    continue;
                }
                int lit = std::stoi(first);
                int clause_idx = 0;
                iss >> clause_idx;
                unit_clause.insert({lit, clause_idx});
                seen += 1;
            }
            if (expected >= 0 && seen != expected)
            {
                throw std::runtime_error("Unit clause count mismatch while loading snapshot");
            }
            have_unit_clause = true;
        }

        auto parse_list_section = [&](const std::vector<std::string> &lines, std::vector<int> &out)
        {
            int expected = -1;
            for (const auto &ln : lines)
            {
                std::istringstream iss(ln);
                std::string first;
                if (!(iss >> first))
                {
                    continue;
                }
                if (first == "count")
                {
                    iss >> expected;
                    continue;
                }
                if (first != "lits" && first != "entries" && first != "trail")
                {
                    iss.clear();
                    iss.str(ln);
                }
                int v = 0;
                while (iss >> v)
                {
                    out.push_back(v);
                }
            }
            if (expected >= 0 && static_cast<int>(out.size()) != expected)
            {
                throw std::runtime_error("List count mismatch while loading snapshot");
            }
        };

        auto big_u_it = sections.find("big_u");
        if (big_u_it != sections.end())
        {
            std::vector<int> lits;
            parse_list_section(big_u_it->second, lits);
            for (int lit : lits)
            {
                big_U.insert(lit);
            }
        }

        auto cur_wl_it = sections.find("cur_wl");
        if (cur_wl_it != sections.end())
        {
            std::vector<int> entries;
            parse_list_section(cur_wl_it->second, entries);
            cur_wl = entries;
        }

        bool have_activities = false;
        if (include_activities)
        {
            auto act_it = sections.find("activities");
            if (act_it != sections.end())
            {
                int expected = -1;
                int seen = 0;
                for (const auto &ln : act_it->second)
                {
                    std::istringstream iss(ln);
                    std::string first;
                    if (!(iss >> first))
                    {
                        continue;
                    }
                    if (first == "count")
                    {
                        iss >> expected;
                        continue;
                    }
                    int var = std::stoi(first);
                    int value = 0;
                    iss >> value;
                    activities.insert({var, value});
                    seen += 1;
                }
                if (expected >= 0 && seen != expected)
                {
                    throw std::runtime_error("Activities count mismatch while loading snapshot");
                }
                have_activities = true;
            }
        }

        bool have_watchlists = false;
        if (include_watchlists)
        {
            auto wl_it = sections.find("watchlists");
            if (wl_it != sections.end())
            {
                int expected = -1;
                int seen = 0;
                for (const auto &ln : wl_it->second)
                {
                    std::istringstream iss(ln);
                    std::string first;
                    if (!(iss >> first))
                    {
                        continue;
                    }
                    if (first == "count")
                    {
                        iss >> expected;
                        continue;
                    }
                    int lit = std::stoi(first);
                    int size = 0;
                    iss >> size;
                    WatchList wl;
                    for (int i = 0; i < size; ++i)
                    {
                        int clause_idx = 0;
                        if (!(iss >> clause_idx))
                        {
                            throw std::runtime_error("Watchlist entry missing clause id");
                        }
                        wl.wl.push_back(clause_idx);
                    }
                    wls.insert({lit, wl});
                    seen += 1;
                }
                if (expected >= 0 && seen != expected)
                {
                    throw std::runtime_error("Watchlist count mismatch while loading snapshot");
                }
                have_watchlists = true;
            }
        }

        if (phi.find(0) == phi.end())
        {
            Clause *empty_clause = new Clause;
            phi.insert({0, empty_clause});
        }

        if (!have_unit_clause)
        {
            for (const auto &kv : phi)
            {
                if (kv.first == 0)
                {
                    continue;
                }
                if (kv.second->literals.size() == 1)
                {
                    unit_clause.insert({kv.second->literals[0], kv.first});
                }
            }
        }

        if (!have_activities)
        {
            for (int i = 1; i < var_num + 1; ++i)
            {
                activities.insert({i, 0});
            }
        }
        else
        {
            for (int i = 1; i < var_num + 1; ++i)
            {
                if (activities.find(i) == activities.end())
                {
                    activities.insert({i, 0});
                }
            }
        }

        auto ensure_wls_keys = [&]()
        {
            if (wls.find(0) == wls.end())
            {
                wls.insert({0, WatchList()});
            }
            for (int i = 1; i < var_num + 1; ++i)
            {
                if (wls.find(i) == wls.end())
                {
                    wls.insert({i, WatchList()});
                }
                if (wls.find(-i) == wls.end())
                {
                    wls.insert({-i, WatchList()});
                }
            }
        };

        if (!have_watchlists)
        {
            wls.clear();
            ensure_wls_keys();
            for (const auto &kv : phi)
            {
                if (kv.first == 0)
                {
                    continue;
                }
                construct_wl(kv.first);
            }
            for (const auto &kv : conflict_phi)
            {
                construct_wl(kv.first);
            }
        }
        else
        {
            ensure_wls_keys();
        }
    }

    int make_decision()
    {
        int assigned = 0;
        if (!if_artificial_decision_level)
        {
            big_V.cur_dl += 1; // not in epoch mode
        }
        // in epoch mode cur_dl is already set from the step index in begin_giant_step()

        assigned = pick_variable_VSIDS();
        if (force_decision != 0)
        {
            assigned = force_decision;
            force_decision = 0;
        }
        // record the decision in big_V and big_U
        big_V.set_decision(assigned, big_V.cur_dl);
        big_U.insert(assigned);
        if (big_V.cur_dl == 1)
            root_level_decision_literal = assigned;
        return assigned;
    }

    void get_w_from_cur_wl()
    {
        green_count++; // count calls to get_w_from_cur_wl
        if (pending_decision)
            return;
        if (if_detail_debug)
            std::cout << "### get w from cur wl: ";
        // Called only when pnc_state is 0. The current watchlist is already loaded;
        // big_U is updated in get_w_from_big_U and get_w_from_new_decision.
    }

    void get_w_from_big_U()
    {
        blue_count++; // count calls to get_w_from_big_U
        if (pending_decision)
            return;
        if (if_detail_debug)
            std::cout << "### get w from big U: ";
        cur_wl.clear();
        auto next_unit = std::min_element(
            big_U.begin(), big_U.end(), [](int lhs, int rhs)
            {
                int lhs_var = std::abs(lhs);
                int rhs_var = std::abs(rhs);
                return lhs_var != rhs_var ? lhs_var < rhs_var : lhs > rhs;
            });
        int a = *next_unit;
        big_U.erase(next_unit);
        auto a_wl = wls.find(-a);
        // copy the clauses of a_wl into cur_wl
        for (auto c : a_wl->second.wl)
        {
            cur_wl.push_back(c);
        }
        // pad cur_wl with zeros up to emp_max_wl_size
        for (int i = cur_wl.size(); i < emp_max_wl_size; ++i)
        {
            cur_wl.push_back(0);
        }

        // oblivious mode: count the padding as extra steps
        int current_wl_size = cur_wl.size();
        if (current_wl_size < emp_max_wl_size)
        {
            int dummy_steps = emp_max_wl_size - current_wl_size;
            oblivious_extra_steps += dummy_steps;
            naive_oblivious_extra_steps += dummy_steps;
        }
        ctr = cur_wl.size() - 1;
        a_idx = a;
    }

    void get_w_from_new_decision()
    {
        if (if_detail_debug)
            std::cout << "### get w from new decision: ";
        cur_wl.clear();
        int a = make_decision();
        big_U.erase(a);
        auto a_wl = wls.find(-a);
        // copy the clauses of a_wl into cur_wl
        for (auto c : a_wl->second.wl)
        {
            cur_wl.push_back(c);
        }
        // pad cur_wl with zeros up to emp_max_wl_size
        for (int i = cur_wl.size(); i < emp_max_wl_size; ++i)
        {
            cur_wl.push_back(0);
        }
        // oblivious mode: count the padding as extra steps
        int current_wl_size = cur_wl.size();
        if (current_wl_size < emp_max_wl_size)
        {
            int dummy_steps = emp_max_wl_size - current_wl_size;
            oblivious_extra_steps += dummy_steps;
            naive_oblivious_extra_steps += dummy_steps;
        }
        // oblivious mode: simulate the delay between decision events
        int decision_delay = oblivious_decision_delay - (step_idx - last_decision_event_step);
        if (decision_delay > 0)
        {
            oblivious_extra_steps += decision_delay;
        }
        last_decision_event_step = step_idx;
        ctr = cur_wl.size() - 1;
        a_idx = a;
    }

    // pick a new clause to visit in the next giant step
    void pick_new_clause()
    {
        if (pending_conflict)
        {
            return;
        }
        if (if_detail_debug)
        {
            std::cout << "### big_U: ";
            for (auto i : big_U)
            {
                std::cout << i << " ";
            }
            std::cout << std::endl;
        }
        pnc_state = ((ctr >= 0) ? 0 : (big_U.empty() ? 1 : 2));
        if (pnc_state == 1 && !pending_decision)
        {
            // Check satisfiability when no unit literal is pending and before a
            // decision is created. The watchlist of the last consumed unit must
            // be scanned completely first.
            if (check_sat())
            {
                return;
            }
            decision_log.push_back(step_idx);
        }
        if (if_detail_debug)
            std::cout << "### pnc_state: " << pnc_state << std::endl;
        if (pnc_state == 0)
        {
            get_w_from_cur_wl();
        }
        else
        {
            if (pnc_state == 1)
            {
                pending_decision = true;
                if (is_public_decision_slot(step_idx))
                {
                    get_w_from_new_decision();
                    pending_decision = false;
                }
                else
                {
                    dummy_step = true;
                    return;
                }
            }
            else
            {
                get_w_from_big_U();
            }
        }
        if (if_debug)
            std::cout << "pending_decision: " << pending_decision << std::endl;
        if (max_wl_size < cur_wl.size())
        {
            max_wl_size = cur_wl.size();
        }
        wls_size.push_back(cur_wl.size());
        w_idx = ctr <= -1 ? 0 : cur_wl[ctr];
        if (pending_decision)
        {
            w_idx = 0;
        }
        if (if_detail_debug)
        {
            std::cout << " w_idx: " << w_idx << " a_idx: " << a_idx << std::endl;
            std::cout << "### cur_wl: ";
            for (auto i : cur_wl)
            {
                std::cout << i << " ";
            }
        }
    }

    // length of the i-th restart interval is ruby_base times t_i (Luby sequence),
    // where t_i = 2^{k-1} if i = 2^k -1, and t_{i-2^{k-1}+1} if 2^{k-1} <= i < 2^k -1
    bool compute_ruby_restart_interval(int conflict_ctr, int previous_restart_conflict_ctr, int ruby_base)
    {
        {
            int interval_idx = restart_ctr + 1;
            int idx = interval_idx;
            int t_i = 1;

            while (true)
            {
                int k = 1;
                while (((1 << k) - 1) < idx)
                {
                    ++k;
                }
                if (((1 << k) - 1) == idx)
                {
                    t_i = 1 << (k - 1);
                    break;
                }
                idx = idx - (1 << (k - 1)) + 1;
            }

            int interval_len = ruby_base * t_i;
            if ((conflict_ctr - previous_restart_conflict_ctr) >= interval_len)
            {
                if (if_debug)
                    std::cout << "### RUBY restart interval length: " << interval_len << std::endl;
            }
            return (conflict_ctr - previous_restart_conflict_ctr) >= interval_len;
        }
    }

    void backtrack_aftermath()
    {
        construct_wl(0 - (conflict_ctr - 1));
        if (if_debug)
            big_V.print_all();
        if (if_debug)
            std::cout << "### imply asserting lit after backtrack...\n";
        add_implication(asserting_literal, 0 - (conflict_ctr - 1), big_V.cur_dl);
    }

    void restart()
    {
        if (if_detail_debug)
            std::cout << "### RESTART!!!" << std::endl;
        backtrack(0);
        force_decision = 0;
        restart_ctr++;
        if (if_detail_debug)
            big_V.print_all();
        push_unit_clause_to_big_U();
    }

    bool check_sat()
    {
        if (if_all_assigned() && big_U.empty())
        {
            if_sat = true;
        }
        return if_sat;
    }

    // search for two non-falsified literals in a clause, return one or two literals
    std::vector<int> search_two_non_falsified(int clause_idx, int fresh_falsified_lit)
    {
        std::vector<int> non_falsified_literals = {0, 0};
        // if clause_idx is negative, fetch from conflict_phi, if positive from phi
        const Clause *cur_clause = get_clause_from_idx(clause_idx);
        // linear scan; the result depends on the number of non-falsified literals
        // 0 non-falsified: 0,0
        // 1 non-falsified: l,l
        // 2 non-falsified: l,l'
        for (auto l : cur_clause->literals)
        {
            if (!big_V.is_literal_falsified(l) && l != fresh_falsified_lit)
            {
                non_falsified_literals[1] = (non_falsified_literals[1] == non_falsified_literals[0]) ? l
                                                                                                     : non_falsified_literals[1];
                non_falsified_literals[0] = (non_falsified_literals[0] == 0) ? l : non_falsified_literals[0];
            }
        }
        if (if_detail_debug)
        {
            std::cout << "search_two_non_falsified in clause w_idx: " << clause_idx << std::endl;
            std::cout << "found: " << non_falsified_literals[0] << " " << non_falsified_literals[1] << std::endl;
        }
        return non_falsified_literals;
    }

    std::vector<int> search_two_shortest_non_falsified(int clause_idx, int fresh_falsified_lit)
    {
        std::vector<int> best_literals = {0, 0};
        // Set initial best sizes to the maximum int value.
        int best_size = std::numeric_limits<int>::max();
        int second_best_size = std::numeric_limits<int>::max();

        // if clause_idx is negative, fetch from conflict_phi, if positive from phi.
        const Clause *cur_clause = get_clause_from_idx(clause_idx);

        // Linear scan over all literals in the clause.
        for (auto l : cur_clause->literals)
        {
            if (!big_V.is_literal_falsified(l) &&
                l != fresh_falsified_lit &&
                l != best_literals[0] && l != best_literals[1])
            {
                int current_size = wls[l].size();
                if (current_size < best_size)
                {
                    // New best candidate found: demote current best to second.
                    second_best_size = best_size;
                    best_literals[1] = best_literals[0];
                    best_size = current_size;
                    best_literals[0] = l;
                }
                else if (current_size < second_best_size)
                {
                    // Candidate is better than second best.
                    second_best_size = current_size;
                    best_literals[1] = l;
                }
            }
        }
        if (best_literals[0] != 0 && best_literals[1] == 0)
        {
            best_literals[1] = best_literals[0];
        }

        if (if_detail_debug)
        {
            std::cout << "search_two_shortest_non_falsified in clause w_idx: " << clause_idx << std::endl;
            std::cout << "found: " << best_literals[0] << " " << best_literals[1] << std::endl;
        }
        return best_literals;
    }

    // add implication to big_V and big_U
    void add_implication(int literal, int antecedent, int dl)
    {
        if (if_debug)
            std::cout << "add implication: " << literal << " antecedent: " << antecedent << " dl: " << dl
                      << std::endl;
        if (big_V.if_variable_unassigned(abs(literal)))
        {
            // if the variable is unassigned, assign it
            big_V.set_unit_literal(literal, antecedent, dl);
            big_U.insert(literal);
        }
        else
        {
            // if the variable is assigned, check if the assignment is consistent
            if (big_V.get_assignment(abs(literal)) != (literal > 0))
            {
                throw std::runtime_error(
                    "conflict in assignment! trying to imply a variable that is already assigned to a different value");
            }
        }
    }

    void ensure_seen_initialized()
    {
    }

    int count_reasoned_at_level(int level) const
    {
        int count = 0;
        for (int i = 1; i <= var_num; ++i)
        {
            if (big_V.get_assignment(i) != -1 &&
                big_V.get_decision_level(i) == level &&
                big_V.get_antecedent(i) != 0)
            {
                count += 1;
            }
        }
        return count;
    }

    ConflictAnalysisResult conflict_analysis(int conflict_clause_idx)
    {
        ConflictAnalysisResult result;
        const std::vector<int> &trail = big_V.get_trail();
        vector<int> cand_vars;
        vector<int> cand_var_ctrs;
        cand_vars.reserve(var_num + 1);
        cand_var_ctrs.reserve(var_num + 1);

        int cur_dl = big_V.cur_dl;
        if (cur_dl == 0)
        {
            if_unsat = true;
            return result;
        }
        const Clause *c = get_clause_from_idx(conflict_clause_idx);

        std::vector<int> out_learnt;
        out_learnt.reserve(var_num);

        int idx = static_cast<int>(trail.size()) - 1;
        int pathC = 0;
        int analyze_iterations = 0;
        const int analyze_bound = count_reasoned_at_level(cur_dl) + 1;
        int pivot = 0;
        std::vector<char> seen;      // mark array for conflict analysis
        seen.assign(var_num + 1, 0);
        std::unordered_set<int> variable_to_be_bumped;
        while (true)
        {
            // add the unseen literals of the current clause: those at the current level are
            // pivot candidates, the others go to the learned clause
            for (int q : c->literals)
            {
                int v = std::abs(q);
                if (v == std::abs(pivot))
                {
                    continue;
                }
                int lvl = big_V.get_decision_level(v);
                if (lvl == 0 || seen[v])
                {
                    continue;
                }
                seen[v] = 1;
                // v must not already be in variable_to_be_bumped
                if (variable_to_be_bumped.find(v) != variable_to_be_bumped.end())
                {
                    throw std::runtime_error("variable already in variable_to_be_bumped during conflict analysis");
                }
                variable_to_be_bumped.insert(v);
                if (lvl == cur_dl)
                {
                    cand_vars.push_back(v);
                    cand_var_ctrs.push_back(big_V.get_order_ctr(v));
                    pathC++;
                }
                else
                {
                    out_learnt.push_back(q);
                }
            }

            while (idx >= 0 && !seen[std::abs(trail[idx])])
            {
                idx--;
            }
            if (idx < 0)
            {
                throw std::runtime_error("trail exhausted during conflict analysis");
            }
            int old_pivot = trail[idx];

            // the pivot is the variable in cand_vars with the highest order counter
            if (cand_vars.empty())
            {
                throw std::runtime_error("no pivot found during conflict analysis");
            }
            int max_ctr_idx = -1;
            for (int i = 0; i < cand_vars.size(); ++i)
            {
                int v = cand_vars[i];
                if (max_ctr_idx < 0 || cand_var_ctrs[i] > cand_var_ctrs[max_ctr_idx])
                    max_ctr_idx = i;
            }

            pivot = cand_vars[max_ctr_idx];
            pivot = big_V.get_assignment(pivot) ? pivot : -pivot;

            if (pivot != old_pivot)
            {
                big_V.print_trail();
                big_V.print_ctr_based_trail();
                throw std::runtime_error("pivot literal does not match trail literal during conflict analysis");
            }

            // remove pivot from cand_vars
            cand_vars.erase(cand_vars.begin() + max_ctr_idx);
            cand_var_ctrs.erase(cand_var_ctrs.begin() + max_ctr_idx);

            if (big_V.get_decision_level(std::abs(pivot)) != cur_dl)
            {
                throw std::runtime_error("pivot literal not at current decision level during conflict analysis");
            }
            analyze_iterations++;

            int pivot_var = std::abs(pivot);
            int reason_idx = big_V.get_antecedent(pivot_var);
            seen[pivot_var] = 0;
            pathC--;

            if (pathC <= 0)
            {
                break;
            }
            c = get_clause_from_idx(reason_idx);

            if (uip_loop_cap != 0 && analyze_iterations >= uip_loop_cap)
            {
                result.over_uip_cap = true;
                break;
            }
        }

        const int candidate_asserting_literal = -pivot;
        out_learnt.push_back(candidate_asserting_literal);

        Clause learned;
        learned.literals = out_learnt;
        // sanity check: exactly one literal is from the current decision level

        int count_cur_dl = 0;
        for (auto l : learned.literals)
        {
            int lvl = big_V.get_decision_level(abs(l));
            if (lvl == cur_dl)
            {
                count_cur_dl++;
            }
        }
        if (count_cur_dl != 1)
        {
            throw std::runtime_error("conflict analysis failed sanity check: learned clause has " + std::to_string(count_cur_dl) + " literals from current decision level");
        }

        if (!big_V.is_literal_falsified(candidate_asserting_literal))
        {
            throw std::runtime_error("conflict analysis failed sanity check: asserting literal is not falsified");
        }
        learned.lbd = compute_lbd(learned);
        conflict_clause_size.push_back(static_cast<int>(learned.literals.size()));
        result.learned_clause_size = static_cast<int>(learned.literals.size());
        result.over_cap = learned_clause_len_cap > 0 && result.learned_clause_size > learned_clause_len_cap;
        result.over_count_cap = emp_max_conflict_phi_size > 0 &&
                                conflict_ctr > emp_max_conflict_phi_size;

        if (learned.literals.size() > 1)
        {
            // beta is the highest decision level below cur_dl among the learned literals
            for (int i = 0; i < learned.literals.size(); ++i)
            {
                int lvl = big_V.get_decision_level(abs(learned.literals[i]));
                if (lvl > result.beta && lvl < cur_dl)
                {
                    result.beta = lvl;
                }
            }
        }

        const bool rejected = result.over_cap || result.over_uip_cap || result.over_count_cap;

        if (if_mVSIDS || if_eVSIDS)
        {
            for (auto l : learned.literals)
            {
                variable_to_be_bumped.insert(abs(l));
            }
        }

        if (if_eVSIDS)
        {
            for (auto &i : activities)
            {
                if (variable_to_be_bumped.find(i.first) != variable_to_be_bumped.end())
                {
                    i.second = i.second * decay_factor + 1 - decay_factor;
                }
                else
                {
                    i.second = i.second * decay_factor;
                }
            }
        }
        else if (if_mVSIDS)
        {
            for (auto i : variable_to_be_bumped)
            {
                activities.find(i)->second += 100;
            }
        }

        // Decay the activities after every conflict analysis below the root level,
        // including candidates rejected by a cap.
        decay_activities();

        if (!rejected)
        {
            asserting_literal = candidate_asserting_literal;
            conflict_phi.insert({-conflict_ctr, learned});
            conflict_ctr += 1;
        }
        else
        {
            if (if_detail_debug)
            {
                std::cout << "rejecting learned candidate: length_cap=" << result.over_cap
                          << " uip_cap=" << result.over_uip_cap
                          << " count_cap=" << result.over_count_cap << std::endl;
            }
        }

        uip_loop_size.push_back(analyze_iterations);
        uip_bound_size.push_back(analyze_bound);
        uip_bound_real_diff.push_back(analyze_bound - analyze_iterations);
        assert(analyze_iterations <= analyze_bound);

        if (if_detail_debug)
        {
            std::cout << "conflict clause learnt: ";
            learned.print();
            std::cout << std::endl;
            std::cout << "decision level of each literal: ";
            for (int i = 0; i < learned.literals.size(); ++i)
            {
                std::cout << big_V.get_decision_level(abs(learned.literals[i])) << " ";
            }
            std::cout << std::endl;
            std::cout << "beta: " << result.beta << std::endl;
            std::cout << "conflict literal: " << asserting_literal << std::endl;
            std::cout << "conflict analysis iterations: " << analyze_iterations << std::endl;
        }
        if (if_debug)
            std::cout << "conflict clause learnt: ";
        if (if_debug)
            learned.print();
        return result;
    }

    void decay_activities()
    {
        if (if_eVSIDS)
        {
        }
        else if (if_mVSIDS)
        {
            for (auto &i : activities)
            {
                i.second = (i.second * 95) / 100;
            }
        }
    }

    void delete_single_clause(int clause_idx)
    {
        const Clause *c = get_clause_from_idx(clause_idx);
        for (auto l : c->literals)
        {
            wls.find(l)->second.remove_clause(clause_idx);
        }
        conflict_phi.erase(clause_idx);
    }

    void delete_old_clause()
    {
        if (conflict_phi.size() > delete_old_clause_threshold)
        {
            int candidate_key = -(conflict_ctr - delete_old_clause_threshold);
            // Loop until a non-unit clause is found, or until there are no candidates left.
            while (conflict_phi.find(candidate_key) != conflict_phi.end() &&
                   conflict_phi.find(candidate_key)->second.literals.size() == 1)
            {
                candidate_key--; // Move to the next older clause.
            }
            // Delete the candidate if it exists.
            if (conflict_phi.find(candidate_key) != conflict_phi.end())
            {
                delete_single_clause(candidate_key);
            }
        }
    }

    void construct_wl(int clause_idx)
    {
        const Clause *c = get_clause_from_idx(clause_idx);
        if (conflict_ctr == 0)
        {
            throw std::runtime_error("conflict_ctr is 0");
        }
        if (c->literals.size() == 1)
        {
            if (if_detail_debug)
            {
                std::cout << "### construct unit wl: " << clause_idx << std::endl;
            }
        }
        // Watch the first non-falsified literal (the asserting literal of a learned
        // clause installed after backtracking) and one other distinct literal, even
        // if that literal is currently falsified. This matches the secure solver.
        int first = 0;
        for (int literal : c->literals)
        {
            if (literal != 0 && !big_V.is_literal_falsified(literal))
            {
                first = literal;
                break;
            }
        }
        if (first == 0)
        {
            for (int literal : c->literals)
            {
                if (literal != 0)
                {
                    first = literal;
                    break;
                }
            }
        }

        int second = 0;
        for (int literal : c->literals)
        {
            if (literal != 0 && literal != first)
            {
                second = literal;
                break;
            }
        }
        if (first != 0)
            wls.find(first)->second.add_clause(clause_idx);
        if (second != 0)
            wls.find(second)->second.add_clause(clause_idx);
    }

    // pop from stack until decision level is beta
    void backtrack(int beta)
    {
        big_U.clear();
        ctr = -1;
        if (beta < 0)
            throw std::runtime_error("beta < 0");

        if (if_debug)
            std::cout << "backtrack cutoff beta=" << beta
                      << " (cur_epoch=" << big_V.cur_dl << ")\n";

        // in epoch mode cur_dl is derived from the step index and is left unchanged
        if (!if_artificial_decision_level)
        {
            while (big_V.cur_dl > beta)
                big_V.cur_dl -= 1;
            big_V.clean_up_after_backtrack(big_V.cur_dl);
        }
        else
        {
            big_V.clean_up_after_backtrack(beta);
            // cur_dl stays the step-based epoch
        }
    }

    // if clause_idx is negative, fetch from conflict_phi, if positive from phi
    const Clause *get_clause_from_idx(int clause_idx)
    {
        Clause *c;
        if (clause_idx < 0)
        {
            c = &conflict_phi.find(clause_idx)->second;
        }
        else
        {
            c = phi.find(clause_idx)->second;
        }
        return c;
    }

    // update watch list if found two non-falsified literals in a clause
    void update_wl(int clause_idx, int fresh_falsified_lit, std::vector<int> non_falsified_literals)
    {
        // remove clause from fresh_falsified_lit's watch list, and add it to a new non-falsified literal's watch list
        auto fresh_falsified_lit_wl = wls.find(fresh_falsified_lit);
        fresh_falsified_lit_wl->second.remove_clause(clause_idx);

        // Preserve the two-watched-literal invariant by only adding the clause to the first
        // candidate literal that is not already watching it.
        for (auto non_falsified_lit : non_falsified_literals)
        {
            auto &target_wl = wls.find(non_falsified_lit)->second;
            if (!target_wl.contains(clause_idx))
            {
                target_wl.add_clause(clause_idx);
                break;
            }
        }
    }

    void print_phi()
    {
        std::cout << "phi" << std::endl;
        for (auto i : phi)
        {
            std::cout << i.first << ": ";
            i.second->print();
        }
    }

    void print_wls()
    {
        std::cout << "watch lists" << std::endl;
        for (auto i : wls)
        {
            std::cout << i.first << ": ";
            i.second.print();
        }
    }

    void print_all_assigned()
    {
        for (int i = 1; i < var_num + 1; ++i)
        {
            std::cout << i << ": " << big_V.get_assignment(i) << std::endl;
        }
    }

    void print_all_variable_state()
    {
        for (int i = 1; i < var_num + 1; ++i)
        {
            // print variable, assignment, antecedent, decision level
            std::cout << i << ": " << big_V.get_assignment(i) << " " << big_V.get_antecedent(i) << " "
                      << big_V.get_decision_level(i) << std::endl;
        }
    }

    void init_log()
    {
        procedure_log.insert({"cur_wl", 0});
        procedure_log.insert({"decide", 0});
        procedure_log.insert({"big_U", 0});
        procedure_log.insert({"unit", 0});
        procedure_log.insert({"conflict", 0});
        procedure_log.insert({"not unit", 0});
        procedure_log.insert({"blank", 0});
    }

    void log_current_step()
    {
        // 0: cur_wl, 1: decide, 2: big_U
        string pnc_state_meaning = pnc_state == 0 ? "cur_wl" : pnc_state == 1 ? "decide"
                                                                              : "big_U";
        // 0: conflict, 1: unit, 2: not unit, 3: blank
        string step_state_meaning = step_state == 0 ? "conflict" : step_state == 1 ? "unit"
                                                               : step_state == 2   ? "not unit"
                                                                                   : "blank";
        procedure_log.find(pnc_state_meaning)->second += 1;
        procedure_log.find(step_state_meaning)->second += 1;
    }

    void print_log()
    {
        for (auto i : procedure_log)
        {
            std::cout << i.first << ": " << (double)i.second / (double)step_idx * 100.0 << "%" << std::endl;
        }
    }

    bool verifiy_sat()
    {
        // check if current assignment satisfies all clauses
        for (auto c : phi)
        {
            bool if_satisfied = false;
            for (auto l : c.second->literals)
            {
                if (big_V.is_literal_satisfied(l))
                {
                    if_satisfied = true;
                }
            }
            if (!if_satisfied && c.first != 0)
            {
                print_all_assigned();
                std::cout << "unsatisfied clause: ";
                c.second->print();
                return false;
            }
        }
        // also check unit clauses
        for (auto c : unit_clause)
        {
            if (!big_V.is_literal_satisfied(c.first))
            {
                std::cout << "unsatisfied unit clause: " << c.first << std::endl;
                return false;
            }
        }
        return true;
    }
};

#endif // CDCL_H
