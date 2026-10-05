#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "src/public_schedule.h"

namespace
{

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void test_modulo_one_slots()
{
    using ppcdcl::policy::count_modulo_one_slots;
    using ppcdcl::policy::is_modulo_one_slot;

    require(!is_modulo_one_slot(0, 4), "step zero is outside the one-based schedule");
    require(is_modulo_one_slot(1, 4), "t=1 must be the first modulo-one slot");
    require(!is_modulo_one_slot(4, 4), "t=4 is a legacy modulo-zero slot, not a paper slot");
    require(is_modulo_one_slot(5, 4), "t=5 must be the second modulo-one slot");
    require(is_modulo_one_slot(9, 4), "t=9 must be the third modulo-one slot");
    require(!is_modulo_one_slot(1, 0), "a zero period must not schedule a slot");

    require(count_modulo_one_slots(0, 4) == 0, "an empty execution has no slots");
    require(count_modulo_one_slots(1, 4) == 1, "one executed step includes t=1");
    require(count_modulo_one_slots(4, 4) == 1, "steps 1..4 include only t=1");
    require(count_modulo_one_slots(5, 4) == 2, "steps 1..5 include t=1 and t=5");
    require(count_modulo_one_slots(9, 4) == 3, "steps 1..9 include three slots");
    require(count_modulo_one_slots(100, 0) == 0, "a zero period must count no slots");

    for (std::uint64_t step = 1; step <= 32; ++step)
        require(is_modulo_one_slot(step, 1), "period one must schedule every positive step");
    require(count_modulo_one_slots(32, 1) == 32, "period-one count must equal executed steps");
}

void test_watchlist_frame_boundaries()
{
    using ppcdcl::policy::count_watchlist_frame_boundaries;
    using ppcdcl::policy::is_watchlist_frame_boundary;

    require(!is_watchlist_frame_boundary(0, 4), "step zero is not a frame boundary");
    require(!is_watchlist_frame_boundary(1, 4), "t=1 is inside the first frame");
    require(is_watchlist_frame_boundary(4, 4), "t=W must close the first frame");
    require(is_watchlist_frame_boundary(8, 4), "t=2W must close the second frame");
    require(!is_watchlist_frame_boundary(8, 0), "a zero frame width has no boundary");

    require(count_watchlist_frame_boundaries(0, 4) == 0, "an empty execution has no boundaries");
    require(count_watchlist_frame_boundaries(3, 4) == 0, "a partial frame has no boundary");
    require(count_watchlist_frame_boundaries(4, 4) == 1, "one complete frame has one boundary");
    require(count_watchlist_frame_boundaries(9, 4) == 2, "nine steps contain two W=4 boundaries");
    require(count_watchlist_frame_boundaries(9, 0) == 0, "a zero width must count no boundaries");
}

void test_possible_source_refresh_schedule()
{
    using ppcdcl::policy::build_possible_source_work_schedule;
    using ppcdcl::policy::build_possible_source_refresh_schedule;

    // With W=4, decision sources may start at 1,7,13,19 and post-conflict
    // sources at 2,12.  Every such source can start another frame four steps
    // later.  Only 3,4,8 are impossible source-refresh steps in the first
    // twenty steps.
    const std::vector<unsigned char> expected = {
        1, 1, 0, 0, 1, 1, 1, 0, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    require(build_possible_source_refresh_schedule(20, 4, 6, 10) == expected,
            "B schedule must include decision starts, post-conflict starts, and W-step successors");

    // D<W is the case that makes an unconditional reset at every decision
    // slot unsafe.  These are starts, not forced resets: the active W=14
    // frame is still allowed to finish.
    const std::vector<unsigned char> d_less_than_w = {
        1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 0, 0, 0, 1, 1, 0, 0, 0, 0,
        1, 0, 0, 0, 1, 0, 0, 0, 1, 1,
        1, 0, 0, 0, 1, 0, 0, 0, 1, 0,
        1, 0};
    require(build_possible_source_refresh_schedule(42, 14, 10, 100) == d_less_than_w,
            "decision opportunities must not starve a wider watchlist frame");

    const auto flat = build_possible_source_refresh_schedule(60, 10, 50, 50);
    std::uint64_t flat_refresh_steps = 0;
    for (unsigned char possible : flat)
        flat_refresh_steps += possible != 0;
    require(flat_refresh_steps == 12,
            "flat W=10,D=C=50 must expose exactly two possible refresh residues per frame");

    const auto split = build_possible_source_work_schedule(11, 14, 10, 100);
    require(split.source_attempts[1] != 0,
            "the step after a conflict slot must permit Big-U source discovery");
    require(split.source_attempts[10] == 0 && split.source_refreshes[10] != 0,
            "a pending decision activation must refresh without repeating Big-U discovery");
    for (std::size_t i = 0; i < split.source_attempts.size(); ++i)
    {
        require(split.source_attempts[i] == 0 || split.source_refreshes[i] != 0,
                "every source attempt must also permit its resulting watchlist refresh");
    }

    const auto sparse_d_less_than_w =
        build_possible_source_refresh_schedule(100, 100, 10, 1000);
    std::vector<std::uint64_t> sparse_steps;
    for (std::size_t i = 0; i < sparse_d_less_than_w.size(); ++i)
    {
        if (sparse_d_less_than_w[i] != 0)
            sparse_steps.push_back(static_cast<std::uint64_t>(i + 1));
    }
    require(sparse_steps == std::vector<std::uint64_t>({1, 2, 11}),
            "later D<W decision slots must be skipped when no frame can request them");

    const auto uf20_legacy =
        build_possible_source_refresh_schedule(50, 14, 10, 10);
    std::vector<std::uint64_t> uf20_heavy_steps;
    for (std::size_t i = 0; i < uf20_legacy.size(); ++i)
    {
        if (uf20_legacy[i] != 0)
            uf20_heavy_steps.push_back(static_cast<std::uint64_t>(i + 1));
    }
    const std::vector<std::uint64_t> expected_uf20_heavy_steps = {
        1, 2, 11, 12, 15, 16, 21, 22, 25, 26, 29, 30, 31,
        32, 35, 36, 39, 40, 41, 42, 43, 44, 45, 46, 49, 50};
    require(uf20_heavy_steps == expected_uf20_heavy_steps,
            "UF20 legacy schedule must include both decision and post-conflict reachability");

    const auto count_work = [](const ppcdcl::policy::PossibleSourceWorkSchedule &schedule) {
        std::uint64_t attempts = 0;
        std::uint64_t refreshes = 0;
        for (std::size_t i = 0; i < schedule.source_attempts.size(); ++i)
        {
            attempts += schedule.source_attempts[i] != 0;
            refreshes += schedule.source_refreshes[i] != 0;
        }
        return std::make_pair(attempts, refreshes);
    };
    require(count_work(build_possible_source_work_schedule(765, 14, 10, 10)) ==
                std::make_pair<std::uint64_t, std::uint64_t>(735, 741),
            "paper-aligned UF20 full horizon must expose its nearly dense source schedule");
    require(count_work(build_possible_source_work_schedule(2812, 14, 10, 10)) ==
                std::make_pair<std::uint64_t, std::uint64_t>(2782, 2788),
            "long legacy UF20 horizon must not overstate sparse-gate savings");
    require(count_work(build_possible_source_work_schedule(2812, 14, 50, 200)) ==
                std::make_pair<std::uint64_t, std::uint64_t>(2437, 2443),
            "paper-like UF20 horizon must include conflict and frame closure");
    require(count_work(build_possible_source_work_schedule(3721, 10, 50, 200)) ==
                std::make_pair<std::uint64_t, std::uint64_t>(745, 745),
            "flat100-6 horizon must retain the genuinely sparse source schedule");
}

void test_luby_sequence_and_intervals()
{
    using ppcdcl::policy::kDefaultLubyUnit;
    using ppcdcl::policy::count_luby_restarts;
    using ppcdcl::policy::luby_interval;
    using ppcdcl::policy::luby_value;

    const std::array<std::uint64_t, 31> expected = {{
        1, 1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8, 1,
        1, 2, 1, 1, 2, 4, 1, 1, 2, 1, 1, 2, 4, 8, 16}};

    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const std::uint64_t index = static_cast<std::uint64_t>(i + 1);
        require(luby_value(index) == expected[i], "unexpected Luby value at one-based index " + std::to_string(index));
        require(luby_interval(index) == expected[i] * kDefaultLubyUnit,
                "default Luby interval must use the paper's 256-slot unit");
        require(luby_interval(index, 10) == expected[i] * 10,
                "custom Luby interval must scale the sequence exactly");
    }

    const std::uint64_t largest_index = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t largest_luby_value = (std::uint64_t(1) << 63);
    require(luby_value(largest_index) == largest_luby_value,
            "the maximum uint64_t Mersenne index must not wrap");
    require(luby_interval(largest_index, 1) == largest_luby_value,
            "the maximum representable sequence value must support a unit-one interval");

    require(count_luby_restarts(255) == 0,
            "a partial first Luby interval must not restart");
    require(count_luby_restarts(256) == 1,
            "the first complete Luby interval must restart once");
    require(count_luby_restarts(512) == 2,
            "two complete unit intervals must restart twice");
    require(count_luby_restarts(1023) == 2,
            "a partial third interval must not restart");
    require(count_luby_restarts(1024) == 3,
            "the third Luby interval has length two units");

    bool rejected_zero_index = false;
    try
    {
        (void)luby_value(0);
    }
    catch (const std::invalid_argument &)
    {
        rejected_zero_index = true;
    }
    require(rejected_zero_index, "Luby indices must be one-based");

    bool rejected_zero_unit = false;
    try
    {
        (void)luby_interval(1, 0);
    }
    catch (const std::invalid_argument &)
    {
        rejected_zero_unit = true;
    }
    require(rejected_zero_unit, "a zero Luby unit must be rejected");

    bool rejected_overflow = false;
    try
    {
        (void)luby_interval(63, std::numeric_limits<std::uint64_t>::max());
    }
    catch (const std::overflow_error &)
    {
        rejected_overflow = true;
    }
    require(rejected_overflow, "an unrepresentable Luby interval must be rejected");
}

} // namespace

int main()
{
    test_modulo_one_slots();
    test_watchlist_frame_boundaries();
    test_possible_source_refresh_schedule();
    test_luby_sequence_and_intervals();
    std::cout << "public schedule policy tests passed" << std::endl;
    return 0;
}
