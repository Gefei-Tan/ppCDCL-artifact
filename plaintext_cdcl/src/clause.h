#ifndef CLAUSE_H
#define CLAUSE_H

#include "iostream"
#include "vector"

class Clause {
public:
    int length = 0;
    int lbd = -1;
    std::vector<int> literals;

    Clause() {

    }

    Clause(std::vector<int> literals) {
        this->literals = literals;
    }

    Clause(std::vector<std::string> literals_str) {
        this->literals.reserve(literals_str.size());
        for (int i = 0; i < literals_str.size(); ++i) {
            this->literals.push_back(std::atoi(literals_str[i].c_str()));
        }

    }

    void print() const {
        for (auto l: this->literals) {
            std::cout << l << " ";
        }
        std::cout << "\n";
    }
};

#endif //CLAUSE_H
