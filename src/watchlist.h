#ifndef PPCDCL_WATCHLIST_H
#define PPCDCL_WATCHLIST_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

#include "clause.h"
#include "util.h"

enum class WatchBackendRequest
{
    Auto,
    BlockLocal,
    Indexed,
};

enum class WatchBackendKind
{
    BlockLocal,
    Indexed,
};

inline const char *watch_backend_name(WatchBackendRequest request)
{
    switch (request)
    {
    case WatchBackendRequest::Auto:
        return "auto";
    case WatchBackendRequest::BlockLocal:
        return "block-local";
    case WatchBackendRequest::Indexed:
        return "indexed";
    }
    throw std::logic_error("unknown watch backend request");
}

inline const char *watch_backend_name(WatchBackendKind kind)
{
    switch (kind)
    {
    case WatchBackendKind::BlockLocal:
        return "block-local";
    case WatchBackendKind::Indexed:
        return "indexed";
    }
    throw std::logic_error("unknown watch backend kind");
}

inline WatchBackendRequest parse_watch_backend_request(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    std::replace(value.begin(), value.end(), '_', '-');
    if (value == "auto")
        return WatchBackendRequest::Auto;
    if (value == "block" || value == "block-local" ||
        value == "blocklocal" || value == "packed")
        return WatchBackendRequest::BlockLocal;
    if (value == "indexed")
        return WatchBackendRequest::Indexed;
    throw std::invalid_argument("unknown watch backend '" + value +
                                "' (expected auto, block-local, or indexed)");
}

class WatchList
{
public:
    // Each literal's watchlist is a fixed-size list of clause indices stored in ORAM blocks
    FloRAM<NetIO> *wl_oram = nullptr;
    // wl_oram is the "Data" store. pair_oram ("Pair") and occ_oram ("Occ") are
    // allocated only for the indexed backend. Pair is keyed by a unified
    // original/learned clause key; Occ is keyed by the same signed-literal
    // mapping as Data.
    FloRAM<NetIO> *pair_oram = nullptr;
    FloRAM<NetIO> *occ_oram = nullptr;
    Integer offset;
    WatchBackendKind selected_backend = WatchBackendKind::BlockLocal;
    std::string backend_selection_reason = "not initialized";
    // Local-only counters for the fixed public access shape. They never
    // inspect or reveal secret indices or contents.
    std::uint64_t logical_range_reads = 0;
    std::uint64_t logical_watchlist_writes = 0;
    std::uint64_t logical_point_writes = 0;
    std::uint64_t logical_pair_reads = 0;
    std::uint64_t logical_pair_writes = 0;
    std::uint64_t logical_occ_reads = 0;
    std::uint64_t logical_occ_writes = 0;

    struct AddClauseResult
    {
        Bit inserted;
        Bit overflow;
    };

    struct InstallResult
    {
        Bit installed;
        Bit overflow;
    };

    struct MoveResult
    {
        Bit moved;
        Bit overflow;
    };

    struct EmptySlot
    {
        Integer slot;
        Bit available;
    };

    WatchList()
    {
    }

    ~WatchList()
    {
        delete wl_oram;
        delete pair_oram;
        delete occ_oram;
    }

    WatchList(FloramMPC<NetIO> *backend,
              WatchBackendRequest request = WatchBackendRequest::Auto)
    {
        init(backend, request);
    }

    void init(FloramMPC<NetIO> *backend,
              WatchBackendRequest request = WatchBackendRequest::Auto)
    {
        delete wl_oram;
        delete pair_oram;
        delete occ_oram;
        wl_oram = nullptr;
        pair_oram = nullptr;
        occ_oram = nullptr;

        if (request == WatchBackendRequest::Indexed && MAX_CLAUSE_IN_WL > 128)
        {
            throw std::invalid_argument(
                "indexed watch backend requires public W <= 128");
        }
        select_backend_from_public_geometry(request);

        wl_oram = new FloRAM<NetIO>(backend, WL_ORAM_SIZE_BIT,
                                    WL_ORAM_UNIT_SIZE, -1, nullptr, -1, -1,
                                    false, false);
        if (selected_backend == WatchBackendKind::Indexed)
        {
            pair_oram = new FloRAM<NetIO>(backend, WATCH_PAIR_ORAM_SIZE_BIT,
                                          WATCH_PAIR_ORAM_UNIT_SIZE, -1,
                                          nullptr, -1, -1, false, false);
            occ_oram = new FloRAM<NetIO>(backend, WATCH_OCC_ORAM_SIZE_BIT,
                                         WATCH_OCC_ORAM_UNIT_SIZE, -1,
                                         nullptr, -1, -1, false, false);
        }
        offset = Integer(WL_ORAM_SIZE_BIT, VAR_NUM, PUBLIC);

        if (IF_DEBUG)
        {
            std::cout << "#####WatchList init! "
                      << "\noram size: " << (WL_ORAM_SIZE_BIT)
                      << "\nORAM block size (bit): " << (1 << WL_ORAM_UNIT_SIZE)
                      << "\n# of blocks for one watchlist: " << WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL
                      << "\nMax clauses per watchlist: " << MAX_CLAUSE_IN_WL
                      << "\nbackend: " << watch_backend_name(selected_backend)
                      << std::endl;
        }
    }

    WatchBackendKind backend_kind() const
    {
        return selected_backend;
    }

    const std::string &selection_reason() const
    {
        return backend_selection_reason;
    }

