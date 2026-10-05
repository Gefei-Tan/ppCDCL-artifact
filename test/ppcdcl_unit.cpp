#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "emp-tool/emp-tool.h"
#include "src/ppCDCL.h"

using namespace emp;
using Bit = Bit_T<GbWire>;
using Integer = Integer_T<GbWire>;

FloramMPC<NetIO> *base = nullptr;
constexpr int BASE_PORT = 12380;
bool g_use_oram_variable_state = true;

void init_backend(int party, int threads = 2, const char *addr = "127.0.0.1")
{
    NetIO **io = new NetIO *[threads];
    for (int i = 0; i < std::max(2, threads); i++)
    {
        io[i] = new NetIO((party - 1) ? addr : nullptr, BASE_PORT + i, true);
    }
    block delta;
    PRG().random_block(&delta, 1);
    delta ^= makeBlock(0, (getSLSB(delta) << 1) | getLSB(delta));
    if (party == ALICE)
        delta ^= makeBlock(0, 1);
    else
        delta ^= makeBlock(0, 3);

    base = new FloramMPC<NetIO>(party, threads, io, delta);
    emp::backend = base;

    Integer lowmc_keys;
    base->random_sample(lowmc_keys, 94208);
    base->round_key = lowmc_keys.bits;
}

void require(bool cond, const std::string &msg)
{
    if (!cond)
    {
        throw std::runtime_error(msg);
    }
}

Bit legacy_read_seen(const std::vector<Bit> &seen, const Integer &idx)
{
    Bit out(false, PUBLIC);
    for (int i = 0; i < static_cast<int>(seen.size()); ++i)
    {
        Bit match = idx == Integer(idx.size(), i, PUBLIC);
        out = If(match, seen[i], out);
    }
    return out;
}

void legacy_add_unique_integer(std::vector<Integer> &values,
                               const Integer &value,
                               const Bit &flag)
{
    Bit duplicate(false, PUBLIC);
    for (const auto &entry : values)
    {
        duplicate = duplicate | entry.equal(value);
    }

    Bit inserted(false, PUBLIC);
    for (auto &entry : values)
    {
        Bit empty = entry == Integer(value.size(), 0, PUBLIC);
        Bit place_here = flag & !duplicate & empty & !inserted;
        entry = If(place_here, value, entry);
        inserted = inserted | place_here;
    }
}

void legacy_write_seen(std::vector<Bit> &seen,
                       const Integer &idx,
                       const Bit &value,
                       const Bit &flag)
{
    for (int i = 0; i < static_cast<int>(seen.size()); ++i)
    {
        Bit match = idx == Integer(idx.size(), i, PUBLIC);
        seen[i] = If(flag & match, value, seen[i]);
    }
}

void legacy_add_candidate(std::vector<Integer> &candidate_vars,
                          std::vector<Integer> &candidate_orders,
                          const Integer &var,
                          const Integer &order,
                          const Bit &flag)
{
    Bit inserted(false, PUBLIC);
    for (int i = 0; i < static_cast<int>(candidate_vars.size()); ++i)
    {
        Bit empty = candidate_vars[i] == Integer(VAR_SIZE_BIT, 0, PUBLIC);
        Bit place_here = flag & empty & !inserted;
        candidate_vars[i] = If(place_here, var, candidate_vars[i]);
        candidate_orders[i] = If(place_here, order, candidate_orders[i]);
        inserted = inserted | place_here;
    }
}

struct LegacyCandidateChoice
{
    Bit found;
    Integer slot;
    Integer var;
    Integer order;
};

LegacyCandidateChoice legacy_select_candidate(
    const std::vector<Integer> &candidate_vars,
    const std::vector<Integer> &candidate_orders,
    const Bit &flag)
{
    LegacyCandidateChoice choice{Bit(false, PUBLIC),
                                 Integer(VAR_SIZE_BIT, 0, PUBLIC),
                                 Integer(VAR_SIZE_BIT, 0, PUBLIC),
                                 Integer(candidate_orders[0].size(), 0, PUBLIC)};
    for (int i = 0; i < static_cast<int>(candidate_vars.size()); ++i)
    {
        Bit valid = flag &
                    (candidate_vars[i] != Integer(VAR_SIZE_BIT, 0, PUBLIC));
        Bit better = valid & (!choice.found |
                              (candidate_orders[i] > choice.order));
        choice.slot = If(better, Integer(VAR_SIZE_BIT, i, PUBLIC), choice.slot);
        choice.var = If(better, candidate_vars[i], choice.var);
        choice.order = If(better, candidate_orders[i], choice.order);
        choice.found = choice.found | valid;
    }
    return choice;
}

void legacy_remove_candidate(std::vector<Integer> &candidate_vars,
                             std::vector<Integer> &candidate_orders,
                             const Integer &slot,
                             const Bit &flag)
{
    for (int i = 0; i < static_cast<int>(candidate_vars.size()); ++i)
    {
        Bit remove_here = flag & (slot == Integer(VAR_SIZE_BIT, i, PUBLIC));
        candidate_vars[i] = If(
            remove_here, Integer(VAR_SIZE_BIT, 0, PUBLIC), candidate_vars[i]);
        candidate_orders[i] = If(
            remove_here,
            Integer(candidate_orders[i].size(), 0, PUBLIC),
            candidate_orders[i]);
    }
}

std::unique_ptr<CDCL> make_solver_from(const std::string &file_name, int party,
                                       int max_lit_in_wl = 8,
                                       int max_lit_in_clause = 3,
                                       int max_lit_in_con_clause = 3,
                                       int max_con_clause = 8,
                                       WatchBackendRequest watch_backend_request =
                                           WatchBackendRequest::Auto)
{
    (void)party;
    return std::unique_ptr<CDCL>(new CDCL(file_name, base, PUBLIC, max_lit_in_wl, max_lit_in_clause, max_lit_in_con_clause, max_con_clause,
                                          false, 0, 0, g_use_oram_variable_state,
                                          false, watch_backend_request));
}

std::unique_ptr<CDCL> make_solver(int party)
{
    return make_solver_from("./test/cnfs/conflict_simple.cnf", party);
}

// Test-only differential oracle for the source watch-frame cache. Production
// never calls this helper: doing so would add an authoritative Data read to the
// secure transcript. The returned bit stays secret until a test explicitly
// reveals it on fixtures whose source literal, cursor, and watchlists are all
// public test data.
//
// At a stable giant-step boundary, slots [0, wl_scan_ctr] have not yet been
// consumed and must equal Data[-a_idx]. Between update_wl_from_frame() and the
// following advance_wl_scan_ctr(), the slot at wl_scan_ctr has already been
// consumed and is deliberately allowed to remain stale in cur_wl; callers mark
// that short-lived phase with current_slot_consumed=true.
Bit watch_frame_cache_coherent_for_test(
    CDCL &solver,
    const Bit &current_slot_consumed = Bit(false, PUBLIC))
{
    Bit frame_active = solver.wl_scan_ctr_active();
    Integer source_lit = -solver.a_idx;
    Integer safe_source = If(
        frame_active, source_lit, Integer(VAR_SIZE_BIT, 0, PUBLIC));
    std::vector<Integer> authoritative =
        solver.wl->get_watchlist_clauses(safe_source);

    Integer max_cursor(solver.wl_scan_ctr.size(),
                       MAX_CLAUSE_IN_WL - 1, PUBLIC);
    Bit coherent = !frame_active |
                   (max_cursor.geq(solver.wl_scan_ctr) &
                    solver.wl->valid_literal_domain(source_lit));
    for (int i = 0; i < MAX_CLAUSE_IN_WL; ++i)
    {
        Integer slot(solver.wl_scan_ctr.size(), i, PUBLIC);
        Bit at_or_before_cursor = solver.wl_scan_ctr.geq(slot);
        Bit is_current = solver.wl_scan_ctr.equal(slot);
        Bit unconsumed = frame_active & at_or_before_cursor &
                         !(current_slot_consumed & is_current);
        coherent = coherent &
                   (!unconsumed | solver.cur_wl[i].equal(authoritative[i]));
    }
    return coherent;
}

void require_watch_frame_cache_coherent(CDCL &solver,
                                        const std::string &context,
                                        bool current_slot_consumed = false)
{
    require(watch_frame_cache_coherent_for_test(
                solver, Bit(current_slot_consumed, PUBLIC))
                .reveal(),
            context);
}

void test_watch_frame_cache_coherence_for_backend(
    int party, WatchBackendRequest request, WatchBackendKind expected_kind)
{
    auto solver = make_solver_from("./test/cnfs/shortest_choice.cnf", party,
                                   8, 4, 4, 8, request);
    require(solver->wl->backend_kind() == expected_kind,
            "watch-frame oracle fixture selected the wrong backend");

    // Literal 1 initially watches clauses 1, 2, and 3. Install one fresh
    // learned clause at slot 3 with watcher pair (1, 2), so moving watcher 1
    // to literal 3 exercises both the block-local and Indexed transition.
    Integer learned_idx(CLAUSE_IDX_SIZE_IN_WL, -1, PUBLIC);
    Clause learned(std::vector<int>{1, 2, 3});
    solver->construct_wl_from_clause(learned_idx, learned,
                                     Bit(true, PUBLIC));
    require(!solver->if_failure.reveal(),
            "watch-frame oracle fixture overflowed during installation");

    // Drive the real source-selection path. A negative queued literal makes
    // -a_idx == 1, loads Data[1] into cur_wl, and starts at public slot W-1.
    solver->big_u[1] = Integer(2, 2, PUBLIC);
    solver->step_idx = 0;
    solver->pick_new_clause(Bit(true, PUBLIC));
    require(reveal_32(solver->a_idx) == -1 &&
                reveal_32(solver->wl_scan_ctr) == MAX_CLAUSE_IN_WL - 1,
            "source selection did not load the expected literal-1 frame");
    require_watch_frame_cache_coherent(
        *solver, "freshly loaded watch frame disagrees with Data");

    // Complete the public empty-slot steps from W-1 down to slot 4. The
    // invariant is checked both at each stable boundary and after selecting
    // the next slot through the production pick_new_clause path.
    for (int next_slot = MAX_CLAUSE_IN_WL - 2;
         next_slot >= 3; --next_slot)
    {
        solver->advance_wl_scan_ctr(Bit(true, PUBLIC));
        require(solver->wl_scan_ctr
                    .equal(Integer(solver->wl_scan_ctr.size(),
                                   next_slot, PUBLIC))
                    .reveal(),
                "watch-frame fixture did not follow its descending cursor");
        require_watch_frame_cache_coherent(
            *solver, "cache diverged after an empty frame slot was consumed");
        if (next_slot > 3)
        {
            solver->pick_new_clause(Bit(true, PUBLIC), false, false);
            require(reveal_32(solver->w_idx) == 0,
                    "watch-frame fixture expected public padding before slot 3");
            require_watch_frame_cache_coherent(
                *solver, "cache diverged while selecting an empty frame slot");
        }
    }

    solver->pick_new_clause(Bit(true, PUBLIC), false, false);
    require(reveal_32(solver->w_idx) == -1,
            "watch-frame fixture did not select the learned clause at slot 3");

    std::vector<Integer> replacements{
        Integer(VAR_SIZE_BIT, 2, PUBLIC),
        Integer(VAR_SIZE_BIT, 3, PUBLIC)};
    solver->update_wl_from_frame(
        learned_idx, Integer(VAR_SIZE_BIT, 1, PUBLIC),
        solver->wl_scan_ctr, learned, replacements, Bit(true, PUBLIC));
    require(!solver->if_failure.reveal(),
            "watch-frame move unexpectedly overflowed");

    // After the move the source Data slot is cleared, while its cache
    // snapshot intentionally retains -1. Excluding the in-flight consumed
    // slot must pass; including it must fail. This checks the cache contract
    // without requiring write-through coherence.
    require_watch_frame_cache_coherent(
        *solver, "an unconsumed cache slot changed during a successful move",
        true);
    require(!watch_frame_cache_coherent_for_test(
                 *solver, Bit(false, PUBLIC))
                 .reveal(),
            "oracle did not detect the intentionally stale consumed slot");

    solver->advance_wl_scan_ctr(Bit(true, PUBLIC));
    require(reveal_32(solver->wl_scan_ctr) == 2,
            "watch-frame move did not advance to the next source slot");
    require_watch_frame_cache_coherent(
        *solver, "cache diverged after the consumed move slot was retired");

    // A write to a genuinely unconsumed authoritative slot must be detected.
    // Restoring the same public clause ID fills the same first hole and makes
    // the frame coherent again. Indexed mode also exercises its Occ update.
    Integer original_clause_two(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC);
    solver->wl->remove_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                              original_clause_two, Bit(true, PUBLIC));
    require(!watch_frame_cache_coherent_for_test(*solver).reveal(),
            "oracle missed an authoritative mutation in an unconsumed slot");
    require(!solver->wl->add_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                    original_clause_two, Bit(true, PUBLIC))
                 .reveal(),
            "could not restore the watch-frame oracle fixture");
    require_watch_frame_cache_coherent(
        *solver, "restored authoritative source did not match its cache");

    // Backtracking invalidates the whole frame. Cache contents may remain as
    // dead local state, so the invariant is intentionally vacuous afterward.
    solver->backtrack(Integer(DL_SIZE_BIT, 0, PUBLIC), Bit(true, PUBLIC));
    require(reveal_32(solver->wl_scan_ctr) == -1,
            "backtrack did not invalidate the watch frame");
    solver->cur_wl[0] = Integer(CLAUSE_IDX_SIZE_IN_WL, -7, PUBLIC);
    require_watch_frame_cache_coherent(
        *solver, "inactive watch frame was treated as live cache state");
}

void test_watch_frame_cache_coherence(int party)
{
    test_watch_frame_cache_coherence_for_backend(
        party, WatchBackendRequest::BlockLocal,
        WatchBackendKind::BlockLocal);
    test_watch_frame_cache_coherence_for_backend(
        party, WatchBackendRequest::Indexed,
        WatchBackendKind::Indexed);
}

void test_variable_state_basic(int party)
{
    auto solver = make_solver(party);
    Integer lit(VAR_SIZE_BIT, 1, PUBLIC);
    Integer ante(ANTE_SIZE_BIT, 2, PUBLIC);
    Integer dl(DL_SIZE_BIT, 1, PUBLIC);
    solver->big_v->set_unit_literal(lit, ante, dl, Bit(true, PUBLIC));

    Integer ass;
    Integer out_dl;
    Integer out_ante;
    solver->big_v->get_assignment(lit, ass);
    solver->big_v->get_decision_level(lit, out_dl);
    solver->big_v->get_antecedent(lit, out_ante);

    require(reveal_32(ass, true) == reveal_32(solver->big_v->true_assigned, true), "Variable assignment mismatch");
    require(reveal_32(out_dl) == reveal_32(dl), "Decision level mismatch");
    require(reveal_32(out_ante) == reveal_32(ante), "Antecedent mismatch");
}

void test_decision_level_width_and_live_order_counter(int party)
{
    // Solver dimensions are process-global in the current implementation.  Do
    // not keep solvers with different dimensions alive at the same time.
    {
        auto depth_solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                             2, 2, 20, 8);
        Integer var(VAR_SIZE_BIT, 20, PUBLIC);
        Integer max_level(DL_SIZE_BIT, 20, PUBLIC);
        depth_solver->big_v->set_decision(var, max_level, Bit(true, PUBLIC));
        Integer stored_level;
        depth_solver->big_v->get_decision_level(var, stored_level);
        require(reveal_32(stored_level) == 20,
                "decision-level storage lost the legal n-level boundary");
        require((stored_level > Integer(DL_SIZE_BIT, 19, PUBLIC)).reveal(),
                "decision-level comparison treated n as a negative EMP integer");
    }

    auto solver = make_solver(party);
    solver->big_v->cur_dl = Integer(DL_SIZE_BIT, 2, PUBLIC);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                                Integer(DL_SIZE_BIT, 2, PUBLIC));
    solver->backtrack(Integer(DL_SIZE_BIT, 1, PUBLIC), Bit(true, PUBLIC));
    solver->big_v->cur_dl = Integer(DL_SIZE_BIT, 2, PUBLIC);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                                Integer(DL_SIZE_BIT, 2, PUBLIC));
    Integer order;
    solver->big_v->get_order_counter(Integer(VAR_SIZE_BIT, 3, PUBLIC), order);
    require(reveal_32(order) == 1,
            "assignment order remained a lifetime counter after backtrack");
}

