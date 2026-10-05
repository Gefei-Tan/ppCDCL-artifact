#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "emp-dpf/emp-dpf.h"
#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"

using namespace emp;

FloramMPC<NetIO> *base = nullptr;

namespace
{

constexpr int kDefaultPort = 12780;
constexpr int kDefaultThreads = 4;
constexpr int kMinimumPackedWatchlist = 8;

struct CnfMeta
{
    int variable_count = 0;
    int clause_count = 0;
    int max_clause_length = 0;
    int max_literal_occurrence = 0;
};

struct Options
{
    int party = 0;
    std::string cnf_path;
    int sample_steps = 0;
    std::uint64_t target_steps = 0;
    int decision_delay = 1;
    int conflict_delay = 1;
    int max_watchlist = 0;
    int max_phi_literals = 0;
    int max_conflict_literals = 0;
    int max_conflict_clauses = 0;
    int uip_cap = 0;
    int base_port = kDefaultPort;
    int threads = kDefaultThreads;
    bool use_oram_variable_state = false;
    bool use_shortest_watchlist = false;
    WatchBackendRequest watch_backend_request = WatchBackendRequest::Auto;
    bool public_early_exit = true;
    bool verbose = false;
};

std::uint64_t parse_positive_u64(const std::string &value, const char *name)
{
    if (value.empty() || value[0] == '-')
        throw std::runtime_error(std::string(name) + " must be a positive integer");
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed == 0)
        throw std::runtime_error(std::string(name) + " must be a positive integer");
    return static_cast<std::uint64_t>(parsed);
}

int parse_positive_int(const std::string &value, const char *name)
{
    const std::uint64_t parsed = parse_positive_u64(value, name);
    if (parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error(std::string(name) + " exceeds INT_MAX");
    return static_cast<int>(parsed);
}

std::string usage(const char *argv0)
{
    std::ostringstream out;
    out << "Usage: " << argv0
        << " <party:1|2> <cnf> <sample_steps> <target_steps> [options]\n"
        << "       " << argv0 << " --self-test\n"
        << "Options:\n"
        << "  --decision-delay N --conflict-delay N\n"
        << "  --max-watchlist N --max-phi-literals N\n"
        << "  --max-conflict-literals N --max-conflict-clauses N --uip-cap N\n"
        << "  --oramV|--no-oramV --shortest-wl|--no-shortest-wl\n"
        << "  --watch-backend auto|block|indexed\n"
        << "  --public-early-exit|--no-public-early-exit\n"
        << "  --threads N --port N --verbose\n"
        << "The sample executes the real CDCL giant-step loop.  Projection fails if\n"
        << "the target schedule contains a decision/conflict/restart bucket absent\n"
        << "from the sample.";
    return out.str();
}

bool option_value(const std::string &arg, const std::string &name, std::string &value)
{
    const std::string prefix = name + "=";
    if (arg.size() > prefix.size() && arg.compare(0, prefix.size(), prefix) == 0)
    {
        value = arg.substr(prefix.size());
        return true;
    }
    return false;
}

Options parse_options(int argc, char **argv)
{
    if (argc < 5)
        throw std::runtime_error("missing required arguments");

    Options options;
    options.party = parse_positive_int(argv[1], "party");
    if (options.party != ALICE && options.party != BOB)
        throw std::runtime_error("party must be 1 or 2");
    options.cnf_path = argv[2];
    options.sample_steps = parse_positive_int(argv[3], "sample_steps");
    options.target_steps = parse_positive_u64(argv[4], "target_steps");

    for (int i = 5; i < argc; ++i)
    {
        const std::string arg = argv[i];
        std::string value;
        auto consume_int = [&](const char *name) {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string(name) + " requires a value");
            return parse_positive_int(argv[++i], name);
        };

        if (arg == "--oramV" || arg == "--oram-v")
            options.use_oram_variable_state = true;
        else if (arg == "--no-oramV" || arg == "--no-oram-v")
            options.use_oram_variable_state = false;
        else if (arg == "--shortest-wl")
            options.use_shortest_watchlist = true;
        else if (arg == "--no-shortest-wl" || arg == "--first-fit-wl")
            options.use_shortest_watchlist = false;
        else if (arg == "--watch-backend")
        {
            if (i + 1 >= argc)
                throw std::runtime_error("--watch-backend requires a value");
            options.watch_backend_request = parse_watch_backend_request(argv[++i]);
        }
        else if (arg == "--public-early-exit")
            options.public_early_exit = true;
        else if (arg == "--no-public-early-exit")
            options.public_early_exit = false;
        else if (arg == "--verbose" || arg == "-v")
            options.verbose = true;
        else if (arg == "--decision-delay" || arg == "--dec-dly")
            options.decision_delay = consume_int("decision delay");
        else if (arg == "--conflict-delay" || arg == "--con-dly")
            options.conflict_delay = consume_int("conflict delay");
        else if (arg == "--max-watchlist")
            options.max_watchlist = consume_int("max watchlist");
        else if (arg == "--max-phi-literals")
            options.max_phi_literals = consume_int("max phi literals");
        else if (arg == "--max-conflict-literals")
            options.max_conflict_literals = consume_int("max conflict literals");
        else if (arg == "--max-conflict-clauses")
            options.max_conflict_clauses = consume_int("max conflict clauses");
        else if (arg == "--uip-cap")
            options.uip_cap = consume_int("UIP cap");
        else if (arg == "--threads")
            options.threads = consume_int("threads");
        else if (arg == "--port" || arg == "--base-port")
            options.base_port = consume_int("port");
        else if (option_value(arg, "--decision-delay", value) || option_value(arg, "--dec-dly", value))
            options.decision_delay = parse_positive_int(value, "decision delay");
        else if (option_value(arg, "--conflict-delay", value) || option_value(arg, "--con-dly", value))
            options.conflict_delay = parse_positive_int(value, "conflict delay");
        else if (option_value(arg, "--max-watchlist", value))
            options.max_watchlist = parse_positive_int(value, "max watchlist");
        else if (option_value(arg, "--max-phi-literals", value))
            options.max_phi_literals = parse_positive_int(value, "max phi literals");
        else if (option_value(arg, "--max-conflict-literals", value))
            options.max_conflict_literals = parse_positive_int(value, "max conflict literals");
        else if (option_value(arg, "--max-conflict-clauses", value))
            options.max_conflict_clauses = parse_positive_int(value, "max conflict clauses");
        else if (option_value(arg, "--uip-cap", value))
            options.uip_cap = parse_positive_int(value, "UIP cap");
        else if (option_value(arg, "--threads", value))
            options.threads = parse_positive_int(value, "threads");
        else if (option_value(arg, "--port", value) || option_value(arg, "--base-port", value))
            options.base_port = parse_positive_int(value, "port");
        else if (option_value(arg, "--watch-backend", value))
            options.watch_backend_request = parse_watch_backend_request(value);
        else
            throw std::runtime_error("unknown option: " + arg);
    }

    if (options.threads < 2)
        throw std::runtime_error("threads must be at least 2");
    if (options.base_port > 65535 - options.threads)
        throw std::runtime_error("port range exceeds 65535");
    return options;
}