    long double estimated_floram_physical_blocks(int logical_size_bits,
                                                 int unit_size_bits) const
    {
        const int unit_element_bits = 7 - unit_size_bits;
        const int physical_address_bits =
            std::max(2, logical_size_bits - unit_element_bits);
        return std::ldexp(static_cast<long double>(1.0),
                          physical_address_bits);
    }

    void select_backend_from_public_geometry(WatchBackendRequest request)
    {
        if (request == WatchBackendRequest::BlockLocal)
        {
            selected_backend = WatchBackendKind::BlockLocal;
            backend_selection_reason = "explicit block-local request";
            return;
        }
        if (request == WatchBackendRequest::Indexed)
        {
            selected_backend = WatchBackendKind::Indexed;
            backend_selection_reason = "explicit indexed request; public W <= 128";
            return;
        }
        if (MAX_CLAUSE_IN_WL > 128)
        {
            selected_backend = WatchBackendKind::BlockLocal;
            backend_selection_reason =
                "auto: indexed occupancy bitmap exceeds 128 public bits";
            return;
        }

        const long double data_blocks = estimated_floram_physical_blocks(
            WL_ORAM_SIZE_BIT, WL_ORAM_UNIT_SIZE);
        const long double pair_blocks = estimated_floram_physical_blocks(
            WATCH_PAIR_ORAM_SIZE_BIT, WATCH_PAIR_ORAM_UNIT_SIZE);
        const long double occ_blocks = estimated_floram_physical_blocks(
            WATCH_OCC_ORAM_SIZE_BIT, WATCH_OCC_ORAM_UNIT_SIZE);

        // One scalar private write performs both an origin scan and an OWOM
        // update. A range read performs one domain scan per returned block in
        // the current FloRAM implementation. Only the leading public
        // physical-domain work is counted; LowMC and stash constants are omitted.
        const long double block_local_cost =
            2.0L * data_blocks * WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL +
            4.0L * data_blocks;
        const long double indexed_cost =
            3.0L * pair_blocks + // one read and one two-scan write
            4.0L * data_blocks + // two point writes
            5.0L * occ_blocks;   // one read and two point writes

        // The physical-domain estimate captures asymptotic scaling, but it
        // underprices the fixed LowMC/stash overhead of seven protected
        // Indexed calls. Measurements on benchmark-range public geometries put
        // the crossover at four Data words per list. The cost comparison is also
        // kept as a guard for unusual clause/literal ratios.
        static constexpr int INDEXED_MIN_DATA_BLOCKS_PER_LIST = 4;
        const bool indexed_has_enough_amortization =
            WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL >=
            INDEXED_MIN_DATA_BLOCKS_PER_LIST;
        selected_backend = indexed_has_enough_amortization &&
                                   indexed_cost < block_local_cost
                               ? WatchBackendKind::Indexed
                               : WatchBackendKind::BlockLocal;
        std::ostringstream reason;
        reason << "auto public cost: block-local="
               << static_cast<unsigned long long>(block_local_cost)
               << ", indexed="
               << static_cast<unsigned long long>(indexed_cost)
               << ", W=" << MAX_CLAUSE_IN_WL
               << ", data_blocks/list=" << WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL
               << ", calibrated_min_blocks="
               << INDEXED_MIN_DATA_BLOCKS_PER_LIST;
        backend_selection_reason = reason.str();
    }

    // Convert a literal index (which may be negative for negated literals) to a storage index
    Integer parse_lit_idx(Integer const &lit_idx) const
    {
        Bit if_greater = lit_idx >= Integer(WL_ORAM_SIZE_BIT, 0, PUBLIC);
        Integer neg_lit_idx = lit_idx.abs() + offset;

        // Both operands of If must have the same size.
        if (lit_idx.size() != neg_lit_idx.size())
        {
            neg_lit_idx.resize(lit_idx.size());
        }

        return If(if_greater, lit_idx, neg_lit_idx);
    }

    int parse_lit_idx(int lit_idx)
    {
        return lit_idx > 0 ? lit_idx : -lit_idx + VAR_NUM;
    }

    // Compute the ORAM index for a given literal's watchlist block
    Integer compute_watchlist_index(const Integer &lit_idx, const Integer &block_offset)
    {
        Integer parsed = parse_lit_idx(lit_idx);
        Integer multiplier = Integer(parsed.size(), WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL, PUBLIC);
        return parsed * multiplier + block_offset;
    }

    // Add a clause with one read-modify-write, exposing only secret status bits
    // to the caller. `inserted` distinguishes a new insertion from an
    // existing membership; `overflow` marks a requested new insertion with no
    // empty slot.
    AddClauseResult add_clause_with_status(Integer lit_idx, Integer clause_idx, Bit flag = true)
    {
        std::vector<Integer> current_wl = get_watchlist_clauses(lit_idx);

        if (clause_idx.size() != CLAUSE_IDX_SIZE_IN_WL)
        {
            clause_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
        }

        Integer zero(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);

        // Check whether the clause is already present. This avoids
        // duplicating an existing clause into an earlier empty slot after
        // removals have left holes in the fixed-size watchlist.
        Bit found_existing = Bit(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; i++)
        {
            found_existing = found_existing | current_wl[i].equal(clause_idx);
        }

        // Insert into the first empty slot only if this clause is not already
        // being watched by the literal.
        Bit inserted = Bit(false, PUBLIC);
        const int slot_bits = std::max(bits_required(MAX_CLAUSE_IN_WL - 1), 1);
        Integer inserted_slot(slot_bits, 0, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; i++)
        {
            Bit is_empty = current_wl[i].equal(zero);
            Bit should_insert = flag & !found_existing & is_empty & !inserted;
            current_wl[i] = If(should_insert, clause_idx, current_wl[i]);
            inserted_slot = If(should_insert,
                               Integer(slot_bits, i, PUBLIC), inserted_slot);
            inserted = inserted | should_insert;
        }

        write_watchlist_clauses(lit_idx, current_wl);
        if (selected_backend == WatchBackendKind::Indexed)
        {
            // add_clause updates Occ but cannot establish immutable Pair
            // positions. Clauses added this way must not be passed to
            // move_clause_from_frame until Pair is installed for them.
            write_occ_slot(lit_idx, inserted_slot, Bit(true, PUBLIC), inserted);
        }
        Bit overflow = flag & !found_existing & !inserted;
        return {inserted, overflow};
    }

