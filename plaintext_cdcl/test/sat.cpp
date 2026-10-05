#include <iostream>
#include "plaintext_cdcl/src/cdcl.h"
#include "emp-tool/emp-tool.h"

int main()
{
    vector<int> wls_size;
    vector<int> conflict_clause_size;
    vector<int> uip_loop_size;
    int total_conflict = 0, total_step = 0;
    int total_learned_clause = 0;
    int total_restart = 0;
    int total_variable = 0;
    int total_clause = 0;
    long long total_mock_step = 0;
    long long total_cur_wl = 0;
    long long total_decide = 0;
    long long total_big_U = 0;
    long long total_unit = 0;
    long long total_conflict_ = 0;
    long long total_not_unit = 0;
    long long total_blank = 0;
    long long total_max_wl_size = 0;
    long long lucky_mock_step = 0;

    int i = 1;
    auto t1 = emp::clock_start();
    for (i = 1; i < 29; ++i)
    {
        std::string file_name = "n8/genos.haps." + std::to_string(i) + ".cnf";
        std::cout << file_name << std::endl;
        CDCL cdcl(file_name);
        {
            cdcl.oblivious_decision_delay = 1;
            cdcl.oblivious_conflict_delay = 1;
        }
        cdcl.emp_max_wl_size = 1;
        int if_sat = cdcl.begin_giant_step(1e8);
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
        lucky_mock_step += cdcl.lucky_mock_step_idx;
        total_cur_wl += cdcl.procedure_log.find("cur_wl")->second;
        total_decide += cdcl.procedure_log.find("decide")->second;
        total_big_U += cdcl.procedure_log.find("big_U")->second;
        total_unit += cdcl.procedure_log.find("unit")->second;
        total_conflict_ += cdcl.procedure_log.find("conflict")->second;
        total_not_unit += cdcl.procedure_log.find("not unit")->second;
        total_blank += cdcl.procedure_log.find("blank")->second;
        if (total_max_wl_size < cdcl.max_wl_size)
            total_max_wl_size = cdcl.max_wl_size;
        if (if_sat == -1 || if_sat == 0)
        {
            std::cout << "uf20-0" << i << std::endl;
            std::cout << "unexpected solver result" << std::endl;
            break;
        }
    }
    std::cout << "============WL STATs=============" << std::endl;
    CDCL::print_histogram(wls_size, "Watch List Size");
    std::cout << "============CONFLICT CLAUSE STATs=============" << std::endl;
    CDCL::print_histogram(conflict_clause_size, "Conflict Clause Size");
    std::cout << "============UIP STATs=============" << std::endl;
    CDCL::print_histogram(uip_loop_size, "UIP Loop Size");
    auto total_time = emp::time_from(t1);
    std::cout << "### STATS ###" << std::endl;
    std::cout << "avg #variable: " << total_variable / (i - 1) << std::endl;
    std::cout << "avg #clause: " << total_clause / (i - 1) << std::endl;
    std::cout << "avg_step_count: " << total_step / (i - 1) << std::endl;
    std::cout << "max_wl_size: " << total_max_wl_size << std::endl;
    std::cout << "avg_mock_step_count: " << total_mock_step / (i - 1) << std::endl;
    std::cout << "avg_lucky_mock_step_count: " << lucky_mock_step / (i - 1) << std::endl;
    std::cout << "avg_conflict_count: " << total_conflict / (i - 1) << std::endl;
    std::cout << "avg_learned_clause: " << total_learned_clause / (i - 1) << std::endl;
    std::cout << "avg_restart: " << total_restart / (i - 1) << std::endl;
    std::cout << "avg_time: " << total_time / 1000.0 / 1000.0 / (i - 1) << " s" << std::endl;
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