void test_linear_variable_state_update_reuses_secret_route(int party)
{
    if (g_use_oram_variable_state)
    {
        return;
    }
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);
    Integer secret_literal(VAR_SIZE_BIT, -20, ALICE);
    const std::uint64_t gates_before = base->num_and();
    solver->big_v->set_unit_literal(secret_literal,
                                    Integer(ANTE_SIZE_BIT, 3, PUBLIC),
                                    Integer(DL_SIZE_BIT, 7, PUBLIC),
                                    Bit(true, PUBLIC));
    const std::uint64_t gate_count = base->num_and() - gates_before;

    Integer assignment;
    Integer antecedent;
    Integer level;
    solver->big_v->get_assignment(20, assignment);
    solver->big_v->get_antecedent(20, antecedent);
    solver->big_v->get_decision_level(20, level);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->false_assigned, true),
            "Fused linear update stored the wrong assignment");
    require(reveal_32(antecedent) == 3 && reveal_32(level) == 7,
            "Fused linear update lost antecedent or level fields");
    require(gate_count <= 758,
            "Linear variable-state update routed each field independently: " +
                std::to_string(gate_count) + " AND gates");

    const int live_before = reveal_32(solver->big_v->live_assignment_count());
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 19, ALICE),
                                Integer(DL_SIZE_BIT, 8, PUBLIC),
                                Bit(false, PUBLIC));
    solver->big_v->get_assignment(19, assignment);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->not_assigned, true),
            "Disabled fused linear update changed an assignment");
    require(reveal_32(solver->big_v->live_assignment_count()) == live_before,
            "Disabled fused linear update changed the live trail length");
}

void test_implication_compare_and_set(int party)
{
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);

    Integer assignment;
    Integer antecedent;
    Integer level;
    Integer phase;
    Integer order;

    Bit did_assign = solver->big_v->try_set_unit_literal(
        Integer(VAR_SIZE_BIT, -20, ALICE),
        Integer(ANTE_SIZE_BIT, 3, PUBLIC),
        Integer(DL_SIZE_BIT, 7, PUBLIC),
        Bit(true, PUBLIC));
    require(did_assign.reveal(),
            "Fresh implication compare-and-set rejected an unassigned variable");

    solver->big_v->get_assignment(20, assignment);
    solver->big_v->get_antecedent(20, antecedent);
    solver->big_v->get_decision_level(20, level);
    solver->big_v->get_phase(20, phase);
    solver->big_v->get_order_counter(20, order);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->false_assigned, true),
            "Compare-and-set stored the wrong negative assignment");
    require(reveal_32(antecedent) == 3 && reveal_32(level) == 7,
            "Compare-and-set lost the antecedent or decision level");
    require(reveal_32(phase, true) == reveal_32(solver->big_v->false_assigned, true),
            "Compare-and-set lost the saved phase");
    require(reveal_32(order) == 0 &&
                reveal_32(solver->big_v->live_assignment_count()) == 1,
            "Compare-and-set stored the wrong trail order or live count");

    Bit duplicate = solver->big_v->try_set_unit_literal(
        Integer(VAR_SIZE_BIT, 20, ALICE),
        Integer(ANTE_SIZE_BIT, 5, PUBLIC),
        Integer(DL_SIZE_BIT, 9, PUBLIC),
        Bit(true, PUBLIC));
    require(!duplicate.reveal(),
            "Compare-and-set accepted an already-assigned variable");
    solver->big_v->get_assignment(20, assignment);
    solver->big_v->get_antecedent(20, antecedent);
    solver->big_v->get_decision_level(20, level);
    solver->big_v->get_phase(20, phase);
    solver->big_v->get_order_counter(20, order);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->false_assigned, true) &&
                reveal_32(antecedent) == 3 && reveal_32(level) == 7 &&
                reveal_32(phase, true) == reveal_32(solver->big_v->false_assigned, true) &&
                reveal_32(order) == 0 &&
                reveal_32(solver->big_v->live_assignment_count()) == 1,
            "Rejected compare-and-set changed an existing variable record");

    Bit duplicate_same = solver->big_v->try_set_unit_literal(
        Integer(VAR_SIZE_BIT, -20, ALICE),
        Integer(ANTE_SIZE_BIT, 6, PUBLIC),
        Integer(DL_SIZE_BIT, 10, PUBLIC),
        Bit(true, PUBLIC));
    solver->big_v->get_antecedent(20, antecedent);
    solver->big_v->get_decision_level(20, level);
    require(!duplicate_same.reveal() && reveal_32(antecedent) == 3 &&
                reveal_32(level) == 7 &&
                reveal_32(solver->big_v->live_assignment_count()) == 1,
            "Same-polarity duplicate compare-and-set changed state");

    Bit inactive = solver->big_v->try_set_unit_literal(
        Integer(VAR_SIZE_BIT, 19, ALICE),
        Integer(ANTE_SIZE_BIT, 4, PUBLIC),
        Integer(DL_SIZE_BIT, 8, PUBLIC),
        Bit(false, PUBLIC));
    require(!inactive.reveal(), "Inactive compare-and-set reported success");
    solver->big_v->get_assignment(19, assignment);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->not_assigned, true) &&
                reveal_32(solver->big_v->live_assignment_count()) == 1,
            "Inactive compare-and-set changed assignment state");

    Bit zero_literal = solver->big_v->try_set_unit_literal(
        Integer(VAR_SIZE_BIT, 0, ALICE),
        Integer(ANTE_SIZE_BIT, 4, PUBLIC),
        Integer(DL_SIZE_BIT, 8, PUBLIC),
        Bit(true, PUBLIC));
    require(!zero_literal.reveal(), "Zero-literal compare-and-set reported success");
    solver->big_v->get_assignment(0, assignment);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->not_assigned, true) &&
                reveal_32(solver->big_v->live_assignment_count()) == 1,
            "Zero-literal compare-and-set corrupted the sentinel or live count");

    const int live_before_padding =
        reveal_32(solver->big_v->live_assignment_count());
    Bit padded_literal = solver->big_v->try_set_unit_literal(
        Integer(VAR_SIZE_BIT, VAR_NUM + 1, ALICE),
        Integer(ANTE_SIZE_BIT, 4, PUBLIC),
        Integer(DL_SIZE_BIT, 8, PUBLIC),
        Bit(true, PUBLIC));
    require(!padded_literal.reveal(),
            "Out-of-domain padded literal compare-and-set reported success");
    solver->big_v->get_assignment(
        Integer(VAR_SIZE_BIT, VAR_NUM + 1, ALICE), assignment);
    require(reveal_32(assignment, true) ==
                reveal_32(solver->big_v->not_assigned, true) &&
                reveal_32(solver->big_v->live_assignment_count()) ==
                    live_before_padding,
            "Out-of-domain compare-and-set changed padding or live count");

    solver->add_implication(Integer(VAR_SIZE_BIT, 18, ALICE),
                            Integer(ANTE_SIZE_BIT, 6, PUBLIC),
                            Integer(DL_SIZE_BIT, 10, PUBLIC),
                            Bit(true, PUBLIC));
    solver->big_v->get_assignment(18, assignment);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->true_assigned, true),
            "CDCL implication did not commit the accepted assignment");
    require(reveal_32(solver->big_u[18], true) == 1,
            "CDCL implication did not enqueue the accepted positive literal");

    solver->add_implication(Integer(VAR_SIZE_BIT, -18, ALICE),
                            Integer(ANTE_SIZE_BIT, 7, PUBLIC),
                            Integer(DL_SIZE_BIT, 11, PUBLIC),
                            Bit(true, PUBLIC));
    solver->big_v->get_assignment(18, assignment);
    require(reveal_32(assignment, true) == reveal_32(solver->big_v->true_assigned, true) &&
                reveal_32(solver->big_u[18], true) == 1 &&
                reveal_32(solver->big_v->live_assignment_count()) == 2,
            "Rejected CDCL implication changed the record, queue, or live count");

    if (!g_use_oram_variable_state)
    {
        Integer legacy_literal(VAR_SIZE_BIT, -17, ALICE);
        const std::uint64_t legacy_before = base->num_and();
        Bit legacy_unassigned = solver->big_v->is_variable_unassigned(legacy_literal);
        solver->big_v->set_unit_literal(
            legacy_literal,
            Integer(ANTE_SIZE_BIT, 8, PUBLIC),
            Integer(DL_SIZE_BIT, 12, PUBLIC),
            legacy_unassigned & Bit(true, PUBLIC));
        const std::uint64_t legacy_gates = base->num_and() - legacy_before;

        const std::uint64_t fused_before = base->num_and();
        Bit fused_did_assign = solver->big_v->try_set_unit_literal(
            Integer(VAR_SIZE_BIT, -16, ALICE),
            Integer(ANTE_SIZE_BIT, 8, PUBLIC),
            Integer(DL_SIZE_BIT, 12, PUBLIC),
            Bit(true, PUBLIC));
        const std::uint64_t fused_gates = base->num_and() - fused_before;
        require(fused_did_assign.reveal(),
                "Fused implication gate fixture did not assign its fresh variable");
        require(fused_gates < legacy_gates,
                "Fused implication route did not beat read-then-write: legacy=" +
                    std::to_string(legacy_gates) + " fused=" +
                    std::to_string(fused_gates));
        if (party == ALICE)
        {
            std::cout << "IMPLICATION_CAS_GATES legacy=" << legacy_gates
                      << " fused=" << fused_gates
                      << " saved=" << (legacy_gates - fused_gates) << std::endl;
        }
    }

    // With n=3, the routing tree has two index bits while the signed literal
    // type can represent 5.  Truncating 5 would alias variable 1, so exercise
    // the full-domain guard rather than only a padding leaf.
    solver.reset();
    auto alias_solver = make_solver(party);
    Bit aliased_literal = alias_solver->big_v->try_set_unit_literal(
        Integer(VAR_SIZE_BIT, 5, ALICE),
        Integer(ANTE_SIZE_BIT, 1, PUBLIC),
        Integer(DL_SIZE_BIT, 1, PUBLIC),
        Bit(true, PUBLIC));
    alias_solver->big_v->get_assignment(1, assignment);
    require(!aliased_literal.reveal() &&
                reveal_32(assignment, true) ==
                    reveal_32(alias_solver->big_v->not_assigned, true) &&
                reveal_32(alias_solver->big_v->live_assignment_count()) == 0,
            "Out-of-domain literal aliased a real variable-state leaf");

    // Two's-complement abs(min) is still negative.  A range check of the form
    // abs(lit) != 0 && abs(lit) <= n therefore accepts the minimum signed
    // value and can mutate a physical ORAM slot.  Both backends must reject it
    // before changing variable state or coupling the secret acceptance bit to
    // Big-U.
    alias_solver.reset();
    auto min_solver = make_solver(party);
    const int min_literal_value = -(1 << (VAR_SIZE_BIT - 1));
    Integer min_literal(VAR_SIZE_BIT, min_literal_value, ALICE);
    min_solver->add_implication(
        min_literal,
        Integer(ANTE_SIZE_BIT, 1, PUBLIC),
        Integer(DL_SIZE_BIT, 1, PUBLIC),
        Bit(true, PUBLIC));
    bool queue_unchanged = true;
    for (const auto &entry : min_solver->big_u)
    {
        queue_unchanged = queue_unchanged && (reveal_32(entry, true) == 0);
    }
    require(reveal_32(min_solver->big_v->live_assignment_count()) == 0 &&
                queue_unchanged,
            "Minimum signed literal changed variable state or Big-U");
}

void test_watchlist_roundtrip(int party)
{
    auto solver = make_solver(party);
    Integer lit(VAR_SIZE_BIT, 1, PUBLIC);
    Integer clause_idx(CLAUSE_IDX_SIZE_IN_WL, 3, PUBLIC);
    solver->wl->add_clause(lit, clause_idx, Bit(true, PUBLIC));
    std::vector<Integer> wl_vec = solver->wl->get_watchlist_clauses(lit);

    bool found = false;
    for (const auto &entry : wl_vec)
    {
        found = found | (reveal_32(entry) == reveal_32(clause_idx));
    }
    require(found, "Watchlist add failed");

    solver->wl->remove_clause(lit, clause_idx, Bit(true, PUBLIC));
    wl_vec = solver->wl->get_watchlist_clauses(lit);
    bool removed = true;
    for (const auto &entry : wl_vec)
    {
        removed = removed & (reveal_32(entry) != reveal_32(clause_idx));
    }
    require(removed, "Watchlist remove failed");
}

void test_watchlist_overflow_is_fail_stop(int party)
{
    const bool trace = std::getenv("PPCDCL_UNIT_DEBUG") != nullptr;
    auto checkpoint = [&](const char *label) {
        if (trace && party == ALICE)
            std::cerr << "watch-overflow checkpoint: " << label << std::endl;
    };
    auto solver = make_solver_from("./test/cnfs/conflict_simple.cnf", party,
                                   8, 3, 3, 16);
    checkpoint("constructed");
    Integer target(VAR_SIZE_BIT, 1, PUBLIC);
    for (int clause_idx = -1; clause_idx >= -8; --clause_idx)
    {
        require(!solver->wl->add_clause(
                     target, Integer(CLAUSE_IDX_SIZE_IN_WL, clause_idx, PUBLIC),
                     Bit(true, PUBLIC))
                     .reveal(),
                "watchlist insertion unexpectedly overflowed before capacity");
    }
    checkpoint("filled target");
    Integer learned_one(CLAUSE_IDX_SIZE_IN_WL, -1, PUBLIC);
    Integer overflow_clause(CLAUSE_IDX_SIZE_IN_WL, -9, PUBLIC);
    require(!solver->wl->add_clause(target, learned_one, Bit(true, PUBLIC)).reveal(),
            "duplicate insertion into a full watchlist reported overflow");
    require(solver->wl->add_clause(target, overflow_clause, Bit(true, PUBLIC)).reveal(),
            "full watchlist did not report overflow");
    checkpoint("observed overflow");

    std::vector<Integer> entries = solver->wl->get_watchlist_clauses(target);
    for (int i = 0; i < 8; ++i)
        require(reveal_32(entries[i]) == -(i + 1),
                "failed insertion changed the full target watchlist");

    Integer source(VAR_SIZE_BIT, 2, PUBLIC);
    require(!solver->wl->add_clause(source, overflow_clause, Bit(true, PUBLIC)).reveal(),
            "source watch insertion failed before move-overflow probe");
    checkpoint("prepared source");
    solver->update_wl(overflow_clause, source,
                      std::vector<Integer>{target, Integer(VAR_SIZE_BIT, 0, PUBLIC)},
                      Bit(true, PUBLIC));
    checkpoint("updated watchlist");
    require(solver->if_failure.reveal(),
            "watch move into a full target did not set terminal FAILURE");
    require(solver->reveal_terminal_status() == CDCL::TERMINAL_FAILURE,
            "watchlist overflow was allowed to report SAT/UNSAT");
}

