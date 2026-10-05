#ifndef PPCDCL_BLK_WATCHLIST_H
#define PPCDCL_BLK_WATCHLIST_H
#include "unordered_map"

struct Watch{
    int clause_idx;
    int blocking_lit;
    Watch(int clause_idx, int blocking_lit): clause_idx(clause_idx), blocking_lit(blocking_lit) {}
};

class BlockWatchList {
public:
    std::unordered_map<int, int> wl; // clause_idx -> blocking literal


    BlockWatchList() {
    }

    void add_clause(int clause_idx) {
        wl.insert({clause_idx, 0});
    }

    void update_block_lit(int clause_idx, int blocking_lit) {
        wl[clause_idx] = blocking_lit;
    }


    void remove_clause(int clause_idx) {
        wl.erase(clause_idx);
    }

    void print() {
        for (auto c: wl) {
            std::cout << c.first << " ";
        }
        std::cout << "\n";
    }

    int size() {
        return wl.size();
    }

};
#endif //PPCDCL_BLK_WATCHLIST_H