    // Add a clause to the watchlist obliviously.
    // Returns true when insertion was requested but the target watchlist had
    // neither the clause already present nor an empty slot.
    Bit add_clause(Integer lit_idx, Integer clause_idx, Bit flag = true)
    {
        return add_clause_with_status(lit_idx, clause_idx, flag).overflow;
    }

    // Remove a clause from the watchlist obliviously
    void remove_clause(Integer lit_idx, Integer clause_idx, Bit flag = true)
    {
        std::vector<Integer> current_wl = get_watchlist_clauses(lit_idx);

        Integer zero(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);

        const int slot_bits = std::max(bits_required(MAX_CLAUSE_IN_WL - 1), 1);
        Integer removed_slot(slot_bits, 0, PUBLIC);
        Bit removed(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; i++)
        {
            Bit is_target = current_wl[i].equal(clause_idx);
            Bit remove_here = flag & is_target & !removed;

            current_wl[i] = If(flag & is_target, zero, current_wl[i]);
            removed_slot = If(remove_here,
                              Integer(slot_bits, i, PUBLIC), removed_slot);
            removed = removed | remove_here;
        }

        write_watchlist_clauses(lit_idx, current_wl);
        if (selected_backend == WatchBackendKind::Indexed)
        {
            write_occ_slot(lit_idx, removed_slot, Bit(false, PUBLIC), removed);
        }
    }

    // Atomically install up to two distinct watcher endpoints. Positions are
    // immutable, 1-based indices into the padded clause; position zero means
    // "no endpoint". Indexed mode maintains Pair+Occ+Data together. The
    // block-local fallback performs two range reads and two point writes and
    // deliberately ignores Pair metadata.
    InstallResult install_clause_watchers(Integer clause_idx,
                                           Integer lit1, Integer pos1, Bit valid1,
                                           Integer lit2, Integer pos2, Bit valid2,
                                           Bit flag = Bit(true, PUBLIC))
    {
        Bit valid_clause = valid_clause_id_domain(clause_idx);
        Bit use1 = flag & valid_clause & valid1 &
                   valid_literal_domain(lit1) & valid_watch_position(pos1);
        // A repeated literal is one effective watcher even when it appears at
        // two immutable clause positions.
        // Canonical Pair encodings are (0,0), (p,0), or (p0,p1): a second
        // endpoint is meaningful only when the first endpoint is valid.
        Bit use2 = use1 & valid2 & valid_literal_domain(lit2) &
                   valid_watch_position(pos2) & !lit1.equal(lit2);

        if (selected_backend == WatchBackendKind::Indexed)
        {
            Bit requested = use1 | use2;
            Integer safe_lit1 = secret_or_zero(lit1, use1);
            Integer safe_lit2 = secret_or_zero(lit2, use2);
            Integer occ1 = read_occ(safe_lit1);
            Integer occ2 = read_occ(safe_lit2);
            EmptySlot first = first_empty_from_occ(occ1);
            EmptySlot second = first_empty_from_occ(occ2);

            Bit overflow = (use1 & !first.available) |
                           (use2 & !second.available);
            // Clause IDs are installed exactly once. Gating Pair by
            // `requested` makes a call with no valid endpoint a no-op instead
            // of clearing existing Pair metadata. Pair is not pre-read, to keep
            // initialization cheap, so callers must not reinstall an
            // already-installed ID.
            Bit commit = requested & !overflow;

            Integer stored_clause = clause_idx;
            stored_clause.resize(CLAUSE_IDX_SIZE_IN_WL);
            write_data_slot(safe_lit1, first.slot, stored_clause,
                            commit & use1);
            write_data_slot(safe_lit2, second.slot, stored_clause,
                            commit & use2);
            write_occ_slot(safe_lit1, first.slot, Bit(true, PUBLIC),
                           commit & use1);
            write_occ_slot(safe_lit2, second.slot, Bit(true, PUBLIC),
                           commit & use2);

            Integer stored_pos1 = normalize_watch_position(pos1, use1);
            Integer stored_pos2 = normalize_watch_position(pos2, use2);
            Integer pair_value = pack_pair(stored_pos1, stored_pos2);
            write_pair(clause_idx, pair_value, commit);

            return {commit, overflow};
        }

        Integer safe_lit1 = secret_or_zero(lit1, use1);
        Integer safe_lit2 = secret_or_zero(lit2, use2);
        std::vector<Integer> list1 = get_watchlist_clauses(safe_lit1);
        std::vector<Integer> list2 = get_watchlist_clauses(safe_lit2);
        Integer stored_clause = clause_idx;
        stored_clause.resize(CLAUSE_IDX_SIZE_IN_WL);

        Bit found1 = list_contains(list1, stored_clause);
        Bit found2 = list_contains(list2, stored_clause);
        EmptySlot first = first_empty_from_list(list1);
        EmptySlot second = first_empty_from_list(list2);
        Bit need1 = use1 & !found1;
        Bit need2 = use2 & !found2;
        Bit overflow = (need1 & !first.available) |
                       (need2 & !second.available);
        Bit commit = flag & !overflow;

        write_data_slot(safe_lit1, first.slot, stored_clause,
                        commit & need1);
        write_data_slot(safe_lit2, second.slot, stored_clause,
                        commit & need2);
        Bit requested = use1 | use2;
        return {commit & requested, overflow};
    }

