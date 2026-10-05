#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "plaintext_cdcl/src/cdcl.h"
#include "plaintext_cdcl/test/dimacs_clause_count.h"
#include "src/public_schedule.h"
#include "emp-tool/emp-tool.h"

struct Overrides
{
    int max_wl_size = -1;
    int max_lit_in_clause = -1;
    int max_lit_in_con_clause = -1;
    int max_num_of_con_clause = -1;
};

struct RunConfig
{
    std::string cnf_path;
    int con_dly = 0;
    int dec_dly = 0;
    Overrides overrides;
    int learned_clause_cap = 0;
    int uip_cap = 0;
    bool use_shortest_watchlist = false;
    CDCL::ClauseCapPolicy cap_policy = CDCL::CLAUSE_CAP_RESTART;
};

struct ProbeOutput
{
    int var_num = 0;
    int clause_num = 0;
    int max_num_of_con_clause = 0;
    int max_lit_in_clause = 0;
    int max_lit_in_con_clause = 0;
    int max_clause_in_wl = 0;
    long long orange_total = 0;
    long long yellow_total = 0;
    long long blue_total = 0;
    long long red_total = 0;
    int con_dly = 0;
    int dec_dly = 0;
    double plaintext_time_seconds = 0.0;
    int learned_clause_count = 0;
    int observed_max_wl = 0;
    int observed_max_conflict_clause = 0;
};

static void print_usage(const char *prog)
{
    std::cout << "Usage: " << prog
              << " <cnf_path> <con_dly> <dec_dly>"
              << " [--max-wl-size N] [--max-lit-in-clause N]"
              << " [--max-lit-in-con-clause N] [--max-num-of-con-clause N]"
              << " [--learned-clause-cap N] [--cap-action restart|chrono]"
              << " [--uip-cap N] [--shortest-wl|--no-shortest-wl]"
              << std::endl;
}

static bool parse_args(int argc, char **argv, RunConfig &cfg)
{
    if (argc < 4)
    {
        return false;
    }

    cfg.cnf_path = argv[1];
    cfg.con_dly = std::max(1, std::atoi(argv[2]));
    cfg.dec_dly = std::max(1, std::atoi(argv[3]));

    std::vector<std::string> extra_args;
    for (int i = 4; i < argc; ++i)
    {
        extra_args.emplace_back(argv[i]);
    }

    auto require_value = [&](size_t idx, const std::string &flag) -> bool
    {
        if (idx + 1 >= extra_args.size())
        {
            std::cerr << "Missing value for flag " << flag << std::endl;
            return false;
        }
        return true;
    };

    for (size_t i = 0; i < extra_args.size(); ++i)
    {
        const std::string &flag = extra_args[i];
        try
        {
            if (flag == "--max-wl-size")
            {
                if (!require_value(i, flag))
                    return false;
                cfg.overrides.max_wl_size = std::stoi(extra_args[++i]);
            }
            else if (flag == "--max-lit-in-clause")
            {
                if (!require_value(i, flag))
                    return false;
                cfg.overrides.max_lit_in_clause = std::stoi(extra_args[++i]);
            }
            else if (flag == "--max-lit-in-con-clause")
            {
                if (!require_value(i, flag))
                    return false;
                cfg.overrides.max_lit_in_con_clause = std::stoi(extra_args[++i]);
            }
            else if (flag == "--max-num-of-con-clause")
            {
                if (!require_value(i, flag))
                    return false;
                cfg.overrides.max_num_of_con_clause = std::stoi(extra_args[++i]);
            }
            else if (flag == "--learned-clause-cap")
            {
                if (!require_value(i, flag))
                    return false;
                cfg.learned_clause_cap = std::max(0, std::stoi(extra_args[++i]));
            }
            else if (flag == "--cap-action")
            {
                if (!require_value(i, flag))
                    return false;
                std::string action = extra_args[++i];
                std::string lower = action;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c)
                               { return std::tolower(c); });
                if (lower == "restart")
                {
                    cfg.cap_policy = CDCL::CLAUSE_CAP_RESTART;
                }
                else if (lower == "chrono" || lower == "chronological" || lower == "chronobacktrack")
                {
                    cfg.cap_policy = CDCL::CLAUSE_CAP_CHRONO_BACKTRACK;
                }
                else
                {
                    std::cerr << "Unknown cap action: " << action << std::endl;
                    return false;
                }
            }
            else if (flag == "--uip-cap")
            {
                if (!require_value(i, flag))
                    return false;
                int uip_cap = std::max(0, std::stoi(extra_args[++i]));
                cfg.uip_cap = uip_cap;
            }
            else if (flag == "--shortest-wl" || flag == "--shortest-watchlist")
            {
                cfg.use_shortest_watchlist = true;
            }
            else if (flag == "--no-shortest-wl" || flag == "--no-shortest-watchlist")
            {
                cfg.use_shortest_watchlist = false;
            }
            else
            {
                std::cerr << "Unknown flag: " << flag << std::endl;
                return false;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to parse value for " << flag << ": " << e.what() << std::endl;
            return false;
        }
    }

    return true;
}