CnfMeta read_cnf_meta(const std::string &path)
{
    const ppcdcl::DimacsCnf cnf = ppcdcl::parse_dimacs_file(path);
    CnfMeta meta;
    meta.variable_count = cnf.variable_count;
    meta.clause_count = cnf.declared_clause_count;
    std::vector<int> occurrences(static_cast<std::size_t>(2 * meta.variable_count + 1), 0);
    for (const std::vector<int> &clause : cnf.clauses)
    {
        meta.max_clause_length = std::max(meta.max_clause_length, static_cast<int>(clause.size()));
        for (int literal : clause)
        {
            const int index = literal > 0 ? literal : meta.variable_count + std::abs(literal);
            meta.max_literal_occurrence = std::max(meta.max_literal_occurrence, ++occurrences[index]);
        }
    }
    if (meta.variable_count <= 0 || meta.clause_count <= 0)
        throw std::runtime_error("CNF must declare positive variable and clause counts");
    meta.max_clause_length = std::max(1, meta.max_clause_length);
    meta.max_literal_occurrence = std::max(1, meta.max_literal_occurrence);
    return meta;
}

void init_backend(const Options &options)
{
    NetIO **io = new NetIO *[options.threads];
    for (int i = 0; i < options.threads; ++i)
        io[i] = new NetIO(options.party == BOB ? "127.0.0.1" : nullptr,
                          options.base_port + i, true);

    block delta;
    PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    delta ^= options.party == ALICE ? makeBlock(0, 1) : makeBlock(0, 3);

    base = new FloramMPC<NetIO>(options.party, options.threads, io, delta);
    emp::backend = base;
    Integer lowmc_keys;
    base->random_sample(lowmc_keys, 94208);
    base->round_key = lowmc_keys.bits;
}