void test_indexed_watch_state_install_move_and_overflow(int party)
{
    (void)party;
    init_constant(8, 12, 8, 4, 4, 8);
    WatchList watchlists(base, WatchBackendRequest::Indexed);
    require(watchlists.backend_kind() == WatchBackendKind::Indexed,
            "Explicit Indexed watch backend was not selected");

    WatchList::AddClauseResult source_padding = watchlists.add_clause_with_status(
        Integer(VAR_SIZE_BIT, 1, PUBLIC),
        Integer(CLAUSE_IDX_SIZE_IN_WL, 12, PUBLIC), Bit(true, PUBLIC));
    require(source_padding.inserted.reveal() && !source_padding.overflow.reveal(),
            "Could not create the nonzero secret source-slot fixture");

    Integer clause_idx(CLAUSE_IDX_SIZE_IN_WL, 3, PUBLIC);
    WatchList::InstallResult installed = watchlists.install_clause_watchers(
        clause_idx,
        Integer(VAR_SIZE_BIT, 1, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 2, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    require(installed.installed.reveal() && !installed.overflow.reveal(),
            "Indexed watch installation failed on empty lists");

    auto count_clause = [&](int literal, int clause) {
        int count = 0;
        std::vector<Integer> entries = watchlists.get_watchlist_clauses(
            Integer(VAR_SIZE_BIT, literal, PUBLIC));
        for (const Integer &entry : entries)
            count += reveal_32(entry) == clause ? 1 : 0;
        return count;
    };
    require(count_clause(1, 3) == 1 && count_clause(2, 3) == 1,
            "Indexed install did not establish exactly two Data memberships");

    Clause clause(std::vector<int>{1, 2, 3, 4});
    Integer secret_source_slot(std::max(bits_required(MAX_CLAUSE_IN_WL), 1),
                               1, ALICE);
    const std::uint64_t move_data_writes_before = watchlists.logical_point_writes;
    const std::uint64_t move_pair_reads_before = watchlists.logical_pair_reads;
    const std::uint64_t move_pair_writes_before = watchlists.logical_pair_writes;
    const std::uint64_t move_occ_reads_before = watchlists.logical_occ_reads;
    const std::uint64_t move_occ_writes_before = watchlists.logical_occ_writes;
    const std::uint64_t move_gates_before = base->num_and();
    WatchList::MoveResult moved = watchlists.move_clause_from_frame(
        clause_idx, Integer(VAR_SIZE_BIT, 1, ALICE), secret_source_slot,
        clause,
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, ALICE),
                             Integer(VAR_SIZE_BIT, 3, ALICE)},
        Bit(true, ALICE));
    const std::uint64_t move_gates = base->num_and() - move_gates_before;
    require(moved.moved.reveal() && !moved.overflow.reveal(),
            "Indexed watcher move did not commit");
    require(watchlists.logical_point_writes - move_data_writes_before == 2 &&
                watchlists.logical_pair_reads - move_pair_reads_before == 1 &&
                watchlists.logical_pair_writes - move_pair_writes_before == 1 &&
                watchlists.logical_occ_reads - move_occ_reads_before == 1 &&
                watchlists.logical_occ_writes - move_occ_writes_before == 2,
            "Indexed watcher move did not use its fixed Pair/Occ/Data schedule");
    require(count_clause(1, 3) == 0 && count_clause(2, 3) == 1 &&
                count_clause(3, 3) == 1,
            "Indexed watcher move violated the two-watch membership invariant");

    // A full destination must not clear the source or mutate Pair.  Populate
    // literal 4 through add_clause_with_status so Occ and Data are both full.
    for (int id = 4; id < 12; ++id)
    {
        WatchList::AddClauseResult fill = watchlists.add_clause_with_status(
            Integer(VAR_SIZE_BIT, 4, PUBLIC),
            Integer(CLAUSE_IDX_SIZE_IN_WL, id, PUBLIC), Bit(true, PUBLIC));
        require(fill.inserted.reveal() && !fill.overflow.reveal(),
                "Indexed overflow fixture did not fill the destination");
    }
    const std::uint64_t overflow_data_writes_before = watchlists.logical_point_writes;
    const std::uint64_t overflow_pair_reads_before = watchlists.logical_pair_reads;
    const std::uint64_t overflow_pair_writes_before = watchlists.logical_pair_writes;
    const std::uint64_t overflow_occ_reads_before = watchlists.logical_occ_reads;
    const std::uint64_t overflow_occ_writes_before = watchlists.logical_occ_writes;
    const std::uint64_t overflow_gates_before = base->num_and();
    WatchList::MoveResult overflow = watchlists.move_clause_from_frame(
        clause_idx, Integer(VAR_SIZE_BIT, 3, ALICE),
        Integer(secret_source_slot.size(), 0, ALICE),
        clause,
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, ALICE),
                             Integer(VAR_SIZE_BIT, 4, ALICE)},
        Bit(true, ALICE));
    const std::uint64_t overflow_gates = base->num_and() - overflow_gates_before;
    require(!overflow.moved.reveal() && overflow.overflow.reveal(),
            "Indexed full destination did not report a masked overflow");
    require(watchlists.logical_point_writes - overflow_data_writes_before == 2 &&
                watchlists.logical_pair_reads - overflow_pair_reads_before == 1 &&
                watchlists.logical_pair_writes - overflow_pair_writes_before == 1 &&
                watchlists.logical_occ_reads - overflow_occ_reads_before == 1 &&
                watchlists.logical_occ_writes - overflow_occ_writes_before == 2,
            "Indexed overflow changed the public access transcript");
    require(count_clause(2, 3) == 1 && count_clause(3, 3) == 1 &&
                count_clause(4, 3) == 0,
            "Indexed overflow was not an atomic logical no-op");
    Integer source_occ_after_overflow = watchlists.read_occ(
        Integer(VAR_SIZE_BIT, 3, PUBLIC));
    require(source_occ_after_overflow.bits[0].reveal(),
            "Indexed overflow cleared the private source occupancy bit");

    const std::uint64_t inactive_data_writes_before = watchlists.logical_point_writes;
    const std::uint64_t inactive_pair_reads_before = watchlists.logical_pair_reads;
    const std::uint64_t inactive_pair_writes_before = watchlists.logical_pair_writes;
    const std::uint64_t inactive_occ_reads_before = watchlists.logical_occ_reads;
    const std::uint64_t inactive_occ_writes_before = watchlists.logical_occ_writes;
    const std::uint64_t inactive_gates_before = base->num_and();
    WatchList::MoveResult inactive = watchlists.move_clause_from_frame(
        clause_idx, Integer(VAR_SIZE_BIT, 3, ALICE),
        Integer(secret_source_slot.size(), 0, ALICE),
        clause,
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, ALICE),
                             Integer(VAR_SIZE_BIT, 4, ALICE)},
        Bit(false, ALICE));
    const std::uint64_t inactive_gates = base->num_and() - inactive_gates_before;
    require(!inactive.moved.reveal() && !inactive.overflow.reveal(),
            "Inactive Indexed move changed secret status");
    require(watchlists.logical_point_writes - inactive_data_writes_before == 2 &&
                watchlists.logical_pair_reads - inactive_pair_reads_before == 1 &&
                watchlists.logical_pair_writes - inactive_pair_writes_before == 1 &&
                watchlists.logical_occ_reads - inactive_occ_reads_before == 1 &&
                watchlists.logical_occ_writes - inactive_occ_writes_before == 2,
            "Inactive Indexed move changed the public access transcript");
    require(move_gates == overflow_gates && move_gates == inactive_gates,
            "Indexed move gate count depended on success, overflow, or activity");

    // A successful follow-up from the same coherent source frame proves that
    // the failed transaction preserved Pair as well as Data and Occ.
    WatchList::MoveResult after_rollback = watchlists.move_clause_from_frame(
        clause_idx, Integer(VAR_SIZE_BIT, 3, BOB),
        Integer(secret_source_slot.size(), 0, BOB), clause,
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, BOB),
                             Integer(VAR_SIZE_BIT, 1, BOB)},
        Bit(true, BOB));
    require(after_rollback.moved.reveal() &&
                !after_rollback.overflow.reveal() &&
                count_clause(1, 3) == 1 && count_clause(2, 3) == 1 &&
                count_clause(3, 3) == 0,
            "Indexed overflow did not preserve Pair/Occ/Data for a later move");
    Integer old_source_occ = watchlists.read_occ(
        Integer(VAR_SIZE_BIT, 3, PUBLIC));
    Integer restored_destination_occ = watchlists.read_occ(
        Integer(VAR_SIZE_BIT, 1, PUBLIC));
    require(!old_source_occ.bits[0].reveal() &&
                restored_destination_occ.bits[1].reveal(),
            "Successful post-rollback move did not update private occupancy");
}

void test_indexed_learned_key_and_install_rollback(int party)
{
    (void)party;
    init_constant(8, 12, 8, 4, 4, 8);
    WatchList watchlists(base, WatchBackendRequest::Indexed);

    Integer learned_idx(CLAUSE_IDX_SIZE_IN_WL, -1, PUBLIC);
    WatchList::InstallResult learned = watchlists.install_clause_watchers(
        learned_idx,
        Integer(VAR_SIZE_BIT, -1, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 2, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    require(learned.installed.reveal() && !learned.overflow.reveal(),
            "Negative learned-clause ID did not install through unified Pair key");
    Integer pair_before_empty_install = watchlists.read_pair(learned_idx);
    WatchList::InstallResult empty_reinstall = watchlists.install_clause_watchers(
        learned_idx,
        Integer(VAR_SIZE_BIT, 0, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 0, ALICE), Bit(false, ALICE),
        Integer(VAR_SIZE_BIT, 0, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 0, ALICE), Bit(false, ALICE),
        Bit(true, ALICE));
    Integer pair_after_empty_install = watchlists.read_pair(learned_idx);
    require(!empty_reinstall.installed.reveal() &&
                !empty_reinstall.overflow.reveal() &&
                reveal_32(watchlists.unpack_pair_position(pair_before_empty_install, 0)) ==
                    reveal_32(watchlists.unpack_pair_position(pair_after_empty_install, 0)) &&
                reveal_32(watchlists.unpack_pair_position(pair_before_empty_install, 1)) ==
                    reveal_32(watchlists.unpack_pair_position(pair_after_empty_install, 1)),
            "Zero-endpoint Indexed install cleared existing Pair metadata");
    Clause learned_clause(std::vector<int>{-1, 2, 3});
    WatchList::MoveResult learned_move = watchlists.move_clause_from_frame(
        learned_idx, Integer(VAR_SIZE_BIT, -1, BOB),
        Integer(std::max(bits_required(MAX_CLAUSE_IN_WL), 1), 0, BOB),
        learned_clause,
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, BOB),
                             Integer(VAR_SIZE_BIT, 3, BOB)},
        Bit(true, BOB));
    require(learned_move.moved.reveal() && !learned_move.overflow.reveal() &&
                !watchlists.contains_clause(Integer(VAR_SIZE_BIT, -1, PUBLIC), learned_idx).reveal() &&
                watchlists.contains_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC), learned_idx).reveal() &&
                watchlists.contains_clause(Integer(VAR_SIZE_BIT, 3, PUBLIC), learned_idx).reveal(),
            "Negative learned-clause Pair key did not survive a watcher move");

    // Fill one destination, then request a two-list install.  The empty list
    // must remain untouched when the other endpoint overflows.
    for (int id = 1; id <= 8; ++id)
    {
        WatchList::AddClauseResult fill = watchlists.add_clause_with_status(
            Integer(VAR_SIZE_BIT, 4, PUBLIC),
            Integer(CLAUSE_IDX_SIZE_IN_WL, id, PUBLIC), Bit(true, PUBLIC));
        require(fill.inserted.reveal() && !fill.overflow.reveal(),
                "Rollback fixture failed to fill one destination");
    }
    Integer rollback_clause(CLAUSE_IDX_SIZE_IN_WL, -2, PUBLIC);
    WatchList::InstallResult rollback = watchlists.install_clause_watchers(
        rollback_clause,
        Integer(VAR_SIZE_BIT, 5, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 4, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    require(!rollback.installed.reveal() && rollback.overflow.reveal() &&
                !watchlists.contains_clause(Integer(VAR_SIZE_BIT, 5, PUBLIC), rollback_clause).reveal() &&
                !watchlists.contains_clause(Integer(VAR_SIZE_BIT, 4, PUBLIC), rollback_clause).reveal(),
            "Two-endpoint Indexed install did not roll back atomically");
}

void test_watch_backend_public_geometry_policy(int party)
{
    (void)party;
    init_constant(300, 1117, 1, 3, 10, 10);
    {
        WatchList flat(base, WatchBackendRequest::Auto);
        require(flat.backend_kind() == WatchBackendKind::BlockLocal,
                "Auto policy ignored the measured one-word Flat crossover");
    }

    init_constant(1400, 13483, 10000, 10, 10, 34);
    {
        WatchList geno(base, WatchBackendRequest::Auto);
        require(geno.backend_kind() == WatchBackendKind::Indexed,
                "Auto policy missed the measured five-word Geno crossover");
    }

    init_constant(8, 12, 8, 4, 4, 128);
    {
        WatchList eligible(base, WatchBackendRequest::Auto);
        require(eligible.backend_kind() == WatchBackendKind::Indexed,
                "Auto policy did not consider the largest one-word occupancy bitmap");
    }

    init_constant(8, 12, 8, 4, 4, 129);
    {
        WatchList fallback(base, WatchBackendRequest::Auto);
        require(fallback.backend_kind() == WatchBackendKind::BlockLocal,
                "Auto policy did not fall back when Occ exceeds one word");
    }
    bool rejected = false;
    try
    {
        WatchList invalid(base, WatchBackendRequest::Indexed);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    require(rejected, "Explicit Indexed request accepted public W > 128");
}

void test_watch_state_distinct_duplicate_and_block_fallback(int party)
{
    (void)party;
    init_constant(8, 12, 8, 4, 4, 8);

    WatchList indexed(base, WatchBackendRequest::Indexed);

    Integer zero_clause(CLAUSE_IDX_SIZE_IN_WL, 0, PUBLIC);
    WatchList::InstallResult zero_id = indexed.install_clause_watchers(
        zero_clause,
        Integer(VAR_SIZE_BIT, 1, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 0, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 0, ALICE), Bit(false, ALICE),
        Bit(true, ALICE));
    Integer zero_pair = indexed.read_pair(zero_clause);
    Integer one_occ_after_zero = indexed.read_occ(
        Integer(VAR_SIZE_BIT, 1, PUBLIC));
    require(!zero_id.installed.reveal() && !zero_id.overflow.reveal() &&
                !one_occ_after_zero.bits[0].reveal() &&
                reveal_32(indexed.unpack_pair_position(zero_pair, 0)) == 0 &&
                reveal_32(indexed.unpack_pair_position(zero_pair, 1)) == 0,
            "Clause ID zero mutated Indexed Pair/Occ state");

    Integer alias_sentinel(CLAUSE_IDX_SIZE_IN_WL, -1, PUBLIC);
    Clause alias_clause(std::vector<int>{1, 2, 3});
    WatchList::InstallResult alias_installed = indexed.install_clause_watchers(
        alias_sentinel,
        Integer(VAR_SIZE_BIT, 1, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 2, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    require(alias_installed.installed.reveal() &&
                !alias_installed.overflow.reveal(),
            "Could not prepare invalid-ID Pair-alias sentinel");
    Integer alias_pair_before = indexed.read_pair(alias_sentinel);

    for (int invalid_id : {CLAUSE_NUM + 1,
                           -(MAX_NUM_OF_CONFLICT_CLAUSE + 1)})
    {
        Integer invalid_clause(CLAUSE_IDX_SIZE_IN_WL, invalid_id, PUBLIC);
        WatchList::InstallResult invalid = indexed.install_clause_watchers(
            invalid_clause,
            Integer(VAR_SIZE_BIT, 3, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
            Integer(VAR_SIZE_BIT, 0, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 0, ALICE), Bit(false, ALICE),
            Bit(true, ALICE));
        require(!invalid.installed.reveal() && !invalid.overflow.reveal() &&
                    !indexed.contains_clause(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                                             invalid_clause)
                         .reveal(),
                "Out-of-domain clause ID mutated Indexed Data state");
    }
    Integer alias_pair_after = indexed.read_pair(alias_sentinel);
    Integer three_occ_after_invalid = indexed.read_occ(
        Integer(VAR_SIZE_BIT, 3, PUBLIC));
    require(reveal_32(indexed.unpack_pair_position(alias_pair_before, 0)) ==
                reveal_32(indexed.unpack_pair_position(alias_pair_after, 0)) &&
                reveal_32(indexed.unpack_pair_position(alias_pair_before, 1)) ==
                    reveal_32(indexed.unpack_pair_position(alias_pair_after, 1)) &&
                !three_occ_after_invalid.bits[0].reveal(),
            "Out-of-domain install mutated aliased Pair/Occ state");

    Integer positive_alias(CLAUSE_IDX_SIZE_IN_WL, CLAUSE_NUM + 1, PUBLIC);
    WatchList::MoveResult invalid_alias_move = indexed.move_clause_from_frame(
        positive_alias, Integer(VAR_SIZE_BIT, 1, BOB),
        Integer(watchlist_scan_cursor_bits(), 0, BOB), alias_clause,
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, BOB),
                             Integer(VAR_SIZE_BIT, 3, BOB)},
        Bit(true, BOB));
    require(!invalid_alias_move.moved.reveal() &&
                !invalid_alias_move.overflow.reveal() &&
                indexed.contains_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                        alias_sentinel)
                    .reveal() &&
                indexed.contains_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                                        alias_sentinel)
                    .reveal() &&
                !indexed.contains_clause(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                                         alias_sentinel)
                     .reveal(),
            "Out-of-domain move read or mutated an aliased valid Pair");

    Integer noncanonical_clause(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC);
    WatchList::InstallResult second_only = indexed.install_clause_watchers(
        noncanonical_clause,
        Integer(VAR_SIZE_BIT, 0, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 0, ALICE), Bit(false, ALICE),
        Integer(VAR_SIZE_BIT, 2, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    Integer second_only_pair = indexed.read_pair(noncanonical_clause);
    require(!second_only.installed.reveal() &&
                !second_only.overflow.reveal() &&
                !indexed.contains_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                                         noncanonical_clause)
                     .reveal() &&
                reveal_32(indexed.unpack_pair_position(second_only_pair, 0)) == 0 &&
                reveal_32(indexed.unpack_pair_position(second_only_pair, 1)) == 0,
            "Second-only watcher install violated canonical Pair encoding");

    Integer duplicate_clause(CLAUSE_IDX_SIZE_IN_WL, 3, PUBLIC);
    WatchList::InstallResult duplicate = indexed.install_clause_watchers(
        duplicate_clause,
        Integer(VAR_SIZE_BIT, 1, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 1, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    require(duplicate.installed.reveal() && !duplicate.overflow.reveal(),
            "Effectively-unit duplicate clause was rejected");
    int duplicate_hits = 0;
    for (const Integer &entry : indexed.get_watchlist_clauses(Integer(VAR_SIZE_BIT, 1, PUBLIC)))
        duplicate_hits += reveal_32(entry) == 3 ? 1 : 0;
    require(duplicate_hits == 1,
            "Duplicate literal installed the same clause more than once");

    WatchList block(base, WatchBackendRequest::BlockLocal);
    require(block.backend_kind() == WatchBackendKind::BlockLocal,
            "Explicit block-local watch backend was not selected");
    Integer block_clause(CLAUSE_IDX_SIZE_IN_WL, 4, PUBLIC);
    WatchList::InstallResult block_install = block.install_clause_watchers(
        block_clause,
        Integer(VAR_SIZE_BIT, -1, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 2, ALICE), Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    require(block_install.installed.reveal() && !block_install.overflow.reveal(),
            "Block-local deep watch installation failed");
    Clause block_clause_value(std::vector<int>{-1, 2, 3, 4});
    Integer secret_slot(bits_required(MAX_CLAUSE_IN_WL), 0, BOB);
    WatchList::MoveResult block_move = block.move_clause_from_frame(
        block_clause, Integer(VAR_SIZE_BIT, -1, BOB), secret_slot,
        block_clause_value,
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, BOB),
                             Integer(VAR_SIZE_BIT, 3, BOB)},
        Bit(true, BOB));
    require(block_move.moved.reveal() && !block_move.overflow.reveal(),
            "Block-local secret-source watcher move failed");
    require(!block.contains_clause(Integer(VAR_SIZE_BIT, -1, PUBLIC), block_clause).reveal() &&
                block.contains_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC), block_clause).reveal() &&
                block.contains_clause(Integer(VAR_SIZE_BIT, 3, PUBLIC), block_clause).reveal(),
            "Block-local move disagreed with Indexed watch semantics");
}

void test_clause_storage(int party)
{
    auto solver = make_solver(party);
    Clause c(std::vector<int>{1, -2});
    Integer clause_idx(CLAUSE_IDX_SIZE_IN_WL, 5, PUBLIC);
    solver->cl->insert_clause(clause_idx, c, Bit(true, PUBLIC));

    Clause out;
    solver->cl->get_clause(clause_idx, out);
    require(reveal_32(out.literals[0]) == 1, "Clause literal[0] mismatch");
    require(reveal_32(out.literals[1]) == -2, "Clause literal[1] mismatch");
}

void test_initialization_writes_only_original_clause_store(int party)
{
    auto solver = make_solver(party);
    require(solver->cl->logical_phi_clause_writes > 0,
            "Solver initialization did not populate the original-clause store");
    require(solver->cl->logical_con_clause_writes == 0,
            "Public original clauses triggered dummy learned-store writes: " +
                std::to_string(solver->cl->logical_con_clause_writes));
}

void test_exact_public_storage_geometry()
{
    init_constant(300, 1117, 1, 3, 10, 10);
    require(VAR_SIZE_BIT == 10 && CLAUSE_IDX_SIZE_IN_WL == 12,
            "Flat fixture index widths changed unexpectedly");
    require(PHI_CLAUSE_BITSTRING_SIZE_BIT == 30 &&
                PHI_CLAUSE_ORAM_UNIT_SIZE == 5 &&
                PHI_ORAM_BLOCKS_PER_CLAUSE == 1,
            "Flat original clauses are not packed as one exact 30-bit payload");
    require(CON_CLAUSE_BITSTRING_SIZE_BIT == 100 &&
                CON_CLAUSE_ORAM_UNIT_SIZE == 7 &&
                CON_ORAM_BLOCKS_PER_CLAUSE == 1,
            "Flat learned clauses are not packed as one exact 100-bit payload");
    require(WL_BITSTRING_SIZE_BIT == 120 &&
                WL_ORAM_UNIT_SIZE == 7 &&
                WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL == 1 &&
                WL_ORAM_SIZE_BIT == 10,
            "Flat watchlists are not packed as one exact 120-bit payload");

    init_constant(100, 100, 8, 8, 8, 8);
    require(PHI_CLAUSE_BITSTRING_SIZE_BIT == 64 &&
                PHI_CLAUSE_ORAM_UNIT_SIZE == 6 &&
                PHI_ORAM_BLOCKS_PER_CLAUSE == 1,
            "Exact-power clause payload was rounded to the next unit");
    require(WL_BITSTRING_SIZE_BIT == 64 &&
                WL_ORAM_UNIT_SIZE == 6 &&
                WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL == 1,
            "Exact-power watchlist payload was rounded to the next unit");

    init_constant(100, 100, 8, 32, 8, 8);
    require(PHI_CLAUSE_BITSTRING_SIZE_BIT == 256 &&
                PHI_CLAUSE_ORAM_UNIT_SIZE == 7 &&
                PHI_ORAM_BLOCKS_PER_CLAUSE == 2,
            "Exact 256-bit clause payload allocated an extra block");

    init_constant(100, 100, 8, 15, 8, 8);
    require(PHI_ORAM_BLOCKS_PER_CLAUSE == 1,
            "Sub-128-bit clause payload allocated multiple blocks");
    init_constant(100, 100, 8, 17, 8, 8);
    require(PHI_ORAM_BLOCKS_PER_CLAUSE == 2,
            "Above-128-bit clause payload did not allocate a second block");

    // The indexed watch backend uses block-local packing: no clause ID may
    // straddle two FloRAM words.  This fixture has 15-bit IDs and therefore
    // fits eight entries per 128-bit word.  A 34-entry list needs five words;
    // a tight bitstream would need only four, but its IDs would cross words.
    init_constant(1400, 13483, 10000, 10, 10, 34);
    require(CLAUSE_IDX_SIZE_IN_WL == 15 &&
                WL_ORAM_UNIT_SIZE == 7 &&
                WL_ENTRIES_PER_BLOCK == 8 &&
                WL_ORAM_NUM_OF_BLOCK_FOR_ONE_WL == 5,
            "Watchlist entries are not block-local at Geno geometry");
    require(WATCH_POSITION_SIZE_BIT == 4 &&
                WATCH_PAIR_PAYLOAD_BITS == 8 &&
                WATCH_OCC_PAYLOAD_BITS == 34,
            "Indexed watcher metadata geometry is not minimal");
}

void test_packed_neighbor_roundtrip(int party)
{
    (void)party;
    init_constant(100, 100, 8, 8, 8, 8);
    ClauseList clauses(base);
    WatchList watchlists(base);

    Clause clause_two(std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8});
    Clause clause_three(std::vector<int>{-1, -2, -3, -4, -5, -6, -7, -8});
    Integer clause_idx_two(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC);
    Integer clause_idx_three(CLAUSE_IDX_SIZE_IN_WL, 3, PUBLIC);
    clauses.insert_phi_clause(clause_idx_two, clause_two, Bit(true, PUBLIC));
    clauses.insert_phi_clause(clause_idx_three, clause_three, Bit(true, PUBLIC));

    Clause out_two;
    Clause out_three;
    clauses.get_phi_clause(clause_idx_two, out_two);
    clauses.get_phi_clause(clause_idx_three, out_three);
    for (int offset = 0; offset < 8; ++offset)
    {
        require(reveal_32(out_two.literals[offset]) == offset + 1,
                "Adjacent packed original clause overwrote its neighbor");
        require(reveal_32(out_three.literals[offset]) == -(offset + 1),
                "Adjacent packed original clause failed to roundtrip");
    }

    Integer literal_two(VAR_SIZE_BIT, 2, PUBLIC);
    Integer literal_three(VAR_SIZE_BIT, 3, PUBLIC);
    watchlists.add_clause(literal_two, Integer(CLAUSE_IDX_SIZE_IN_WL, 11, PUBLIC), Bit(true, PUBLIC));
    watchlists.add_clause(literal_three, Integer(CLAUSE_IDX_SIZE_IN_WL, 12, PUBLIC), Bit(true, PUBLIC));
    std::vector<Integer> wl_two = watchlists.get_watchlist_clauses(literal_two);
    std::vector<Integer> wl_three = watchlists.get_watchlist_clauses(literal_three);
    require(reveal_32(wl_two[0]) == 11,
            "Adjacent packed watchlist overwrote its neighbor");
    require(reveal_32(wl_three[0]) == 12,
            "Adjacent packed watchlist failed to roundtrip");
}

