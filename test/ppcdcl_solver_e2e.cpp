#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "emp-tool/emp-tool.h"
#include "src/dimacs.h"
#include "src/ppCDCL.h"

// Example, one process per party:
//   ./bin/test_ppcdcl_solver_e2e 1 ./test/cnfs/prop_chain_unsat.cnf unsat 100 --verbose --progress-interval 50 &
//   ./bin/test_ppcdcl_solver_e2e 2 ./test/cnfs/prop_chain_unsat.cnf unsat 100 --verbose --progress-interval 50

using namespace emp;
using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

FloramMPC<NetIO> *base = nullptr;
constexpr int BASE_PORT = 12480;
constexpr int DEFAULT_BACKEND_THREADS = 4;
constexpr int MIN_SAFE_WATCHLIST_CAP = 8;
constexpr int RESULT_FAILURE = -2;
constexpr int EXPECTED_ANY = -999;
constexpr const char *WATCHLIST_OVERFLOW_REASON = "WATCHLIST_OVERFLOW";

struct CnfMeta
{
    int var_num;
    int clause_num;
    int max_lits;
    int max_literal_occurrence;
};

struct RunOptions
{
    bool verbose = false;
    bool public_early_exit = false;
    bool use_oram_variable_state = false;
    bool use_shortest_watchlist = false;
    WatchBackendRequest watch_backend_request = WatchBackendRequest::Auto;
    int max_watchlist_override = 0;
    int max_phi_literals_override = 0;
    int max_conflict_literals_override = 0;
    int max_conflict_clauses_override = 0;
    int uip_cap_override = 0;
    int decision_delay_override = 0;
    int conflict_delay_override = 0;
    int base_port_override = 0;
    int backend_threads = DEFAULT_BACKEND_THREADS;
    long long steps_override = 0;
    long long progress_interval = 0;
    std::vector<std::string> positional;
};

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

bool starts_with(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

long long parse_positive_ll(const std::string &value, const std::string &name)
{
    char *end = nullptr;
    long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed <= 0)
    {
        throw std::runtime_error(name + " must be a positive integer");
    }
    return parsed;
}

int parse_backend_threads(const std::string &value)
{
    long long parsed = parse_positive_ll(value, "threads");
    if (parsed < 2)
    {
        throw std::runtime_error("threads must be at least 2");
    }
    if (parsed > std::numeric_limits<int>::max())
    {
        throw std::runtime_error("threads is too large");
    }
    return static_cast<int>(parsed);
}

std::string format_seconds(double seconds)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << seconds;
    return oss.str();
}

std::string party_prefix(int party)
{
    std::ostringstream oss;
    oss << "[ppCDCL party " << party << "] ";
    return oss.str();
}

void verbose_log(int party, bool verbose, const std::string &message)
{
    if (verbose)
    {
        std::cout << party_prefix(party) << message << std::endl;
    }
}

struct CaseResult
{
    int solver_result = -1;
    double output_seconds = 0.0;

    CaseResult() {}
    CaseResult(int result, double output_time)
        : solver_result(result), output_seconds(output_time)
    {
    }
};

std::string result_word(int result)
{
    if (result == 1)
    {
        return "SAT";
    }
    if (result == 0)
    {
        return "UNSAT";
    }
    if (result == -1)
    {
        return "TIMEOUT";
    }
    if (result == RESULT_FAILURE)
    {
        return "FAILURE";
    }
    return "UNKNOWN";
}

std::string dimacs_status_word(int result)
{
    if (result == 1)
    {
        return "SATISFIABLE";
    }
    if (result == 0)
    {
        return "UNSATISFIABLE";
    }
    if (result == -1)
    {
        return "TIMEOUT";
    }
    if (result == RESULT_FAILURE)
    {
        return "FAILURE";
    }
    return "UNKNOWN";
}

