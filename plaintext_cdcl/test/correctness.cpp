#include <iostream>
#include "plaintext_cdcl/src/cdcl.h"

int main() {
    /* UNSAT test */
    int i;
    float no_preprocess_time = 0;
    int no_preprocess_conflict_count = 0;
    int no_preprocess_step_count = 0;
    auto t1 = emp::clock_start();
    for (i = 1; i < 2; ++i) {
       string file_name = "./UUF50.218.1000/uuf50-0546.cnf";
        CDCL cdcl(file_name);
        int if_sat = cdcl.begin_giant_step(1e7);
        no_preprocess_time += emp::time_from(t1);
        no_preprocess_conflict_count += cdcl.conflict_ctr;
        no_preprocess_step_count += cdcl.step_idx;
        if (if_sat != 0) {
            std::cout << file_name << std::endl;
            std::cout << "unexpected solver result" << std::endl;
            break;
        }
    }

    auto t2 = emp::time_from(t1);
    std::cout << "total time: " << t2 / 1000.0 / 1000.0 << " s" << std::endl;
    std::cout << " time per instance: " << t2 / 1000.0 / 1000.0 / (i - 1) << " s" << std::endl;
    std::cout << "step count: " << no_preprocess_step_count << std::endl;
    std::cout << "conflict count: " << no_preprocess_conflict_count << std::endl;
    return 0;
}