void test_multiblock_clause_physical_address_width(int party)
{
    (void)party;
    // Geometry from benchmarks/Haplotype/n10/genos.haps.20.cnf.  Logical
    // clause IDs are 15 bits, while three blocks per original clause require
    // a 16-bit physical address.  Multiplying at 15 bits would make the high
    // clause overlap a lower clause before either address reached the ORAM.
    {
        init_constant(1280, 14800, 10000, 30, 10, 40);
        ClauseList clauses(base);

        std::vector<int> low_literals;
        std::vector<int> high_literals;
        for (int literal = 1; literal <= 30; ++literal)
        {
            low_literals.push_back(literal);
            high_literals.push_back(-literal);
        }
        Clause low(low_literals);
        Clause high(high_literals);
        Integer low_idx(CLAUSE_IDX_SIZE_IN_WL, 3877, PUBLIC);
        Integer high_idx(CLAUSE_IDX_SIZE_IN_WL, 14800, PUBLIC);

        clauses.insert_phi_clause(low_idx, low, Bit(true, PUBLIC));
        clauses.insert_phi_clause(high_idx, high, Bit(true, PUBLIC));

        Clause low_out;
        Clause high_out;
        clauses.get_phi_clause(low_idx, low_out);
        clauses.get_phi_clause(high_idx, high_out);
        for (int offset = 0; offset < 30; ++offset)
        {
            require(reveal_32(low_out.literals[offset]) == offset + 1,
                    "High original-clause address wrapped onto a lower multi-block clause");
            require(reveal_32(high_out.literals[offset]) == -(offset + 1),
                    "High multi-block original clause did not roundtrip at its physical address");
        }
    }

    // Exercise the symmetric learned-clause path with four blocks per clause.
    // At a 15-bit width, 10000*4 would wrap to 7232 and collide with learned
    // clause 1808.  The physical conflict-ORAM address is 16 bits.
    {
        init_constant(1280, 14800, 10000, 20, 40, 40);
        ClauseList clauses(base);

        std::vector<int> low_literals;
        std::vector<int> high_literals;
        for (int literal = 1; literal <= 40; ++literal)
        {
            low_literals.push_back(literal);
            high_literals.push_back(-literal);
        }
        Clause low(low_literals);
        Clause high(high_literals);
        Integer low_idx(CLAUSE_IDX_SIZE_IN_WL, -1808, PUBLIC);
        Integer high_idx(CLAUSE_IDX_SIZE_IN_WL, -10000, PUBLIC);

        clauses.insert_clause(low_idx, low, Bit(true, PUBLIC));
        clauses.insert_clause(high_idx, high, Bit(true, PUBLIC));

        Clause low_out;
        Clause high_out;
        clauses.get_clause(low_idx, low_out);
        clauses.get_clause(high_idx, high_out);
        for (int offset = 0; offset < 40; ++offset)
        {
            require(reveal_32(low_out.literals[offset]) == offset + 1,
                    "High learned-clause address wrapped onto a lower multi-block clause");
            require(reveal_32(high_out.literals[offset]) == -(offset + 1),
                    "High multi-block learned clause did not roundtrip at its physical address");
        }
    }
}

void test_paper_capacity_learned_clause_roundtrip(int party)
{
    // At capacity 10,000 the conflict FloRAM stash holds more than one
    // pending write.  This checks a range_read after a write and before a
    // refresh, which the solver relies on when it installs a newly learned
    // clause into its watchlists.
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   8, 2, 10, 10000);
    Clause learned(std::vector<int>{2, 4, 1, 6});
    Integer learned_idx(CON_CLAUSE_IDX_SIZE_BIT, -1, PUBLIC);
    solver->cl->insert_clause(learned_idx, learned, Bit(true, PUBLIC));

    Clause out;
    solver->cl->get_clause(learned_idx, out);
    require(reveal_32(out.literals[0]) == 2 &&
                reveal_32(out.literals[1]) == 4 &&
                reveal_32(out.literals[2]) == 1 &&
                reveal_32(out.literals[3]) == 6,
            "Paper-capacity learned clause was stale after write-before-refresh");

    solver->construct_wl(learned_idx, Bit(true, PUBLIC));
    std::vector<Integer> watched =
        solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, 2, PUBLIC));
    bool found = false;
    for (const auto &entry : watched)
        found = found || reveal_32(entry) == -1;
    require(found, "Paper-capacity learned clause was not installed in its watchlist");
}

void setup_two_round_uip_conflict(CDCL &solver);
void require_clause_is_nonzero(const Clause &clause, const std::string &message);

void test_paper_capacity_learned_index_boundary(int party)
{
    auto solver = make_solver_from("./test/cnfs/depth20_conflict.cnf", party,
                                   8, 2, 3, 10000);
    setup_two_round_uip_conflict(*solver);
    solver->conflict_ctr = Integer(CON_CLAUSE_IDX_SIZE_BIT, 8193, PUBLIC);

    auto result = solver->conflict_analysis(
        Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));
    require(result.valid.reveal(),
            "Legal learned clause above the physical sign boundary was rejected");
    require(reveal_32(result.learned_clause_idx) == -8193,
            "Learned clause ID wrapped at the physical ORAM address sign bit");

    Clause learned;
    solver->cl->get_clause(result.learned_clause_idx, learned);
    require_clause_is_nonzero(
        learned, "Learned clause above ID -8192 was not stored in conflict ORAM");

    solver->conflict_ctr = Integer(CON_CLAUSE_IDX_SIZE_BIT, 10000, PUBLIC);
    result = solver->conflict_analysis(
        Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));
    require(result.valid.reveal(), "The final legal learned-clause slot was rejected");
    require(reveal_32(result.learned_clause_idx) == -10000,
            "The final paper-capacity learned-clause ID was not representable");
    solver->cl->get_clause(result.learned_clause_idx, learned);
    require_clause_is_nonzero(
        learned, "The final paper-capacity learned clause was not stored");

    solver->backtrack(result.beta, result.valid);
    solver->backtrack_aftermath(result.valid, result.learned_clause_idx,
                                result.learned_clause);
    Integer antecedent;
    solver->big_v->get_antecedent(solver->asserting_literal.abs(), antecedent);
    require(reveal_32(antecedent) == -10000,
            "The final learned-clause ID was corrupted in antecedent storage");
}

void test_search_two_states(int party)
{
    auto solver = make_solver(party);
    Integer zero_lit(VAR_SIZE_BIT, 0, PUBLIC);

    std::vector<Integer> open_clause = solver->search_two_non_falsified(Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC), zero_lit);
    require(reveal_32(open_clause[0]) == 3, "search_two did not return first open literal");
    require(reveal_32(open_clause[1]) == -2, "search_two did not return second open literal");

    solver->big_v->set_unit_literal(Integer(VAR_SIZE_BIT, -3, PUBLIC),
                                    Integer(ANTE_SIZE_BIT, 1, PUBLIC),
                                    Integer(DL_SIZE_BIT, 1, PUBLIC));
    std::vector<Integer> unit_clause = solver->search_two_non_falsified(Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC), zero_lit);
    require(reveal_32(unit_clause[0]) == -2, "search_two did not identify the remaining unit literal");
    require(reveal_32(unit_clause[1]) == -2, "search_two did not duplicate the unit literal");

    solver->big_v->set_unit_literal(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                                    Integer(ANTE_SIZE_BIT, 2, PUBLIC),
                                    Integer(DL_SIZE_BIT, 1, PUBLIC));
    std::vector<Integer> conflict_clause = solver->search_two_non_falsified(Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC), zero_lit);
    require(reveal_32(conflict_clause[0]) == 0, "search_two conflict state should return zero first literal");
    require(reveal_32(conflict_clause[1]) == 0, "search_two conflict state should return zero second literal");

    auto unit_solver = make_solver_from("./test/cnfs/unit_pair.cnf", party);
    std::vector<Integer> parsed_unit = unit_solver->search_two_non_falsified(Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC), zero_lit);
    require(reveal_32(parsed_unit[0]) == 1, "search_two did not keep a unit clause literal");
    require(reveal_32(parsed_unit[1]) == 1, "search_two let padded zero erase a unit clause result");
}

