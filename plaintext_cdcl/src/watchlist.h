#ifndef PPCDCL_WATCHLIST_H
#define PPCDCL_WATCHLIST_H
#include <algorithm>
#include <vector>

class WatchList {
public:
    // Fixed-slot watchlist, as in the secure solver: removing a clause leaves a
    // zero in its slot and a new clause takes the first empty slot. The slot
    // order fixes the clause scan order, and with it the CDCL trajectory.
    std::vector<int> wl;


    WatchList() {
    }

    void add_clause(int clause_idx) {
        if (clause_idx == 0 || contains(clause_idx)) {
            return;
        }
        auto empty = std::find(wl.begin(), wl.end(), 0);
        if (empty != wl.end()) {
            *empty = clause_idx;
        } else {
            wl.push_back(clause_idx);
        }
    }

    void remove_clause(int clause_idx) {
        auto found = std::find(wl.begin(), wl.end(), clause_idx);
        if (found != wl.end()) {
            *found = 0;
        }
    }

    void print() {
        for (auto c: wl) {
            if (c != 0) {
                std::cout << c << " ";
            }
        }
        std::cout << "\n";
    }

    int size() const {
        return static_cast<int>(std::count_if(wl.begin(), wl.end(),
                                              [](int clause_idx) { return clause_idx != 0; }));
    }

    bool empty() const {
        return size() == 0;
    }

    bool contains(int clause_idx) const {
        return clause_idx != 0 && std::find(wl.begin(), wl.end(), clause_idx) != wl.end();
    }

};

#endif //PPCDCL_WATCHLIST_H
