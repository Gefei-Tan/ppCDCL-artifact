#ifndef DATA_STRUCTURES_H
#define DATA_STRUCTURES_H

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <functional>
#include "src/clause.h"

typedef std::function<bool(int)> Predicate;

class Activities {
public:
    std::vector<double> act;

    Activities(int var_num) {
        act.resize(var_num + 1, 0.0);
    }

    void update(std::vector<double> increment) {
        for (int i = 0; i < act.size(); ++i) {
            act[i] += increment[i];
        }
    }

    // Return the index of the highest value that satisfies the predicate.
    int readBest(Predicate pred) {
        int best = 0;
        for (int i = 1; i < act.size(); ++i) {
            if (pred(i) && act[i] > act[best]) {
                best = i;
            }
        }
        return best;
    }
};

class WatchLists {
public:
    std::unordered_map<int, std::vector<Clause *>> wl;

    void insert(int literal, Clause *clause) {
        wl[literal].push_back(clause);
    }

    void remove(int literal, Clause *clause) {
        auto &vec = wl[literal];
        vec.erase(std::remove(vec.begin(), vec.end(), clause), vec.end());
    }

    const std::vector<Clause *> &getList(int literal) const {
        auto it = wl.find(literal);
        if (it != wl.end()) {
            return it->second;
        }
        static std::vector<Clause *> empty;
        return empty;
    }
};

class Big_U {
public:
    std::unordered_set<int> unit_literals;

    void push(int literal) {
        unit_literals.insert(literal);
    }

    // Pop an arbitrary literal from the set.
    int pop() {
        if (!unit_literals.empty()) {
            int literal = *unit_literals.begin();
            unit_literals.erase(unit_literals.begin());
            return literal;
        }
        return 0; // Return 0 when empty.
    }

    void clear() {
        unit_literals.clear();
    }
};

class ConPhi {
public:
    int public_idx = 0;
    std::vector<Clause> conflictClauses;

    int append(const Clause &clause) {
        conflictClauses.push_back(clause);
        public_idx++;
        return public_idx - 1;
    }

    Clause read(/*private*/int idx) {
        return conflictClauses[idx];
    }
};

#endif // DATA_STRUCTURES_H
