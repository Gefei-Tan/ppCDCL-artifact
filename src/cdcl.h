#ifndef CDCL_H
#define CDCL_H

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "clause.h"
#include "variable_state_backend.h"
#include "watchlist.h"
#include "random"
#include "emp-tool/emp-tool.h"
#include "clause_list.h"
#include "dimacs.h"
#include "public_schedule.h"
#include "util.h"

class CDCL
{
public:
    FloramMPC<NetIO> *backend;
    bool use_oram_variable_state = true;
    WatchBackendRequest watch_backend_request = WatchBackendRequest::Auto;

    /* heuristics config flags */
    bool static const if_restart_by_step = false;
    bool static const if_restart_by_conflict = false;
    bool static const if_delete_clause = false;
    bool static const if_step_decay = false;
    bool static const if_phase_saving = true;
    bool use_shortest_watchlist = false;                 // choose two least-loaded replacement watchlists
    static const int SOLVER_RESULT_FAILURE = -2;
    static const int TERMINAL_RUNNING = 0;
    static const int TERMINAL_SAT = 1;
    static const int TERMINAL_UNSAT = 2;
    static const int TERMINAL_FAILURE = 3;
    int last_restart_step_idx = 0;                       // step index at the last restart
    int oblivious_decision_delay = 1;                   // fixed delay interval for decisions
    int oblivious_conflict_delay = 1;                   // fixed delay interval for conflicts
    int uip_loop_cap = 0;                               // 0 means use VAR_NUM rounds
    int last_conflict_analysis_rounds = 0;              // public test/benchmark instrumentation

    /* decision config flags */
    static const bool if_mVSIDS = true;
    static const int activity_bump = 100;
    float decay_factor = 0.95;

    /* debug flags */
    static const bool if_verify = false;
    static const bool if_debug = false;
    static const bool if_detail_debug = false;
    static const bool if_lazy_conflict_debug = false;