void test_shortest_non_falsified_heuristic(int party)
{
    auto solver = make_solver_from("./test/cnfs/shortest_choice.cnf", party, 8, 4, 4, 8);
    std::vector<Integer> best = solver->search_two_shortest_non_falsified(Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC),
                                                                          Integer(VAR_SIZE_BIT, 0, PUBLIC));
    require(reveal_32(best[0]) == 2, "Shortest-watchlist search did not pick literal 2 first");
    require(reveal_32(best[1]) == 1, "Shortest-watchlist search did not keep literal 1 as second best");
}

void test_unit_clause_parsing_and_handling(int party)
{
    auto solver = make_solver_from("./test/cnfs/unit_pair.cnf", party);
    require(solver->cl->unit_clause.size() == 2, "Unit clause parser recorded the wrong count");

    solver->check_unit_clause_conflict();
    require(!solver->if_unsat.reveal(), "Non-conflicting unit clauses marked UNSAT");

    solver->handle_unit_clause(Bit(true, PUBLIC));
    Integer ass1, ass2;
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, 1, PUBLIC), ass1);
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, 2, PUBLIC), ass2);
    require(reveal_32(ass1, true) == reveal_32(solver->big_v->true_assigned, true), "Unit literal 1 not assigned");
    require(reveal_32(ass2, true) == reveal_32(solver->big_v->true_assigned, true), "Unit literal 2 not assigned");
    require(reveal_32(solver->big_u[1], true) == 1, "Unit literal 1 not pushed to big_U");
    require(reveal_32(solver->big_u[2], true) == 1, "Unit literal 2 not pushed to big_U");
}

void test_contradictory_unit_detection(int party)
{
    auto solver = make_solver_from("./test/cnfs/unit_conflict.cnf", party);
    solver->check_unit_clause_conflict();
    require(solver->if_unsat.reveal(), "Contradictory unit clauses were not detected");
}

void test_check_sat_requires_assigned_and_empty_queue(int party)
{
    auto solver = make_solver(party);
    require(!solver->if_all_assigned().reveal(), "Fresh solver should not be all assigned");
    require(!solver->check_sat().reveal(), "Fresh solver should not be SAT");

    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC), Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 2, PUBLIC), Integer(DL_SIZE_BIT, 2, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 3, PUBLIC), Integer(DL_SIZE_BIT, 3, PUBLIC));
    for (auto &entry : solver->big_u)
    {
        entry = Integer(2, 0, PUBLIC);
    }

    require(solver->if_all_assigned().reveal(), "Assigned solver should report all assigned");
    solver->check_sat(Bit(false, PUBLIC));
    require(!solver->if_sat.reveal(), "Disabled SAT check committed SAT");

    solver->backtrack(Integer(DL_SIZE_BIT, 1, PUBLIC), Bit(true, PUBLIC));
    require(!solver->if_all_assigned().reveal(),
            "Backtrack did not reduce the live assignment count");
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 2, PUBLIC), Integer(DL_SIZE_BIT, 2, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 3, PUBLIC), Integer(DL_SIZE_BIT, 3, PUBLIC));
    require(solver->if_all_assigned().reveal(),
            "Reassignment did not restore the live assignment count");
    require(solver->check_sat().reveal(), "Assigned solver with empty big_U should report SAT");
}

void test_if_all_assigned_uses_live_assignment_count(int party)
{
    // The all-assigned check uses the maintained live trail length instead of
    // rescanning every public variable.  The test pins this as a circuit-size
    // bound.
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);
    const std::uint64_t gates_before = base->num_and();
    Bit all_assigned = solver->if_all_assigned();
    const std::uint64_t gate_count = base->num_and() - gates_before;

    require(!all_assigned.reveal(), "Fresh depth-20 solver should not be all assigned");
    require(gate_count < static_cast<std::uint64_t>(VAR_NUM),
            "if_all_assigned rescanned variables instead of checking the live count: " +
                std::to_string(gate_count) + " AND gates for " +
                std::to_string(VAR_NUM) + " variables");
}

void assign_all_variables_for_sat_fixture(CDCL &solver)
{
    for (int var = 1; var <= VAR_NUM; ++var)
    {
        solver.big_v->set_decision(Integer(VAR_SIZE_BIT, var, PUBLIC),
                                   Integer(DL_SIZE_BIT, var, PUBLIC));
    }
    for (auto &entry : solver.big_u)
    {
        entry = Integer(2, 0, PUBLIC);
    }
}

void test_sat_check_precedes_decision_request_at_source_attempt(int party)
{
    auto solver = make_solver(party);
    assign_all_variables_for_sat_fixture(*solver);
    solver->oblivious_decision_delay = 5;
    solver->step_idx = 1; // logical t=2 is not a decision-activation slot

    solver->pick_new_clause(Bit(true, PUBLIC), true, true);

    require(solver->if_sat.reveal(),
            "Empty Big-U source attempt did not recognize SAT before requesting a decision");
    require(!solver->if_decision_pending.reveal(),
            "SAT source attempt left a spurious decision pending");
}

void test_sat_waits_until_consumed_unit_frame_finishes(int party)
{
    auto solver = make_solver(party);
    assign_all_variables_for_sat_fixture(*solver);
    solver->big_u[3] = Integer(2, 1, PUBLIC);
    solver->oblivious_decision_delay = 5;
    solver->step_idx = 1;

    solver->pick_new_clause(Bit(true, PUBLIC), true, true);

    require(!solver->if_sat.reveal(),
            "Solver declared SAT before scanning the final pending literal's watchlist frame");
    require(reveal_32(solver->a_idx) == 3,
            "Final pending literal was not selected before its SAT checkpoint");
}

void test_sat_check_uses_public_source_attempt_schedule(int party)
{
    auto solver = make_solver(party);
    solver->oblivious_decision_delay = 5;
    solver->step_idx = 1;
    const std::uint64_t calls_before = solver->logical_check_sat_calls;

    solver->pick_new_clause(Bit(true, PUBLIC), false, false);
    require(solver->logical_check_sat_calls == calls_before,
            "Non-source public slot executed CheckSAT");

    solver->step_idx = 2;
    solver->pick_new_clause(Bit(true, PUBLIC), true, true);
    require(solver->logical_check_sat_calls == calls_before + 1,
            "Source-attempt slot did not execute exactly one logical CheckSAT");
}

void test_make_decision_respects_flag(int party)
{
    auto solver = make_solver(party);
    solver->make_decision(Bit(false, PUBLIC));
    require(reveal_32(solver->big_v->cur_dl) == 0, "Disabled decision changed decision level");
    require(!solver->if_all_assigned().reveal(), "Disabled decision assigned variables");

    Integer assigned = solver->make_decision(Bit(true, PUBLIC));
    int assigned_lit = reveal_32(assigned);
    require(assigned_lit != 0, "Enabled decision returned zero literal");
    require(reveal_32(solver->big_v->cur_dl) == 1, "Enabled decision did not advance decision level");

    Integer ass;
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, std::abs(assigned_lit), PUBLIC), ass);
    require(reveal_32(ass, true) != reveal_32(solver->big_v->not_assigned, true), "Enabled decision did not assign variable");
}

void test_make_decision_omits_impossible_terminal_check(int party)
{
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);
    const std::uint64_t gates_before = base->num_and();
    solver->make_decision(Bit(false, PUBLIC));
    const std::uint64_t gate_count = base->num_and() - gates_before;

    require(reveal_32(solver->big_v->cur_dl) == 0,
            "Disabled decision changed state during circuit-size test");
    // This bound characterizes the linear variable-state circuit.  The ORAM
    // adapter deliberately performs its private accesses and therefore has a
    // different gate budget; the state-equivalence assertion above still
    // applies to both backends.
    if (!g_use_oram_variable_state)
    {
        require(gate_count <= 2343,
                "make_decision retained its impossible terminal check: " +
                    std::to_string(gate_count) + " AND gates");
    }
}

void test_make_decision_rejects_zero_when_all_assigned(int party)
{
    auto solver = make_solver(party);
    assign_all_variables_for_sat_fixture(*solver);
    const int dl_before = reveal_32(solver->big_v->cur_dl);
    const int live_before = reveal_32(solver->big_v->live_assignment_count());

    Integer assigned = solver->make_decision(Bit(true, PUBLIC), false);

    require(reveal_32(assigned) == 0,
            "All-assigned VSIDS fixture unexpectedly selected a real variable");
    require(reveal_32(solver->big_v->cur_dl) == dl_before,
            "Zero decision advanced the decision level");
    require(reveal_32(solver->big_v->live_assignment_count()) == live_before,
            "Zero decision corrupted the live assignment count");
}

void test_vsids_skips_assigned_ties(int party)
{
    auto solver = make_solver(party);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC), Integer(DL_SIZE_BIT, 1, PUBLIC));
    Integer picked = solver->pick_variable_VSIDS();
    require(reveal_32(picked) == 2, "VSIDS tie handling did not skip the assigned variable or use the positive default phase");
}

void test_vsids_uses_exact_activity_and_positive_default_phase(int party)
{
    auto solver = make_solver(party);
    solver->activities[1] = Integer(ACTIVITY_SIZE_BIT, 1900, PUBLIC);
    solver->activities[2] = Integer(ACTIVITY_SIZE_BIT, 1100, PUBLIC);

    Integer picked = solver->pick_variable_VSIDS();
    const int picked_lit = reveal_32(picked);
    require(std::abs(picked_lit) == 1,
            "VSIDS compared truncated activity buckets instead of the exact score");
    require(picked_lit == 1, "VSIDS did not use the positive default phase");

    solver->big_v->cur_dl = Integer(DL_SIZE_BIT, 1, PUBLIC);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, -1, PUBLIC),
                                Integer(DL_SIZE_BIT, 1, PUBLIC),
                                Bit(true, PUBLIC));
    solver->backtrack(Integer(DL_SIZE_BIT, 0, PUBLIC), Bit(true, PUBLIC));
    picked = solver->pick_variable_VSIDS();
    require(reveal_32(picked) == -1, "VSIDS did not reuse the saved negative phase");
}

void test_get_w_from_big_u_respects_flag(int party)
{
    auto solver = make_solver(party);
    solver->pnc_state = solver->PNC_STATE_BIG_U;
    solver->big_u[2] = Integer(2, 2, PUBLIC);
    solver->a_idx = Integer(VAR_SIZE_BIT, 1, PUBLIC);

    solver->get_w_from_big_U(Bit(false, PUBLIC));
    require(reveal_32(solver->a_idx) == 1, "Disabled big_U selection changed a_idx");
    require(reveal_32(solver->big_u[2], true) == 2, "Disabled big_U selection removed entry");

    solver->get_w_from_big_U(Bit(true, PUBLIC));
    require(reveal_32(solver->a_idx) == -2, "big_U selection did not choose negative literal");
    require(reveal_32(solver->big_u[2], true) == 0, "big_U selection did not remove selected entry");
}

void test_get_w_from_big_u_reuses_literal_classification(int party)
{
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);
    solver->big_u[20] = Integer(2, 2, PUBLIC);

    const std::uint64_t gates_before = base->num_and();
    Bit found = solver->get_w_from_big_U(Bit(false, PUBLIC));
    const std::uint64_t gate_count = base->num_and() - gates_before;

    require(found.reveal(), "Big-U classification did not find the queued literal");
    require(gate_count <= 511,
            "get_w_from_big_U recomputed literal classifications: " +
                std::to_string(gate_count) + " AND gates");
}

void test_get_w_from_new_decision_consumes_decision_literal(int party)
{
    auto solver = make_solver(party);
    solver->pnc_state = solver->PNC_STATE_DECISION;

    solver->get_w_from_new_decision(Bit(false, PUBLIC));
    require(reveal_32(solver->a_idx) == -1, "Disabled new decision changed a_idx");
    require(reveal_32(solver->big_v->cur_dl) == 0, "Disabled new decision changed decision level");

    solver->get_w_from_new_decision(Bit(true, PUBLIC));
    int lit = reveal_32(solver->a_idx);
    require(lit != 0, "New decision did not select a literal");
    require(reveal_32(solver->big_u[std::abs(lit)], true) == 0, "New decision left its literal in big_U");
}

void test_get_w_from_new_decision_avoids_big_u_round_trip(int party)
{
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);
    solver->pnc_state = solver->PNC_STATE_DECISION;

    const std::uint64_t gates_before = base->num_and();
    solver->get_w_from_new_decision(Bit(false, PUBLIC));
    const std::uint64_t gate_count = base->num_and() - gates_before;

    require(reveal_32(solver->big_v->cur_dl) == 0,
            "Disabled source decision changed state during circuit-size test");
    // This ceiling covers the linear variable-state route.  ORAM decision
    // writes intentionally have a different access cost, while the no-op
    // semantic assertion above remains backend-independent.
    if (!g_use_oram_variable_state)
    {
        require(gate_count <= 1976,
                "Source decision inserted and immediately erased its Big-U literal: " +
                    std::to_string(gate_count) + " AND gates");
    }
}

void test_decision_delay_waits_for_scheduled_slot(int party)
{
    auto solver = make_solver(party);
    solver->oblivious_decision_delay = 5;

    solver->step_idx = 2;
    solver->pick_new_clause(Bit(true, PUBLIC));
    require(solver->if_decision_pending.reveal(), "Decision was not marked pending before the scheduled slot");
    require(reveal_32(solver->big_v->cur_dl) == 0, "Unscheduled decision advanced the decision level");
    require(reveal_32(solver->wl_scan_ctr) == -1, "Unscheduled decision started a dummy watchlist scan");
    require(reveal_32(solver->w_idx) == 0, "Unscheduled decision selected a watched clause");

    solver->step_idx = 3;
    solver->pick_new_clause(Bit(true, PUBLIC));
    require(solver->if_decision_pending.reveal(), "Pending decision was cleared before the scheduled slot");
    require(reveal_32(solver->big_v->cur_dl) == 0, "Pending decision performed logical work on a dummy step");
    require(reveal_32(solver->wl_scan_ctr) == -1, "Pending decision consumed watchlist cursor on a dummy step");

    solver->step_idx = 5;
    solver->pick_new_clause(Bit(true, PUBLIC));
    require(!solver->if_decision_pending.reveal(), "Scheduled decision slot did not clear pending decision");
    require(reveal_32(solver->big_v->cur_dl) == 1, "Scheduled decision did not advance decision level");
    require(reveal_32(solver->wl_scan_ctr) == MAX_CLAUSE_IN_WL - 1, "Scheduled decision did not initialize watchlist scan");
    require(solver->scanned_wl_this_step.reveal(), "Scheduled decision did not scan the first watchlist slot");
}

void test_scan_cursor_advances_only_when_active(int party)
{
    auto solver = make_solver(party);
    solver->wl_scan_ctr = solver->wl_scan_ctr_value(3);

    solver->advance_wl_scan_ctr(Bit(false, PUBLIC));
    require(reveal_32(solver->wl_scan_ctr) == 3, "Inactive scan advanced watchlist cursor");

    solver->advance_wl_scan_ctr(Bit(true, PUBLIC));
    require(reveal_32(solver->wl_scan_ctr) == 2, "Active scan did not advance watchlist cursor");

    solver->backtrack(Integer(DL_SIZE_BIT, 0, PUBLIC), Bit(false, PUBLIC));
    require(reveal_32(solver->wl_scan_ctr) == 2, "Disabled backtrack reset watchlist cursor");

    solver->backtrack(Integer(DL_SIZE_BIT, 0, PUBLIC), Bit(true, PUBLIC));
    require(reveal_32(solver->wl_scan_ctr) == -1, "Backtrack did not reset watchlist cursor");
}

void test_repeated_giant_step_uses_conservative_source_work(int party)
{
    auto solver = make_solver(party);
    solver->oblivious_decision_delay = 3;
    solver->oblivious_conflict_delay = 5;

    solver->begin_giant_step(1);
    solver->begin_giant_step(5);

    require(solver->possible_source_attempt_steps == 5,
            "a repeated bounded run did not conservatively attempt source work on every step");
    require(solver->possible_source_refresh_steps == 5,
            "a repeated bounded run did not conservatively permit refresh work on every step");
}