    // Move the clause selected from the active source-frame cache. The source
    // slot is secret: callers must pass the same secret cursor used to select
    // clause_idx from that cache. On overflow the transaction is an atomic
    // no-op. Direct callers that do not own a coherent active frame must use
    // add_clause and remove_clause instead.
    MoveResult move_clause_from_frame(Integer clause_idx,
                                      Integer source_lit,
                                      Integer source_slot,
                                      const Clause &clause,
                                      const std::vector<Integer> &candidates,
                                      Bit flag = Bit(true, PUBLIC))
    {
        Bit valid_clause = valid_clause_id_domain(clause_idx);
        Integer candidate1(VAR_SIZE_BIT, 0, PUBLIC);
        Integer candidate2(VAR_SIZE_BIT, 0, PUBLIC);
        if (!candidates.empty())
            candidate1 = candidates[0];
        if (candidates.size() > 1)
            candidate2 = candidates[1];

        if (selected_backend == WatchBackendKind::Indexed)
        {
            Integer safe_clause_idx = If(
                valid_clause, clause_idx,
                Integer(clause_idx.size(), 0, PUBLIC));
            Integer pair_value = read_pair(safe_clause_idx);
            Integer pair_pos1 = unpack_pair_position(pair_value, 0);
            Integer pair_pos2 = unpack_pair_position(pair_value, 1);
            Integer pair_lit1 = literal_at_position(clause, pair_pos1);
            Integer pair_lit2 = literal_at_position(clause, pair_pos2);

            Bit source_is_first = (pair_pos1 != Integer(pair_pos1.size(), 0, PUBLIC)) &
                                  pair_lit1.equal(source_lit);
            Bit source_is_second = (pair_pos2 != Integer(pair_pos2.size(), 0, PUBLIC)) &
                                   pair_lit2.equal(source_lit);
            Bit exactly_one_source = source_is_first ^ source_is_second;
            Integer kept_lit = If(source_is_first, pair_lit2, pair_lit1);

            Integer cand_pos1 = first_position_of_literal(clause, candidate1);
            Integer cand_pos2 = first_position_of_literal(clause, candidate2);
            Bit cand1_valid = valid_literal_domain(candidate1) &
                              !candidate1.equal(source_lit) &
                              !candidate1.equal(kept_lit) &
                              (cand_pos1 != Integer(cand_pos1.size(), 0, PUBLIC));
            Bit cand2_valid = valid_literal_domain(candidate2) &
                              !candidate2.equal(source_lit) &
                              !candidate2.equal(kept_lit) &
                              (cand_pos2 != Integer(cand_pos2.size(), 0, PUBLIC));
            Bit choose_first = cand1_valid;
            Bit choose_second = !choose_first & cand2_valid;
            Integer replacement_lit = If(choose_first, candidate1,
                                         If(choose_second, candidate2,
                                            Integer(VAR_SIZE_BIT, 0, PUBLIC)));
            Integer replacement_pos = If(choose_first, cand_pos1,
                                         If(choose_second, cand_pos2,
                                            Integer(WATCH_POSITION_SIZE_BIT, 0, PUBLIC)));
            Bit replacement_valid = choose_first | choose_second;
            Bit move_active = flag & valid_clause & valid_data_slot(source_slot) &
                              exactly_one_source & replacement_valid;

            Integer safe_destination = secret_or_zero(replacement_lit, move_active);
            Integer destination_occ = read_occ(safe_destination);
            EmptySlot destination = first_empty_from_occ(destination_occ);
            Bit overflow = move_active & !destination.available;
            Bit commit = move_active & destination.available;

            Integer zero_clause(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
            Integer stored_clause = clause_idx;
            stored_clause.resize(CLAUSE_IDX_SIZE_IN_WL);
            Integer safe_source = secret_or_zero(source_lit, commit);
            write_data_slot(safe_source, source_slot, zero_clause, commit);
            write_data_slot(safe_destination, destination.slot, stored_clause, commit);
            write_occ_slot(safe_source, source_slot, Bit(false, PUBLIC), commit);
            write_occ_slot(safe_destination, destination.slot, Bit(true, PUBLIC), commit);

            Integer new_pos1 = If(commit & source_is_first,
                                  replacement_pos, pair_pos1);
            Integer new_pos2 = If(commit & source_is_second,
                                  replacement_pos, pair_pos2);
            write_pair(clause_idx, pack_pair(new_pos1, new_pos2), commit);
            return {commit, overflow};
        }

        // Block-local fallback recovers which candidate is the retained
        // watcher using two fixed range reads, then commits one source and one
        // destination block write.
        Bit frame_active = flag & valid_clause & valid_data_slot(source_slot);
        Bit candidate1_nonzero = frame_active & valid_literal_domain(candidate1) &
                                 !candidate1.equal(source_lit);
        Bit candidate2_nonzero = frame_active & valid_literal_domain(candidate2) &
                                 !candidate2.equal(source_lit);
        Integer safe_candidate1 = secret_or_zero(candidate1, candidate1_nonzero);
        Integer safe_candidate2 = secret_or_zero(candidate2, candidate2_nonzero);
        std::vector<Integer> list1 = get_watchlist_clauses(safe_candidate1);
        std::vector<Integer> list2 = get_watchlist_clauses(safe_candidate2);
        Integer stored_clause = clause_idx;
        stored_clause.resize(CLAUSE_IDX_SIZE_IN_WL);
        Bit found1 = list_contains(list1, stored_clause);
        Bit found2 = list_contains(list2, stored_clause);
        EmptySlot empty1 = first_empty_from_list(list1);
        EmptySlot empty2 = first_empty_from_list(list2);

        Bit try1 = candidate1_nonzero & !found1;
        // A new but full first candidate consumes the attempt instead of
        // falling through to the second candidate.
        Bit try2 = !try1 & candidate2_nonzero & !found2;
        Bit attempted = try1 | try2;
        Integer destination_lit = If(try1, candidate1,
                                     If(try2, candidate2,
                                        Integer(VAR_SIZE_BIT, 0, PUBLIC)));
        Integer destination_slot = If(try1, empty1.slot, empty2.slot);
        Bit destination_free = (try1 & empty1.available) |
                               (try2 & empty2.available);
        Bit overflow = attempted & !destination_free;
        Bit commit = attempted & destination_free;

        Integer zero_clause(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        Integer safe_source = secret_or_zero(source_lit, commit);
        Integer safe_destination = secret_or_zero(destination_lit, commit);
        write_data_slot(safe_source, source_slot, zero_clause, commit);
        write_data_slot(safe_destination, destination_slot, stored_clause, commit);
        return {commit, overflow};
    }

    // Get all clauses in a watchlist using range_read
    std::vector<Integer> get_watchlist_clauses(Integer lit_idx)
    {
        logical_range_reads += 1;
        lit_idx.resize(WL_ORAM_SIZE_BIT);

        Integer base_idx = compute_watchlist_index(lit_idx, Integer(WL_ORAM_SIZE_BIT, 0, PUBLIC));

        int bits_per_block = 1 << WL_ORAM_UNIT_SIZE;

        std::vector<Integer> blocks(WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL, Integer(bits_per_block, 0, PUBLIC));

        auto t1 = clock_start();
        wl_oram->range_read(base_idx, blocks, WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL);
        WL_ORAM_TIME += time_from(t1);

        for (int j = 0; j < WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL; j++)
        {
            if (IF_DEBUG)
            {
                std::cout << "Reading watchlist for literal " << reveal_int_32(lit_idx)
                          << " using range_read, block " << (j + 1)
                          << "/" << WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL << " block: "
                          << reveal_bool_string(blocks[j], CLAUSE_IDX_SIZE_IN_WL) << std::endl;
            }
        }

        // Entries are block-local: no clause ID straddles a FloRAM word.
        std::vector<Integer> clauses;
        for (int i = 0; i < MAX_CLAUSE_IN_WL; i++)
        {
            Integer clause_idx(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
            int block_idx = i / WL_ENTRIES_PER_BLOCK;
            int bit_start = (i % WL_ENTRIES_PER_BLOCK) * CLAUSE_IDX_SIZE_IN_WL;

            for (int b = 0; b < CLAUSE_IDX_SIZE_IN_WL; b++)
            {
                if (bit_start + b < bits_per_block)
                {
                    clause_idx.bits[b] = blocks[block_idx].bits[bit_start + b];
                }
            }

            clauses.push_back(clause_idx);
        }

        return clauses;
    }

    Bit contains_clause(Integer lit_idx, Integer clause_idx)
    {
        std::vector<Integer> clauses = get_watchlist_clauses(lit_idx);
        clause_idx.resize(CLAUSE_IDX_SIZE_IN_WL);
        Bit found = Bit(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            found = found | clauses[i].equal(clause_idx);
        }
        return found;
    }

    Integer watchlist_size(Integer lit_idx)
    {
        std::vector<Integer> clauses = get_watchlist_clauses(lit_idx);
        Integer count(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        Integer zero(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            count = count + If(clauses[i] != zero,
                               Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC),
                               Integer(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC));
        }
        return count;
    }

    // Write clause indices to a watchlist
    void write_watchlist_clauses(Integer lit_idx, const std::vector<Integer> &clauses)
    {
        logical_watchlist_writes += 1;
        lit_idx.resize(WL_ORAM_SIZE_BIT);

        int bits_per_block = 1 << WL_ORAM_UNIT_SIZE;
        for (int j = 0; j < WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL; j++)
        {
            Integer block(bits_per_block, 0, PUBLIC);
            for (int entry = 0; entry < WL_ENTRIES_PER_BLOCK; ++entry)
            {
                int logical_slot = j * WL_ENTRIES_PER_BLOCK + entry;
                if (logical_slot >= MAX_CLAUSE_IN_WL ||
                    logical_slot >= static_cast<int>(clauses.size()))
                {
                    continue;
                }
                int bit_start = entry * CLAUSE_IDX_SIZE_IN_WL;
                for (int b = 0; b < CLAUSE_IDX_SIZE_IN_WL; ++b)
                {
                    block.bits[bit_start + b] = clauses[logical_slot].bits[b];
                }
            }

            Integer index = compute_watchlist_index(lit_idx, Integer(WL_ORAM_SIZE_BIT, j, PUBLIC));

            oram_write(index, block);
        }
    }

    Bit nonzero_literal(const Integer &literal) const
    {
        return literal != Integer(literal.size(), 0, PUBLIC);
    }

    Bit valid_literal_domain(const Integer &literal) const
    {
        Integer normalized = literal;
        normalized.resize(std::max<int>(literal.size(), VAR_SIZE_BIT), true);
        Integer zero(normalized.size(), 0, PUBLIC);
        Integer lower(normalized.size(), -VAR_NUM, PUBLIC);
        Integer upper(normalized.size(), VAR_NUM, PUBLIC);
        return (normalized != zero) & normalized.geq(lower) & upper.geq(normalized);
    }

    Bit valid_clause_id_domain(const Integer &clause_idx) const
    {
        const int wide_bits = std::max<int>(
            static_cast<int>(clause_idx.size()) + 1,
            CLAUSE_IDX_SIZE_IN_WL + 1);
        Integer wide = clause_idx;
        wide.resize(wide_bits, true);
        Integer one(wide_bits, 1, PUBLIC);
        Integer original_max(wide_bits, CLAUSE_NUM, PUBLIC);
        Integer learned_min(wide_bits, -MAX_NUM_OF_CONFLICT_CLAUSE, PUBLIC);
        Integer minus_one(wide_bits, -1, PUBLIC);
        Bit valid_original = wide.geq(one) & original_max.geq(wide);
        Bit valid_learned = wide.geq(learned_min) & minus_one.geq(wide);
        return valid_original | valid_learned;
    }

    Bit valid_data_slot(const Integer &slot) const
    {
        Bit valid(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            valid = valid | slot.equal(Integer(slot.size(), i, PUBLIC));
        }
        return valid;
    }

    Integer secret_or_zero(const Integer &value, const Bit &use_value) const
    {
        return If(use_value, value, Integer(value.size(), 0, PUBLIC));
    }

    Bit valid_watch_position(const Integer &position) const
    {
        Integer normalized = position;
        normalized.resize(WATCH_POSITION_SIZE_BIT, false);
        Bit valid(false, PUBLIC);
        for (int i = 1; i <= MAX_LIT_IN_CLAUSE; ++i)
        {
            valid = valid |
                    normalized.equal(Integer(WATCH_POSITION_SIZE_BIT, i, PUBLIC));
        }
        return valid;
    }

    Integer normalize_watch_position(const Integer &position,
                                     const Bit &valid) const
    {
        Integer normalized = position;
        normalized.resize(WATCH_POSITION_SIZE_BIT, false);
        return If(valid, normalized,
                  Integer(WATCH_POSITION_SIZE_BIT, 0, PUBLIC));
    }

    Integer unified_clause_key(const Integer &clause_idx) const
    {
        const int wide_bits = std::max<int>(
            static_cast<int>(clause_idx.size()) + 1,
            WATCH_PAIR_ORAM_SIZE_BIT + 1);
        Integer wide = clause_idx;
        wide.resize(wide_bits, true);
        Bit nonnegative = wide.geq(Integer(wide_bits, 0, PUBLIC));
        Integer magnitude = wide.abs();
        magnitude.resize(WATCH_PAIR_ORAM_SIZE_BIT, false);
        Integer learned_key = magnitude +
                              Integer(WATCH_PAIR_ORAM_SIZE_BIT,
                                      CLAUSE_NUM, PUBLIC);
        return If(nonnegative, magnitude, learned_key);
    }

    Integer unified_literal_key(const Integer &literal,
                                int output_bits) const
    {
        const int wide_bits = std::max<int>(
            static_cast<int>(literal.size()) + 1, output_bits + 1);
        Integer wide = literal;
        wide.resize(wide_bits, true);
        Bit nonnegative = wide.geq(Integer(wide_bits, 0, PUBLIC));
        Integer magnitude = wide.abs();
        magnitude.resize(output_bits, false);
        Integer negative_key = magnitude + Integer(output_bits, VAR_NUM, PUBLIC);
        return If(nonnegative, magnitude, negative_key);
    }

    Bit list_contains(const std::vector<Integer> &entries,
                      const Integer &clause_idx) const
    {
        Bit found(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            found = found | entries[i].equal(clause_idx);
        }
        return found;
    }

    EmptySlot first_empty_from_list(const std::vector<Integer> &entries) const
    {
        const int slot_bits = std::max(bits_required(MAX_CLAUSE_IN_WL - 1), 1);
        Integer slot(slot_bits, 0, PUBLIC);
        Integer zero(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
        Bit found(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            Bit take = !found & entries[i].equal(zero);
            slot = If(take, Integer(slot_bits, i, PUBLIC), slot);
            found = found | take;
        }
        return {slot, found};
    }

    EmptySlot first_empty_from_occ(const Integer &occupancy) const
    {
        const int slot_bits = std::max(bits_required(MAX_CLAUSE_IN_WL - 1), 1);
        Integer slot(slot_bits, 0, PUBLIC);
        Bit found(false, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            Bit take = !found & !occupancy.bits[i];
            slot = If(take, Integer(slot_bits, i, PUBLIC), slot);
            found = found | take;
        }
        return {slot, found};
    }

    Integer pack_pair(const Integer &position1,
                      const Integer &position2) const
    {
        Integer result(1 << WATCH_PAIR_ORAM_UNIT_SIZE, 0, PUBLIC);
        Integer first = position1;
        Integer second = position2;
        first.resize(WATCH_POSITION_SIZE_BIT, false);
        second.resize(WATCH_POSITION_SIZE_BIT, false);
        for (int b = 0; b < WATCH_POSITION_SIZE_BIT; ++b)
        {
            result.bits[b] = first.bits[b];
            result.bits[WATCH_POSITION_SIZE_BIT + b] = second.bits[b];
        }
        return result;
    }

    Integer unpack_pair_position(const Integer &pair_value, int endpoint) const
    {
        Integer position(WATCH_POSITION_SIZE_BIT, 0, PUBLIC);
        const int start = endpoint * WATCH_POSITION_SIZE_BIT;
        for (int b = 0; b < WATCH_POSITION_SIZE_BIT; ++b)
        {
            position.bits[b] = pair_value.bits[start + b];
        }
        return position;
    }

    Integer literal_at_position(const Clause &clause,
                                const Integer &position) const
    {
        Integer literal(VAR_SIZE_BIT, 0, PUBLIC);
        const int limit = std::min<int>(MAX_LIT_IN_CLAUSE,
                                        static_cast<int>(clause.literals.size()));
        for (int i = 0; i < limit; ++i)
        {
            Bit selected = position.equal(
                Integer(position.size(), i + 1, PUBLIC));
            literal = If(selected, clause.literals[i], literal);
        }
        return literal;
    }

    Integer first_position_of_literal(const Clause &clause,
                                      const Integer &literal) const
    {
        Integer position(WATCH_POSITION_SIZE_BIT, 0, PUBLIC);
        Bit found(false, PUBLIC);
        const int limit = std::min<int>(MAX_LIT_IN_CLAUSE,
                                        static_cast<int>(clause.literals.size()));
        for (int i = 0; i < limit; ++i)
        {
            Bit take = !found & nonzero_literal(literal) &
                       clause.literals[i].equal(literal);
            position = If(take,
                          Integer(WATCH_POSITION_SIZE_BIT, i + 1, PUBLIC),
                          position);
            found = found | take;
        }
        return position;
    }

    Integer read_pair(const Integer &clause_idx)
    {
        logical_pair_reads += 1;
        Integer key = unified_clause_key(clause_idx);
        Integer result(1 << WATCH_PAIR_ORAM_UNIT_SIZE, 0, PUBLIC);
        auto t1 = clock_start();
        pair_oram->read(key, result);
        WL_ORAM_TIME += time_from(t1);
        return result;
    }

    void write_pair(const Integer &clause_idx,
                    const Integer &pair_value,
                    const Bit &flag)
    {
        logical_pair_writes += 1;
        Integer key = unified_clause_key(clause_idx);
        key = If(flag, key,
                 Integer(WATCH_PAIR_ORAM_SIZE_BIT, 0, PUBLIC));
        auto t1 = clock_start();
        pair_oram->write(key, [&](const Integer &in, Integer &out)
                         { out = If(flag, pair_value, in); });
        WL_ORAM_TIME += time_from(t1);
    }

    Integer read_occ(const Integer &literal)
    {
        logical_occ_reads += 1;
        Integer key = unified_literal_key(literal, WATCH_OCC_ORAM_SIZE_BIT);
        Integer result(1 << WATCH_OCC_ORAM_UNIT_SIZE, 0, PUBLIC);
        auto t1 = clock_start();
        occ_oram->read(key, result);
        WL_ORAM_TIME += time_from(t1);
        return result;
    }

    void write_occ_slot(const Integer &literal,
                        const Integer &slot,
                        const Bit &value,
                        const Bit &flag)
    {
        logical_occ_writes += 1;
        Integer safe_literal = secret_or_zero(literal, flag);
        Integer safe_slot = secret_or_zero(slot, flag);
        Integer key = unified_literal_key(safe_literal,
                                          WATCH_OCC_ORAM_SIZE_BIT);
        auto t1 = clock_start();
        occ_oram->write(key, [&](const Integer &in, Integer &out)
                        {
                            out = in;
                            for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
                            {
                                Bit selected = flag & safe_slot.equal(
                                    Integer(safe_slot.size(), i, PUBLIC));
                                out.bits[i] = If(selected, value, in.bits[i]);
                            }
                        });
        WL_ORAM_TIME += time_from(t1);
    }

    void write_data_slot(const Integer &literal,
                         const Integer &slot,
                         const Integer &clause_idx,
                         const Bit &flag)
    {
        logical_watchlist_writes += 1;
        logical_point_writes += 1;
        Integer safe_literal = secret_or_zero(literal, flag);
        safe_literal.resize(WL_ORAM_SIZE_BIT, true);
        Integer safe_slot = secret_or_zero(slot, flag);
        Integer block_offset(WL_ORAM_SIZE_BIT, 0, PUBLIC);
        const int entry_bits = std::max(bits_required(WL_ENTRIES_PER_BLOCK - 1), 1);
        Integer entry_offset(entry_bits, 0, PUBLIC);
        for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
        {
            Bit selected = safe_slot.equal(
                Integer(safe_slot.size(), i, PUBLIC));
            block_offset = If(selected,
                              Integer(WL_ORAM_SIZE_BIT,
                                      i / WL_ENTRIES_PER_BLOCK, PUBLIC),
                              block_offset);
            entry_offset = If(selected,
                              Integer(entry_bits,
                                      i % WL_ENTRIES_PER_BLOCK, PUBLIC),
                              entry_offset);
        }

        Integer index = compute_watchlist_index(safe_literal, block_offset);
        Integer stored_clause = clause_idx;
        stored_clause.resize(CLAUSE_IDX_SIZE_IN_WL);
        auto t1 = clock_start();
        wl_oram->write(index, [&](const Integer &in, Integer &out)
                       {
                           out = in;
                           for (int entry = 0; entry < WL_ENTRIES_PER_BLOCK; ++entry)
                           {
                               Bit selected_entry = flag & entry_offset.equal(
                                   Integer(entry_bits, entry, PUBLIC));
                               int bit_start = entry * CLAUSE_IDX_SIZE_IN_WL;
                               for (int b = 0; b < CLAUSE_IDX_SIZE_IN_WL; ++b)
                               {
                                   out.bits[bit_start + b] =
                                       If(selected_entry, stored_clause.bits[b],
                                          in.bits[bit_start + b]);
                               }
                           }
                       });
        WL_ORAM_TIME += time_from(t1);
    }

    // Print the watchlist contents in human-readable format
    void print_human()
    {
        std::cout << "\n==================== Watchlist ====================\n";
        for (int i = 0; i < VAR_NUM + 1; ++i)
        {
            Integer posLit = Integer(WL_ORAM_SIZE_BIT, i, PUBLIC);
            std::cout << "var " << i << " : ";
            std::vector<Integer> pos_clauses = get_watchlist_clauses(posLit);
            for (auto &clause_idx : pos_clauses)
            {
                int idx = reveal_32(clause_idx);
                if (idx != 0)
                    std::cout << idx << " ";
            }
            std::cout << "\n";

            Integer negLit = Integer(WL_ORAM_SIZE_BIT, -i, PUBLIC);
            std::cout << "var " << -i << " : ";
            std::vector<Integer> neg_clauses = get_watchlist_clauses(negLit);
            for (auto &clause_idx : neg_clauses)
            {
                int idx = reveal_32(clause_idx);
                if (idx != 0)
                    std::cout << idx << " ";
            }
            std::cout << "\n";
        }
        std::cout << "\n";
    }

    // Print clauses in a specific literal's watchlist
    void print_clause_in_wl(Integer var_idx)
    {
        std::vector<Integer> clauses = get_watchlist_clauses(var_idx);
        std::cout << reveal_int_32(var_idx) << ": ";
        for (auto &clause_idx : clauses)
        {
            int idx = reveal_32(clause_idx);
            if (idx != 0)
                std::cout << idx << " ";
        }
        std::cout << "\n";
    }

    void oram_write(Integer &idx, const Integer &val)
    {
        auto t1 = clock_start();
        if (IF_PLAIN_ORAM)
        {
            Integer tmp = idx;
            tmp.resize(32);
            int plain_idx = tmp.reveal<int32_t>();
            (void)plain_idx; // suppress unused variable warning
        }

        wl_oram->write(idx, [&](const Integer &in, Integer &out)
                       { out = val; });

        if (IF_DEBUG)
        {
            std::cout << "WL ORAM writing #" << idx.reveal<int>() << " block: "
                      << reveal_bool_string(val, CLAUSE_IDX_SIZE_BIT) << std::endl;
        }
        WL_ORAM_TIME += time_from(t1);
    }

    void oram_write(Integer &idx, const Integer &val, Bit flag)
    {
        auto t1 = clock_start();

        wl_oram->write(idx, [&](const Integer &in, Integer &out)
                       { out = If(flag, val, in); });

        if (IF_DEBUG)
        {
            std::cout << "WL ORAM writing with flag #" << idx.reveal<int>() << " block: "
                      << reveal_bool_string(val, CLAUSE_IDX_SIZE_BIT) << std::endl;
        }
        WL_ORAM_TIME += time_from(t1);
    }

    void oram_write(int idx, const Integer &val)
    {
        auto t1 = clock_start();
        assert(idx <= (1 << WL_ORAM_SIZE_BIT) - 1);

        wl_oram->write(idx, [&](const Integer &in, Integer &out)
                       { out = val; });
        WL_ORAM_TIME += time_from(t1);
    }

    void oram_read(Integer &idx, Integer &result)
    {
        auto t1 = clock_start();
        if (result.size() != (1 << WL_ORAM_UNIT_SIZE))
        {
            result.resize(1 << WL_ORAM_UNIT_SIZE);
        }

        if (IF_PLAIN_ORAM)
        {
            Integer tmp = idx;
            tmp.resize(32);
            int plain_idx = tmp.reveal<int32_t>();
            wl_oram->read(plain_idx, result);
        }
        else
        {
            wl_oram->read(idx, result);
        }

        if (IF_DEBUG)
        {
            std::cout << "WL ORAM reading #" << idx.reveal<int>() << " block: "
                      << reveal_bool_string(result, CLAUSE_IDX_SIZE_BIT) << std::endl;
        }
        WL_ORAM_TIME += time_from(t1);
    }

    void oram_read(int idx, Integer &result)
    {
        auto t1 = clock_start();
        assert(idx <= (1 << WL_ORAM_SIZE_BIT) - 1);

        wl_oram->read(idx, result);
        WL_ORAM_TIME += time_from(t1);
    }
};

#endif // PPCDCL_WATCHLIST_H
