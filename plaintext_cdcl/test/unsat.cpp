#include <filesystem>
#include <fstream>
#include "plaintext_cdcl//src/cdcl.h"
#include "emp-tool/emp-tool.h"

namespace fs = std::filesystem;
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cout << "Please provide the path to the directory containing CNF files." << std::endl;
        return 1;
    }
    string path = argv[1];
    int uip_loop_cap = argc >= 3 ? std::atoi(argv[2]) : 0;
    int ruby_interval = argc >= 4 ? std::atoi(argv[3]) : 128;
    int learned_clause_len_cap = argc >= 5 ? std::atoi(argv[4]) : 0;
    int con_dly = argc >= 6 ? std::atoi(argv[5]) : 1;
    int dec_dly = argc >= 7 ? std::atoi(argv[6]) : 1;
    std::cout << "Using uip_loop_cap: " << uip_loop_cap << ", ruby_interval: " << ruby_interval << ", learned_clause_len_cap: " << learned_clause_len_cap << std::endl;
    vector<int> wls_size;
    vector<int> conflict_clause_size;
    vector<int> uip_loop_size;
    int total_conflict = 0, total_step = 0;
    int total_learned_clause = 0;
    int total_restart = 0;
    int total_variable = 0;
    int total_clause = 0;
    long long total_mock_step = 0;
    long long total_lucky_mock_step = 0;
    long long total_cur_wl = 0;
    long long total_decide = 0;
    long long total_big_U = 0;
    long long total_unit = 0;
    long long total_conflict_ = 0;
    long long total_not_unit = 0;
    long long total_blank = 0;
    long long total_max_wl_size = 0;

    int i = 1;
    auto t1 = emp::clock_start();
    // collect the CNF files under the given path
    vector<string> cnf_files;
    // a single .cnf file is used as is
    if (fs::is_regular_file(path) && fs::path(path).extension() == ".cnf")
    {
        cnf_files.push_back(path);
    }
    else
    {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(path))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".cnf")
                cnf_files.emplace_back(entry.path().string());
        }
    }
    int timeout_num = 0;
    int file_count = cnf_files.size();
    std::cout << "Found " << file_count << " CNF files in the directory." << std::endl;
    for (auto &file_name : cnf_files)
    {
        std::cout << "Processing file: " << file_name << std::endl;
        CDCL cdcl(file_name);
        cdcl.learned_clause_len_cap = learned_clause_len_cap;
        cdcl.ruby_restart_unit = ruby_interval;
        cdcl.clause_cap_policy = CDCL::CLAUSE_CAP_NON_CHRONO_BACKTRACK;
        cdcl.uip_loop_cap = uip_loop_cap;
        cdcl.oblivious_decision_delay = dec_dly;
        cdcl.oblivious_conflict_delay = con_dly;
        int if_sat = cdcl.begin_giant_step(1e7);
        wls_size.insert(wls_size.end(), cdcl.wls_size.begin(), cdcl.wls_size.end());
        conflict_clause_size.insert(conflict_clause_size.end(), cdcl.conflict_clause_size.begin(), cdcl.conflict_clause_size.end());
        uip_loop_size.insert(uip_loop_size.end(), cdcl.uip_loop_size.begin(), cdcl.uip_loop_size.end());

        total_variable += cdcl.var_num;
        total_clause += cdcl.phi.size();
        total_learned_clause += cdcl.conflict_phi.size();
        total_conflict += cdcl.conflict_ctr;
        total_restart += cdcl.restart_ctr;
        total_step += cdcl.step_idx;
        total_mock_step += cdcl.mock_step_idx;
        total_lucky_mock_step += cdcl.lucky_mock_step_idx;
        if (total_max_wl_size < cdcl.max_wl_size)
            total_max_wl_size = cdcl.max_wl_size;
        total_cur_wl += cdcl.procedure_log.find("cur_wl")->second;
        total_decide += cdcl.procedure_log.find("decide")->second;
        total_big_U += cdcl.procedure_log.find("big_U")->second;
        total_unit += cdcl.procedure_log.find("unit")->second;
        total_conflict_ += cdcl.procedure_log.find("conflict")->second;
        total_not_unit += cdcl.procedure_log.find("not unit")->second;
        total_blank += cdcl.procedure_log.find("blank")->second;
        if (if_sat == -1)
            timeout_num++;
    }
    std::cout << "============TOTAL UIP STATs=============" << std::endl;
    CDCL::print_histogram(uip_loop_size, "UIP Loop Size");

    auto total_time = emp::time_from(t1);
    std::cout << "### STATS ###" << std::endl;
    std::cout << "timeout on " << timeout_num << "/" << file_count << "files." << std::endl;
    file_count = file_count - timeout_num;
    std::cout << "avg #variable: " << total_variable / file_count << std::endl;
    std::cout << "avg #clause: " << total_clause / file_count << std::endl;
    std::cout << "avg_step_count: " << total_step / file_count << std::endl;
    std::cout << "max_wl_size: " << total_max_wl_size << std::endl;
    std::cout << "avg_mock_step_count: " << total_mock_step / file_count << std::endl;
    std::cout << "avg_lucky_mock_step_count: " << total_lucky_mock_step / file_count << std::endl;
    std::cout << "avg_conflict_count: " << total_conflict / file_count << std::endl;
    std::cout << "avg_learned_clause: " << total_learned_clause / file_count << std::endl;
    std::cout << "avg_restart: " << total_restart / file_count << std::endl;
    std::cout << "avg_time: " << total_time / 1000.0 / 1000.0 / file_count << " s" << std::endl;
    std::cout << "total time: " << total_time / 1000.0 / 1000.0 << " s" << std::endl;
    std::cout << "total_cur_wl: " << (double)total_cur_wl / (double)total_step * 100.0 << "%" << std::endl;
    std::cout << "total_decide: " << (double)total_decide / (double)total_step * 100.0 << "%" << std::endl;
    std::cout << "total_big_U: " << (double)total_big_U / (double)total_step * 100.0 << "%" << std::endl;
    std::cout << "total_unit: " << (double)total_unit / (double)total_step * 100.0 << "%" << std::endl;
    std::cout << "total_conflict: " << (double)total_conflict_ / (double)total_step * 100.0 << "%" << std::endl;
    std::cout << "total_not_unit: " << (double)total_not_unit / (double)total_step * 100.0 << "%" << std::endl;
    std::cout << "total_blank: " << (double)total_blank / (double)total_step * 100.0 << "%" << std::endl;
    return 0;
}