std::array<std::uint64_t, CDCL::PUBLIC_STEP_TIMING_BUCKETS>
schedule_bucket_counts(std::uint64_t steps,
                       std::uint64_t decision_delay,
                       std::uint64_t conflict_delay,
                       std::uint64_t watchlist_width)
{
    std::array<std::uint64_t, CDCL::PUBLIC_STEP_TIMING_BUCKETS> counts{};
    const ppcdcl::policy::PossibleSourceWorkSchedule source_work =
        ppcdcl::policy::build_possible_source_work_schedule(
            steps, watchlist_width, decision_delay, conflict_delay);
    std::uint64_t restart_index = 1;
    std::uint64_t restart_progress = 0;
    for (std::uint64_t t = 1; t <= steps; ++t)
    {
        const bool decision = ppcdcl::policy::is_modulo_one_slot(t, decision_delay);
        const bool conflict = ppcdcl::policy::is_modulo_one_slot(t, conflict_delay);
        bool restart = false;
        if (conflict)
        {
            restart_progress += 1;
            if (restart_progress >= ppcdcl::policy::luby_interval(restart_index))
            {
                restart = true;
                restart_progress = 0;
                restart_index += 1;
            }
        }
        const bool possible_attempt = source_work.source_attempts[t - 1] != 0;
        const bool possible_refresh = source_work.source_refreshes[t - 1] != 0;
        const int bucket = (decision ? 1 : 0) |
                           (conflict ? 2 : 0) |
                           (restart ? 4 : 0) |
                           (possible_attempt ? 8 : 0) |
                           (possible_refresh ? 16 : 0);
        counts[bucket] += 1;
    }
    return counts;
}

void require_equal(std::uint64_t actual, std::uint64_t expected, const char *message)
{
    if (actual != expected)
    {
        std::ostringstream out;
        out << message << ": expected " << expected << ", got " << actual;
        throw std::runtime_error(out.str());
    }
}

std::uint64_t count_base_slots(
    const std::array<std::uint64_t, CDCL::PUBLIC_STEP_TIMING_BUCKETS> &counts,
    int base_bits)
{
    std::uint64_t total = 0;
    for (int bucket = 0; bucket < CDCL::PUBLIC_STEP_TIMING_BUCKETS; ++bucket)
    {
        if ((bucket & 7) == base_bits)
            total += counts[bucket];
    }
    return total;
}

std::uint64_t count_work_bit(
    const std::array<std::uint64_t, CDCL::PUBLIC_STEP_TIMING_BUCKETS> &counts,
    int bit)
{
    std::uint64_t total = 0;
    for (int bucket = 0; bucket < CDCL::PUBLIC_STEP_TIMING_BUCKETS; ++bucket)
    {
        if ((bucket & bit) != 0)
            total += counts[bucket];
    }
    return total;
}

int self_test()
{
    static_assert(CDCL::PUBLIC_STEP_TIMING_BUCKETS == 32,
                  "projection buckets must distinguish source-attempt and source-refresh work");
    const auto source_gated = schedule_bucket_counts(20, 6, 10, 4);
    require_equal(source_gated[0], 3, "worked schedule skipped ordinary steps");
    require_equal(source_gated[17], 1, "worked schedule decision+activation-only refresh step");
    require_equal(source_gated[24], 12, "worked schedule attempt+refresh steps");
    require_equal(source_gated[25], 2, "worked schedule decision+attempt+refresh steps");
    require_equal(source_gated[26], 1, "worked schedule conflict+attempt+refresh step");
    require_equal(source_gated[27], 1, "worked schedule combined attempt+refresh step");

    const auto uf20 = schedule_bucket_counts(2117, 50, 200, 14);
    require_equal(count_base_slots(uf20, 0), 2074, "UF20 ordinary steps");
    require_equal(count_base_slots(uf20, 1), 32, "UF20 decision-only steps");
    require_equal(count_base_slots(uf20, 2), 0, "UF20 conflict-only steps");
    require_equal(count_base_slots(uf20, 3), 11, "UF20 decision+conflict steps");
    for (int base_bits = 4; base_bits < 8; ++base_bits)
        require_equal(count_base_slots(uf20, base_bits), 0, "UF20 restart steps");
    require_equal(count_work_bit(uf20, 8), 1742, "UF20 source-attempt steps");
    require_equal(count_work_bit(uf20, 16), 1748, "UF20 source-refresh steps");

    const auto shifting_mix_sample = schedule_bucket_counts(40, 3, 5, 8);
    const auto shifting_mix_target = schedule_bucket_counts(80, 3, 5, 8);
    require_equal(count_work_bit(shifting_mix_sample, 8), 30,
                  "D<W sample source-attempt steps");
    require_equal(count_work_bit(shifting_mix_sample, 16), 34,
                  "D<W sample source-refresh steps");
    require_equal(count_work_bit(shifting_mix_target, 8), 70,
                  "D<W target source-attempt steps");
    require_equal(count_work_bit(shifting_mix_target, 16), 74,
                  "D<W target source-refresh steps");

    const auto first_restart = schedule_bucket_counts(51001, 50, 200, 14);
    require_equal(count_base_slots(first_restart, 7), 1, "first Luby restart bucket");
    require_equal(count_base_slots(first_restart, 3), 255,
                  "ordinary combined bucket before first restart");
    require_equal(std::accumulate(first_restart.begin(), first_restart.end(), std::uint64_t{0}),
                  51001, "bucket total");

    std::cout << "production step projection self-test passed" << std::endl;
    return 0;
}