static ProbeOutput run_probe(const RunConfig &cfg)
{
    CDCL probe(cfg.cnf_path);
    probe.learned_clause_len_cap = std::max(cfg.learned_clause_cap, -1);
    probe.clause_cap_policy = cfg.cap_policy;
    probe.uip_loop_cap = cfg.uip_cap;
    probe.use_shortest_watchlist = cfg.use_shortest_watchlist;
    auto plain_start = emp::clock_start();
    probe.begin_giant_step(1e10);

    double plaintext_time_seconds = emp::time_from(plain_start) / 1000.0 / 1000.0;

    int chosen_wl = cfg.overrides.max_wl_size > 0 ? cfg.overrides.max_wl_size : probe.max_wl_size;
    int chosen_max_lit_clause =
        cfg.overrides.max_lit_in_clause > 0 ? cfg.overrides.max_lit_in_clause : probe.max_lit_in_clause;
    int chosen_max_lit_conf_clause = cfg.overrides.max_lit_in_con_clause > 0 ? cfg.overrides.max_lit_in_con_clause
                                                                             : probe.max_lit_in_conflict_clause;
    // The number of learned clauses depends on the watchlist and delay settings,
    // so the first run's count is not a valid bound for the second run. Unless
    // the caller declares a capacity, leave it unbounded and derive it from the
    // second run.
    int declared_max_conf_clause =
        cfg.overrides.max_num_of_con_clause > 0 ? cfg.overrides.max_num_of_con_clause : 0;

    CDCL cdcl(cfg.cnf_path);
    cdcl.learned_clause_len_cap = std::max(cfg.learned_clause_cap, 0);
    cdcl.clause_cap_policy = cfg.cap_policy;
    cdcl.uip_loop_cap = cfg.uip_cap;
    cdcl.use_shortest_watchlist = cfg.use_shortest_watchlist;
    int wl_bound = std::max(chosen_wl, 1);
    cdcl.emp_max_wl_size = wl_bound;
    cdcl.emp_max_conflict_phi_size = declared_max_conf_clause;
    cdcl.oblivious_conflict_delay = std::max(cfg.con_dly, 1);
    cdcl.oblivious_decision_delay = std::max(cfg.dec_dly, 1);
    cdcl.begin_giant_step(1e10);

    ProbeOutput out{};
    out.var_num = cdcl.var_num;
    out.clause_num = dimacs_clause_count(cdcl.phi);
    out.learned_clause_count = static_cast<int>(cdcl.conflict_phi.size());
    out.observed_max_wl = cdcl.max_wl_size;
    out.observed_max_conflict_clause = cdcl.max_lit_in_conflict_clause;
    out.max_num_of_con_clause = std::max(out.learned_clause_count, std::max(declared_max_conf_clause, 1));
    out.max_lit_in_clause = std::max(std::max(cdcl.max_lit_in_clause, chosen_max_lit_clause), 1);
    int effective_conflict_clause_bound = cdcl.max_lit_in_conflict_clause;
    if (cfg.learned_clause_cap > 0)
    {
        effective_conflict_clause_bound =
            std::max(effective_conflict_clause_bound, std::max(cfg.learned_clause_cap, cdcl.max_lit_in_clause));
    }
    effective_conflict_clause_bound = std::max(effective_conflict_clause_bound, chosen_max_lit_conf_clause);
    out.max_lit_in_con_clause = std::max(effective_conflict_clause_bound, 1);
    out.max_clause_in_wl = std::max(cdcl.max_wl_size, wl_bound);
    // orange_count is the number T of iterations executed, indexed t=1,...,T
    // (there is no t=0 step). Decision and conflict blocks run at t == 1
    // (mod delay), so both include t=1 whenever T is non-zero.
    out.orange_total = cdcl.orange_count;
    out.yellow_total = static_cast<long long>(ppcdcl::policy::count_modulo_one_slots(
        static_cast<std::uint64_t>(out.orange_total),
        static_cast<std::uint64_t>(cdcl.oblivious_decision_delay)));
    out.blue_total = cdcl.blue_count;
    out.red_total = static_cast<long long>(ppcdcl::policy::count_modulo_one_slots(
        static_cast<std::uint64_t>(out.orange_total),
        static_cast<std::uint64_t>(cdcl.oblivious_conflict_delay)));
    out.con_dly = cdcl.oblivious_conflict_delay;
    out.dec_dly = cdcl.oblivious_decision_delay;
    out.plaintext_time_seconds = plaintext_time_seconds;

    return out;
}

