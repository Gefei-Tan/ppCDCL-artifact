#ifndef PPCDCL_PUBLIC_SCHEDULE_H
#define PPCDCL_PUBLIC_SCHEDULE_H

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Public scheduling policy shared by the secure solver, plaintext estimator,
// and tests. Time is one-based: the executed steps are t=1,...,T.
namespace ppcdcl
{
namespace policy
{

constexpr std::uint64_t kDefaultLubyUnit = 256;

// Decision and conflict blocks are scheduled at t == 1 (mod period).
// Step zero and a zero period are treated as unscheduled.
inline bool is_modulo_one_slot(std::uint64_t one_based_step, std::uint64_t period)
{
    return one_based_step != 0 && period != 0 && (one_based_step - 1) % period == 0;
}

// Count slots in the inclusive public range t=1,...,executed_steps.
inline std::uint64_t count_modulo_one_slots(std::uint64_t executed_steps, std::uint64_t period)
{
    if (executed_steps == 0 || period == 0)
        return 0;
    return 1 + (executed_steps - 1) / period;
}

// A W-step propagation frame closes only at the public boundary t == 0 (mod W).
inline bool is_watchlist_frame_boundary(std::uint64_t one_based_step, std::uint64_t watchlist_width)
{
    return one_based_step != 0 && watchlist_width != 0 && one_based_step % watchlist_width == 0;
}

inline std::uint64_t count_watchlist_frame_boundaries(std::uint64_t executed_steps,
                                                       std::uint64_t watchlist_width)
{
    if (watchlist_width == 0)
        return 0;
    return executed_steps / watchlist_width;
}

// Public per-step flags, indexed by t-1.
struct PossibleSourceWorkSchedule
{
    std::vector<unsigned char> source_attempts;
    std::vector<unsigned char> source_refreshes;
};

// A resumed giant-step invocation may inherit a partially consumed secret
// watchlist cursor. Its next source boundary cannot be derived from a fresh
// public t=1 schedule, so every step is marked as a possible source attempt and
// refresh.
inline PossibleSourceWorkSchedule build_conservative_source_work_schedule(
    std::uint64_t executed_steps)
{
    PossibleSourceWorkSchedule schedule;
    schedule.source_attempts.assign(executed_steps, 1);
    schedule.source_refreshes.assign(executed_steps, 1);
    return schedule;
}

// Build a public superset of the steps on which the solver may need to find a
// new propagation source and refresh its cached watchlist. Immediate-decision
// semantics make the actual frame starts secret, so the schedule must cover every
// start that is possible for either party's private input:
//
//   * the initial source at t=1;
//   * a pending decision consumed at a public decision slot;
//   * the step after a public conflict slot, because conflict handling may
//     secretly backtrack and invalidate the current frame; and
//   * the successor of any possible W-step frame.
//
// The pending flag in the loop is a public possibility analysis. It does not
// depend on the solver's secret pending-decision bit and therefore leaks no
// execution state.
inline PossibleSourceWorkSchedule build_possible_source_work_schedule(
    std::uint64_t executed_steps,
    std::uint64_t watchlist_width,
    std::uint64_t decision_period,
    std::uint64_t conflict_period)
{
    if (watchlist_width == 0 || decision_period == 0 || conflict_period == 0)
        throw std::invalid_argument("source-refresh schedule periods must be positive");

    PossibleSourceWorkSchedule schedule;
    schedule.source_attempts.assign(executed_steps, 0);
    schedule.source_refreshes.assign(executed_steps, 0);
    bool pending_decision_possible = false;
    for (std::uint64_t t = 1; t <= executed_steps; ++t)
    {
        const bool decision_slot = is_modulo_one_slot(t, decision_period);
        const bool after_conflict_slot = t > 1 && is_modulo_one_slot(t - 1, conflict_period);
        const bool after_possible_frame =
            t > watchlist_width && schedule.source_refreshes[t - watchlist_width - 1] != 0;
        const bool source_attempt_possible = t == 1 || after_conflict_slot || after_possible_frame;
        const bool pending_decision_activation = decision_slot && pending_decision_possible;
        const bool source_refresh_possible = source_attempt_possible || pending_decision_activation;
        schedule.source_attempts[t - 1] = static_cast<unsigned char>(source_attempt_possible);
        schedule.source_refreshes[t - 1] = static_cast<unsigned char>(source_refresh_possible);

        if (decision_slot)
            pending_decision_possible = false;
        else if (source_attempt_possible)
            pending_decision_possible = true;
    }
    return schedule;
}

inline std::vector<unsigned char> build_possible_source_refresh_schedule(
    std::uint64_t executed_steps,
    std::uint64_t watchlist_width,
    std::uint64_t decision_period,
    std::uint64_t conflict_period)
{
    return build_possible_source_work_schedule(
               executed_steps, watchlist_width, decision_period, conflict_period)
        .source_refreshes;
}

// Return the standard Luby sequence at a one-based index:
//   1, 1, 2, 1, 1, 2, 4, 1, ...
// This integer-only reduction is exact over the full uint64_t index domain.
inline std::uint64_t luby_value(std::uint64_t one_based_index)
{
    if (one_based_index == 0)
        throw std::invalid_argument("Luby sequence index must be one-based");

    std::uint64_t index = one_based_index;
    while ((index & (index + 1)) != 0)
    {
        std::uint64_t highest_power_of_two = 1;
        for (std::uint64_t remaining = index; remaining > 1; remaining >>= 1)
            highest_power_of_two <<= 1;

        // highest_power_of_two - 1 is the largest complete Luby prefix
        // strictly shorter than this index. Reduce into that repeated prefix.
        index -= highest_power_of_two - 1;
    }

    // For a Mersenne index 2^k-1, the sequence value is 2^(k-1).
    // This form also avoids overflowing when index == UINT64_MAX.
    return (index >> 1) + 1;
}

// Return the number of scheduled public conflict slots before the next restart.
// The default unit is 256. Throws if the interval does not fit in uint64_t.
inline std::uint64_t luby_interval(std::uint64_t one_based_index,
                                   std::uint64_t unit = kDefaultLubyUnit)
{
    if (unit == 0)
        throw std::invalid_argument("Luby restart unit must be positive");

    const std::uint64_t value = luby_value(one_based_index);
    if (value > std::numeric_limits<std::uint64_t>::max() / unit)
        throw std::overflow_error("Luby restart interval exceeds uint64_t");
    return value * unit;
}

// Count the periodic restarts a fresh solver performs after a given number of
// public conflict slots. It iterates over whole Luby intervals rather than over
// individual slots, so it stays cheap for long schedules.
inline std::uint64_t count_luby_restarts(
    std::uint64_t conflict_slots,
    std::uint64_t unit = kDefaultLubyUnit)
{
    std::uint64_t restart_index = 1;
    std::uint64_t restart_count = 0;
    while (conflict_slots != 0)
    {
        const std::uint64_t interval = luby_interval(restart_index, unit);
        if (conflict_slots < interval)
            break;
        conflict_slots -= interval;
        restart_count += 1;
        if (restart_index == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Luby restart index exceeds uint64_t");
        restart_index += 1;
    }
    return restart_count;
}

} // namespace policy
} // namespace ppcdcl

#endif // PPCDCL_PUBLIC_SCHEDULE_H