void test_backtrack_levels(int party)
{
    auto solver = make_solver(party);
    solver->big_v->cur_dl = Integer(DL_SIZE_BIT, 3, PUBLIC);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC), Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 2, PUBLIC), Integer(DL_SIZE_BIT, 2, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 3, PUBLIC), Integer(DL_SIZE_BIT, 3, PUBLIC));

    solver->backtrack(Integer(DL_SIZE_BIT, 2, PUBLIC), Bit(true, PUBLIC));

    Integer ass1, ass2, ass3;
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, 1, PUBLIC), ass1);
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, 2, PUBLIC), ass2);
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, 3, PUBLIC), ass3);

    require(reveal_32(ass1, true) == reveal_32(solver->big_v->true_assigned, true), "Level-1 assignment was cleared");
    require(reveal_32(ass2, true) == reveal_32(solver->big_v->true_assigned, true), "Level-2 assignment was cleared");
    require(reveal_32(ass3, true) == reveal_32(solver->big_v->not_assigned, true), "Level-3 assignment not cleared");
}

void test_construct_wl_and_update_wl(int party)
{
    auto solver = make_solver(party);

    Clause unit_learned(std::vector<int>{-1});
    Integer learned_idx(CON_ORAM_SIZE_BIT, -1, PUBLIC);
    solver->cl->insert_clause(learned_idx, unit_learned, Bit(true, PUBLIC));
    solver->construct_wl(learned_idx, Bit(true, PUBLIC));

    std::vector<Integer> neg_one_wl = solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, -1, PUBLIC));
    int learned_hits = 0;
    for (const auto &entry : neg_one_wl)
    {
        learned_hits += (reveal_32(entry) == -1) ? 1 : 0;
    }
    require(learned_hits == 1, "Unit learned clause should be watched exactly once");

    Clause c(std::vector<int>{1, 2, 3});
    Integer clause_idx(CLAUSE_IDX_SIZE_IN_WL, -4, PUBLIC);
    solver->cl->insert_clause(clause_idx, c, Bit(true, PUBLIC));
    Integer source_sentinel(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC);
    solver->wl->add_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                           source_sentinel, Bit(true, PUBLIC));
    solver->wl->add_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC), clause_idx, Bit(true, PUBLIC));
    solver->wl->add_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC), clause_idx, Bit(true, PUBLIC));
    solver->update_wl(clause_idx, Integer(VAR_SIZE_BIT, 1, PUBLIC),
                      {Integer(VAR_SIZE_BIT, 2, PUBLIC), Integer(VAR_SIZE_BIT, 3, PUBLIC)}, Bit(true, PUBLIC));

    std::vector<Integer> one_wl = solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, 1, PUBLIC));
    std::vector<Integer> two_wl = solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, 2, PUBLIC));
    std::vector<Integer> three_wl = solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, 3, PUBLIC));
    bool found_one = false;
    bool found_two = false;
    bool found_three = false;
    for (const auto &entry : one_wl)
        found_one = found_one | (reveal_32(entry) == -4);
    for (const auto &entry : two_wl)
        found_two = found_two | (reveal_32(entry) == -4);
    for (const auto &entry : three_wl)
        found_three = found_three | (reveal_32(entry) == -4);
    require(!found_one, "Fresh falsified literal still watches clause after update");
    require(solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                        source_sentinel)
                .reveal(),
            "Compatibility update cleared an unrelated source slot");
    require(found_two, "Existing non-falsified watcher was lost");
    require(found_three, "Replacement watcher was not added");
}

void test_learned_clause_keeps_two_distinct_watchers(int party)
{
    auto solver = make_solver(party);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, -2, ALICE),
                                Integer(DL_SIZE_BIT, 1, ALICE));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, -3, ALICE),
                                Integer(DL_SIZE_BIT, 1, ALICE));

    Integer learned_idx(CLAUSE_IDX_SIZE_IN_WL, -2, PUBLIC);
    Clause learned(std::vector<int>{1, 2, 3});
    solver->cl->insert_clause(learned_idx, learned, Bit(true, PUBLIC));
    solver->construct_wl(learned_idx, Bit(true, PUBLIC));

    require(solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                        learned_idx)
                    .reveal() &&
                solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                                            learned_idx)
                    .reveal() &&
                !solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                                             learned_idx)
                     .reveal(),
            "Asserting learned clause did not keep one distinct fallback watcher");
}

void test_indexed_compatibility_update_recovers_private_source_slot(int party)
{
    auto solver = make_solver_from(
        "./test/cnfs/conflict_simple.cnf", party, 8, 3, 3, 8,
        WatchBackendRequest::Indexed);
    Integer sentinel(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC);
    require(!solver->wl->add_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                    sentinel, Bit(true, PUBLIC))
                 .reveal(),
            "Could not prepare Indexed compatibility source sentinel");

    Integer clause_idx(CLAUSE_IDX_SIZE_IN_WL, -1, PUBLIC);
    Clause clause(std::vector<int>{1, 2, 3});
    solver->cl->insert_clause(clause_idx, clause, Bit(true, PUBLIC));
    WatchList::InstallResult installed = solver->wl->install_clause_watchers(
        clause_idx,
        Integer(VAR_SIZE_BIT, 1, ALICE),
        Integer(WATCH_POSITION_SIZE_BIT, 1, ALICE), Bit(true, ALICE),
        Integer(VAR_SIZE_BIT, 2, ALICE),
        Integer(WATCH_POSITION_SIZE_BIT, 2, ALICE), Bit(true, ALICE),
        Bit(true, ALICE));
    require(installed.installed.reveal() && !installed.overflow.reveal(),
            "Could not prepare Indexed compatibility Pair/Data/Occ state");

    const std::uint64_t reads_before = solver->wl->logical_range_reads;
    solver->update_wl(
        clause_idx, Integer(VAR_SIZE_BIT, 1, BOB),
        std::vector<Integer>{Integer(VAR_SIZE_BIT, 2, BOB),
                             Integer(VAR_SIZE_BIT, 3, BOB)},
        Bit(true, BOB));
    require(solver->wl->logical_range_reads - reads_before == 1,
            "Indexed compatibility update did not use one fixed source-list read");
    require(solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                        sentinel)
                    .reveal() &&
                !solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                                             clause_idx)
                     .reveal() &&
                solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                                            clause_idx)
                    .reveal() &&
                solver->wl->contains_clause(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                                            clause_idx)
                    .reveal(),
            "Indexed compatibility update did not preserve the recovered source frame");
}

void test_update_wl_adds_one_replacement(int party)
{
    auto solver = make_solver_from("./test/cnfs/shortest_choice.cnf", party, 8, 4, 4, 8);
    Clause c(std::vector<int>{1, 2, 3, 4});
    Integer clause_idx(CLAUSE_IDX_SIZE_IN_WL, -5, PUBLIC);
    solver->cl->insert_clause(clause_idx, c, Bit(true, PUBLIC));
    solver->wl->add_clause(Integer(VAR_SIZE_BIT, 1, PUBLIC), clause_idx, Bit(true, PUBLIC));
    solver->wl->add_clause(Integer(VAR_SIZE_BIT, 2, PUBLIC), clause_idx, Bit(true, PUBLIC));

    std::vector<Integer> replacements;
    replacements.push_back(Integer(VAR_SIZE_BIT, 3, PUBLIC));
    replacements.push_back(Integer(VAR_SIZE_BIT, 4, PUBLIC));
    const std::uint64_t reads_before = solver->wl->logical_range_reads;
    const std::uint64_t writes_before = solver->wl->logical_watchlist_writes;
    solver->update_wl(clause_idx, Integer(VAR_SIZE_BIT, 1, PUBLIC), replacements, Bit(true, PUBLIC));
    const std::uint64_t update_reads = solver->wl->logical_range_reads - reads_before;
    const std::uint64_t update_writes = solver->wl->logical_watchlist_writes - writes_before;

    const std::uint64_t expected_reads =
        solver->wl->backend_kind() == WatchBackendKind::Indexed ? 1 : 3;
    require(update_reads == expected_reads,
            "deep update_wl used an unexpected number of list reads: " +
                std::to_string(update_reads));
    require(update_writes == 2,
            "deep update_wl did not use exactly two Data point writes: " +
                std::to_string(update_writes));

    std::vector<Integer> three_wl = solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, 3, PUBLIC));
    std::vector<Integer> four_wl = solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, 4, PUBLIC));
    bool found_three = false;
    bool found_four = false;
    for (const auto &entry : three_wl)
        found_three = found_three | (reveal_32(entry) == -5);
    for (const auto &entry : four_wl)
        found_four = found_four | (reveal_32(entry) == -5);
    require(found_three, "First replacement watcher was not added");
    require(!found_four, "update_wl added more than one replacement watcher");
}

void test_resolution_uip_beta_and_activity(int party)
{
    auto solver = make_solver(party);
    solver->big_v->cur_dl = Integer(DL_SIZE_BIT, 2, PUBLIC);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC), Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 2, PUBLIC), Integer(DL_SIZE_BIT, 2, PUBLIC));
    solver->big_v->set_unit_literal(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                                    Integer(ANTE_SIZE_BIT, 1, PUBLIC),
                                    Integer(DL_SIZE_BIT, 2, PUBLIC));

    Clause cur(std::vector<int>{-2, -3});
    Clause ante(std::vector<int>{3, -2});
    Clause resolved = solver->resolve_clause(cur, ante, Integer(VAR_SIZE_BIT, 3, PUBLIC), Bit(true, PUBLIC));
    require(reveal_32(resolved.literals[0]) == -2, "resolve_clause kept the pivot or missed the survivor");
    require(reveal_32(resolved.literals[1]) == 0, "resolve_clause did not remove duplicates");

    Clause uip_clause(std::vector<int>{-1, -2});
    require(solver->if_UIP(uip_clause).reveal(), "UIP clause was not recognized");
    Clause non_uip_clause(std::vector<int>{-2, -3});
    require(!solver->if_UIP(non_uip_clause).reveal(), "Non-UIP clause was recognized as UIP");

    Integer alpha(VAR_SIZE_BIT, 0, PUBLIC);
    Integer beta(DL_SIZE_BIT, 0, PUBLIC);
    solver->compute_asserting_and_beta(uip_clause, alpha, beta, Bit(true, PUBLIC));
    require(reveal_32(alpha) == -2, "Asserting literal alpha mismatch");
    require(reveal_32(beta) == 1, "Backtrack level beta mismatch");

    solver->activities[1] = Integer(ACTIVITY_SIZE_BIT, 100, PUBLIC);
    solver->decay_activities(Bit(true, PUBLIC));
    require(reveal_32(solver->activities[1]) == 95, "Activity decay mismatch");
    solver->decay_activities(Bit(false, PUBLIC));
    require(reveal_32(solver->activities[1]) == 95, "Disabled activity decay changed score");
}

void test_restart_requeues_root_units(int party)
{
    auto solver = make_solver_from("./test/cnfs/unit_pair.cnf", party);
    solver->handle_unit_clause(Bit(true, PUBLIC));
    solver->big_v->cur_dl = Integer(DL_SIZE_BIT, 1, PUBLIC);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 3, PUBLIC), Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver->restart();

    require(reveal_32(solver->big_v->cur_dl) == 0, "Restart did not return to decision level 0");
    Integer ass3;
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, 3, PUBLIC), ass3);
    require(reveal_32(ass3, true) == reveal_32(solver->big_v->not_assigned, true), "Restart did not clear higher-level decision");
    require(reveal_32(solver->big_u[1], true) == 1, "Restart did not requeue root unit 1");
    require(reveal_32(solver->big_u[2], true) == 1, "Restart did not requeue root unit 2");
}

void test_conflict_pipeline(int party)
{
    auto solver = make_solver(party);

    Clause antecedent_clause(std::vector<int>{3, -2});
    Clause conflict_clause(std::vector<int>{-2, -3});
    Integer ante_idx(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC);
    Integer conflict_idx(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC);
    solver->cl->insert_clause(ante_idx, antecedent_clause, Bit(true, PUBLIC));
    solver->cl->insert_clause(conflict_idx, conflict_clause, Bit(true, PUBLIC));

    solver->big_v->cur_dl = Integer(DL_SIZE_BIT, 2, PUBLIC);
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC), Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver->big_v->set_decision(Integer(VAR_SIZE_BIT, 2, PUBLIC), Integer(DL_SIZE_BIT, 2, PUBLIC));
    Integer ante_for_three(ANTE_SIZE_BIT, reveal_32(ante_idx), PUBLIC);
    solver->big_v->set_unit_literal(Integer(VAR_SIZE_BIT, 3, PUBLIC), ante_for_three, Integer(DL_SIZE_BIT, 2, PUBLIC), Bit(true, PUBLIC));

    solver->w_idx = conflict_idx;
    const std::uint64_t phi_writes_before = solver->cl->logical_phi_clause_writes;
    const std::uint64_t con_writes_before = solver->cl->logical_con_clause_writes;
    auto res = solver->conflict_analysis(solver->w_idx, Bit(true, PUBLIC));
    require(solver->cl->logical_phi_clause_writes - phi_writes_before == 0,
            "Learned clause triggered a dummy original-store write");
    require(solver->cl->logical_con_clause_writes - con_writes_before == 1,
            "Learned clause did not perform exactly one logical learned-store write");
    require(res.valid.reveal(), "Conflict analysis did not produce a valid clause");
    require(reveal_32(res.beta) == 0, "Unexpected beta from conflict analysis");
    const std::uint64_t phi_reads_before = solver->cl->logical_phi_clause_reads;
    const std::uint64_t con_reads_before = solver->cl->logical_con_clause_reads;
    solver->backtrack(res.beta, res.valid);
    solver->backtrack_aftermath(res.valid, res.learned_clause_idx,
                                res.learned_clause);
    require(solver->cl->logical_phi_clause_reads - phi_reads_before == 0 &&
                solver->cl->logical_con_clause_reads - con_reads_before == 0,
            "Learned-clause watch construction reread both clause stores");

    require(reveal_32(solver->asserting_literal) == -2, "Asserting literal mismatch");
    Integer ass2;
    Integer dl2;
    solver->big_v->get_assignment(Integer(VAR_SIZE_BIT, 2, PUBLIC), ass2);
    solver->big_v->get_decision_level(Integer(VAR_SIZE_BIT, 2, PUBLIC), dl2);
    require(reveal_32(ass2, true) == reveal_32(solver->big_v->false_assigned, true), "Asserting assignment not applied");
    require(reveal_32(dl2) == reveal_32(solver->big_v->cur_dl), "Decision level not updated after backtrack");

    Clause learned;
    solver->cl->get_clause(res.learned_clause_idx, learned);
    require(reveal_32(learned.literals[0]) == -2, "Learned clause content mismatch");

    std::vector<Integer> wl_vec = solver->wl->get_watchlist_clauses(Integer(VAR_SIZE_BIT, -2, PUBLIC));
    bool found = false;
    for (const auto &entry : wl_vec)
    {
        found = found | (reveal_32(entry) == reveal_32(res.learned_clause_idx));
    }
    require(found, "Learned clause not added to watchlist");
}

void setup_direct_three_literal_conflict(CDCL &solver, int conflict_idx = 2)
{
    Clause conflict_clause(std::vector<int>{-1, -2, -3});
    solver.cl->insert_clause(Integer(CLAUSE_IDX_SIZE_IN_WL, conflict_idx, PUBLIC),
                             conflict_clause, Bit(true, PUBLIC));

    solver.big_v->cur_dl = Integer(DL_SIZE_BIT, 2, PUBLIC);
    solver.big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                               Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver.big_v->set_unit_literal(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                                   Integer(ANTE_SIZE_BIT, 1, PUBLIC),
                                   Integer(DL_SIZE_BIT, 1, PUBLIC), Bit(true, PUBLIC));
    solver.big_v->set_decision(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                               Integer(DL_SIZE_BIT, 2, PUBLIC));
}

void require_clause_is_zero(const Clause &clause, const std::string &message)
{
    for (const auto &literal : clause.literals)
    {
        require(reveal_32(literal) == 0, message);
    }
}

void require_clause_is_nonzero(const Clause &clause, const std::string &message)
{
    bool found = false;
    for (const auto &literal : clause.literals)
    {
        found = found || reveal_32(literal) != 0;
    }
    require(found, message);
}

void setup_two_round_uip_conflict(CDCL &solver)
{
    Clause antecedent_clause(std::vector<int>{3, -2});
    Clause conflict_clause(std::vector<int>{-2, -3});
    solver.cl->insert_clause(Integer(CLAUSE_IDX_SIZE_IN_WL, 1, PUBLIC),
                             antecedent_clause, Bit(true, PUBLIC));
    solver.cl->insert_clause(Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC),
                             conflict_clause, Bit(true, PUBLIC));

    solver.big_v->cur_dl = Integer(DL_SIZE_BIT, 2, PUBLIC);
    solver.big_v->set_decision(Integer(VAR_SIZE_BIT, 1, PUBLIC),
                               Integer(DL_SIZE_BIT, 1, PUBLIC));
    solver.big_v->set_decision(Integer(VAR_SIZE_BIT, 2, PUBLIC),
                               Integer(DL_SIZE_BIT, 2, PUBLIC));
    solver.big_v->set_unit_literal(Integer(VAR_SIZE_BIT, 3, PUBLIC),
                                   Integer(ANTE_SIZE_BIT, 1, PUBLIC),
                                   Integer(DL_SIZE_BIT, 2, PUBLIC), Bit(true, PUBLIC));
}