std::string reveal_assignment_line(CDCL &solver, int var_num)
{
    std::ostringstream oss;
    for (int var = 1; var <= var_num; ++var)
    {
        Integer assignment(ASSIGNMENT_SIZE_BIT, NOT_ASSIGNED, PUBLIC);
        solver.big_v->get_assignment(var, assignment);
        int revealed = assignment.reveal<int>(PUBLIC);
        if (revealed != TRUE_ASSIGNED && revealed != FALSE_ASSIGNED)
        {
            throw std::runtime_error("SAT result contains an unassigned variable");
        }
        int literal = var;
        if (revealed == FALSE_ASSIGNED)
        {
            literal = -var;
        }
        oss << literal << ' ';
    }
    oss << '0';
    return oss.str();
}

bool assignment_satisfies_cnf(const std::string &path,
                              const std::string &assignment_line,
                              int var_num)
{
    std::vector<int> values(var_num + 1, 0);
    std::istringstream assignment_stream(assignment_line);
    int assigned_literal = 0;
    while (assignment_stream >> assigned_literal && assigned_literal != 0)
    {
        int var = std::abs(assigned_literal);
        if (var < 1 || var > var_num || values[var] != 0)
        {
            throw std::runtime_error("revealed SAT assignment is malformed");
        }
        values[var] = assigned_literal > 0 ? 1 : -1;
    }
    for (int var = 1; var <= var_num; ++var)
    {
        if (values[var] == 0)
        {
            throw std::runtime_error("revealed SAT assignment is incomplete");
        }
    }

    const ppcdcl::DimacsCnf cnf = ppcdcl::parse_dimacs_file(path);
    if (cnf.variable_count != var_num)
        throw std::runtime_error("CNF variable count changed during SAT validation");
    for (const std::vector<int> &clause : cnf.clauses)
    {
        bool satisfied = false;
        for (int literal : clause)
        {
            int var = std::abs(literal);
            satisfied = satisfied || (literal > 0 ? values[var] > 0 : values[var] < 0);
        }
        if (!satisfied)
        {
            return false;
        }
    }
    return true;
}

void emit_revealed_result(int party, int result, const std::string &assignment_line, const std::string &failure_reason)
{
    std::string status = result_word(result);
    std::cout << "E2E_RESULT party=" << party
              << " result=" << status;
    if (result == 1)
    {
        std::cout << " assignment=" << assignment_line;
    }
    if (result == RESULT_FAILURE && !failure_reason.empty())
    {
        std::cout << " reason=" << failure_reason;
    }
    std::cout << std::endl;

    if (party == ALICE)
    {
        std::cout << dimacs_status_word(result) << std::endl;
        if (result == 1)
        {
            std::cout << assignment_line << std::endl;
        }
        else if (result == RESULT_FAILURE && !failure_reason.empty())
        {
            std::cout << failure_reason << std::endl;
        }
    }
}

std::string usage(const char *argv0)
{
    std::ostringstream oss;
    oss << "Usage: " << argv0
        << " <party:1|2> [cnf_path] [sat|unsat|timeout|any] [steps]\n"
        << "Options:\n"
        << "  --verbose|-v\n"
        << "  --steps N                         public giant-step budget\n"
        << "  --progress-interval N\n"
        << "  --max-watchlist N                 max_clause_in_wl\n"
        << "  --max-phi-literals N              max_lit_in_clause for original clauses\n"
        << "  --max-conflict-literals N         max_lit_in_con_clause / learned clause cap\n"
        << "  --max-conflict-clauses N          max_num_of_con_clause\n"
        << "  --uip-cap N                       conflict-analysis loop cap; omit for VAR_NUM\n"
        << "  --decision-delay N|--dec-dly N    oblivious decision delay\n"
        << "  --conflict-delay N|--con-dly N    oblivious conflict delay\n"
        << "  --port N|--base-port N            first NetIO port for this e2e run\n"
        << "  --threads N                       backend worker/NetIO channels (default 4)\n"
        << "  --oramV|--no-oramV                use ORAM or linear-scan variable state (default linear)\n"
        << "  --shortest-wl|--no-shortest-wl    choose replacement watched literals by shortest watchlist or first-fit (default first-fit)\n"
        << "  --watch-backend auto|block|indexed watcher storage backend (default auto; public geometry only)\n"
        << "  --public-early-exit               stop after public SAT/UNSAT reveal";
    return oss.str();
}

