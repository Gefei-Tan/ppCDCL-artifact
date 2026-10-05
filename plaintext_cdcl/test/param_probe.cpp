#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "emp-tool/emp-tool.h"
#include "plaintext_cdcl/src/cdcl.h"
#include "plaintext_cdcl/test/dimacs_clause_count.h"

struct Overrides
{
    int max_wl_size = -1;
    int max_lit_in_clause = -1;
    int max_lit_in_con_clause = -1;
    int max_num_of_con_clause = -1;
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
};

static void print_usage(const char *prog)
{
    std::cout << "Usage: " << prog
              << " <cnf_path> <con_dly> <dec_dly>"
              << " [--max-wl-size N] [--max-lit-in-clause N]"
              << " [--max-lit-in-con-clause N] [--max-num-of-con-clause N]\n";
}

static Overrides parse_overrides(const std::vector<std::string> &args)
{
    Overrides over;
    for (size_t i = 0; i + 1 < args.size(); ++i)
    {
        const std::string &flag = args[i];
        const std::string &val_str = args[i + 1];
        try
        {
            if (flag == "--max-wl-size")
            {
                over.max_wl_size = std::stoi(val_str);
            }
            else if (flag == "--max-lit-in-clause")
            {
                over.max_lit_in_clause = std::stoi(val_str);
            }
            else if (flag == "--max-lit-in-con-clause")
            {
                over.max_lit_in_con_clause = std::stoi(val_str);
            }
            else if (flag == "--max-num-of-con-clause")
            {
                over.max_num_of_con_clause = std::stoi(val_str);
            }
        }
        catch (const std::exception &)
        {
            std::cerr << "Failed to parse value for flag " << flag << " (" << val_str << ")" << std::endl;
        }
    }
    return over;
}

static ProbeOutput run_probe(const std::string &cnf_path, int con_dly, int dec_dly, const Overrides &over)
{
    // First run: measure the natural maxima, used where no override is given.
    CDCL probe(cnf_path);
    probe.begin_giant_step(1e10);

    int chosen_wl = over.max_wl_size > 0 ? over.max_wl_size : probe.max_wl_size;
    int chosen_max_lit_clause = over.max_lit_in_clause > 0 ? over.max_lit_in_clause : probe.max_lit_in_clause;
    int chosen_max_lit_conf_clause =
        over.max_lit_in_con_clause > 0 ? over.max_lit_in_con_clause : probe.max_lit_in_conflict_clause;
    int chosen_max_conf_clause =
        over.max_num_of_con_clause > 0 ? over.max_num_of_con_clause : static_cast<int>(probe.conflict_phi.size());

    // Second run: enforce chosen parameters for block counting.
    CDCL cdcl(cnf_path);
    cdcl.emp_max_wl_size = std::max(chosen_wl, probe.max_wl_size);
    cdcl.emp_max_conflict_phi_size = std::max(chosen_max_conf_clause, 1);
    cdcl.oblivious_conflict_delay = std::max(con_dly, 1);
    cdcl.oblivious_decision_delay = std::max(dec_dly, 1);

    auto t1 = emp::clock_start();
    cdcl.begin_giant_step(1e10);
    double plaintext_time_ms = emp::time_from(t1) / 1000.0;

    ProbeOutput out{};
    out.var_num = cdcl.var_num;
    out.clause_num = dimacs_clause_count(cdcl.phi);
    out.max_num_of_con_clause =
        std::max<int>(std::max<int>(static_cast<int>(cdcl.conflict_phi.size()), chosen_max_conf_clause), 1);
    out.max_lit_in_clause = std::max(std::max(cdcl.max_lit_in_clause, chosen_max_lit_clause), 1);
    out.max_lit_in_con_clause = std::max(std::max(cdcl.max_lit_in_conflict_clause, chosen_max_lit_conf_clause), 1);
    out.max_clause_in_wl = std::max(std::max(cdcl.max_wl_size, cdcl.emp_max_wl_size), 1);
    out.orange_total = cdcl.orange_count;
    out.yellow_total = cdcl.yellow_count;
    out.blue_total = cdcl.blue_count;
    out.red_total = cdcl.red_count;
    out.con_dly = cdcl.oblivious_conflict_delay;
    out.dec_dly = cdcl.oblivious_decision_delay;
    out.plaintext_time_seconds = plaintext_time_ms / 1000.0;

    return out;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        print_usage(argv[0]);
        return 1;
    }

    std::string cnf_path = argv[1];
    int con_dly = std::atoi(argv[2]);
    int dec_dly = std::atoi(argv[3]);
    std::vector<std::string> extra_args;
    for (int i = 4; i < argc; ++i)
    {
        extra_args.emplace_back(argv[i]);
    }
    Overrides over = parse_overrides(extra_args);

    std::cout << "=============PARAM PROBE START================" << std::endl;
    std::cout << cnf_path << std::endl;
    if (over.max_wl_size > 0)
        std::cout << "override max_wl_size: " << over.max_wl_size << std::endl;
    if (over.max_lit_in_clause > 0)
        std::cout << "override max_lit_in_clause: " << over.max_lit_in_clause << std::endl;
    if (over.max_lit_in_con_clause > 0)
        std::cout << "override max_lit_in_con_clause: " << over.max_lit_in_con_clause << std::endl;
    if (over.max_num_of_con_clause > 0)
        std::cout << "override max_num_of_con_clause: " << over.max_num_of_con_clause << std::endl;

    auto result = run_probe(cnf_path, con_dly, dec_dly, over);

    std::cout << "============PLAINTEXT TIME=============" << std::endl;
    std::cout << "total time: " << result.plaintext_time_seconds << " s" << std::endl;
    std::cout << "=============PARAM PROBE END================" << std::endl;

    // The final line uses the same format as the output of test_big.
    std::cout << result.var_num << " " << result.clause_num << " " << result.max_num_of_con_clause << " "
              << result.max_lit_in_clause << " " << result.max_lit_in_con_clause << " " << result.max_clause_in_wl
              << " " << result.orange_total << " " << result.yellow_total << " " << result.blue_total << " "
              << result.red_total << " " << result.con_dly << " " << result.dec_dly << std::endl;

    return 0;
}