void require_dense_unique_live_orders(CDCL &solver, const std::string &label)
{
    const int live_count = reveal_32(solver.big_v->live_assignment_count());
    std::vector<bool> order_seen(static_cast<std::size_t>(live_count), false);
    int assigned_count = 0;
    const int not_assigned = reveal_32(solver.big_v->not_assigned, true);

    for (int var = 1; var <= VAR_NUM; ++var)
    {
        Integer assignment;
        Integer order;
        solver.big_v->get_assignment(var, assignment);
        solver.big_v->get_order_counter(var, order);
        if (reveal_32(assignment, true) == not_assigned)
        {
            continue;
        }

        const int revealed_order = reveal_32(order);
        require(revealed_order >= 0 && revealed_order < live_count,
                label + " has a live order outside [0, live_count): var=" +
                    std::to_string(var) + " order=" +
                    std::to_string(revealed_order) + " live=" +
                    std::to_string(live_count));
        require(!order_seen[static_cast<std::size_t>(revealed_order)],
                label + " has duplicate live assignment order " +
                    std::to_string(revealed_order));
        order_seen[static_cast<std::size_t>(revealed_order)] = true;
        assigned_count += 1;
    }

    require(assigned_count == live_count,
            label + " live count disagrees with assigned records: assigned=" +
                std::to_string(assigned_count) + " live=" +
                std::to_string(live_count));
    for (int order = 0; order < live_count; ++order)
    {
        require(order_seen[static_cast<std::size_t>(order)],
                label + " has a hole at live assignment order " +
                    std::to_string(order));
    }
}

void test_solver_live_order_invariant(int party)
{
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);

    solver->add_implication(Integer(VAR_SIZE_BIT, 1, ALICE),
                            Integer(ANTE_SIZE_BIT, 1, PUBLIC),
                            Integer(DL_SIZE_BIT, 0, PUBLIC),
                            Bit(true, PUBLIC));
    require_dense_unique_live_orders(*solver, "root implication");

    Integer first_decision = solver->make_decision(Bit(true, PUBLIC), false);
    require(reveal_32(first_decision) != 0,
            "Live-order fixture failed to make its first decision");
    solver->add_implication(Integer(VAR_SIZE_BIT, 3, ALICE),
                            Integer(ANTE_SIZE_BIT, 2, PUBLIC),
                            solver->big_v->cur_dl(), Bit(true, PUBLIC));
    require_dense_unique_live_orders(*solver, "decision plus implication");

    const int live_before_duplicate =
        reveal_32(solver->big_v->live_assignment_count());
    solver->add_implication(Integer(VAR_SIZE_BIT, -3, ALICE),
                            Integer(ANTE_SIZE_BIT, 3, PUBLIC),
                            solver->big_v->cur_dl(), Bit(true, PUBLIC));
    require(reveal_32(solver->big_v->live_assignment_count()) ==
                live_before_duplicate,
            "Rejected duplicate implication advanced the live order counter");
    require_dense_unique_live_orders(*solver, "rejected duplicate implication");

    Integer second_decision = solver->make_decision(Bit(true, PUBLIC), false);
    require(reveal_32(second_decision) != 0,
            "Live-order fixture failed to make its second decision");
    require_dense_unique_live_orders(*solver, "second decision");

    solver->backtrack(Integer(DL_SIZE_BIT, 1, PUBLIC), Bit(true, PUBLIC));
    require_dense_unique_live_orders(*solver, "non-chronological backtrack");

    Integer replacement_decision = solver->make_decision(Bit(true, PUBLIC), false);
    require(reveal_32(replacement_decision) != 0,
            "Live-order fixture failed to make its replacement decision");
    require_dense_unique_live_orders(*solver, "post-backtrack reassignment");
}