RunOptions parse_run_options(int argc, char **argv)
{
    RunOptions opts;
    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        std::string lower = lower_copy(arg);
        if (lower == "--verbose" || lower == "-v" ||
            lower == "verbose" || lower == "true" || lower == "yes")
        {
            opts.verbose = true;
            continue;
        }
        if (lower == "--public-early-exit" || lower == "--early-exit")
        {
            opts.public_early_exit = true;
            continue;
        }
        if (lower == "--oramv" || lower == "--oram-v")
        {
            opts.use_oram_variable_state = true;
            continue;
        }
        if (lower == "--no-oramv" || lower == "--no-oram-v")
        {
            opts.use_oram_variable_state = false;
            continue;
        }
        if (lower == "--shortest-wl" || lower == "--shortest-watchlist" ||
            lower == "--wl-shortest")
        {
            opts.use_shortest_watchlist = true;
            continue;
        }
        if (lower == "--no-shortest-wl" || lower == "--no-shortest-watchlist" ||
            lower == "--first-wl" || lower == "--first-fit-wl")
        {
            opts.use_shortest_watchlist = false;
            continue;
        }

        const std::string progress_eq = "--progress-interval=";
        const std::string progress_short_eq = "--progress=";
        const std::string steps_eq = "--steps=";
        const std::string max_wl_eq = "--max-watchlist=";
        const std::string max_phi_lits_eq = "--max-phi-literals=";
        const std::string max_clause_lits_eq = "--max-clause-literals=";
        const std::string max_conflict_lits_eq = "--max-conflict-literals=";
        const std::string max_learned_lits_eq = "--max-learned-clause-length=";
        const std::string max_conflict_clauses_eq = "--max-conflict-clauses=";
        const std::string max_num_conflict_clauses_eq = "--max-num-conflict-clauses=";
        const std::string uip_cap_eq = "--uip-cap=";
        const std::string decision_delay_eq = "--decision-delay=";
        const std::string dec_dly_eq = "--dec-dly=";
        const std::string conflict_delay_eq = "--conflict-delay=";
        const std::string con_dly_eq = "--con-dly=";
        const std::string port_eq = "--port=";
        const std::string base_port_eq = "--base-port=";
        const std::string threads_eq = "--threads=";
        const std::string watch_backend_eq = "--watch-backend=";
        if (starts_with(lower, watch_backend_eq))
        {
            opts.watch_backend_request = parse_watch_backend_request(
                arg.substr(watch_backend_eq.size()));
            continue;
        }
        if (lower == "--watch-backend")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires auto, block, or indexed");
            }
            opts.watch_backend_request = parse_watch_backend_request(argv[++i]);
            continue;
        }
        if (starts_with(lower, progress_eq))
        {
            opts.progress_interval = parse_positive_ll(arg.substr(progress_eq.size()), "progress interval");
            continue;
        }
        if (starts_with(lower, progress_short_eq))
        {
            opts.progress_interval = parse_positive_ll(arg.substr(progress_short_eq.size()), "progress interval");
            continue;
        }
        if (lower == "--progress-interval" || lower == "--progress")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.progress_interval = parse_positive_ll(argv[++i], "progress interval");
            continue;
        }
        if (starts_with(lower, steps_eq))
        {
            opts.steps_override = parse_positive_ll(arg.substr(steps_eq.size()), "steps");
            continue;
        }
        if (lower == "--steps")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.steps_override = parse_positive_ll(argv[++i], "steps");
            continue;
        }
        if (starts_with(lower, max_wl_eq))
        {
            opts.max_watchlist_override = static_cast<int>(parse_positive_ll(arg.substr(max_wl_eq.size()), "max watchlist"));
            continue;
        }
        if (lower == "--max-watchlist")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.max_watchlist_override = static_cast<int>(parse_positive_ll(argv[++i], "max watchlist"));
            continue;
        }
        if (starts_with(lower, max_phi_lits_eq))
        {
            opts.max_phi_literals_override = static_cast<int>(parse_positive_ll(arg.substr(max_phi_lits_eq.size()), "max phi literals"));
            continue;
        }
        if (starts_with(lower, max_clause_lits_eq))
        {
            opts.max_phi_literals_override = static_cast<int>(parse_positive_ll(arg.substr(max_clause_lits_eq.size()), "max clause literals"));
            continue;
        }
        if (lower == "--max-phi-literals" || lower == "--max-clause-literals" || lower == "--max-literals-per-clause")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.max_phi_literals_override = static_cast<int>(parse_positive_ll(argv[++i], "max phi literals"));
            continue;
        }
        if (starts_with(lower, max_conflict_lits_eq))
        {
            opts.max_conflict_literals_override = static_cast<int>(parse_positive_ll(arg.substr(max_conflict_lits_eq.size()), "max conflict literals"));
            continue;
        }
        if (starts_with(lower, max_learned_lits_eq))
        {
            opts.max_conflict_literals_override = static_cast<int>(parse_positive_ll(arg.substr(max_learned_lits_eq.size()), "max conflict literals"));
            continue;
        }
        if (lower == "--max-conflict-literals" || lower == "--max-learned-clause-length")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.max_conflict_literals_override = static_cast<int>(parse_positive_ll(argv[++i], "max conflict literals"));
            continue;
        }
        if (starts_with(lower, max_conflict_clauses_eq))
        {
            opts.max_conflict_clauses_override = static_cast<int>(parse_positive_ll(arg.substr(max_conflict_clauses_eq.size()), "max conflict clauses"));
            continue;
        }
        if (starts_with(lower, max_num_conflict_clauses_eq))
        {
            opts.max_conflict_clauses_override = static_cast<int>(parse_positive_ll(arg.substr(max_num_conflict_clauses_eq.size()), "max conflict clauses"));
            continue;
        }
        if (lower == "--max-conflict-clauses" || lower == "--max-num-conflict-clauses")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.max_conflict_clauses_override = static_cast<int>(parse_positive_ll(argv[++i], "max conflict clauses"));
            continue;
        }
        if (starts_with(lower, uip_cap_eq))
        {
            opts.uip_cap_override = static_cast<int>(parse_positive_ll(arg.substr(uip_cap_eq.size()), "UIP cap"));
            continue;
        }
        if (lower == "--uip-cap")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.uip_cap_override = static_cast<int>(parse_positive_ll(argv[++i], "UIP cap"));
            continue;
        }
        if (starts_with(lower, decision_delay_eq))
        {
            opts.decision_delay_override = static_cast<int>(parse_positive_ll(arg.substr(decision_delay_eq.size()), "decision delay"));
            continue;
        }
        if (starts_with(lower, dec_dly_eq))
        {
            opts.decision_delay_override = static_cast<int>(parse_positive_ll(arg.substr(dec_dly_eq.size()), "decision delay"));
            continue;
        }
        if (lower == "--decision-delay" || lower == "--dec-dly")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.decision_delay_override = static_cast<int>(parse_positive_ll(argv[++i], "decision delay"));
            continue;
        }
        if (starts_with(lower, conflict_delay_eq))
        {
            opts.conflict_delay_override = static_cast<int>(parse_positive_ll(arg.substr(conflict_delay_eq.size()), "conflict delay"));
            continue;
        }
        if (starts_with(lower, con_dly_eq))
        {
            opts.conflict_delay_override = static_cast<int>(parse_positive_ll(arg.substr(con_dly_eq.size()), "conflict delay"));
            continue;
        }
        if (lower == "--conflict-delay" || lower == "--con-dly")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.conflict_delay_override = static_cast<int>(parse_positive_ll(argv[++i], "conflict delay"));
            continue;
        }
        if (starts_with(lower, port_eq))
        {
            opts.base_port_override = static_cast<int>(parse_positive_ll(arg.substr(port_eq.size()), "port"));
            continue;
        }
        if (starts_with(lower, base_port_eq))
        {
            opts.base_port_override = static_cast<int>(parse_positive_ll(arg.substr(base_port_eq.size()), "base port"));
            continue;
        }
        if (lower == "--port" || lower == "--base-port")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.base_port_override = static_cast<int>(parse_positive_ll(argv[++i], "base port"));
            continue;
        }
        if (starts_with(lower, threads_eq))
        {
            opts.backend_threads = parse_backend_threads(arg.substr(threads_eq.size()));
            continue;
        }
        if (lower == "--threads")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(arg + " requires an integer argument");
            }
            opts.backend_threads = parse_backend_threads(argv[++i]);
            continue;
        }

        opts.positional.push_back(arg);
    }
    if (opts.positional.size() > 3)
    {
        throw std::runtime_error("too many positional arguments");
    }
    return opts;
}

