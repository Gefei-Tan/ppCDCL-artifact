#include "plaintext_cdcl/src/cdcl.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
std::string status_string(int solver_ret)
{
    if (solver_ret == 1)
        return "sat";
    if (solver_ret == 0)
        return "unsat";
    return "timeout";
}

void write_json_array_int(const std::vector<int> &vals)
{
    std::cout << "[";
    for (size_t i = 0; i < vals.size(); ++i)
    {
        if (i > 0)
            std::cout << ",";
        std::cout << vals[i];
    }
    std::cout << "]";
}

void write_json_matrix_int(const std::vector<std::vector<int>> &matrix)
{
    std::cout << "[";
    for (size_t i = 0; i < matrix.size(); ++i)
    {
        if (i > 0)
            std::cout << ",";
        write_json_array_int(matrix[i]);
    }
    std::cout << "]";
}

void usage()
{
    std::cerr << "Usage: cdcl_dump <cnf_path> [max_steps]\n";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        usage();
        return 1;
    }

    std::string cnf_path = argv[1];
    long long max_steps = 10000000000LL; // default step budget
    if (argc >= 3)
    {
        max_steps = std::atoll(argv[2]);
        if (max_steps <= 0)
        {
            max_steps = 10000000000LL;
        }
    }
    bool if_verbose = false;
    if (argc > 3)
    {
        std::string verb_arg = argv[3];
        std::transform(verb_arg.begin(), verb_arg.end(), verb_arg.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if_verbose = (verb_arg == "true" || verb_arg == "1" || verb_arg == "yes");
    }

    try
    {
        CDCL solver(cnf_path);
        solver.if_mute = true; // silence solver-side logging for machine-readable output
        int ret = solver.begin_giant_step(max_steps);
        solver.finalize_stats();

        auto units = solver.get_unit_literals_dl0();
        auto learned_entries = solver.get_learned_clause_entries();
        std::vector<std::vector<int>> learned;
        std::vector<int> learned_lbd;
        learned.reserve(learned_entries.size());
        learned_lbd.reserve(learned_entries.size());
        for (const auto &clause : learned_entries)
        {
            learned.push_back(clause.literals);
            learned_lbd.push_back(clause.lbd);
        }
        std::sort(units.begin(), units.end());

        CDCL::CdclStats stats = solver.export_stats();

        std::cout << "{";
        std::cout << "\"status\":\"" << status_string(ret) << "\"";
        std::cout << ",\"units\":";
        write_json_array_int(units);
        std::cout << ",\"learned\":";
        write_json_matrix_int(learned);
        std::cout << ",\"learned_lbd\":";
        write_json_array_int(learned_lbd);
        std::cout << ",\"stats\":{";
        std::cout << "\"var_num\":" << stats.var_num;
        std::cout << ",\"clause_count\":" << stats.clause_count;
        std::cout << ",\"learned_clause_count\":" << stats.learned_clause_count;
        std::cout << ",\"max_wl_size\":" << stats.max_wl_size;
        std::cout << ",\"max_lit_in_clause\":" << stats.max_lit_in_clause;
        std::cout << ",\"max_lit_in_conflict_clause\":" << stats.max_lit_in_conflict_clause;
        std::cout << ",\"max_uip_loop\":" << stats.max_uip_loop;
        std::cout << ",\"orange_blocks\":" << stats.orange_blocks;
        std::cout << ",\"blue_blocks\":" << stats.blue_blocks;
        std::cout << ",\"green_blocks\":" << stats.green_blocks;
        std::cout << ",\"red_blocks\":" << stats.red_blocks;
        std::cout << ",\"yellow_blocks\":" << stats.yellow_blocks;
        std::cout << ",\"step_count\":" << stats.step_count;
        std::cout << ",\"conflict_count\":" << stats.conflict_count;
        std::cout << ",\"decision_count\":" << stats.decision_count;
        std::cout << ",\"restart_count\":" << stats.restart_count;
        std::cout << "}";
        std::cout << "}" << std::endl;
        if (!if_verbose)
            return 0;
        // print length statistics of the learned clauses: min, max, average, median, histogram
        std::vector<int> learned_clause_lengths;
        learned_clause_lengths.reserve(learned.size());
        int min_len = 0;
        int max_len = 0;
        long long sum_len = 0;
        for (const auto &clause : learned)
        {
            int len = static_cast<int>(clause.size());
            learned_clause_lengths.push_back(len);
            if (learned_clause_lengths.size() == 1)
            {
                min_len = len;
                max_len = len;
            }
            else
            {
                if (len < min_len)
                    min_len = len;
                if (len > max_len)
                    max_len = len;
            }
            sum_len += len;
        }

        double avg_len = 0.0;
        double median_len = 0.0;
        std::vector<int> histogram;
        if (!learned_clause_lengths.empty())
        {
            const size_t n = learned_clause_lengths.size();
            avg_len = static_cast<double>(sum_len) / static_cast<double>(n);

            std::vector<int> tmp = learned_clause_lengths;
            const size_t mid = n / 2;
            std::nth_element(tmp.begin(), tmp.begin() + mid, tmp.end());
            if (n % 2 == 1)
            {
                median_len = static_cast<double>(tmp[mid]);
            }
            else
            {
                const int upper = tmp[mid];
                std::nth_element(tmp.begin(), tmp.begin() + mid - 1, tmp.begin() + mid);
                const int lower = tmp[mid - 1];
                median_len = (static_cast<double>(lower) + static_cast<double>(upper)) / 2.0;
            }

            histogram.assign(static_cast<size_t>(max_len) + 1, 0);
            for (int len : learned_clause_lengths)
            {
                ++histogram[static_cast<size_t>(len)];
            }
        }

        std::cout << "{\"learned_clause_length_stats\":{";
        std::cout << "\"count\":" << learned_clause_lengths.size();
        std::cout << ",\"min\":" << min_len;
        std::cout << ",\"max\":" << max_len;
        std::cout << ",\"average\":" << avg_len;
        std::cout << ",\"median\":" << median_len;
        std::cout << ",\"histogram\":";
        write_json_array_int(histogram);
        std::cout << "}}" << std::endl;
        
    }
    catch (const std::exception &ex)
    {
        std::cerr << "cdcl_dump error: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