int run_estimator(const Options &options)
{
    const CnfMeta meta = read_cnf_meta(options.cnf_path);
    const int max_watchlist = std::max(
        kMinimumPackedWatchlist,
        options.max_watchlist > 0 ? options.max_watchlist : meta.max_literal_occurrence);
    const int max_phi_literals = std::max(
        meta.max_clause_length,
        options.max_phi_literals > 0 ? options.max_phi_literals : meta.max_clause_length);
    const int max_conflict_literals = std::max(
        1,
        options.max_conflict_literals > 0
            ? options.max_conflict_literals
            : std::max(meta.max_clause_length, meta.variable_count));
    const int max_conflict_clauses = std::max(
        1,
        options.max_conflict_clauses > 0
            ? options.max_conflict_clauses
            : std::max(16, 2 * meta.variable_count + meta.clause_count));

    if (max_phi_literals > meta.variable_count || max_conflict_literals > meta.variable_count)
        throw std::runtime_error("literal capacities may not exceed the CNF variable count");

    init_backend(options);
    base->switch_to_gt();

    const auto construction_timer = clock_start();
    std::unique_ptr<CDCL> solver(new CDCL(
        options.cnf_path, base, PUBLIC, max_watchlist, max_phi_literals,
        max_conflict_literals, max_conflict_clauses, options.verbose, 0,
        options.party, options.use_oram_variable_state, options.use_shortest_watchlist,
        options.watch_backend_request));
    const double construction_seconds = time_from(construction_timer) / 1e6;
    solver->oblivious_decision_delay = options.decision_delay;
    solver->oblivious_conflict_delay = options.conflict_delay;
    solver->uip_loop_cap = options.uip_cap > 0 ? options.uip_cap : meta.variable_count;
    solver->allow_public_early_exit = options.public_early_exit;
    solver->collect_public_step_timings = true;

    std::cout << "PRODUCTION_CONFIG party=" << options.party
              << " cnf=" << options.cnf_path
              << " variables=" << meta.variable_count
              << " clauses=" << meta.clause_count
              << " max_watchlist=" << max_watchlist
              << " max_phi_literals=" << max_phi_literals
              << " max_conflict_literals=" << max_conflict_literals
              << " max_conflict_clauses=" << max_conflict_clauses
              << " uip_cap=" << solver->uip_loop_cap
              << " decision_delay=" << options.decision_delay
              << " conflict_delay=" << options.conflict_delay
              << " variable_state=" << (options.use_oram_variable_state ? "oram" : "linear")
              << " watchlist_strategy=" << (options.use_shortest_watchlist ? "shortest" : "first_fit")
              << " watch_backend_requested=" << watch_backend_name(options.watch_backend_request)
              << " watch_backend_effective=" << watch_backend_name(solver->wl->backend_kind())
              << " watch_backend_reason=" << solver->wl->selection_reason()
              << " threads=" << options.threads
              << " base_port=" << options.base_port
              << std::endl;

    const int result = solver->begin_giant_step(options.sample_steps);
    const std::uint64_t executed_steps = static_cast<std::uint64_t>(solver->step_idx);
    if (result == CDCL::SOLVER_RESULT_FAILURE)
        throw std::runtime_error("sample hit the solver failure state");
    if (options.public_early_exit && executed_steps < static_cast<std::uint64_t>(options.sample_steps) &&
        options.target_steps != executed_steps)
    {
        std::ostringstream out;
        out << "sample terminated after " << executed_steps
            << " steps, but target_steps is " << options.target_steps;
        throw std::runtime_error(out.str());
    }

    const auto expected_sample_counts = schedule_bucket_counts(
        executed_steps, options.decision_delay, options.conflict_delay, max_watchlist);
    const auto target_counts = schedule_bucket_counts(
        options.target_steps, options.decision_delay, options.conflict_delay, max_watchlist);

    double timed_step_seconds = 0.0;
    for (int bucket = 0; bucket < CDCL::PUBLIC_STEP_TIMING_BUCKETS; ++bucket)
    {
        require_equal(static_cast<std::uint64_t>(solver->public_step_timing_counts[bucket]),
                      expected_sample_counts[bucket], "production timing bucket count");
        timed_step_seconds += solver->public_step_timing_seconds[bucket];
    }
    const double raw_fixed_seconds = solver->last_runtime_seconds - timed_step_seconds;
    if (raw_fixed_seconds < -1e-6)
        throw std::runtime_error("per-step timings exceed the measured solver runtime");
    const double fixed_seconds = std::max(0.0, raw_fixed_seconds);

    double projected_loop_seconds = 0.0;
    bool missing_bucket = false;
    std::cout << std::fixed << std::setprecision(9);
    std::cout << "PRODUCTION_SAMPLE party=" << options.party
              << " result=" << result
              << " requested_steps=" << options.sample_steps
              << " executed_steps=" << executed_steps
              << " target_steps=" << options.target_steps
              << " solver_seconds=" << solver->last_runtime_seconds
              << " construction_seconds=" << construction_seconds
              << " timed_step_seconds=" << timed_step_seconds
              << " fixed_seconds=" << fixed_seconds
              << " early_exit=" << (options.public_early_exit ? 1 : 0)
              << std::endl;

    for (int bucket = 0; bucket < CDCL::PUBLIC_STEP_TIMING_BUCKETS; ++bucket)
    {
        const long long sample_count = solver->public_step_timing_counts[bucket];
        const double sample_seconds = solver->public_step_timing_seconds[bucket];
        const double mean_seconds = sample_count > 0 ? sample_seconds / sample_count : 0.0;
        const bool bucket_missing = target_counts[bucket] > 0 && sample_count == 0;
        missing_bucket = missing_bucket || bucket_missing;
        const double bucket_projection = mean_seconds * static_cast<double>(target_counts[bucket]);
        projected_loop_seconds += bucket_projection;
        std::cout << "PRODUCTION_BUCKET party=" << options.party
                  << " bucket=" << bucket
                  << " decision=" << ((bucket & 1) ? 1 : 0)
                  << " conflict=" << ((bucket & 2) ? 1 : 0)
                  << " restart=" << ((bucket & 4) ? 1 : 0)
                  << " source_attempt=" << ((bucket & 8) ? 1 : 0)
                  << " source_refresh=" << ((bucket & 16) ? 1 : 0)
                  << " sample_count=" << sample_count
                  << " sample_seconds=" << sample_seconds
                  << " mean_seconds=" << mean_seconds
                  << " target_count=" << target_counts[bucket]
                  << " projected_seconds=" << bucket_projection
                  << " missing=" << (bucket_missing ? 1 : 0)
                  << std::endl;
    }

    if (missing_bucket)
    {
        std::cout << "PRODUCTION_PROJECTION party=" << options.party
                  << " status=missing_bucket projected_solver_seconds=nan"
                  << std::endl;
        return 2;
    }

    std::cout << "PRODUCTION_PROJECTION party=" << options.party
              << " status=ok"
              << " fixed_seconds=" << fixed_seconds
              << " projected_loop_seconds=" << projected_loop_seconds
              << " projected_solver_seconds=" << (fixed_seconds + projected_loop_seconds)
              << " projected_case_seconds="
              << (construction_seconds + fixed_seconds + projected_loop_seconds)
              << std::endl;
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--self-test")
        return self_test();

    try
    {
        const Options options = parse_options(argc, argv);
        const int status = run_estimator(options);
        delete base;
        base = nullptr;
        return status;
    }
    catch (const std::exception &error)
    {
        std::cerr << "production estimator error: " << error.what() << "\n"
                  << usage(argv[0]) << std::endl;
        delete base;
        base = nullptr;
        return 1;
    }
}