void init_backend(int party, bool verbose = false, int threads = DEFAULT_BACKEND_THREADS, int base_port = BASE_PORT, const char *addr = "127.0.0.1")
{
    verbose_log(party, verbose, "backend init: opening NetIO channels");
    NetIO **io = new NetIO *[threads];
    for (int i = 0; i < std::max(2, threads); i++)
    {
        io[i] = new NetIO((party - 1) ? addr : nullptr, base_port + i, true);
    }
    verbose_log(party, verbose, "backend init: NetIO channels ready");

    block delta;
    PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE)
        delta ^= makeBlock(0, 1);
    else
        delta ^= makeBlock(0, 3);

    verbose_log(party, verbose, "backend init: constructing FloramMPC backend");
    base = new FloramMPC<NetIO>(party, threads, io, delta);
    emp::backend = base;

    verbose_log(party, verbose, "backend init: sampling public-size LowMC round-key material");
    Integer lowmc_keys;
    base->random_sample(lowmc_keys, 94208);
    base->round_key = lowmc_keys.bits;
    verbose_log(party, verbose, "backend init complete");
}

void require(bool cond, const std::string &msg)
{
    if (!cond)
    {
        throw std::runtime_error(msg);
    }
}

CnfMeta read_cnf_meta(const std::string &path)
{
    const ppcdcl::DimacsCnf cnf = ppcdcl::parse_dimacs_file(path);
    CnfMeta meta{cnf.variable_count, cnf.declared_clause_count, 0, 0};
    std::vector<int> literal_occurrences(meta.var_num * 2 + 1, 0);
    for (const std::vector<int> &clause : cnf.clauses)
    {
        meta.max_lits = std::max(meta.max_lits, static_cast<int>(clause.size()));
        for (int lit : clause)
        {
            int occ_idx = lit > 0 ? lit : meta.var_num + std::abs(lit);
            literal_occurrences[occ_idx] += 1;
            meta.max_literal_occurrence = std::max(meta.max_literal_occurrence,
                                                    literal_occurrences[occ_idx]);
        }
    }

    require(meta.var_num > 0, "CNF header did not provide a positive variable count");
    require(meta.clause_num > 0, "CNF header did not provide a positive clause count");
    meta.max_lits = std::max(meta.max_lits, 1);
    meta.max_literal_occurrence = std::max(meta.max_literal_occurrence, 1);
    return meta;
}