    /* emp data structures */
    Clause w;
    Integer a_idx = Integer(VAR_SIZE_BIT, -1);       // the literal we are visiting in current giant step
    Integer w_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0); // the clause we are visiting in current giant step
    int ctr = -1;
    Integer wl_scan_ctr = Integer(3, -1, PUBLIC);    // secret-gated logical cursor for the current fixed-size watchlist
    Bit scanned_wl_this_step = Bit(false, PUBLIC);
    long long step_idx;
    bool giant_step_has_run = false;                         // set after the first call; resumed calls use the conservative source-work schedule
    Integer conflict_ctr = Integer(CON_CLAUSE_IDX_SIZE_BIT, 1); // number of conflicts
    Integer asserting_literal = Integer(VAR_SIZE_BIT, 0);        // the asserting literal in UIP learned clause
    Integer pending_conflict_clause_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0);
    int restart_ctr = 0;                                         // number of periodic restarts
    int previous_restart_conflict_ctr = 0;
    std::uint64_t conflict_schedule_counter = 0;                 // public scheduled conflict slots
    std::uint64_t luby_restart_index = 1;                        // one-based public Luby interval
    std::uint64_t luby_interval_progress = 0;                    // slots in current interval
    Integer pnc_state = Integer(4, 0);                           // state of pick_new_clause; 0: current watchlist, 1: new decision, 2: big_u, 3: idle
    Integer step_state = Integer(4, -1);                         // result of the non-falsified literal search; 0: conflict, 1: unit, 2: non-unit, 3: no active clause
    Integer STEP_STATE_NO_WL = Integer(4, 3);
    Integer STEP_STATE_CONFLICT = Integer(4, 0);
    Integer STEP_STATE_UNIT = Integer(4, 1);
    Integer STEP_STATE_NON_UNIT = Integer(4, 2);
    Integer PNC_STATE_CUR_WL = Integer(4, 0);
    Integer PNC_STATE_DECISION = Integer(4, 1);
    Integer PNC_STATE_BIG_U = Integer(4, 2);
    Integer PNC_STATE_IDLE = Integer(4, 3);
    Bit if_sat = Bit(false, PUBLIC);
    Bit if_unsat = Bit(false, PUBLIC);
    Bit if_failure = Bit(false, PUBLIC);          // public failure; set on watchlist overflow
    Bit if_conflict_pending = Bit(false, PUBLIC); // a conflict was found and is waiting for its public conflict slot
    Bit if_decision_pending = Bit(false, PUBLIC);

    vector<Integer> cur_wl; // snapshot of the watchlist of -a_idx for the current scan; 0 marks an empty slot
    // all unit literals to be propagated
    vector<Integer> big_u;
    // all variables' states, including assignment, antecedent, decision level
    VariableStateBackend *big_v;
    // all clauses and conflict clauses
    ClauseList *cl;
    // watchlists of all literals
    WatchList *wl;
    // activity scores of all variables
    vector<Integer> activities;
    PRG prg;

    /* public progress logging */
    bool if_verbose = false;
    bool allow_public_early_exit = false;
    int verbose_party = 0;
    long long verbose_progress_interval = 0;
    double last_runtime_seconds = 0.0;
    long long orange_count = 0;
    long long red_count = 0;
    long long yellow_count = 0;
    long long handle_unit_clause_calls = 0;
    long long check_unit_clause_conflict_calls = 0;
    long long pick_new_clause_calls = 0;
    long long get_w_from_cur_wl_calls = 0;
    long long get_w_from_big_u_calls = 0;
    long long get_w_from_new_decision_calls = 0;
    long long possible_source_attempt_steps = 0;
    long long possible_source_refresh_steps = 0;
    long long make_decision_calls = 0;
    long long search_two_non_falsified_calls = 0;
    long long handle_conflict_w_calls = 0;
    long long handle_unit_w_calls = 0;
    long long handle_not_unit_w_calls = 0;
    long long conflict_analysis_calls = 0;
    long long backtrack_calls = 0;
    long long construct_wl_calls = 0;
    long long update_wl_calls = 0;
    long long add_implication_calls = 0;
    long long restart_calls = 0;
    // Local-only count of logical CheckSAT invocations under the public
    // schedule; no secret state is inspected by this counter.
    std::uint64_t logical_check_sat_calls = 0;

    // Optional local-only timing of the exact production giant-step body.
    // Buckets are keyed by decision (bit 0), conflict (bit 1), public periodic
    // restart (bit 2), possible source attempt (bit 3), and possible source
    // refresh (bit 4). Enabling this does not alter the circuit or reveal
    // secret state; it is used only to calibrate benchmark estimates.
    static constexpr int PUBLIC_STEP_TIMING_BUCKETS = 32;
    bool collect_public_step_timings = false;
    std::array<long long, PUBLIC_STEP_TIMING_BUCKETS> public_step_timing_counts{};
    std::array<double, PUBLIC_STEP_TIMING_BUCKETS> public_step_timing_seconds{};

    void reset_public_step_timings()
    {
        public_step_timing_counts.fill(0);
        public_step_timing_seconds.fill(0.0);
    }

    struct ConflictAnalysisResult
    {
        Integer beta;
        Integer learned_clause_idx;
        Bit valid;
        Bit cap_hit;
        Clause learned_clause;
    };

    struct IndexedCandidateChoice
    {
        Bit found;
        Integer var;
        Integer order;
    };

    struct WatchEndpointChoice
    {
        Integer first_literal;
        Integer first_position;
        Bit has_first;
        Integer second_literal;
        Integer second_position;
        Bit has_second;
    };

    WatchEndpointChoice select_watch_endpoints(const Clause &clause,
                                               bool prefer_non_falsified)
    {
        WatchEndpointChoice choice{
            Integer(VAR_SIZE_BIT, 0, PUBLIC),
            Integer(WATCH_POSITION_SIZE_BIT, 0, PUBLIC),
            Bit(false, PUBLIC),
            Integer(VAR_SIZE_BIT, 0, PUBLIC),
            Integer(WATCH_POSITION_SIZE_BIT, 0, PUBLIC),
            Bit(false, PUBLIC)};

        Integer fallback_literal(VAR_SIZE_BIT, 0, PUBLIC);
        Integer fallback_position(WATCH_POSITION_SIZE_BIT, 0, PUBLIC);
        Bit has_fallback(false, PUBLIC);

        // Learned-clause installation runs after backtracking. Prefer its
        // non-falsified (normally asserting) literal as the primary watcher,
        // but remember the first real literal as a total fallback. The public
        // mode bit only distinguishes original-clause initialization from
        // learned-clause installation; all clause and assignment data remain
        // secret circuit values.
        for (int i = 0; i < static_cast<int>(clause.literals.size()); ++i)
        {
            const Integer &literal = clause.literals[i];
            Bit valid = literal != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit take_fallback = valid & !has_fallback;
            fallback_literal = If(take_fallback, literal, fallback_literal);
            fallback_position = If(
                take_fallback,
                Integer(WATCH_POSITION_SIZE_BIT, i + 1, PUBLIC),
                fallback_position);
            has_fallback = has_fallback | take_fallback;

            Bit preferred = valid;
            if (prefer_non_falsified)
            {
                preferred = preferred & !big_v->is_literal_falsified(literal);
            }
            Bit take_first = preferred & !choice.has_first;
            choice.first_literal = If(take_first, literal, choice.first_literal);
            choice.first_position = If(
                take_first,
                Integer(WATCH_POSITION_SIZE_BIT, i + 1, PUBLIC),
                choice.first_position);
            choice.has_first = choice.has_first | take_first;
        }

        choice.first_literal = If(choice.has_first, choice.first_literal,
                                  fallback_literal);
        choice.first_position = If(choice.has_first, choice.first_position,
                                   fallback_position);
        choice.has_first = choice.has_first | has_fallback;

        // The second watcher may currently be falsified. That is necessary
        // for an asserting learned clause, whose other literals are normally
        // false immediately after backtracking. A second fixed padded scan
        // selects the first distinct literal without revealing which one won.
        // Thus [a,a,b] installs a,b while [a,a] remains effectively unit.
        for (int i = 0; i < static_cast<int>(clause.literals.size()); ++i)
        {
            const Integer &literal = clause.literals[i];
            Bit valid = literal != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit take_second = valid & choice.has_first & !choice.has_second &
                              (literal != choice.first_literal);
            choice.second_literal = If(take_second, literal, choice.second_literal);
            choice.second_position = If(
                take_second,
                Integer(WATCH_POSITION_SIZE_BIT, i + 1, PUBLIC),
                choice.second_position);
            choice.has_second = choice.has_second | take_second;
        }
        return choice;
    }

    void set_verbose(bool enabled, long long progress_interval = 0, int log_party = 0)
    {
        if_verbose = enabled;
        verbose_progress_interval = progress_interval;
        verbose_party = log_party;
    }

    std::string verbose_prefix() const
    {
        std::ostringstream oss;
        oss << "[ppCDCL";
        if (verbose_party > 0)
        {
            oss << " party " << verbose_party;
        }
        oss << "] ";
        return oss.str();
    }

    std::string format_seconds(double seconds) const
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << seconds;
        return oss.str();
    }

    void verbose_log(const std::string &message) const
    {
        if (if_verbose)
        {
            std::cout << verbose_prefix() << message << std::endl;
        }
    }

    long long effective_progress_interval(long long step_num) const
    {
        if (verbose_progress_interval > 0)
        {
            return verbose_progress_interval;
        }
        if (step_num <= 0)
        {
            return 1000;
        }
        return std::max(1LL, std::min(1000LL, step_num / 20));
    }

    int wl_scan_ctr_bits() const
    {
        return watchlist_scan_cursor_bits();
    }

    Integer wl_scan_ctr_value(int value) const
    {
        return Integer(wl_scan_ctr_bits(), value, PUBLIC);
    }

    Bit wl_scan_ctr_active() const
    {
        return wl_scan_ctr >= Integer(wl_scan_ctr.size(), 0, PUBLIC);
    }

    Integer select_cur_wl_clause(const Bit &active) const
    {
        Integer selected(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            Integer i_secret(wl_scan_ctr.size(), i, PUBLIC);
            selected = If(active & (wl_scan_ctr == i_secret), cur_wl[i], selected);
        }
        return selected;
    }

    void advance_wl_scan_ctr(const Bit &active)
    {
        Integer one(wl_scan_ctr.size(), 1, PUBLIC);
        wl_scan_ctr = If(active & wl_scan_ctr_active(), wl_scan_ctr - one, wl_scan_ctr);
    }

    void reset_verbose_counters()
    {
        orange_count = 0;
        red_count = 0;
        yellow_count = 0;
        handle_unit_clause_calls = 0;
        check_unit_clause_conflict_calls = 0;
        pick_new_clause_calls = 0;
        get_w_from_cur_wl_calls = 0;
        get_w_from_big_u_calls = 0;
        get_w_from_new_decision_calls = 0;
        possible_source_attempt_steps = 0;
        possible_source_refresh_steps = 0;
        make_decision_calls = 0;
        search_two_non_falsified_calls = 0;
        handle_conflict_w_calls = 0;
        handle_unit_w_calls = 0;
        handle_not_unit_w_calls = 0;
        conflict_analysis_calls = 0;
        backtrack_calls = 0;
        construct_wl_calls = 0;
        update_wl_calls = 0;
        add_implication_calls = 0;
        restart_calls = 0;
    }

    void verbose_log_constants(const std::string &phase) const
    {
        if (!if_verbose)
        {
            return;
        }
        std::ostringstream oss;
        oss << phase
            << ": var_num=" << VAR_NUM
            << " clause_num=" << CLAUSE_NUM
            << " max_phi_lits=" << MAX_LIT_IN_PHI_CLAUSE
            << " max_conflict_lits=" << MAX_LIT_IN_CON_CLAUSE
            << " max_conflict_clauses=" << MAX_NUM_OF_CONFLICT_CLAUSE
            << " max_watchlist=" << MAX_CLAUSE_IN_WL
            << " watchlist_strategy=" << (use_shortest_watchlist ? "shortest" : "first");
        verbose_log(oss.str());
    }

    void verbose_log_storage() const
    {
        if (!if_verbose)
        {
            return;
        }
        std::ostringstream oss;
        oss << "storage initialized: BigV "
            << (use_oram_variable_state ? "ORAM" : "linear-scan vectors")
            << ", ClauseList PHI/CON ORAMs, WatchList "
            << watch_backend_name(wl->backend_kind());
        verbose_log(oss.str());

        std::ostringstream params;
        params << "storage params: phi_oram_bits=" << PHI_ORAM_SIZE_BIT
               << " conflict_oram_bits=" << CON_ORAM_SIZE_BIT
               << " wl_oram_bits=" << WL_ORAM_SIZE_BIT
               << " phi_blocks_per_clause=" << PHI_ORAM_BLOCKS_PER_CLAUSE
               << " conflict_blocks_per_clause=" << CON_ORAM_BLOCKS_PER_CLAUSE
               << " wl_blocks_per_watchlist=" << WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL
               << " wl_entries_per_block=" << WL_ENTRIES_PER_BLOCK;
        verbose_log(params.str());
    }

    void verbose_log_begin(long long step_num, long long progress_interval) const
    {
        if (!if_verbose)
        {
            return;
        }
        std::ostringstream begin;
        begin << "begin_giant_step: step_budget=" << step_num
              << " progress_interval=" << progress_interval;
        verbose_log(begin.str());

        verbose_log(std::string("public per-step schedule: pick_new_clause -> ") +
                    (use_shortest_watchlist ? "search_two_shortest_non_falsified" : "search_two_non_falsified") +
                    " -> scheduled handle_conflict_w -> handle_unit_w -> handle_not_unit_w");

        std::ostringstream slots;
        slots << "scheduled slots: decision_delay=" << oblivious_decision_delay
              << " conflict_delay=" << oblivious_conflict_delay;
        verbose_log(slots.str());
    }

    void verbose_log_progress(long long current_step, long long step_num, double elapsed_seconds) const
    {
        if (!if_verbose)
        {
            return;
        }
        std::ostringstream oss;
        oss << "progress: step=" << current_step << "/" << step_num
            << " elapsed_s=" << format_seconds(elapsed_seconds)
            << " scheduled{core=" << orange_count
            << ",decision_slots=" << yellow_count
            << ",conflict_slots=" << red_count
            << "} calls{pick_new_clause=" << pick_new_clause_calls
            << ",make_decision=" << make_decision_calls
            << ",search_two=" << search_two_non_falsified_calls
            << ",handle_conflict_w=" << handle_conflict_w_calls
            << ",handle_unit_w=" << handle_unit_w_calls
            << ",handle_not_unit_w=" << handle_not_unit_w_calls
            << "}";
        verbose_log(oss.str());
    }

    void verbose_log_finish(const std::string &status, long long executed_steps) const
    {
        if (!if_verbose)
        {
            return;
        }
        std::ostringstream finish;
        finish << "finish: status=" << status
               << " public_steps_executed=" << executed_steps
               << " solver_runtime_s=" << format_seconds(last_runtime_seconds);
        verbose_log(finish.str());

        std::ostringstream calls;
        calls << "procedure calls: handle_unit_clause=" << handle_unit_clause_calls
              << " check_unit_clause_conflict=" << check_unit_clause_conflict_calls
              << " pick_new_clause=" << pick_new_clause_calls
              << " get_w_from_cur_wl=" << get_w_from_cur_wl_calls
              << " get_w_from_big_U=" << get_w_from_big_u_calls
              << " get_w_from_new_decision=" << get_w_from_new_decision_calls
              << " possible_source_attempt_steps=" << possible_source_attempt_steps
              << " possible_source_refresh_steps=" << possible_source_refresh_steps
              << " make_decision=" << make_decision_calls
              << " search_two=" << search_two_non_falsified_calls
              << " handle_conflict_w=" << handle_conflict_w_calls
              << " handle_unit_w=" << handle_unit_w_calls
              << " handle_not_unit_w=" << handle_not_unit_w_calls;
        verbose_log(calls.str());

        std::ostringstream subcalls;
        subcalls << "subprocedure calls: conflict_analysis=" << conflict_analysis_calls
                 << " backtrack=" << backtrack_calls
                 << " construct_wl=" << construct_wl_calls
                 << " update_wl=" << update_wl_calls
                 << " add_implication=" << add_implication_calls
                 << " restart=" << restart_calls;
        verbose_log(subcalls.str());
    }

    void init_oram()
    {
        verbose_log("initializing solver storage/ORAMs");
        big_v = new VariableStateBackend(backend, use_oram_variable_state);
        cl = new ClauseList(backend);
        wl = new WatchList(backend, watch_backend_request);
        activities.resize(VAR_NUM + 1, Integer(ACTIVITY_SIZE_BIT, 0));
        big_u.resize(VAR_NUM + 1, Integer(2, 0));
        cur_wl.resize(MAX_CLAUSE_IN_WL, Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC));
        verbose_log_storage();
    }

    ~CDCL()
    {
        delete big_v;
        delete cl;
        delete wl;
    }

    CDCL(std::string file_str, FloramMPC<NetIO> *backend, int party, int max_lit_in_wl, int max_lit_in_clause, int max_lit_in_con_clause, int max_con_clause, bool verbose = false, long long progress_interval = 0, int log_party = 0, bool use_oram_variable_state = true, bool use_shortest_watchlist = false, WatchBackendRequest watch_backend_request = WatchBackendRequest::Auto)
    {
        auto t1 = clock_start();
        this->backend = backend;
        this->use_oram_variable_state = use_oram_variable_state;
        this->use_shortest_watchlist = use_shortest_watchlist;
        this->watch_backend_request = watch_backend_request;
        set_verbose(verbose, progress_interval, log_party);
        verbose_log("constructing solver for CNF: " + file_str);
        const ppcdcl::DimacsCnf cnf = ppcdcl::parse_dimacs_file(file_str);
        if (cnf.variable_count <= 0)
        {
            throw std::runtime_error("variable number is 0");
        }
        init_constant(cnf.variable_count, cnf.declared_clause_count, max_con_clause,
                      max_lit_in_clause, max_lit_in_con_clause, max_lit_in_wl);
        verbose_log_constants("constants initialized");
        w = Clause(); // resize clause buffer after constants are initialized
        a_idx = Integer(VAR_SIZE_BIT, -1, PUBLIC);
        w_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        wl_scan_ctr = Integer(wl_scan_ctr_bits(), -1, PUBLIC);
        scanned_wl_this_step = Bit(false, PUBLIC);
        pending_conflict_clause_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        init_oram();
        if_failure = Bit(false, PUBLIC);
        int clause_idx = 1;
        for (const std::vector<int> &literals : cnf.clauses)
        {
            if (literals.empty())
            {
                // An explicit empty clause is an immediate root contradiction,
                // but it still occupies its declared clause index.
                if_unsat = Bit(true, PUBLIC);
                cl->insert_phi_clause(Integer(CLAUSE_IDX_SIZE_IN_WL, clause_idx),
                                      Clause(), Bit(true, PUBLIC));
                clause_idx += 1;
                continue;
            }
            Clause cur_clause(literals, party);
            if (literals.size() == 1)
            {
                cl->unit_clause.push_back({Integer(VAR_SIZE_BIT, literals[0]),
                                           Integer(CLAUSE_IDX_SIZE_IN_WL, clause_idx)});
            }
            cl->insert_phi_clause(Integer(CLAUSE_IDX_SIZE_IN_WL, clause_idx),
                                  cur_clause, Bit(true, PUBLIC));
            WatchEndpointChoice endpoints = select_watch_endpoints(cur_clause, false);
            WatchList::InstallResult install = wl->install_clause_watchers(
                Integer(CLAUSE_IDX_SIZE_IN_WL, clause_idx, PUBLIC),
                endpoints.first_literal, endpoints.first_position, endpoints.has_first,
                endpoints.second_literal, endpoints.second_position, endpoints.has_second,
                Bit(true, PUBLIC));
            if_failure = if_failure | install.overflow;
            clause_idx += 1;
        }
        verbose_log("CNF clauses inserted into clause and watchlist storage");

        conflict_ctr = Integer(CON_CLAUSE_IDX_SIZE_BIT, 1, PUBLIC);
        asserting_literal = Integer(VAR_SIZE_BIT, 0, PUBLIC);
        pending_conflict_clause_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        wl_scan_ctr = Integer(wl_scan_ctr_bits(), -1, PUBLIC);
        scanned_wl_this_step = Bit(false, PUBLIC);
        if_conflict_pending = Bit(false, PUBLIC);
        if_decision_pending = Bit(false, PUBLIC);
        conflict_schedule_counter = 0;
        luby_restart_index = 1;
        luby_interval_progress = 0;
        previous_restart_conflict_ctr = 0;

        if (if_debug)
        {
            wl->print_human();
            cl->print();
            big_v->print_full_state();
            std::cout << "init time: " << time_from(t1) / 1e3 / 1e3 << "s" << std::endl;
        }
        else if (if_verbose)
        {
            verbose_log("solver construction complete: init_runtime_s=" + format_seconds(time_from(t1) / 1e6));
        }
    }

    Bit if_all_assigned()
    {
        // global_order_ctr is the secret live trail length: assignments add
        // one and backtracking subtracts exactly the variables it clears.
        // Comparing it with public n is equivalent to scanning every private
        // assignment, but costs one small equality instead of O(n) gates.
        Integer live_count = big_v->live_assignment_count();
        return live_count == Integer(live_count.size(), VAR_NUM, PUBLIC);
    }

    void handle_unit_clause(Bit flag = true)
    {
        if (if_verbose)
        {
            handle_unit_clause_calls += 1;
        }
        check_unit_clause_conflict();
        for (auto i : cl->unit_clause)
        {
            Bit valid_unit = i.first != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            add_implication(i.first, i.second, Integer(DL_SIZE_BIT, 0, PUBLIC), flag & valid_unit & !if_unsat);
        }
    }

    void push_unit_clause_to_big_U(const Bit &flag = Bit(true, PUBLIC))
    {
        // Push every variable that is assigned at decision level 0 into big_u.
        for (int i = 1; i < VAR_NUM + 1; ++i)
        {
            Integer var_dl;
            Integer var_ass;
            big_v->get_decision_level(i, var_dl);
            big_v->get_assignment(i, var_ass);
            Bit is_root_level = (var_dl == Integer(DL_SIZE_BIT, 0));
            Bit is_assigned = var_ass != big_v->not_assigned();
            Integer val = If(var_ass == big_v->true_assigned(),
                             Integer(2, 1, PUBLIC),
                             If(var_ass == big_v->false_assigned(),
                                Integer(2, 2, PUBLIC),
                                Integer(2, 0, PUBLIC)));
            val = If(is_root_level & is_assigned, val, Integer(2, 0, PUBLIC));
            big_u[i] = If(flag, val, big_u[i]);
        }
    }

    void check_unit_clause_conflict()
    {
        if (if_verbose)
        {
            check_unit_clause_conflict_calls += 1;
        }
        // Search for a literal i whose negation -i is also in unit_clause
        for (int i = 0; i < cl->unit_clause.size(); ++i)
        {
            Integer lit_i = cl->unit_clause[i].first;

            for (int j = i + 1; j < cl->unit_clause.size(); ++j)
            {
                Integer lit_j = cl->unit_clause[j].first;
                Bit are_contradicting = (lit_i != Integer(VAR_SIZE_BIT, 0, PUBLIC)) &
                                        (lit_j != Integer(VAR_SIZE_BIT, 0, PUBLIC)) &
                                        lit_i.equal(-lit_j);
                if_unsat = if_unsat | are_contradicting;
            }
        }
    }

    void handle_conflict_w(const std::vector<Integer> &non_falsified_literals,
                           const Integer &conflict_clause_idx,
                           const Bit &flag)
    {
        if (if_verbose)
        {
            handle_conflict_w_calls += 1;
        }
        ConflictAnalysisResult analysis = conflict_analysis(conflict_clause_idx, flag);
        if_unsat = if_unsat | (analysis.valid & (big_v->cur_dl() == Integer(DL_SIZE_BIT, 0, PUBLIC)));
        backtrack(analysis.beta, analysis.valid);
        backtrack_aftermath(analysis.valid, analysis.learned_clause_idx,
                            analysis.learned_clause);
        restart(analysis.cap_hit);
    }

    void handle_unit_w(const std::vector<Integer> &non_falsified_literals, const Bit &flag)
    {
        if (if_verbose)
        {
            handle_unit_w_calls += 1;
        }
        // The single non-falsified literal is implied; add_implication records it
        // in big_v and big_u unless it is already assigned.
        Integer unit_lit = non_falsified_literals[0];
        add_implication(unit_lit, w_idx, big_v->cur_dl(), flag);
    }

    void handle_not_unit_w(const std::vector<Integer> &non_falsified_literals, const Bit &flag)
    {
        if (if_verbose)
        {
            handle_not_unit_w_calls += 1;
        }
        update_wl_from_frame(w_idx, -a_idx, wl_scan_ctr, w,
                             non_falsified_literals, flag);
    }

    void terminate(bool if_sat)
    {
        if (if_sat)
        {
            std::cout << "SAT!!!" << std::endl;
        }
        else
        {
            std::cout << "UNSAT!!!" << std::endl;
        }
    }

    int reveal_terminal_status()
    {
        Integer status(2, TERMINAL_RUNNING, PUBLIC);
        status = If(if_sat, Integer(2, TERMINAL_SAT, PUBLIC), status);
        status = If(if_unsat, Integer(2, TERMINAL_UNSAT, PUBLIC), status);
        status = If(if_failure, Integer(2, TERMINAL_FAILURE, PUBLIC), status);
        return reveal_32(status, true);
    }

    int begin_giant_step(int step_num = -1)
    {
        auto solve_timer = clock_start();
        const bool resume_with_conservative_source_work = giant_step_has_run;
        giant_step_has_run = true;
        reset_verbose_counters();
        long long progress_interval = effective_progress_interval(step_num);
        verbose_log_begin(step_num, progress_interval);

        step_idx = 0;
        if (collect_public_step_timings)
        {
            reset_public_step_timings();
        }
        auto finish_failure = [&]() {
            last_runtime_seconds = time_from(solve_timer) / 1e6;
            verbose_log_finish("FAILURE", step_idx);
            std::cout << "FAILURE!" << std::endl;
            return SOLVER_RESULT_FAILURE;
        };
        if (if_failure.reveal())
        {
            return finish_failure();
        }

        handle_unit_clause();
        if (if_failure.reveal())
        {
            return finish_failure();
        }

        const std::uint64_t scheduled_steps =
            step_num > 0 ? static_cast<std::uint64_t>(step_num) : 0;
        const ppcdcl::policy::PossibleSourceWorkSchedule source_work_schedule =
            resume_with_conservative_source_work
                ? ppcdcl::policy::build_conservative_source_work_schedule(scheduled_steps)
                : ppcdcl::policy::build_possible_source_work_schedule(
                      scheduled_steps,
                      static_cast<std::uint64_t>(MAX_CLAUSE_IN_WL),
                      static_cast<std::uint64_t>(std::max(1, oblivious_decision_delay)),
                      static_cast<std::uint64_t>(std::max(1, oblivious_conflict_delay)));

        int public_terminal_status = TERMINAL_RUNNING;
        bool terminal_status_revealed = false;
        for (; step_idx < step_num; ++step_idx)
        {
            auto public_step_timer = clock_start();
            const std::uint64_t logical_t = static_cast<std::uint64_t>(step_idx) + 1;
            bool conflict_trigger_pub = ppcdcl::policy::is_modulo_one_slot(
                logical_t, static_cast<std::uint64_t>(std::max(1, oblivious_conflict_delay)));
            bool decision_slot_pub = ppcdcl::policy::is_modulo_one_slot(
                logical_t, static_cast<std::uint64_t>(std::max(1, oblivious_decision_delay)));
            const bool source_attempt_slot_pub =
                source_work_schedule.source_attempts[static_cast<std::size_t>(step_idx)] != 0;
            const bool source_refresh_slot_pub =
                source_work_schedule.source_refreshes[static_cast<std::size_t>(step_idx)] != 0;
            bool periodic_restart_pub = false;
            if (if_verbose)
            {
                orange_count += 1;
            }
            Bit solver_active = !if_unsat & !if_sat;
            Bit pending_before = if_conflict_pending & solver_active;
            pick_new_clause(solver_active & !pending_before,
                            source_attempt_slot_pub, source_refresh_slot_pub);
            std::vector<Integer> non_falsified_literals = use_shortest_watchlist
                                                              ? search_two_shortest_non_falsified(w_idx, -a_idx)
                                                              : search_two_non_falsified(w_idx, -a_idx);
            // Without an active clause, step_state is STEP_STATE_NO_WL.
            Bit active_clause = solver_active & !pending_before & !(w_idx == Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC));
            step_state = If(!active_clause, STEP_STATE_NO_WL,
                            If(non_falsified_literals[0] == Integer(VAR_SIZE_BIT, 0, PUBLIC), STEP_STATE_CONFLICT,
                               If(non_falsified_literals[0] == non_falsified_literals[1], STEP_STATE_UNIT,
                                  STEP_STATE_NON_UNIT)));

            Bit is_conflict = step_state == STEP_STATE_CONFLICT;
            Bit is_unit = step_state == STEP_STATE_UNIT;
            Bit is_non_unit = step_state == STEP_STATE_NON_UNIT;

            if (if_verbose && conflict_trigger_pub)
            {
                red_count += 1;
            }
            if (if_verbose && decision_slot_pub)
            {
                yellow_count += 1;
            }
            Bit new_conflict = active_clause & is_conflict & !if_conflict_pending;
            pending_conflict_clause_idx = If(new_conflict, w_idx, pending_conflict_clause_idx);
            Integer conflict_idx_to_handle = If(if_conflict_pending, pending_conflict_clause_idx, w_idx);
            Bit conflict_pending = if_conflict_pending | new_conflict;
            if (conflict_trigger_pub)
            {
                Bit do_conflict_now = conflict_pending;
                if_conflict_pending = Bit(false, PUBLIC);
                handle_conflict_w(non_falsified_literals, conflict_idx_to_handle, do_conflict_now);
                pending_conflict_clause_idx = If(do_conflict_now, Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC), pending_conflict_clause_idx);
            }
            else
            {
                if_conflict_pending = conflict_pending;
            }

            Bit do_regular_update = !conflict_pending;
            handle_unit_w(non_falsified_literals, is_unit & do_regular_update);
            handle_not_unit_w(non_falsified_literals, is_non_unit & do_regular_update);

            if (conflict_trigger_pub)
            {
                conflict_schedule_counter += 1;
                luby_interval_progress += 1;
                const std::uint64_t interval = ppcdcl::policy::luby_interval(luby_restart_index);
                if (luby_interval_progress >= interval)
                {
                    periodic_restart_pub = true;
                    luby_interval_progress = 0;
                    luby_restart_index += 1;
                    restart_ctr += 1;
                    restart(solver_active & !if_sat & !if_unsat);
                }
            }

            advance_wl_scan_ctr(scanned_wl_this_step & !conflict_pending);

            if (allow_public_early_exit)
            {
                public_terminal_status = reveal_terminal_status();
                terminal_status_revealed = true;
            }
            else
            {
                public_terminal_status = if_failure.reveal() ? TERMINAL_FAILURE : TERMINAL_RUNNING;
            }

            if (collect_public_step_timings)
            {
                const int timing_bucket = (decision_slot_pub ? 1 : 0) |
                                          (conflict_trigger_pub ? 2 : 0) |
                                          (periodic_restart_pub ? 4 : 0) |
                                          (source_attempt_slot_pub ? 8 : 0) |
                                          (source_refresh_slot_pub ? 16 : 0);
                public_step_timing_counts[timing_bucket] += 1;
                public_step_timing_seconds[timing_bucket] +=
                    time_from(public_step_timer) / 1e6;
            }

            if (public_terminal_status == TERMINAL_FAILURE)
            {
                step_idx += 1;
                return finish_failure();
            }

            if (if_verbose && progress_interval > 0 && ((step_idx + 1) % progress_interval == 0 || step_idx + 1 == step_num))
            {
                verbose_log_progress(step_idx + 1, step_num, time_from(solve_timer) / 1e6);
            }
            if (allow_public_early_exit && public_terminal_status != TERMINAL_RUNNING)
            {
                step_idx += 1;
                break;
            }
        }

        last_runtime_seconds = time_from(solve_timer) / 1e6;
        long long executed_steps = step_idx;
        if (!allow_public_early_exit || !terminal_status_revealed)
        {
            public_terminal_status = reveal_terminal_status();
        }
        if (public_terminal_status == TERMINAL_FAILURE)
        {
            verbose_log_finish("FAILURE", executed_steps);
            std::cout << "FAILURE!" << std::endl;
            return SOLVER_RESULT_FAILURE;
        }
        if (public_terminal_status == TERMINAL_UNSAT)
        {
            verbose_log_finish("UNSAT", executed_steps);
            terminate(false);
            return 0;
        }
        else if (public_terminal_status == TERMINAL_SAT)
        {
            verbose_log_finish("SAT", executed_steps);
            terminate(true);
            return 1;
        }
        else
        {
            verbose_log_finish("TIMEOUT", executed_steps);
            std::cout << "TIMEOUT!" << std::endl;
            return -1;
        }
    }

    Integer pick_variable_VSIDS()
    {
        Integer max_score(ACTIVITY_SIZE_BIT, 0, PUBLIC);
        Integer var_with_max_activity(VAR_SIZE_BIT, 0, PUBLIC);
        Integer var_saved_phase = Integer(ASSIGNMENT_SIZE_BIT, 0);
        Bit found_unassigned = Bit(false, PUBLIC);

        // Linear scan over all variables.
        for (int i = 1; i < VAR_NUM + 1; ++i)
        {
            Bit is_unassigned = big_v->is_variable_unassigned(i);

            // Take this variable if it is unassigned and either the first such variable or higher in activity.
            Bit is_higher_activity = activities[i] > max_score;
            Bit should_update = is_unassigned & (!found_unassigned | is_higher_activity);

            max_score = If(should_update, activities[i], max_score);
            var_with_max_activity = If(should_update, Integer(VAR_SIZE_BIT, i), var_with_max_activity);
            Integer phase(ASSIGNMENT_SIZE_BIT, 0, PUBLIC);
            big_v->get_phase(i, phase);
            var_saved_phase = If(should_update, phase, var_saved_phase);
            found_unassigned = found_unassigned | is_unassigned;
        }

        Bit if_no_phase = var_saved_phase.equal(big_v->not_assigned());

        // Use positive polarity for an unseen variable. Once a variable has a
        // saved phase, that phase still takes precedence over the default.
        Bit use_positive_polarity = if_no_phase | (var_saved_phase == big_v->true_assigned());
        Integer chosen_literal = If(use_positive_polarity,
                                    var_with_max_activity,
                                    -var_with_max_activity);
        if (if_debug)
        {
            std::cout << "decision: " << reveal_32(chosen_literal) << std::endl;
        }
        return chosen_literal;
    }

    Integer make_decision(const Bit &flag = Bit(true, PUBLIC),
                          bool enqueue_in_big_u = true)
    {
        if (if_verbose)
        {
            make_decision_calls += 1;
        }
        Integer assigned = pick_variable_VSIDS();
        Bit decision_valid = flag & (assigned != Integer(VAR_SIZE_BIT, 0, PUBLIC));

        // Increment the level only for a real variable. The scheduled caller
        // requests a decision only after CheckSAT shows that a variable remains
        // unassigned; the guard keeps the live trail invariant for direct calls.
        Integer next_dl = big_v->cur_dl() + Integer(DL_SIZE_BIT, 1, PUBLIC);
        big_v->set_cur_dl(If(decision_valid, next_dl, big_v->cur_dl()));

        big_v->set_decision(assigned, big_v->cur_dl(), decision_valid);

        // Standalone callers enqueue the decision in big_u. The solver's source
        // path consumes the new decision immediately, so it disables the
        // enqueue.
        if (enqueue_in_big_u)
        {
            for (int i = 0; i < big_u.size(); ++i)
            {
                // Entry i becomes 1 if the decision is i and 2 if it is -i.
                big_u[i] = If(decision_valid & (assigned == Integer(VAR_SIZE_BIT, i, PUBLIC)), Integer(2, 1, PUBLIC),
                              If(decision_valid & (assigned == Integer(VAR_SIZE_BIT, -i, PUBLIC)), Integer(2, 2, PUBLIC), big_u[i]));
            }
        }

        return assigned;
    }
    void get_w_from_cur_wl()
    {
        if (if_verbose)
        {
            get_w_from_cur_wl_calls += 1;
        }
        // Nothing to fetch: the next clause comes from cur_wl.
    }

    Bit get_w_from_big_U(const Bit &consume = Bit(true, PUBLIC))
    {
        if (if_verbose)
        {
            get_w_from_big_u_calls += 1;
        }
        Integer two_bit_one(2, 1);
        Integer two_bit_two(2, 2);
        Integer selected_lit(VAR_SIZE_BIT, 0, PUBLIC);
        Bit found_any = Bit(false, PUBLIC);

        for (int i = 0; i < big_u.size(); ++i)
        {
            // Classify each two-bit queue entry once and reuse the secret
            // predicates for validity and polarity selection.
            Bit is_positive = big_u[i] == two_bit_one;
            Bit is_negative = big_u[i] == two_bit_two;
            Bit is_valid_element = is_positive | is_negative;
            Bit is_first_valid = is_valid_element & !found_any;

            selected_lit = If(is_first_valid & is_positive, Integer(VAR_SIZE_BIT, i, PUBLIC),
                              If(is_first_valid & is_negative, Integer(VAR_SIZE_BIT, -i, PUBLIC), selected_lit));

            big_u[i] = If(consume & is_first_valid, big_v->not_assigned(), big_u[i]);
            found_any = found_any | is_valid_element;
        }
        a_idx = If(consume & found_any, selected_lit, a_idx);
        return found_any;
    }

    void get_w_from_new_decision(const Bit &active = Bit(true, PUBLIC))
    {
        if (if_verbose)
        {
            get_w_from_new_decision_calls += 1;
        }
        Bit do_decision = active & (pnc_state == PNC_STATE_DECISION);
        Integer assigned = make_decision(do_decision, false);
        a_idx = If(do_decision, assigned, a_idx);
    }

    void pick_new_clause(const Bit &active = Bit(true, PUBLIC),
                         bool source_attempt_slot_pub = true,
                         bool source_refresh_slot_pub = true)
    {
        if (source_attempt_slot_pub && !source_refresh_slot_pub)
        {
            throw std::invalid_argument(
                "a public source-attempt slot must also permit source refresh");
        }
        if (if_verbose)
        {
            pick_new_clause_calls += 1;
        }
        scanned_wl_this_step = Bit(false, PUBLIC);
        Bit scan_active_before = active & wl_scan_ctr_active();
        pnc_state = If(scan_active_before, PNC_STATE_CUR_WL, PNC_STATE_IDLE);
        get_w_from_cur_wl();

        Bit need_new_source = active & !scan_active_before;
        Bit pending_decision_before = if_decision_pending;
        Bit may_consume_big_u = need_new_source & !pending_decision_before;
        Bit big_u_found = Bit(false, PUBLIC);
        Bit sat_now = Bit(false, PUBLIC);
        if (source_attempt_slot_pub)
        {
            possible_source_attempt_steps += 1;
            big_u_found = get_w_from_big_U(may_consume_big_u);
            // CheckSAT runs only on a public source-attempt step: that scan is
            // the only one that can show no unit literal remains, and it
            // happens before a new decision is requested. The oblivious scan's
            // secret result is reused instead of rescanning big_u.
            Bit sat_eligible = need_new_source & !pending_decision_before & !big_u_found;
            sat_now = check_sat_with_known_empty_big_u(sat_eligible);
        }
        // Outside a public source-attempt slot, a fresh big_u lookup is
        // impossible. A secret pending decision may still be waiting for its
        // public activation slot, so that state is preserved without scanning
        // big_u.
        Bit decision_requested = source_attempt_slot_pub
                                     ? need_new_source & (pending_decision_before | !big_u_found)
                                     : need_new_source & pending_decision_before;
        decision_requested = decision_requested & !sat_now;
        Bit use_big_u = source_attempt_slot_pub
                            ? may_consume_big_u & big_u_found
                            : Bit(false, PUBLIC);
        pnc_state = If(decision_requested, PNC_STATE_DECISION,
                       If(use_big_u, PNC_STATE_BIG_U,
                          If(scan_active_before, PNC_STATE_CUR_WL, PNC_STATE_IDLE)));

        const std::uint64_t logical_t = static_cast<std::uint64_t>(step_idx) + 1;
        bool decision_slot_pub = ppcdcl::policy::is_modulo_one_slot(
            logical_t, static_cast<std::uint64_t>(std::max(1, oblivious_decision_delay)));
        Bit decision_slot = Bit(decision_slot_pub, PUBLIC);
        Bit do_decision_now = decision_requested & decision_slot;
        if_decision_pending = decision_requested & !decision_slot;

        if (decision_slot_pub && source_refresh_slot_pub)
        {
            get_w_from_new_decision(do_decision_now);
        }

        Bit refresh_wl = do_decision_now | use_big_u;
        if (source_refresh_slot_pub)
        {
            possible_source_refresh_steps += 1;
            Integer wl_lit = If(refresh_wl, -a_idx, Integer(VAR_SIZE_BIT, 0, PUBLIC));
            std::vector<Integer> fetched_wl = wl->get_watchlist_clauses(wl_lit);
            for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
            {
                cur_wl[i] = If(refresh_wl, fetched_wl[i], cur_wl[i]);
            }
            wl_scan_ctr = If(refresh_wl, wl_scan_ctr_value(MAX_CLAUSE_IN_WL - 1), wl_scan_ctr);
        }

        Bit scan_active_after = active & !if_decision_pending & wl_scan_ctr_active();
        scanned_wl_this_step = scan_active_after;
        // Take the next clause from the cached watchlist.
        w_idx = select_cur_wl_clause(scan_active_after);
    }

    void backtrack_aftermath(const Bit &flag, const Integer &learned_idx,
                             const Clause &learned_clause)
    {
        Integer ctr_resized = conflict_ctr;
        ctr_resized.resize(CLAUSE_IDX_SIZE_IN_WL);
        Integer fallback_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC) -
                               (ctr_resized - Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC));
        Integer last_conflict = If(flag, learned_idx, fallback_idx);
        construct_wl_from_clause(last_conflict, learned_clause, flag);
        Integer ante_idx = last_conflict;
        ante_idx.resize(ANTE_SIZE_BIT);
        add_implication(asserting_literal, ante_idx, big_v->cur_dl(), flag);
    }

    void restart(const Bit &flag = Bit(true, PUBLIC))
    {
        if (if_verbose)
        {
            restart_calls += 1;
        }
        backtrack(Integer(DL_SIZE_BIT, 0, PUBLIC), flag);
        if_conflict_pending = if_conflict_pending & !flag;
        if_decision_pending = if_decision_pending & !flag;
        if_sat = if_sat & !flag;
        a_idx = If(flag, Integer(VAR_SIZE_BIT, -1, PUBLIC), a_idx);
        w_idx = If(flag, Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC), w_idx);
        push_unit_clause_to_big_U(flag);
    }

    Bit commit_sat_if_all_assigned(const Bit &flag)
    {
        Bit sat_now = flag & if_all_assigned();
        if_sat = if_sat | sat_now;
        return if_sat;
    }

    Bit check_sat_with_known_empty_big_u(const Bit &flag)
    {
        logical_check_sat_calls += 1;
        return commit_sat_if_all_assigned(flag);
    }

    Bit check_sat(const Bit &flag = Bit(true, PUBLIC))
    {
        logical_check_sat_calls += 1;
        Bit if_big_U_empty = Bit(true, PUBLIC);
        for (auto i : big_u)
        {
            if_big_U_empty = if_big_U_empty & (i == Integer(2, 0, PUBLIC));
        }
        return commit_sat_if_all_assigned(flag & if_big_U_empty);
    }

    // search for two non-falsified literals in a clause, return one or two literals
    std::vector<Integer> search_two_non_falsified(Integer clause_idx, const Integer &fresh_falsified_lit)
    {
        if (if_verbose)
        {
            search_two_non_falsified_calls += 1;
        }
        vector<Integer> non_falsified_literals = {Integer(VAR_SIZE_BIT, 0), Integer(VAR_SIZE_BIT, 0)};

        // Load the clause into the member w.
        cl->get_clause(clause_idx, w);

        // - If 0 non-falsified literals found: result is {0,0}
        // - If 1 non-falsified literal found: result is {l,l}
        // - If 2+ non-falsified literals found: result is {first,second}
        for (auto const &l : w.literals)
        {
            Bit is_nonzero = l != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit if_l_is_not_falsified = !big_v->is_literal_falsified(l);
            Bit if_l_is_not_fresh_falsified = !l.equal(fresh_falsified_lit);
            Bit is_valid_literal = is_nonzero & if_l_is_not_falsified &
                                   if_l_is_not_fresh_falsified;

            Bit is_first_zero = non_falsified_literals[0] == Integer(VAR_SIZE_BIT, 0);
            Bit is_second_zero = non_falsified_literals[1] == non_falsified_literals[0];

            non_falsified_literals[0] = If(is_first_zero & is_valid_literal,
                                           l,
                                           non_falsified_literals[0]);

            // Update second position if it equals first position (either both zero or only one found so far)
            non_falsified_literals[1] = If(is_second_zero & is_valid_literal,
                                           l,
                                           non_falsified_literals[1]);
        }

        return non_falsified_literals;
    }

    std::vector<Integer> search_two_shortest_non_falsified(Integer clause_idx, const Integer &fresh_falsified_lit)
    {
        if (if_verbose)
        {
            search_two_non_falsified_calls += 1;
        }
        std::vector<Integer> best_literals = {Integer(VAR_SIZE_BIT, 0, PUBLIC),
                                              Integer(VAR_SIZE_BIT, 0, PUBLIC)};
        Integer best_size(CLAUSE_IDX_SIZE_IN_WL, MAX_CLAUSE_IN_WL + 1, PUBLIC);
        Integer second_best_size(CLAUSE_IDX_SIZE_IN_WL, MAX_CLAUSE_IN_WL + 1, PUBLIC);

        cl->get_clause(clause_idx, w);

        for (auto const &l : w.literals)
        {
            Bit is_nonzero = l != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit if_l_is_not_falsified = !big_v->is_literal_falsified(l);
            Bit if_l_is_not_fresh_falsified = !l.equal(fresh_falsified_lit);
            Bit is_distinct = (l != best_literals[0]) & (l != best_literals[1]);
            Bit is_valid_literal = is_nonzero & if_l_is_not_falsified &
                                   if_l_is_not_fresh_falsified & is_distinct;

            Integer cur_size = wl->watchlist_size(l);
            Bit better_best = is_valid_literal & (cur_size < best_size);
            Bit better_second = is_valid_literal & !better_best & (cur_size < second_best_size);

            Integer old_best_size = best_size;
            Integer old_best_literal = best_literals[0];

            best_size = If(better_best, cur_size, best_size);
            best_literals[0] = If(better_best, l, best_literals[0]);

            second_best_size = If(better_best, old_best_size,
                                  If(better_second, cur_size, second_best_size));
            best_literals[1] = If(better_best, old_best_literal,
                                  If(better_second, l, best_literals[1]));
        }

        Bit one_found = (best_literals[0] != Integer(VAR_SIZE_BIT, 0, PUBLIC)) &
                        (best_literals[1] == Integer(VAR_SIZE_BIT, 0, PUBLIC));
        best_literals[1] = If(one_found, best_literals[0], best_literals[1]);
        return best_literals;
    }

    // add implication to big_v and big_u
    void add_implication(const Integer &literal, const Integer &antecedent, const Integer &dl, Bit flag = true)
    {
        if (if_verbose)
        {
            add_implication_calls += 1;
        }
        Bit did_assign = big_v->try_set_unit_literal(literal, antecedent, dl, flag);

        Integer val = If(literal >= Integer(VAR_SIZE_BIT, 0, PUBLIC), Integer(2, 1, PUBLIC), Integer(2, 2, PUBLIC));
        assign_vector(big_u, literal.abs(), val, did_assign);
    }

    // check if clause c only contains one literal that is of the current decision level
    Bit if_UIP(const Clause &c)
    {
        Integer literal_ctr(VAR_SIZE_BIT, 0, PUBLIC);
        for (int i = 0; i < c.literals.size(); ++i)
        {
            Integer cur_lit_dl;
            big_v->get_decision_level(c.literals[i].abs(), cur_lit_dl);
            Bit is_nonzero = c.literals[i] != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            literal_ctr = If(is_nonzero & (cur_lit_dl == big_v->cur_dl()),
                             literal_ctr + Integer(VAR_SIZE_BIT, 1, PUBLIC),
                             literal_ctr);
        }
        return literal_ctr == Integer(VAR_SIZE_BIT, 1, PUBLIC);
    }

    Clause resolve_clause(const Clause &cur, const Clause &ante, const Integer &pivot, const Bit &flag)
    {
        Clause result;
        Integer pivot_abs = pivot.abs();
        Integer next_pos(VAR_SIZE_BIT, 0, PUBLIC);
        for (int src = 0; src < 2; ++src)
        {
            const std::vector<Integer> &src_lits = (src == 0) ? cur.literals : ante.literals;
            for (int i = 0; i < src_lits.size(); ++i)
            {
                Integer lit = src_lits[i];
                Bit is_nonzero = lit != Integer(VAR_SIZE_BIT, 0, PUBLIC);
                Bit same_var = lit.abs() == pivot_abs;
                Bit candidate = flag & is_nonzero & !same_var;

                Bit is_duplicate = Bit(false, PUBLIC);
                for (int j = 0; j < result.literals.size(); ++j)
                {
                    is_duplicate = is_duplicate | result.literals[j].equal(lit);
                }

                Integer target_pos = next_pos;
                Bit can_place = candidate & !is_duplicate &
                                (target_pos < Integer(VAR_SIZE_BIT, (int)result.literals.size(), PUBLIC));

                for (int j = 0; j < result.literals.size(); ++j)
                {
                    Bit place_here = can_place & target_pos.equal(Integer(VAR_SIZE_BIT, j, PUBLIC));
                    result.literals[j] = If(place_here, lit, result.literals[j]);
                }

                next_pos = If(can_place, next_pos + Integer(VAR_SIZE_BIT, 1, PUBLIC), next_pos);
            }
        }
        return result;
    }

    void bump_clause_variables(const Clause &c, const Bit &flag)
    {
        for (int i = 1; i < VAR_NUM + 1; ++i)
        {
            Bit in_clause = Bit(false, PUBLIC);
            Integer idx(VAR_SIZE_BIT, i, PUBLIC);
            for (int j = 0; j < c.literals.size(); ++j)
            {
                in_clause = in_clause | (c.literals[j].abs() == idx);
            }
            Integer inc = If(flag & in_clause,
                             Integer(ACTIVITY_SIZE_BIT, activity_bump, PUBLIC),
                             Integer(ACTIVITY_SIZE_BIT, 0, PUBLIC));
            activities[i] = activities[i] + inc;
        }
    }

    Bit add_literal_to_clause(Clause &out, const Integer &lit, const Bit &flag)
    {
        Bit duplicate = Bit(false, PUBLIC);
        for (int i = 0; i < out.literals.size(); ++i)
        {
            duplicate = duplicate | out.literals[i].equal(lit);
        }

        Bit inserted = Bit(false, PUBLIC);
        for (int i = 0; i < out.literals.size(); ++i)
        {
            Bit empty = out.literals[i] == Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit learned_slot = Bit(i < MAX_LIT_IN_CON_CLAUSE, PUBLIC);
            Bit place_here = flag & !duplicate & learned_slot & empty & !inserted;
            out.literals[i] = If(place_here, lit, out.literals[i]);
            inserted = inserted | place_here;
        }
        return flag & !duplicate & !inserted;
    }

    Bit mark_seen_touched_and_candidate(
        std::vector<Bit> &seen,
        std::vector<Bit> &touched,
        std::vector<Bit> &candidate_active,
        std::vector<Integer> &candidate_order,
        const Integer &idx,
        const Integer &order,
        const Bit &eligible,
        const Bit &at_current_level)
    {
        Bit marked(false, PUBLIC);
        // Variable identities are secret, but the domain 1..VAR_NUM is
        // public. A fixed sweep over that domain avoids secret-dependent
        // memory access.
        for (int i = 1; i < VAR_NUM + 1; ++i)
        {
            Bit match = idx == Integer(idx.size(), i, PUBLIC);
            Bit mark_here = eligible & match & !seen[i];
            Bit candidate_here = mark_here & at_current_level;

            // mark_here implies !seen[i], so XOR is equivalent to OR and is
            // free in a garbled circuit.
            seen[i] = seen[i] ^ mark_here;
            touched[i] = touched[i] | mark_here;
            candidate_active[i] = candidate_active[i] | candidate_here;
            candidate_order[i] = If(candidate_here, order,
                                    candidate_order[i]);

            // Exactly one public position can match a nonzero secret index.
            marked = marked ^ mark_here;
        }
        return marked;
    }

    IndexedCandidateChoice select_indexed_candidate(
        const std::vector<Bit> &candidate_active,
        const std::vector<Integer> &candidate_order,
        const Bit &flag)
    {
        IndexedCandidateChoice choice{
            Bit(false, PUBLIC),
            Integer(VAR_SIZE_BIT, 0, PUBLIC),
            Integer(candidate_order[0].size(), 0, PUBLIC)};

        for (int i = 1; i < VAR_NUM + 1; ++i)
        {
            Bit valid = flag & candidate_active[i];
            Bit better = valid &
                         (!choice.found | (candidate_order[i] > choice.order));
            choice.var = If(better, Integer(VAR_SIZE_BIT, i, PUBLIC),
                            choice.var);
            choice.order = If(better, candidate_order[i], choice.order);
            choice.found = choice.found | valid;
        }
        return choice;
    }

    void clear_indexed_candidate(std::vector<Bit> &seen,
                                 std::vector<Bit> &candidate_active,
                                 const Integer &idx,
                                 const Bit &flag)
    {
        for (int i = 1; i < VAR_NUM + 1; ++i)
        {
            Bit clear_here = flag &
                             (idx == Integer(idx.size(), i, PUBLIC));
            seen[i] = seen[i] ^ (clear_here & seen[i]);
            candidate_active[i] = candidate_active[i] ^
                                  (clear_here & candidate_active[i]);
        }
    }

    void compute_beta_below_current(const Clause &c, Integer &beta, const Bit &flag)
    {
        for (int i = 0; i < c.literals.size(); ++i)
        {
            Integer lit = c.literals[i];
            Integer dl;
            big_v->get_decision_level(lit.abs(), dl);
            Bit is_nonzero = lit != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit below_current = dl < big_v->cur_dl();
            beta = If(flag & is_nonzero & below_current & (dl > beta), dl, beta);
        }
    }

    void bump_touched_variables(const std::vector<Bit> &touched, const Clause &learned, const Bit &flag)
    {
        for (int i = 1; i < VAR_NUM + 1; ++i)
        {
            Integer idx(VAR_SIZE_BIT, i, PUBLIC);
            Bit should_bump = touched[i];
            for (int j = 0; j < learned.literals.size(); ++j)
            {
                should_bump = should_bump | (learned.literals[j].abs() == idx);
            }

            Integer inc = If(flag & should_bump,
                             Integer(ACTIVITY_SIZE_BIT, activity_bump, PUBLIC),
                             Integer(ACTIVITY_SIZE_BIT, 0, PUBLIC));
            activities[i] = activities[i] + inc;
        }
    }

    void compute_asserting_and_beta(const Clause &c, Integer &alpha, Integer &beta, const Bit &flag)
    {
        Bit alpha_set = Bit(false, PUBLIC);
        for (int i = 0; i < c.literals.size(); ++i)
        {
            Integer lit = c.literals[i];
            Integer dl;
            big_v->get_decision_level(lit.abs(), dl);
            Bit is_nonzero = lit != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit set_alpha = flag & is_nonzero & (dl == big_v->cur_dl()) & !alpha_set;
            alpha = If(set_alpha, lit, alpha);
            alpha_set = alpha_set | (flag & is_nonzero & (dl == big_v->cur_dl()));
        }

        for (int i = 0; i < c.literals.size(); ++i)
        {
            Integer lit = c.literals[i];
            Integer dl;
            big_v->get_decision_level(lit.abs(), dl);
            Bit is_nonzero = lit != Integer(VAR_SIZE_BIT, 0, PUBLIC);
            Bit is_alpha = alpha_set & lit.equal(alpha);
            Bit consider = flag & is_nonzero & !is_alpha;
            beta = If(consider & (dl > beta), dl, beta);
        }
    }

    ConflictAnalysisResult conflict_analysis(const Integer &conflict_clause_idx, const Bit &flag)
    {
        if (if_verbose)
        {
            conflict_analysis_calls += 1;
        }
        ConflictAnalysisResult result{Integer(DL_SIZE_BIT, 0, PUBLIC),
                                      Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC),
                                      Bit(false, PUBLIC),
                                      Bit(false, PUBLIC),
                                      Clause()};
        Bit valid_conflict = flag & !(conflict_clause_idx == Integer(conflict_clause_idx.size(), 0, PUBLIC));
        Bit root_conflict = valid_conflict & (big_v->cur_dl() == Integer(DL_SIZE_BIT, 0, PUBLIC));
        if_unsat = if_unsat | root_conflict;

        Bit do_analysis = valid_conflict & !root_conflict;
        Bit stopped = Bit(false, PUBLIC);
        Bit uip_cap = Bit(false, PUBLIC);

        Integer c_idx = conflict_clause_idx;
        c_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
        Integer pivot(VAR_SIZE_BIT, 0, PUBLIC);
        Integer pathC(VAR_SIZE_BIT, 0, PUBLIC);

        int oc_bits = big_v->order_ctr_bitlen();
        std::vector<Bit> seen(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> touched(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> candidate_active(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Integer> candidate_order(VAR_NUM + 1,
                                             Integer(oc_bits, 0, PUBLIC));
        Clause out_clause;
        Bit learned_length_cap = Bit(false, PUBLIC);

        // Both the configured UIP bound and VAR_NUM are public. Conflict
        // analysis resolves on at most VAR_NUM distinct pivots, so capping the
        // round count at VAR_NUM avoids dummy rounds without changing solver
        // semantics or leakage.
        int analysis_rounds = uip_loop_cap > 0
                                  ? std::min(uip_loop_cap, VAR_NUM)
                                  : VAR_NUM;
        last_conflict_analysis_rounds = analysis_rounds;
        for (int round = 0; round < analysis_rounds; ++round)
        {
            Clause c;
            cl->get_clause(c_idx, c);

            for (int i = 0; i < c.literals.size(); ++i)
            {
                Integer lit = c.literals[i];
                Integer var = lit.abs();
                Integer dl;
                big_v->get_decision_level(var, dl);
                Integer ord(oc_bits, 0, PUBLIC);
                big_v->get_order_counter(var, ord);

                Bit is_nonzero = lit != Integer(VAR_SIZE_BIT, 0, PUBLIC);
                Bit eligible = do_analysis & is_nonzero &
                               !(var == pivot.abs()) &
                               !(dl == Integer(DL_SIZE_BIT, 0, PUBLIC));
                Bit current_level = dl == big_v->cur_dl();
                Bit is_new = mark_seen_touched_and_candidate(
                    seen, touched, candidate_active, candidate_order,
                    var, ord, eligible, current_level);
                Bit at_current_level = is_new & current_level;
                Bit below_current_level = is_new & !current_level;

                pathC = pathC + If(at_current_level,
                                   Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                   Integer(VAR_SIZE_BIT, 0, PUBLIC));
                learned_length_cap = learned_length_cap |
                                     add_literal_to_clause(out_clause, lit, below_current_level);
            }

            IndexedCandidateChoice choice = select_indexed_candidate(
                candidate_active, candidate_order, do_analysis);
            Bit found_candidate = choice.found;
            Integer best_var = choice.var;

            Bit pick_candidate = found_candidate;
            Integer assignment;
            big_v->get_assignment(best_var, assignment);
            Integer next_pivot = If(assignment == big_v->true_assigned(), best_var, -best_var);
            pivot = If(pick_candidate, next_pivot, pivot);

            clear_indexed_candidate(seen, candidate_active, best_var,
                                    pick_candidate);
            pathC = If(pick_candidate & (pathC > Integer(VAR_SIZE_BIT, 0, PUBLIC)),
                       pathC - Integer(VAR_SIZE_BIT, 1, PUBLIC),
                       pathC);

            Bit stop = pick_candidate & (pathC == Integer(VAR_SIZE_BIT, 0, PUBLIC));
            stopped = stopped | stop;
            Bit last_round = Bit(round == analysis_rounds - 1, PUBLIC);
            Bit no_candidate = do_analysis & !found_candidate;
            uip_cap = uip_cap | no_candidate | (do_analysis & last_round & !stop);

            Integer next_idx;
            big_v->get_antecedent(pivot.abs(), next_idx);
            next_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
            Bit continue_analysis = do_analysis & !stop & !uip_cap & found_candidate;
            c_idx = If(continue_analysis, next_idx, c_idx);
            do_analysis = continue_analysis;
        }

        Integer alpha = -pivot;
        Bit analysis_valid = valid_conflict & !root_conflict & stopped & !uip_cap &
                             !(alpha == Integer(VAR_SIZE_BIT, 0, PUBLIC));
        learned_length_cap = learned_length_cap |
                             add_literal_to_clause(out_clause, alpha, analysis_valid);
        Bit learned_count_cap = analysis_valid &
                                (conflict_ctr > Integer(conflict_ctr.size(),
                                                        MAX_NUM_OF_CONFLICT_CLAUSE, PUBLIC));
        Bit final_valid = analysis_valid & !learned_length_cap & !learned_count_cap;

        Integer beta(DL_SIZE_BIT, 0, PUBLIC);
        compute_beta_below_current(out_clause, beta, final_valid);

        asserting_literal = If(final_valid, alpha, asserting_literal);

        Integer ctr_for_idx = conflict_ctr;
        ctr_for_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
        Integer learned_idx = Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC) - ctr_for_idx;
        cl->insert_con_clause(ctr_for_idx, out_clause, final_valid);
        // VSIDS observes every real non-root analysis, even when the learned
        // candidate is subsequently rejected by a public cap.
        Bit real_analysis = valid_conflict & !root_conflict;
        bump_touched_variables(touched, out_clause, real_analysis);
        decay_activities(real_analysis);

        Integer inc = If(final_valid, Integer(conflict_ctr.size(), 1, PUBLIC), Integer(conflict_ctr.size(), 0, PUBLIC));
        conflict_ctr = conflict_ctr + inc;

        result.beta = If(final_valid, beta, result.beta);
        result.learned_clause_idx = If(final_valid, learned_idx, result.learned_clause_idx);
        result.valid = final_valid;
        result.cap_hit = valid_conflict & !root_conflict &
                         (uip_cap | learned_length_cap | learned_count_cap);
        result.learned_clause = out_clause;
        return result;
    }
    void decay_activities(const Bit &flag = Bit(true, PUBLIC))
    {
        Integer numerator(ACTIVITY_SIZE_BIT + 8, 95, PUBLIC);
        Integer denominator(ACTIVITY_SIZE_BIT + 8, 100, PUBLIC);
        for (int idx = 1; idx < activities.size(); ++idx)
        {
            Integer extended = activities[idx];
            extended.resize(ACTIVITY_SIZE_BIT + 8);
            Integer decayed = (extended * numerator) / denominator;
            decayed.resize(ACTIVITY_SIZE_BIT);
            activities[idx] = If(flag, decayed, activities[idx]);
        }
    }

    void construct_wl(Integer clause_idx, const Bit &flag = Bit(true, PUBLIC))
    {
        Clause c;
        cl->get_clause(clause_idx, c);
        construct_wl_from_clause(clause_idx, c, flag);
    }

    void construct_wl_from_clause(Integer clause_idx, const Clause &c,
                                  const Bit &flag = Bit(true, PUBLIC))
    {
        if (if_verbose)
        {
            construct_wl_calls += 1;
        }
        Integer wl_clause_idx = clause_idx;
        wl_clause_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
        WatchEndpointChoice endpoints = select_watch_endpoints(c, true);
        WatchList::InstallResult install = wl->install_clause_watchers(
            wl_clause_idx,
            endpoints.first_literal, endpoints.first_position, endpoints.has_first,
            endpoints.second_literal, endpoints.second_position, endpoints.has_second,
            flag);
        if_failure = if_failure | install.overflow;
    }

    // unassign every variable above decision level beta
    void backtrack(Integer beta, const Bit &flag = Bit(true, PUBLIC))
    {
        if (if_verbose)
        {
            backtrack_calls += 1;
        }
        Bit if_backtrack = flag;
        for (int i = 0; i < (int)big_u.size(); i++)
        {
            big_u[i] = If(if_backtrack, Integer(2, 0, PUBLIC), big_u[i]);
        }

        big_v->set_cur_dl(If(if_backtrack, beta, big_v->cur_dl()));

        big_v->clean_up_after_backtrack(if_backtrack);
        wl_scan_ctr = If(if_backtrack, wl_scan_ctr_value(-1), wl_scan_ctr);
        scanned_wl_this_step = scanned_wl_this_step & !if_backtrack;
    }

    void update_wl_from_frame(Integer clause_idx, Integer fresh_falsified_lit,
                              const Integer &source_slot, const Clause &source_clause,
                              const std::vector<Integer> &non_falsified_literals,
                              const Bit &flag = Bit(true, PUBLIC))
    {
        if (if_verbose)
        {
            update_wl_calls += 1;
        }
        WatchList::MoveResult result = wl->move_clause_from_frame(
            clause_idx, fresh_falsified_lit, source_slot, source_clause,
            non_falsified_literals, flag);
        if_failure = if_failure | result.overflow;
        // cur_wl is an immutable snapshot for one source scan. The cursor
        // advances right after this handler, so the consumed slot is never
        // revisited. The consumed cache cell is therefore left unchanged, which
        // avoids a W-wide mux pass; Data, Occ and Pair remain authoritative.
    }

    // Entry point for tests and direct callers. The solver uses
    // update_wl_from_frame with the already-loaded clause and its secret frame
    // slot, which avoids an extra clause read.
    void update_wl(Integer clause_idx, Integer fresh_falsified_lit,
                   std::vector<Integer> non_falsified_literals,
                   const Bit &flag = Bit(true, PUBLIC))
    {
        Clause source_clause;
        cl->get_clause(clause_idx, source_clause);

        // Direct callers do not own the solver's frame cursor, so the source
        // list is read once and the clause is located with a fixed W-wide scan.
        // The recovered slot stays private.
        std::vector<Integer> source_entries =
            wl->get_watchlist_clauses(fresh_falsified_lit);
        Integer stored_clause = clause_idx;
        stored_clause.resize(CLAUSE_IDX_SIZE_IN_WL);
        Integer source_slot(wl_scan_ctr_bits(), 0, PUBLIC);
        Bit found(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            Bit take = flag & !found & source_entries[i].equal(stored_clause);
            source_slot = If(take, Integer(wl_scan_ctr_bits(), i, PUBLIC),
                             source_slot);
            found = found | take;
        }
        update_wl_from_frame(
            clause_idx, fresh_falsified_lit,
            source_slot, source_clause, non_falsified_literals, flag & found);
    }
};
#endif // CDCL_H