void test_conflict_indexed_candidates(int party)
{
    auto solver = make_solver_from("./test/cnfs/depth20.cnf", party,
                                   2, 2, 20, 8);
    const int order_bits = solver->big_v->order_ctr_bitlen();
    std::vector<Bit> seen(VAR_NUM + 1, Bit(false, PUBLIC));
    std::vector<Bit> touched(VAR_NUM + 1, Bit(false, PUBLIC));
    std::vector<Bit> candidate_active(VAR_NUM + 1, Bit(false, PUBLIC));
    std::vector<Integer> candidate_order(
        VAR_NUM + 1, Integer(order_bits, 0, PUBLIC));

    seen[1] = Bit(true, ALICE);
    touched[1] = Bit(true, ALICE);
    candidate_active[1] = Bit(true, ALICE);
    candidate_order[1] = Integer(order_bits, 0, ALICE);
    seen[2] = Bit(true, ALICE);
    touched[2] = Bit(true, ALICE);
    candidate_active[2] = Bit(true, ALICE);
    candidate_order[2] = Integer(order_bits, 7, ALICE);
    candidate_order[3] = Integer(order_bits, 31, ALICE); // stale and inactive
    seen[4] = Bit(true, ALICE);
    touched[4] = Bit(true, ALICE);
    candidate_active[4] = Bit(true, ALICE);
    candidate_order[4] = Integer(order_bits, 3, ALICE);

    auto choice = solver->select_indexed_candidate(
        candidate_active, candidate_order, Bit(true, PUBLIC));
    require(choice.found.reveal() && reveal_32(choice.var) == 2 &&
                reveal_32(choice.order) == 7,
            "Indexed candidate selection did not choose the greatest active order");

    solver->clear_indexed_candidate(
        seen, candidate_active, choice.var, choice.found);
    require(!seen[2].reveal() && !candidate_active[2].reveal() &&
                touched[2].reveal() && reveal_32(candidate_order[2]) == 7,
            "Indexed pivot clear changed touched/order or retained active/seen");

    Bit readded = solver->mark_seen_touched_and_candidate(
        seen, touched, candidate_active, candidate_order,
        Integer(VAR_SIZE_BIT, 2, ALICE), Integer(order_bits, 8, ALICE),
        Bit(true, PUBLIC), Bit(true, PUBLIC));
    require(readded.reveal() && seen[2].reveal() && touched[2].reveal() &&
                candidate_active[2].reveal() &&
                reveal_32(candidate_order[2]) == 8,
            "Indexed candidate re-entry did not overwrite stale order state");
    auto readded_choice = solver->select_indexed_candidate(
        candidate_active, candidate_order, Bit(true, PUBLIC));
    require(readded_choice.found.reveal() &&
                reveal_32(readded_choice.var) == 2 &&
                reveal_32(readded_choice.order) == 8,
            "Re-added indexed candidate was not selected by its new order");

    auto disabled_choice = solver->select_indexed_candidate(
        candidate_active, candidate_order, Bit(false, PUBLIC));
    require(!disabled_choice.found.reveal() &&
                reveal_32(disabled_choice.var) == 0,
            "Disabled indexed candidate selection returned a real candidate");
    solver->clear_indexed_candidate(
        seen, candidate_active, readded_choice.var, Bit(false, PUBLIC));
    Bit disabled_mark = solver->mark_seen_touched_and_candidate(
        seen, touched, candidate_active, candidate_order,
        Integer(VAR_SIZE_BIT, 5, ALICE), Integer(order_bits, 9, ALICE),
        Bit(false, PUBLIC), Bit(true, PUBLIC));
    require(!disabled_mark.reveal() && seen[2].reveal() &&
                candidate_active[2].reveal() && !seen[5].reveal() &&
                !touched[5].reveal() && !candidate_active[5].reveal(),
            "Disabled indexed candidate operations changed workspace state");

    // Sequence oracle: compare the packed representation with the
    // public-position representation through multiple candidates, a
    // duplicate, removal, re-entry with a new order, and an order-zero tail.
    {
        std::vector<Bit> legacy_seen(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Integer> legacy_touched(
            VAR_NUM + 1, Integer(VAR_SIZE_BIT, 0, PUBLIC));
        std::vector<Integer> legacy_vars(
            VAR_NUM + 1, Integer(VAR_SIZE_BIT, 0, PUBLIC));
        std::vector<Integer> legacy_orders(
            VAR_NUM + 1, Integer(order_bits, 0, PUBLIC));
        std::vector<Bit> indexed_seen(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> indexed_touched(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> indexed_active(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Integer> indexed_orders(
            VAR_NUM + 1, Integer(order_bits, 0, PUBLIC));
        Integer legacy_path(VAR_SIZE_BIT, 0, PUBLIC);
        Integer indexed_path(VAR_SIZE_BIT, 0, PUBLIC);

        auto mark_both = [&](int var_value, int order_value)
        {
            Integer var(VAR_SIZE_BIT, var_value, ALICE);
            Integer order(order_bits, order_value, ALICE);
            Bit legacy_new = !legacy_read_seen(legacy_seen, var);
            legacy_write_seen(
                legacy_seen, var, Bit(true, PUBLIC), legacy_new);
            legacy_add_unique_integer(
                legacy_touched, var, legacy_new);
            legacy_add_candidate(
                legacy_vars, legacy_orders, var, order, legacy_new);
            Bit indexed_new = solver->mark_seen_touched_and_candidate(
                indexed_seen, indexed_touched, indexed_active,
                indexed_orders, var, order, Bit(true, PUBLIC),
                Bit(true, PUBLIC));
            legacy_path = legacy_path + If(
                legacy_new, Integer(VAR_SIZE_BIT, 1, PUBLIC),
                Integer(VAR_SIZE_BIT, 0, PUBLIC));
            indexed_path = indexed_path + If(
                indexed_new, Integer(VAR_SIZE_BIT, 1, PUBLIC),
                Integer(VAR_SIZE_BIT, 0, PUBLIC));
            require(legacy_new.reveal() == indexed_new.reveal(),
                    "Packed/indexed candidate mark decisions diverged");
        };

        auto select_and_clear_both = [&](int expected_var,
                                         int expected_order)
        {
            LegacyCandidateChoice legacy_choice = legacy_select_candidate(
                legacy_vars, legacy_orders, Bit(true, PUBLIC));
            auto indexed_choice = solver->select_indexed_candidate(
                indexed_active, indexed_orders, Bit(true, PUBLIC));
            require(legacy_choice.found.reveal() &&
                        indexed_choice.found.reveal() &&
                        reveal_32(legacy_choice.var) == expected_var &&
                        reveal_32(indexed_choice.var) == expected_var &&
                        reveal_32(legacy_choice.order) == expected_order &&
                        reveal_32(indexed_choice.order) == expected_order,
                    "Packed/indexed candidate selection sequence diverged");
            legacy_remove_candidate(
                legacy_vars, legacy_orders, legacy_choice.slot,
                legacy_choice.found);
            legacy_write_seen(
                legacy_seen, legacy_choice.var, Bit(false, PUBLIC),
                legacy_choice.found);
            solver->clear_indexed_candidate(
                indexed_seen, indexed_active, indexed_choice.var,
                indexed_choice.found);
            legacy_path = legacy_path - Integer(VAR_SIZE_BIT, 1, PUBLIC);
            indexed_path = indexed_path - Integer(VAR_SIZE_BIT, 1, PUBLIC);

            int active_count = 0;
            for (int var = 1; var <= VAR_NUM; ++var)
            {
                active_count += indexed_active[var].reveal() ? 1 : 0;
            }
            require(reveal_32(legacy_path) == reveal_32(indexed_path) &&
                        reveal_32(indexed_path) == active_count,
                    "Candidate path count diverged from active cardinality");
        };

        mark_both(2, 0);
        mark_both(5, 4);
        mark_both(2, 8); // duplicate: the original order must remain zero
        mark_both(3, 2);
        select_and_clear_both(5, 4);
        mark_both(5, 5); // legitimate re-entry overwrites stale order
        select_and_clear_both(5, 5);
        select_and_clear_both(3, 2);
        select_and_clear_both(2, 0);

        require(reveal_32(indexed_path) == 0 &&
                    indexed_touched[2].reveal() &&
                    indexed_touched[3].reveal() &&
                    indexed_touched[5].reveal(),
                "Candidate sequence lost sticky touched state");
    }

    std::uint64_t legacy_gates = 0;
    std::uint64_t indexed_gates = 0;
    {
        std::vector<Bit> legacy_seen(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Integer> legacy_touched(
            VAR_NUM + 1, Integer(VAR_SIZE_BIT, 0, PUBLIC));
        std::vector<Integer> legacy_vars(
            VAR_NUM + 1, Integer(VAR_SIZE_BIT, 0, PUBLIC));
        std::vector<Integer> legacy_orders(
            VAR_NUM + 1, Integer(order_bits, 0, PUBLIC));
        Integer secret_var(VAR_SIZE_BIT, 6, ALICE);
        Integer secret_order(order_bits, 9, ALICE);
        const std::uint64_t before = base->num_and();
        Bit already_seen = legacy_read_seen(legacy_seen, secret_var);
        Bit is_new = !already_seen;
        legacy_write_seen(
            legacy_seen, secret_var, Bit(true, PUBLIC), is_new);
        legacy_add_unique_integer(legacy_touched, secret_var, is_new);
        legacy_add_candidate(
            legacy_vars, legacy_orders, secret_var, secret_order, is_new);
        LegacyCandidateChoice legacy_choice = legacy_select_candidate(
            legacy_vars, legacy_orders, Bit(true, PUBLIC));
        legacy_remove_candidate(
            legacy_vars, legacy_orders, legacy_choice.slot,
            legacy_choice.found);
        legacy_write_seen(
            legacy_seen, legacy_choice.var, Bit(false, PUBLIC),
            legacy_choice.found);
        legacy_gates = base->num_and() - before;
        require(legacy_choice.found.reveal() &&
                    reveal_32(legacy_choice.var) == 6,
                "Legacy candidate gate oracle failed to select its only candidate");
    }

    {
        std::vector<Bit> indexed_seen(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> indexed_touched(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> indexed_active(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Integer> indexed_orders(
            VAR_NUM + 1, Integer(order_bits, 0, PUBLIC));
        Integer secret_var(VAR_SIZE_BIT, 6, ALICE);
        Integer secret_order(order_bits, 9, ALICE);
        const std::uint64_t before = base->num_and();
        Bit is_new = solver->mark_seen_touched_and_candidate(
            indexed_seen, indexed_touched, indexed_active, indexed_orders,
            secret_var, secret_order, Bit(true, PUBLIC), Bit(true, PUBLIC));
        auto indexed_choice = solver->select_indexed_candidate(
            indexed_active, indexed_orders, Bit(true, PUBLIC));
        solver->clear_indexed_candidate(
            indexed_seen, indexed_active, indexed_choice.var,
            indexed_choice.found);
        indexed_gates = base->num_and() - before;
        require(is_new.reveal() && indexed_choice.found.reveal() &&
                    reveal_32(indexed_choice.var) == 6 &&
                    !indexed_seen[6].reveal() &&
                    !indexed_active[6].reveal() &&
                    indexed_touched[6].reveal(),
                "Indexed candidate gate oracle changed workspace semantics");
    }

    if (!g_use_oram_variable_state)
    {
        require(indexed_gates < legacy_gates,
                "Indexed candidate workspace did not beat packed candidate work: legacy=" +
                    std::to_string(legacy_gates) + " indexed=" +
                    std::to_string(indexed_gates));
    }
    if (party == ALICE && !g_use_oram_variable_state)
    {
        std::cout << "CONFLICT_INDEX_STAGE2_GATES legacy=" << legacy_gates
                  << " indexed=" << indexed_gates
                  << " saved=" << (legacy_gates - indexed_gates) << std::endl;
    }
}

void test_conflict_indexed_workspace(int party)
{
    std::uint64_t conflict_gates = 0;
    constexpr std::uint64_t stage1_conflict_gates = 99612;

    {
        auto solver = make_solver(party);
        const int order_bits = solver->big_v->order_ctr_bitlen();
        std::vector<Bit> seen(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> touched(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Bit> candidate_active(VAR_NUM + 1, Bit(false, PUBLIC));
        std::vector<Integer> candidate_order(
            VAR_NUM + 1, Integer(order_bits, 0, PUBLIC));
        Integer secret_two(VAR_SIZE_BIT, 2, ALICE);

        Bit first = solver->mark_seen_touched_and_candidate(
            seen, touched, candidate_active, candidate_order,
            secret_two, Integer(order_bits, 4, ALICE),
            Bit(true, PUBLIC), Bit(true, PUBLIC));
        Bit duplicate = solver->mark_seen_touched_and_candidate(
            seen, touched, candidate_active, candidate_order,
            secret_two, Integer(order_bits, 5, ALICE),
            Bit(true, PUBLIC), Bit(true, PUBLIC));
        require(first.reveal() && !duplicate.reveal(),
                "Conflict mark sweep did not distinguish first and duplicate visits");
        require(seen[2].reveal() && touched[2].reveal() &&
                    candidate_active[2].reveal() &&
                    reveal_32(candidate_order[2]) == 4,
                "Conflict mark sweep did not set its fused workspace state");

        solver->clear_indexed_candidate(
            seen, candidate_active, secret_two, Bit(true, PUBLIC));
        require(!seen[2].reveal() && !candidate_active[2].reveal() &&
                    touched[2].reveal() &&
                    reveal_32(candidate_order[2]) == 4,
                "Pivot clear changed sticky touched/order state");
        Bit revisit = solver->mark_seen_touched_and_candidate(
            seen, touched, candidate_active, candidate_order,
            secret_two, Integer(order_bits, 5, ALICE),
            Bit(true, PUBLIC), Bit(true, PUBLIC));
        require(revisit.reveal() && seen[2].reveal() && touched[2].reveal() &&
                    candidate_active[2].reveal() &&
                    reveal_32(candidate_order[2]) == 5,
                "Conflict mark sweep rejected a legitimate post-pivot revisit");

        Bit inactive = solver->mark_seen_touched_and_candidate(
            seen, touched, candidate_active, candidate_order,
            Integer(VAR_SIZE_BIT, 1, ALICE), Integer(order_bits, 6, ALICE),
            Bit(false, PUBLIC), Bit(true, PUBLIC));
        Bit zero = solver->mark_seen_touched_and_candidate(
            seen, touched, candidate_active, candidate_order,
            Integer(VAR_SIZE_BIT, 0, ALICE), Integer(order_bits, 7, ALICE),
            Bit(true, PUBLIC), Bit(true, PUBLIC));
        require(!inactive.reveal() && !zero.reveal() &&
                    !seen[0].reveal() && !touched[0].reveal() &&
                    !candidate_active[0].reveal() &&
                    !seen[1].reveal() && !touched[1].reveal() &&
                    !candidate_active[1].reveal(),
                "Inactive or zero mark changed the conflict workspace");

        setup_two_round_uip_conflict(*solver);
        const std::uint64_t gates_before = base->num_and();
        auto result = solver->conflict_analysis(
            Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));
        conflict_gates = base->num_and() - gates_before;

        require(result.valid.reveal(),
                "Conflict workspace fixture did not produce a learned clause");
        require(reveal_32(solver->activities[2]) == 95,
                "Learned/touched variable did not receive the production activity update");
        require(reveal_32(solver->activities[3]) == 95,
                "Pivot-only touched variable did not receive the production activity update");
        if (!g_use_oram_variable_state)
        {
            require(conflict_gates < stage1_conflict_gates,
                    "Stage 2 conflict workspace did not reduce the Stage 1 full analysis: stage1=" +
                        std::to_string(stage1_conflict_gates) + " stage2=" +
                        std::to_string(conflict_gates));
        }
    }

    {
        auto capped = make_solver(party);
        setup_two_round_uip_conflict(*capped);
        capped->uip_loop_cap = 1;
        auto result = capped->conflict_analysis(
            Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));
        require(!result.valid.reveal() && result.cap_hit.reveal(),
                "One-round conflict fixture did not report cap rejection");
        require(reveal_32(capped->activities[2]) == 95 &&
                    reveal_32(capped->activities[3]) == 95,
                "Cap-rejected real analysis lost its touched-variable activity update");
    }

    {
        auto inactive = make_solver(party);
        setup_two_round_uip_conflict(*inactive);
        inactive->conflict_analysis(
            Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(false, PUBLIC));
        for (int var = 1; var <= VAR_NUM; ++var)
        {
            require(reveal_32(inactive->activities[var]) == 0,
                    "Inactive conflict analysis changed activity");
        }
    }

    if (party == ALICE && !g_use_oram_variable_state)
    {
        std::cout << "CONFLICT_INDEX_GATES full_stage1="
                  << stage1_conflict_gates
                  << " full_stage2=" << conflict_gates
                  << " saved=" << (stage1_conflict_gates - conflict_gates)
                  << std::endl;
    }
}

void test_exact_learned_length_commits(int party)
{
    auto solver = make_solver_from("./test/cnfs/conflict_simple.cnf", party, 8, 3, 3, 8);
    setup_direct_three_literal_conflict(*solver);

    solver->handle_conflict_w({}, Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));

    require(reveal_32(solver->big_v->cur_dl) == 1,
            "Exact-fit learned clause did not backjump to its beta level");
    require(reveal_32(solver->conflict_ctr, true) == 2,
            "Exact-fit learned clause did not consume exactly one slot");
    Clause learned;
    solver->cl->get_clause(Integer(CON_ORAM_SIZE_BIT, -1, PUBLIC), learned);
    require(reveal_32(learned.literals[0]) == -1 &&
                reveal_32(learned.literals[1]) == -2 &&
                reveal_32(learned.literals[2]) == -3,
            "Exact-fit learned clause was not stored completely");
}

void test_exact_uip_cap_commits(int party)
{
    auto solver = make_solver(party);
    setup_two_round_uip_conflict(*solver);
    solver->uip_loop_cap = 2;

    solver->handle_conflict_w({}, Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));

    require(reveal_32(solver->conflict_ctr, true) == 2,
            "Exact-fit UIP analysis did not consume one learned slot");
    Clause learned;
    solver->cl->get_clause(Integer(CON_ORAM_SIZE_BIT, -1, PUBLIC), learned);
    require_clause_is_nonzero(learned, "Exact-fit UIP analysis did not store its learned clause");
}

void test_public_uip_round_budget_is_tightened_to_variable_count(int party)
{
    auto solver = make_solver(party);
    setup_two_round_uip_conflict(*solver);
    const int public_round_budget = VAR_NUM + 2;
    solver->uip_loop_cap = public_round_budget;

    auto result = solver->conflict_analysis(
        Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));

    require(result.valid.reveal(),
            "UIP analysis with public dummy tail did not produce its learned clause");
    require(solver->last_conflict_analysis_rounds == VAR_NUM,
            "Public UIP work was not tightened to min(configured_uip, n)");
}

void test_exact_learned_count_commits(int party)
{
    auto solver = make_solver_from("./test/cnfs/conflict_simple.cnf", party, 8, 3, 3, 1);
    setup_direct_three_literal_conflict(*solver);

    solver->handle_conflict_w({}, Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));

    require(reveal_32(solver->conflict_ctr, true) == 2,
            "Exact-fit learned-count analysis did not consume the final available slot");
    Clause learned;
    solver->cl->get_clause(Integer(CON_ORAM_SIZE_BIT, -1, PUBLIC), learned);
    require_clause_is_nonzero(learned, "Exact-fit learned-count analysis did not store its learned clause");
}

void test_learned_length_cap_restarts_without_commit(int party)
{
    auto solver = make_solver_from("./test/cnfs/conflict_simple.cnf", party, 8, 3, 2, 8);
    setup_direct_three_literal_conflict(*solver);

    solver->handle_conflict_w({}, Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));

    require(reveal_32(solver->big_v->cur_dl) == 0,
            "Learned-length cap did not restart at decision level zero");
    require(reveal_32(solver->conflict_ctr, true) == 1,
            "Rejected over-length clause consumed a learned slot");
    Clause learned;
    solver->cl->get_clause(Integer(CON_ORAM_SIZE_BIT, -1, PUBLIC), learned);
    require_clause_is_zero(learned, "Rejected over-length clause was stored or truncated");
}

void test_uip_cap_restarts_without_commit(int party)
{
    auto solver = make_solver(party);
    setup_two_round_uip_conflict(*solver);
    solver->uip_loop_cap = 1;

    solver->handle_conflict_w({}, Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));

    require(reveal_32(solver->big_v->cur_dl) == 0,
            "UIP cap did not restart at decision level zero");
    require(reveal_32(solver->conflict_ctr, true) == 1,
            "Rejected UIP-capped analysis consumed a learned slot");
    Clause learned;
    solver->cl->get_clause(Integer(CON_ORAM_SIZE_BIT, -1, PUBLIC), learned);
    require_clause_is_zero(learned, "Rejected UIP-capped analysis stored a partial clause");
}

void test_learned_count_cap_restarts_without_overwrite(int party)
{
    auto solver = make_solver_from("./test/cnfs/conflict_simple.cnf", party, 8, 3, 3, 1);
    Clause sentinel(std::vector<int>{1});
    solver->cl->insert_clause(Integer(CON_ORAM_SIZE_BIT, -1, PUBLIC), sentinel, Bit(true, PUBLIC));
    solver->conflict_ctr = Integer(CON_CLAUSE_IDX_SIZE_BIT, 2, PUBLIC);
    setup_direct_three_literal_conflict(*solver);

    solver->handle_conflict_w({}, Integer(CLAUSE_IDX_SIZE_IN_WL, 2, PUBLIC), Bit(true, PUBLIC));

    require(reveal_32(solver->big_v->cur_dl) == 0,
            "Learned-count cap did not restart at decision level zero");
    require(reveal_32(solver->conflict_ctr, true) == 2,
            "Learned-count cap advanced beyond declared capacity");
    Clause retained;
    solver->cl->get_clause(Integer(CON_ORAM_SIZE_BIT, -1, PUBLIC), retained);
    require(reveal_32(retained.literals[0]) == 1,
            "Learned-count cap overwrote an existing retained clause");
    Clause overflow;
    solver->cl->get_clause(Integer(CON_ORAM_SIZE_BIT, -2, PUBLIC), overflow);
    require_clause_is_zero(overflow, "Learned-count cap wrote outside retained capacity");
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <party:1|2>" << std::endl;
        return 1;
    }
    int party = atoi(argv[1]);
    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--oramV" || arg == "--oram-v")
        {
            g_use_oram_variable_state = true;
        }
        else if (arg == "--no-oramV" || arg == "--no-oram-v")
        {
            g_use_oram_variable_state = false;
        }
        else
        {
            std::cerr << "Usage: " << argv[0] << " <party:1|2> [--oramV|--no-oramV]" << std::endl;
            return 1;
        }
    }
    init_backend(party);
    base->switch_to_gt();

    try
    {
        const char *only_test = std::getenv("PPCDCL_UNIT_ONLY");
        if (only_test != nullptr)
        {
            const std::string selected(only_test);
            if (selected == "variable_state")
                test_variable_state_basic(party);
            else if (selected == "implication_cas")
                test_implication_compare_and_set(party);
            else if (selected == "conflict_index")
                test_conflict_indexed_workspace(party);
            else if (selected == "conflict_candidates")
                test_conflict_indexed_candidates(party);
            else if (selected == "trail_order")
                test_solver_live_order_invariant(party);
            else if (selected == "depth_order")
                test_decision_level_width_and_live_order_counter(party);
            else if (selected == "sat_count")
            {
                test_check_sat_requires_assigned_and_empty_queue(party);
                test_sat_check_precedes_decision_request_at_source_attempt(party);
                test_sat_waits_until_consumed_unit_frame_finishes(party);
            }
            else if (selected == "watch_overflow")
                test_watchlist_overflow_is_fail_stop(party);
            else if (selected == "watch_frame_cache")
                test_watch_frame_cache_coherence(party);
            else if (selected == "paper_capacity_clause")
                test_paper_capacity_learned_clause_roundtrip(party);
            else if (selected == "paper_capacity_index")
                test_paper_capacity_learned_index_boundary(party);
            else if (selected == "physical_clause_address")
                test_multiblock_clause_physical_address_width(party);
            else if (selected == "public_uip_rounds")
                test_public_uip_round_budget_is_tightened_to_variable_count(party);
            else if (selected == "positive_default_phase")
            {
                test_vsids_skips_assigned_ties(party);
                test_vsids_uses_exact_activity_and_positive_default_phase(party);
            }
            else
                throw std::runtime_error("Unknown PPCDCL_UNIT_ONLY value: " + selected);

            if (party == ALICE)
                std::cout << "Selected ppCDCL unit test passed: " << selected << std::endl;
            delete base;
            return 0;
        }

        const char *only_cap_test = std::getenv("PPCDCL_UNIT_ONLY_CAP");
        if (only_cap_test != nullptr)
        {
            const std::string selected(only_cap_test);
            if (selected == "exact")
            {
                test_exact_learned_length_commits(party);
                test_exact_uip_cap_commits(party);
                test_exact_learned_count_commits(party);
            }
            else if (selected == "exact_length")
            {
                test_exact_learned_length_commits(party);
            }
            else if (selected == "exact_uip")
            {
                test_exact_uip_cap_commits(party);
            }
            else if (selected == "exact_count")
            {
                test_exact_learned_count_commits(party);
            }
            else if (selected == "length")
            {
                test_learned_length_cap_restarts_without_commit(party);
            }
            else if (selected == "uip")
            {
                test_uip_cap_restarts_without_commit(party);
            }
            else if (selected == "count")
            {
                test_learned_count_cap_restarts_without_overwrite(party);
            }
            else
            {
                throw std::runtime_error("Unknown PPCDCL_UNIT_ONLY_CAP value: " + selected);
            }
            if (party == ALICE)
            {
                std::cout << "Selected ppCDCL cap test passed: " << selected << std::endl;
            }
            delete base;
            return 0;
        }

        test_variable_state_basic(party);
        test_decision_level_width_and_live_order_counter(party);
        test_solver_live_order_invariant(party);
        test_linear_variable_state_update_reuses_secret_route(party);
        test_implication_compare_and_set(party);
        test_watchlist_roundtrip(party);
        test_watchlist_overflow_is_fail_stop(party);
        test_watch_frame_cache_coherence(party);
        test_indexed_watch_state_install_move_and_overflow(party);
        test_indexed_learned_key_and_install_rollback(party);
        test_watch_backend_public_geometry_policy(party);
        test_watch_state_distinct_duplicate_and_block_fallback(party);
        test_clause_storage(party);
        test_initialization_writes_only_original_clause_store(party);
        test_exact_public_storage_geometry();
        test_packed_neighbor_roundtrip(party);
        test_paper_capacity_learned_clause_roundtrip(party);
        test_search_two_states(party);
        test_shortest_non_falsified_heuristic(party);
        test_unit_clause_parsing_and_handling(party);
        test_contradictory_unit_detection(party);
        test_check_sat_requires_assigned_and_empty_queue(party);
        test_if_all_assigned_uses_live_assignment_count(party);
        test_sat_check_precedes_decision_request_at_source_attempt(party);
        test_sat_waits_until_consumed_unit_frame_finishes(party);
        test_sat_check_uses_public_source_attempt_schedule(party);
        test_make_decision_respects_flag(party);
        test_make_decision_omits_impossible_terminal_check(party);
        test_make_decision_rejects_zero_when_all_assigned(party);
        test_vsids_skips_assigned_ties(party);
        test_vsids_uses_exact_activity_and_positive_default_phase(party);
        test_get_w_from_big_u_respects_flag(party);
        test_get_w_from_big_u_reuses_literal_classification(party);
        test_get_w_from_new_decision_consumes_decision_literal(party);
        test_get_w_from_new_decision_avoids_big_u_round_trip(party);
        test_decision_delay_waits_for_scheduled_slot(party);
        test_scan_cursor_advances_only_when_active(party);
        test_repeated_giant_step_uses_conservative_source_work(party);
        test_backtrack_levels(party);
        test_construct_wl_and_update_wl(party);
        test_learned_clause_keeps_two_distinct_watchers(party);
        test_indexed_compatibility_update_recovers_private_source_slot(party);
        test_update_wl_adds_one_replacement(party);
        test_resolution_uip_beta_and_activity(party);
        test_restart_requeues_root_units(party);
        test_conflict_pipeline(party);
        test_conflict_indexed_workspace(party);
        test_conflict_indexed_candidates(party);
        test_exact_learned_length_commits(party);
        test_exact_uip_cap_commits(party);
        test_public_uip_round_budget_is_tightened_to_variable_count(party);
        test_exact_learned_count_commits(party);
        test_learned_length_cap_restarts_without_commit(party);
        test_uip_cap_restarts_without_commit(party);
        test_learned_count_cap_restarts_without_overwrite(party);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "Test failure: " << ex.what() << std::endl;
        delete base;
        return 1;
    }

    if (party == ALICE)
    {
        std::cout << "All ppCDCL unit tests passed" << std::endl;
    }

    delete base;
    return 0;
}