int parse_expected(const std::string &value)
{
    std::string lower = lower_copy(value);
    if (lower == "sat" || value == "1")
    {
        return 1;
    }
    if (lower == "unsat" || value == "0")
    {
        return 0;
    }
    if (lower == "timeout" || value == "-1")
    {
        return -1;
    }
    if (lower == "failure" || lower == "failed")
    {
        return RESULT_FAILURE;
    }
    if (lower == "any" || lower == "unknown" || value == "-2")
    {
        return EXPECTED_ANY;
    }
    throw std::runtime_error("expected result must be sat, unsat, timeout, failure, any, 1, 0, -1, or -2");
}

CaseResult solve_case(const std::string &path, int expected, int steps, int party, const RunOptions &options)
{
    auto case_timer = clock_start();
    CnfMeta meta = read_cnf_meta(path);
    int max_lit_in_clause = options.max_phi_literals_override > 0
                                ? options.max_phi_literals_override
                                : meta.max_lits;
    require(max_lit_in_clause >= meta.max_lits, "max phi literals must be at least the largest original clause");
    max_lit_in_clause = std::max(1, std::min(max_lit_in_clause, meta.var_num));
    int max_lit_in_conflict = options.max_conflict_literals_override > 0
                                  ? options.max_conflict_literals_override
                                  : std::max(meta.max_lits, meta.var_num);
    max_lit_in_conflict = std::max(1, std::min(max_lit_in_conflict, meta.var_num));
    int max_conflict_clauses = options.max_conflict_clauses_override > 0
                                   ? options.max_conflict_clauses_override
                                   : std::max(16, meta.var_num * 2 + meta.clause_num);
    int max_watchlist = meta.max_literal_occurrence;
    if (options.max_watchlist_override > 0)
    {
        max_watchlist = options.max_watchlist_override;
    }
    require(max_watchlist >= MIN_SAFE_WATCHLIST_CAP, "max watchlist must be at least 8 for the current FloRAM packing");
    max_watchlist = std::max(MIN_SAFE_WATCHLIST_CAP, max_watchlist);
    int decision_delay = options.decision_delay_override > 0 ? options.decision_delay_override : 1;
    int conflict_delay = options.conflict_delay_override > 0 ? options.conflict_delay_override : 1;

    if (options.verbose)
    {
        std::ostringstream config;
        config << "case config: path=" << path
               << " expected=" << expected
               << " steps=" << steps
               << " var_num=" << meta.var_num
               << " clause_num=" << meta.clause_num
               << " max_lits_in_input=" << meta.max_lits
               << " max_literal_occurrence=" << meta.max_literal_occurrence
               << " max_phi_literals=" << max_lit_in_clause
               << " max_watchlist=" << max_watchlist
               << " max_conflict_literals=" << max_lit_in_conflict
               << " max_conflict_clauses=" << max_conflict_clauses
               << " uip_cap=" << (options.uip_cap_override > 0 ? options.uip_cap_override : meta.var_num)
               << " decision_delay=" << decision_delay
               << " conflict_delay=" << conflict_delay
               << " public_early_exit=" << (options.public_early_exit ? "true" : "false")
               << " variable_state=" << (options.use_oram_variable_state ? "oram" : "linear")
               << " watchlist_strategy=" << (options.use_shortest_watchlist ? "shortest" : "first")
               << " watch_backend_requested=" << watch_backend_name(options.watch_backend_request);
        verbose_log(party, options.verbose, config.str());
    }

    auto construct_timer = clock_start();
    std::unique_ptr<CDCL> solver(new CDCL(path, base, PUBLIC, max_watchlist,
                                          max_lit_in_clause, max_lit_in_conflict,
                                          max_conflict_clauses, options.verbose,
                                          options.progress_interval, party,
                                          options.use_oram_variable_state,
                                          options.use_shortest_watchlist,
                                          options.watch_backend_request));
    std::cout << "WATCH_BACKEND_CONFIG party=" << party
              << " requested=" << watch_backend_name(options.watch_backend_request)
              << " effective=" << watch_backend_name(solver->wl->backend_kind())
              << " reason=" << solver->wl->selection_reason()
              << " occ_bits=" << WATCH_OCC_PAYLOAD_BITS
              << " pair_bits=" << WATCH_PAIR_PAYLOAD_BITS << std::endl;
    solver->allow_public_early_exit = options.public_early_exit;
    if (options.uip_cap_override > 0)
    {
        solver->uip_loop_cap = options.uip_cap_override;
    }
    solver->oblivious_decision_delay = decision_delay;
    solver->oblivious_conflict_delay = conflict_delay;
    verbose_log(party, options.verbose, "solver object ready: construction_runtime_s=" + format_seconds(time_from(construct_timer) / 1e6));

    auto solve_timer = clock_start();
    int result = solver->begin_giant_step(steps);
    long long public_steps_executed = solver->step_idx;
    const ppcdcl::policy::PossibleSourceWorkSchedule expected_source_work =
        ppcdcl::policy::build_possible_source_work_schedule(
            static_cast<std::uint64_t>(public_steps_executed),
            static_cast<std::uint64_t>(max_watchlist),
            static_cast<std::uint64_t>(decision_delay),
            static_cast<std::uint64_t>(conflict_delay));
    long long expected_source_attempt_steps = 0;
    long long expected_source_refresh_steps = 0;
    for (std::size_t i = 0; i < expected_source_work.source_attempts.size(); ++i)
    {
        expected_source_attempt_steps += expected_source_work.source_attempts[i] != 0;
        expected_source_refresh_steps += expected_source_work.source_refreshes[i] != 0;
    }
    require(solver->possible_source_attempt_steps == expected_source_attempt_steps,
            "production loop source-attempt count does not match its public schedule");
    require(solver->possible_source_refresh_steps == expected_source_refresh_steps,
            "production loop source-refresh count does not match its public schedule");
    double solve_seconds = time_from(solve_timer) / 1e6;
    double case_seconds = time_from(case_timer) / 1e6;
    auto output_timer = clock_start();
    std::string assignment_line;
    std::string failure_reason;
    if (result == 1)
    {
        assignment_line = reveal_assignment_line(*solver, meta.var_num);
        require(assignment_satisfies_cnf(path, assignment_line, meta.var_num),
                "Revealed SAT assignment does not satisfy " + path);
        std::cout << "E2E_MODEL_CHECK party=" << party << " valid=1" << std::endl;
    }
    else if (result == RESULT_FAILURE)
    {
        failure_reason = WATCHLIST_OVERFLOW_REASON;
    }
    emit_revealed_result(party, result, assignment_line, failure_reason);
    double output_seconds = time_from(output_timer) / 1e6;
    if (expected != EXPECTED_ANY)
    {
        require(result == expected, "Unexpected solver result for " + path);
    }
    if (options.verbose)
    {
        std::ostringstream done;
        done << "case complete: path=" << path
             << " result=" << result
             << " solver_runtime_s=" << format_seconds(solve_seconds)
             << " case_e2e_runtime_s=" << format_seconds(case_seconds);
        verbose_log(party, options.verbose, done.str());
        std::cout << "RUNTIME party=" << party
                  << " kind=case path=" << path
                  << " solver_seconds=" << format_seconds(solve_seconds)
                  << " case_e2e_seconds=" << format_seconds(case_seconds)
                  << std::endl;
    }
    if (party == ALICE)
    {
        std::cout << path << " result=" << result << " steps=" << steps << std::endl;
    }
    std::cout << "E2E_SUMMARY party=" << party
              << " path=" << path
              << " result=" << result
              << " steps_budget=" << steps
              << " public_steps_executed=" << public_steps_executed
              << " solver_seconds=" << format_seconds(solve_seconds)
              << " source_attempt_steps=" << solver->possible_source_attempt_steps
              << " source_refresh_steps=" << solver->possible_source_refresh_steps
              << " source_skipped_steps="
              << (public_steps_executed - solver->possible_source_refresh_steps)
              << " case_e2e_seconds=" << format_seconds(case_seconds)
              << std::endl;
    return {result, output_seconds};
}

