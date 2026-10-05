#include <iostream>
#include "plaintext_cdcl//src/cdcl.h"
#include "emp-tool/emp-tool.h"

void test_build_wl() {
    std::vector<int> lit_a;
    std::vector<int> lit_b;
    std::vector<int> lit_c;
    std::vector<int> lit_d;
    Clause clause_a(lit_a);
    Clause clause_b(lit_b);
    Clause clause_c(lit_c);
    Clause clause_d(lit_d);
    CDCL cdcl(5);
}

int main() {
}