int main(int argc, char **argv)
{
    RunConfig cfg;
    if (!parse_args(argc, argv, cfg))
    {
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "=============TEST START================" << std::endl;
    std::cout << cfg.cnf_path << std::endl;
    if (cfg.learned_clause_cap > 0)
    {
        std::cout << "learned clause cap: " << cfg.learned_clause_cap << " (action: "
                  << (cfg.cap_policy == CDCL::CLAUSE_CAP_RESTART ? "restart" : "chronological backtrack")
                  << ")" << std::endl;
    }
    if (cfg.overrides.max_wl_size > 0)
        std::cout << "override max_wl_size: " << cfg.overrides.max_wl_size << std::endl;
    if (cfg.overrides.max_lit_in_clause > 0)
        std::cout << "override max_lit_in_clause: " << cfg.overrides.max_lit_in_clause << std::endl;
    if (cfg.overrides.max_lit_in_con_clause > 0)
        std::cout << "override max_lit_in_con_clause: " << cfg.overrides.max_lit_in_con_clause << std::endl;
    if (cfg.overrides.max_num_of_con_clause > 0)
        std::cout << "override max_num_of_con_clause: " << cfg.overrides.max_num_of_con_clause << std::endl;

    ProbeOutput result = run_probe(cfg);

    std::cout << "============PLAINTEXT TIME=============" << std::endl;
    std::cout << "total time: " << result.plaintext_time_seconds << " s" << std::endl;
    std::cout << "=============BEST EMP SETTINGS=============" << std::endl;
    std::cout << "#variable: " << result.var_num << std::endl;
    std::cout << "#clause: " << result.clause_num << std::endl;
    std::cout << "#learned_clause: " << result.learned_clause_count << std::endl;
    std::cout << "max_lit_in_clause: " << result.max_lit_in_clause << std::endl;
    std::cout << "max_lit_in_conflict_clause: " << result.observed_max_conflict_clause << std::endl;
    std::cout << "max_wl_size: " << result.observed_max_wl << std::endl;
    std::cout << "oblivious_conflict_delay: " << result.con_dly << std::endl;
    std::cout << "oblivious_decision_delay: " << result.dec_dly << std::endl;

    std::cout << "=============TEST END================" << std::endl;
    std::cout << result.var_num << " " << result.clause_num << " " << result.max_num_of_con_clause << " "
              << result.max_lit_in_clause << " " << result.max_lit_in_con_clause << " " << result.max_clause_in_wl
              << " " << result.orange_total << " " << result.yellow_total << " " << result.blue_total << " "
              << result.red_total << " " << result.con_dly << " " << result.dec_dly << std::endl;

    return 0;
}