double run_default_suite(int party, const RunOptions &options)
{
    double output_seconds = 0.0;
    output_seconds += solve_case("./test/cnfs/unit_conflict.cnf", 0, 1, party, options).output_seconds;
    output_seconds += solve_case("./test/cnfs/prop_chain_unsat.cnf", 0, 100, party, options).output_seconds;
    output_seconds += solve_case("./unsat.cnf", 0, 500, party, options).output_seconds;
    output_seconds += solve_case("./test/cnfs/simple_sat.cnf", 1, 100, party, options).output_seconds;
    return output_seconds;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << usage(argv[0]) << std::endl;
        return 1;
    }
    if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")
    {
        std::cout << usage(argv[0]) << std::endl;
        return 0;
    }

    auto program_timer = clock_start();
    int party = atoi(argv[1]);
    RunOptions options;
    try
    {
        options = parse_run_options(argc, argv);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Argument error: " << ex.what() << std::endl;
        std::cerr << usage(argv[0]) << std::endl;
        return 1;
    }

    std::string cnf_path;
    int expected = EXPECTED_ANY;
    int steps = 5000;
    bool single_case = !options.positional.empty();
    try
    {
        if (single_case)
        {
            cnf_path = options.positional[0];
            expected = (options.positional.size() >= 2) ? parse_expected(options.positional[1]) : -2;
            steps = (options.positional.size() >= 3) ? atoi(options.positional[2].c_str()) : 5000;
            if (options.steps_override > 0)
            {
                steps = static_cast<int>(options.steps_override);
            }
            require(steps > 0, "steps must be positive");
        }
        else if (options.steps_override > 0)
        {
            throw std::runtime_error("--steps requires a CNF path");
        }
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Argument error: " << ex.what() << std::endl;
        std::cerr << usage(argv[0]) << std::endl;
        return 1;
    }

    int base_port = options.base_port_override > 0 ? options.base_port_override : BASE_PORT;
    std::cout << "BACKEND_CONFIG party=" << party
              << " threads=" << options.backend_threads
              << " base_port=" << base_port << std::endl;
    init_backend(party, options.verbose, options.backend_threads, base_port);
    verbose_log(party, options.verbose, "switching backend to garbled-table mode");
    base->switch_to_gt();
    verbose_log(party, options.verbose, "backend mode ready");

    double output_seconds = 0.0;
    try
    {
        if (single_case)
        {
            output_seconds += solve_case(cnf_path, expected, steps, party, options).output_seconds;
        }
        else
        {
            output_seconds += run_default_suite(party, options);
        }
    }
    catch (const std::exception &ex)
    {
        std::cerr << "E2E failure: " << ex.what() << std::endl;
        verbose_log(party, options.verbose, "final end-to-end runtime before failure: " + format_seconds(time_from(program_timer) / 1e6) + " s");
        delete base;
        return 1;
    }

    double program_seconds = std::max(0.0, time_from(program_timer) / 1e6 - output_seconds);
    if (options.verbose)
    {
        verbose_log(party, options.verbose, "final end-to-end runtime: " + format_seconds(program_seconds) + " s");
        std::cout << "FINAL_RUNTIME party=" << party
                  << " e2e_seconds=" << format_seconds(program_seconds)
                  << std::endl;
    }
    std::cout << "E2E_FINAL party=" << party
              << " e2e_seconds=" << format_seconds(program_seconds)
              << std::endl;

    if (party == ALICE)
    {
        std::cout << "ppCDCL solver end-to-end tests passed" << std::endl;
    }

    delete base;
    return 0;
}
