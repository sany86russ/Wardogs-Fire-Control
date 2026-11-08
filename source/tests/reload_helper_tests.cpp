#include "wardogs/reload_helper.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using wardogs::ReloadDirection;
using wardogs::ReloadHelperRunner;
using wardogs::ReloadHelperState;
using wardogs::ReloadObservation;
int checks{};
int failures{};

void check(bool condition, const char* message) {
    ++checks;
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template<class Operation>
bool rejects(Operation operation) {
    try { operation(); }
    catch (const std::exception&) { return true; }
    return false;
}

bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 2500ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

wardogs::ReloadHelperTiming fast_timing() { return {2, 3, 5, 60, 1500}; }

ReloadObservation band() {
    ReloadObservation result;
    result.bounds = {100, 200, 230, 246};
    result.confidence = 0.99;
    const std::array directions{ReloadDirection::up, ReloadDirection::right,
        ReloadDirection::down, ReloadDirection::left};
    for (std::size_t index = 0; index < directions.size(); ++index)
        result.arrows[index] = {directions[index], false};
    return result;
}

struct Event {
    ReloadDirection direction;
    bool pressed;
    int captures;
    std::chrono::steady_clock::time_point at;
};

struct Fixture {
    std::atomic<bool> foreground{true};
    std::atomic<bool> released{true};
    std::atomic<bool> accept_down{true};
    std::atomic<bool> accept_up{true};
    std::atomic<bool> throw_down{};
    std::atomic<bool> throw_up_once{};
    std::atomic<bool> throw_observe{};
    std::atomic<bool> hold_modifier_after_up{};
    mutable std::mutex mutex;
    std::array<std::optional<ReloadObservation>, 4> snapshots;
    std::vector<Event> events;
    int captures{};

    Fixture() { snapshots.fill(band()); }

    wardogs::ReloadHelperPlatform platform() {
        return {
            [this] { return foreground.load(); },
            [this] { return released.load(); },
            [this]() -> std::optional<ReloadObservation> {
                std::lock_guard lock(mutex);
                ++captures;
                if (throw_observe.load()) throw std::runtime_error("capture failure");
                if (captures > 4) throw std::runtime_error("unexpected extra snapshot");
                return snapshots[static_cast<std::size_t>(captures - 1)];
            },
            [this](ReloadDirection direction, bool pressed) {
                std::lock_guard lock(mutex);
                events.push_back({direction, pressed, captures, std::chrono::steady_clock::now()});
                if (pressed && throw_down.load()) throw std::runtime_error("uncertain DOWN delivery");
                if (pressed) return accept_down.load();
                if (throw_up_once.exchange(false)) throw std::runtime_error("uncertain UP delivery");
                if (!accept_up.load()) return false;
                if (hold_modifier_after_up.load()) released = false;
                return true;
            }
        };
    }

    std::vector<Event> snapshot() const {
        std::lock_guard lock(mutex);
        return events;
    }
    int capture_count() const {
        std::lock_guard lock(mutex);
        return captures;
    }
    int downs() const {
        const auto values = snapshot();
        int count{};
        for (const auto& event : values) if (event.pressed) ++count;
        return count;
    }
    void reset() {
        std::lock_guard lock(mutex);
        events.clear();
        captures = 0;
    }
};

void lifecycle_and_production_gate() {
    check(!wardogs::send_reload_key(0, ReloadDirection::up, true) &&
        !wardogs::send_reload_key(std::numeric_limits<std::uintptr_t>::max(), ReloadDirection::right, true),
        "production DOWN rejects missing and invalid HWND without sending input");
    check(!wardogs::send_reload_key(0, static_cast<ReloadDirection>(77), false),
        "unknown directions cannot send even a keyboard UP");
    ReloadHelperRunner runner;
    Fixture fixture;
    check(!runner.running() && runner.state() == ReloadHelperState::stopped &&
        runner.sent_count() == 0 && runner.last_error().empty(),
        "construction never restores an armed batch");
    check(rejects([&] { runner.start({}); }), "missing platform callbacks fail before starting");
    const wardogs::ReloadHelperTiming defaults;
    check(defaults.hold_ms == 50 && defaults.key_gap_ms == 90 && defaults.stage_gap_ms == 750 &&
        defaults.activation_timeout_ms == 3000 && defaults.overall_timeout_ms == 15000,
        "production uses fixed 50 ms holds, 90 ms key gaps and 750 ms stage gaps");
    for (int field = 0; field < 6; ++field) {
        auto timing = fast_timing();
        if (field == 0) timing.hold_ms = 0;
        if (field == 1) timing.key_gap_ms = 0;
        if (field == 2) timing.stage_gap_ms = 0;
        if (field == 3) timing.activation_timeout_ms = 0;
        if (field == 4) timing.overall_timeout_ms = 0;
        if (field == 5) timing.hold_ms = 1001;
        check(rejects([&] { runner.start(fixture.platform(), timing); }) && fixture.snapshot().empty(),
            "invalid timing fails before any input worker");
    }
    runner.stop();
    check(!runner.running() && fixture.snapshot().empty(), "STOP without an owned key sends no unrelated UP");
}

void exactly_four_snapshots_and_sixteen_pairs() {
    Fixture fixture;
    const std::array directions{ReloadDirection::down, ReloadDirection::down,
        ReloadDirection::left, ReloadDirection::up};
    for (std::size_t stage = 0; stage < fixture.snapshots.size(); ++stage)
        for (auto& arrow : fixture.snapshots[stage]->arrows) arrow.direction = directions[stage];
    ReloadHelperRunner runner;
    runner.start(fixture.platform(), fast_timing());
    check(wait_for([&] { return !runner.running(); }) && runner.state() == ReloadHelperState::completed &&
        runner.sent_count() == 16 && fixture.capture_count() == 4 && runner.last_error().empty(),
        "four stages use exactly four snapshots and send sixteen complete pairs without visual acknowledgements");
    const auto events = fixture.snapshot();
    bool paired = events.size() == 32;
    for (std::size_t index = 0; paired && index < 16; ++index) {
        const auto& down = events[index * 2];
        const auto& up = events[index * 2 + 1];
        paired = down.pressed && !up.pressed && down.direction == directions[index / 4] &&
            up.direction == down.direction && down.captures == static_cast<int>(index / 4 + 1) &&
            up.captures == down.captures;
    }
    check(paired, "repeated identical directions have distinct DOWN/UP pairs with no snapshots inside any batch");
    runner.stop();
    check(fixture.snapshot().size() == events.size(), "completed batch owns no key and STOP adds no UP");
}

void configured_timing_and_humanization() {
    {
        const auto timing = wardogs::reload_helper_timing({});
        check(timing.hold_ms == 50 && timing.key_gap_ms == 90 && timing.stage_gap_ms == 750 &&
            timing.overall_timeout_ms == 15000 && !timing.key_random_enabled && !timing.stage_random_enabled &&
            timing.key_random_min_ms == 0 && timing.key_random_max_ms == 30 &&
            timing.stage_random_min_ms == 0 && timing.stage_random_max_ms == 200,
            "default preferences use the owner-selected cadence and disable both random additions");
        wardogs::ReloadHelperPreferences preferences;
        preferences.hold_ms = 60;
        preferences.key_gap_ms = 130;
        preferences.stage_gap_ms = 1600;
        preferences.key_random_enabled = true;
        preferences.key_random_min_ms = 4;
        preferences.key_random_max_ms = 20;
        preferences.stage_random_enabled = true;
        preferences.stage_random_min_ms = 50;
        preferences.stage_random_max_ms = 100;
        const auto configured = wardogs::reload_helper_timing(preferences);
        check(configured.hold_ms == 60 && configured.key_gap_ms == 130 && configured.stage_gap_ms == 1600 &&
            configured.key_random_enabled && configured.key_random_min_ms == 4 && configured.key_random_max_ms == 20 &&
            configured.stage_random_enabled && configured.stage_random_min_ms == 50 &&
            configured.stage_random_max_ms == 100 && configured.overall_timeout_ms == 16180,
            "configured timings copy both independent ranges and budget the worst-case sixteen-key cycle");
        preferences.hold_ms = 1000;
        preferences.key_gap_ms = 2000;
        preferences.stage_gap_ms = 10000;
        preferences.key_random_max_ms = preferences.stage_random_max_ms = 2000;
        check(wardogs::reload_helper_timing(preferences).overall_timeout_ms == 140000,
            "maximum allowed delays receive a bounded cycle budget instead of expiring at the old fifteen seconds");
        preferences.hold_ms = 19;
        check(rejects([&] { (void)wardogs::reload_helper_timing(preferences); }),
            "preference conversion rejects invalid persisted timing before any worker can start");
    }
    for (int invalid = 0; invalid < 6; ++invalid) {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto timing = fast_timing();
        if (invalid == 0) timing.key_random_min_ms = -1;
        if (invalid == 1) timing.key_random_max_ms = 2001;
        if (invalid == 2) { timing.key_random_min_ms = 2; timing.key_random_max_ms = 1; }
        if (invalid == 3) timing.stage_random_min_ms = -1;
        if (invalid == 4) timing.stage_random_max_ms = 2001;
        if (invalid == 5) { timing.stage_random_min_ms = 2; timing.stage_random_max_ms = 1; }
        check(rejects([&] { runner.start(fixture.platform(), timing); }) &&
            fixture.capture_count() == 0 && fixture.snapshot().empty(),
            "invalid random ranges cannot start an input worker even when their toggle is disabled");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        std::atomic<int> samples{};
        platform.random_between = [&](int, int) -> int { ++samples; throw std::runtime_error("disabled sampler"); };
        runner.start(std::move(platform), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && runner.sent_count() == 16 && samples == 0,
            "disabled humanization never samples and preserves the original four-snapshot sixteen-key cycle");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        struct Sample { int minimum; int maximum; int captures; int events; };
        std::vector<Sample> samples;
        int key_samples{}, stage_samples{};
        platform.random_between = [&](int minimum, int maximum) {
            samples.push_back({minimum, maximum, fixture.capture_count(), static_cast<int>(fixture.snapshot().size())});
            if (minimum == 1) return ++key_samples % 2 ? minimum : maximum;
            return ++stage_samples % 2 ? minimum : maximum;
        };
        auto timing = fast_timing();
        timing.key_random_enabled = timing.stage_random_enabled = true;
        timing.key_random_min_ms = 1; timing.key_random_max_ms = 2;
        timing.stage_random_min_ms = 3; timing.stage_random_max_ms = 4;
        timing.overall_timeout_ms = 3000;
        runner.start(std::move(platform), timing);
        const bool finished = wait_for([&] { return !runner.running(); }, 4000ms);
        runner.stop();
        check(finished && runner.state() == ReloadHelperState::completed && runner.sent_count() == 16 &&
            fixture.capture_count() == 4 && key_samples == 28 && stage_samples == 3,
            "every hold and key gap receives an independent key sample while only the three stage gaps use stage samples");
        std::size_t index{};
        bool ordered = samples.size() == 31;
        for (int stage = 0; ordered && stage < 4; ++stage) {
            for (int key = 0; ordered && key < 4; ++key) {
                const auto& hold = samples[index++];
                ordered = hold.minimum == 1 && hold.maximum == 2 && hold.captures == stage + 1 &&
                    hold.events == stage * 8 + key * 2;
                if (ordered && key < 3) {
                    const auto& gap = samples[index++];
                    ordered = gap.minimum == 1 && gap.maximum == 2 && gap.captures == stage + 1 &&
                        gap.events == stage * 8 + key * 2 + 2;
                }
            }
            if (ordered && stage < 3) {
                const auto& gap = samples[index++];
                ordered = gap.minimum == 3 && gap.maximum == 4 && gap.captures == stage + 1 &&
                    gap.events == (stage + 1) * 8;
            }
        }
        check(ordered, "random values are sampled at each individual wait without adding snapshots or reusing a stage-wide value");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        platform.random_between = [](int minimum, int) { return minimum; };
        auto timing = fast_timing();
        timing.key_random_enabled = true;
        timing.key_random_min_ms = timing.key_random_max_ms = 30;
        runner.start(std::move(platform), timing);
        const bool sent_two = wait_for([&] { return runner.sent_count() >= 2 || !runner.running(); });
        runner.stop();
        const auto events = fixture.snapshot();
        check(sent_two && events.size() == 4 && events[1].at - events[0].at >= 32ms &&
            events[2].at - events[1].at >= 33ms,
            "the inclusive sampled addition is applied to both actual hold duration and the released key gap");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        std::atomic<int> samples{};
        platform.random_between = [&](int minimum, int) { ++samples; return minimum; };
        auto timing = fast_timing();
        timing.stage_random_enabled = true;
        timing.stage_random_min_ms = timing.stage_random_max_ms = 40;
        runner.start(std::move(platform), timing);
        const bool next_stage = wait_for([&] { return fixture.downs() >= 5 || !runner.running(); });
        runner.stop();
        const auto events = fixture.snapshot();
        check(next_stage && events.size() >= 9 && events[8].at - events[7].at >= 45ms && samples == 1,
            "stage humanization affects only the pause before the next single snapshot and leaves key sampling disabled");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto timing = fast_timing();
        timing.key_random_enabled = timing.stage_random_enabled = true;
        timing.key_random_min_ms = timing.key_random_max_ms = 0;
        timing.stage_random_min_ms = timing.stage_random_max_ms = 0;
        runner.start(fixture.platform(), timing);
        check(wait_for([&] { return !runner.running(); }) && runner.sent_count() == 16 &&
            fixture.capture_count() == 4, "the production inclusive random sampler supports a zero-addition range");
    }
}

void random_sampling_failures() {
    for (int invalid = 0; invalid < 3; ++invalid) {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        platform.random_between = [&](int minimum, int maximum) -> int {
            if (invalid == 0) return minimum - 1;
            if (invalid == 1) return maximum + 1;
            throw std::runtime_error("random sample failure");
        };
        auto timing = fast_timing();
        timing.key_random_enabled = true;
        timing.key_random_min_ms = 1; timing.key_random_max_ms = 2;
        runner.start(std::move(platform), timing);
        check(wait_for([&] { return !runner.running(); }) && runner.state() == ReloadHelperState::error &&
            runner.sent_count() == 0 && fixture.snapshot().empty() && !runner.release_pending(),
            "invalid or throwing random samples fail before the first DOWN and acquire no release obligation");
    }
    for (bool stage_gap : {false, true}) {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        int samples{};
        platform.random_between = [&](int minimum, int maximum) {
            return stage_gap || ++samples == 2 ? maximum + 1 : minimum;
        };
        auto timing = fast_timing();
        timing.key_random_enabled = !stage_gap;
        timing.stage_random_enabled = stage_gap;
        timing.key_random_min_ms = timing.stage_random_min_ms = 1;
        timing.key_random_max_ms = timing.stage_random_max_ms = 2;
        runner.start(std::move(platform), timing);
        check(wait_for([&] { return !runner.running(); }) && runner.state() == ReloadHelperState::error &&
            runner.sent_count() == (stage_gap ? 4 : 1) && fixture.capture_count() == 1 &&
            fixture.downs() == (stage_gap ? 4 : 1) && !runner.release_pending(),
            "bad key-gap or stage-gap samples stop before any following DOWN or stage capture");
    }
    for (bool focus_loss : {false, true}) {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        platform.random_between = [&](int minimum, int) {
            if (focus_loss) fixture.foreground = false;
            else std::this_thread::sleep_for(40ms);
            return minimum;
        };
        auto timing = fast_timing();
        timing.key_random_enabled = true;
        timing.overall_timeout_ms = 30;
        runner.start(std::move(platform), timing);
        check(wait_for([&] { return !runner.running(); }) && fixture.snapshot().empty() &&
            runner.state() == ReloadHelperState::error && !runner.release_pending(),
            "a valid random sample cannot bypass focus or budget gates after its callback returns");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        platform.random_between = [](int minimum, int) { return minimum; };
        auto timing = fast_timing();
        timing.key_random_enabled = true;
        timing.key_random_min_ms = timing.key_random_max_ms = 500;
        runner.start(std::move(platform), timing);
        const bool held = wait_for([&] { return fixture.downs() == 1; });
        const auto before = std::chrono::steady_clock::now();
        runner.stop();
        check(held && std::chrono::steady_clock::now() - before < 250ms &&
            fixture.snapshot().size() == 2 && runner.sent_count() == 0 && !runner.release_pending(),
            "STOP interrupts a randomized hold promptly and still releases its owned key");
    }
}

void invalid_stage_snapshots() {
    for (int missing_stage : {0, 2}) {
        Fixture fixture;
        fixture.snapshots[static_cast<std::size_t>(missing_stage)].reset();
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && runner.state() == ReloadHelperState::error &&
            fixture.capture_count() == missing_stage + 1 && runner.sent_count() == missing_stage * 4 &&
            fixture.downs() == missing_stage * 4,
            "an unreadable stage fails on its single snapshot without polling or stage input");
    }
    for (int colored : {0, 2, 4}) {
        Fixture fixture;
        if (colored == 0) fixture.snapshots[0]->arrows[2].accepted = true;
        else for (int index = 0; index < colored; ++index)
            fixture.snapshots[0]->arrows[static_cast<std::size_t>(index)].accepted = true;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && runner.state() == ReloadHelperState::error &&
            runner.sent_count() == 0 && fixture.capture_count() == 1 && fixture.snapshot().empty(),
            "a colored snapshot cannot deliberately replay a partial or stale completed row");
    }
    for (int invalid = 0; invalid < 7; ++invalid) {
        Fixture fixture;
        auto& value = *fixture.snapshots[0];
        if (invalid == 0) value.confidence = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 1) value.confidence = 0.69;
        if (invalid == 2) value.confidence = 1.01;
        if (invalid == 3) value.arrows[1].direction = static_cast<ReloadDirection>(99);
        if (invalid == 4) value.bounds.left = -1;
        if (invalid == 5) value.bounds.right = value.bounds.left;
        if (invalid == 6) value.bounds.right = 32769;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && runner.state() == ReloadHelperState::error &&
            fixture.capture_count() == 1 && fixture.snapshot().empty(),
            "malformed directions, confidence or bounds never authorize input");
    }
}

void cancellation_and_activation() {
    for (bool focus_loss : {false, true}) {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto timing = fast_timing();
        timing.hold_ms = 500;
        runner.start(fixture.platform(), timing);
        check(wait_for([&] { return fixture.downs() == 1; }) && runner.sent_count() == 0,
            "DOWN alone is never counted as a delivered pair");
        const auto before = std::chrono::steady_clock::now();
        if (focus_loss) {
            fixture.foreground = false;
            check(wait_for([&] { return !runner.running(); }), "focus loss stops the held key independently of UI");
        } else runner.stop();
        const auto events = fixture.snapshot();
        check(std::chrono::steady_clock::now() - before < 250ms && events.size() == 2 &&
            events[0].pressed && !events[1].pressed && events[0].direction == events[1].direction &&
            runner.sent_count() == 0 && !runner.release_pending(),
            "STOP and focus loss promptly release exactly the owned key without counting an interrupted hold");
        fixture.foreground = true;
        std::this_thread::sleep_for(20ms);
        check(fixture.downs() == 1, "restored focus never resumes an interrupted batch");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto timing = fast_timing();
        timing.stage_gap_ms = 500;
        runner.start(fixture.platform(), timing);
        const bool reached_gap = wait_for([&] { return runner.sent_count() == 4; });
        runner.stop();
        check(reached_gap && fixture.capture_count() == 1 && fixture.snapshot().size() == 8 &&
            runner.sent_count() == 4, "STOP during the fixed stage gap prevents the next snapshot and batch");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto timing = fast_timing();
        timing.key_gap_ms = 500;
        runner.start(fixture.platform(), timing);
        const bool reached_gap = wait_for([&] { return runner.sent_count() == 1; });
        fixture.foreground = false;
        check(reached_gap && wait_for([&] { return !runner.running(); }) &&
            runner.state() == ReloadHelperState::error && fixture.snapshot().size() == 2 &&
            fixture.capture_count() == 1, "focus loss during a key gap prevents the next DOWN");
    }
    {
        Fixture fixture;
        fixture.released = false;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && fixture.capture_count() == 0 &&
            fixture.snapshot().empty() && runner.state() == ReloadHelperState::error,
            "held activation times out before any snapshot or input");
    }
    {
        Fixture fixture;
        fixture.released = false;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        std::this_thread::sleep_for(15ms);
        check(fixture.capture_count() == 0 && fixture.snapshot().empty(), "launch release is required before the first snapshot");
        fixture.released = true;
        check(wait_for([&] { return !runner.running(); }) && runner.sent_count() == 16,
            "normal launch release permits a new four-stage batch");
    }
    {
        Fixture fixture;
        fixture.hold_modifier_after_up = true;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && runner.sent_count() == 1 &&
            fixture.snapshot().size() == 2 && runner.state() == ReloadHelperState::error,
            "modifiers or activation pressed between keys prevent the next DOWN");
    }
}

void delivery_errors_and_deadlines() {
    for (int error = 0; error < 3; ++error) {
        Fixture fixture;
        fixture.accept_down = error != 0;
        fixture.throw_down = error == 1;
        fixture.throw_up_once = error == 2;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        const bool finished = wait_for([&] { return !runner.running(); });
        const auto events = fixture.snapshot();
        check(finished && runner.state() == ReloadHelperState::error && runner.sent_count() == 0 &&
            fixture.downs() == 1 && events.size() == static_cast<std::size_t>(error + 1) &&
            !runner.release_pending() && (error == 0 || !events.back().pressed),
            "refused DOWN, throwing DOWN and recovered throwing UP stop without another key or a false delivered count");
    }
    {
        Fixture fixture;
        fixture.accept_up = false;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && runner.state() == ReloadHelperState::error &&
            runner.release_pending() && runner.sent_count() == 0, "an unresolved UP remains visible to the helper coordinator");
        check(rejects([&] { runner.start(fixture.platform(), fast_timing()); }) && fixture.downs() == 1,
            "unresolved input ownership blocks restart before any new DOWN");
        fixture.accept_up = true;
        runner.stop();
        check(!runner.release_pending(), "a successful owed UP clears the input latch");
        fixture.reset();
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && runner.sent_count() == 16,
            "recovery permits only an explicitly requested new batch");
    }
    {
        Fixture fixture;
        fixture.throw_observe = true;
        ReloadHelperRunner runner;
        runner.start(fixture.platform(), fast_timing());
        check(wait_for([&] { return !runner.running(); }) && fixture.snapshot().empty() &&
            runner.last_error() == "capture failure", "snapshot exception stops before input and retains its diagnostic");
    }
    for (bool lose_focus : {false, true}) {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto platform = fixture.platform();
        const auto observe = platform.observe;
        platform.observe = [&]() -> std::optional<ReloadObservation> {
            auto value = observe();
            if (lose_focus) fixture.foreground = false;
            else std::this_thread::sleep_for(40ms);
            return value;
        };
        auto timing = fast_timing();
        timing.overall_timeout_ms = 30;
        runner.start(std::move(platform), timing);
        check(wait_for([&] { return !runner.running(); }) && fixture.snapshot().empty() &&
            runner.state() == ReloadHelperState::error, "a snapshot crossing focus or the overall deadline cannot authorize DOWN");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto timing = fast_timing();
        timing.hold_ms = 500;
        timing.overall_timeout_ms = 40;
        runner.start(fixture.platform(), timing);
        check(wait_for([&] { return !runner.running(); }) && fixture.snapshot().size() == 2 &&
            !fixture.snapshot().back().pressed && runner.sent_count() == 0 &&
            !runner.release_pending() && runner.state() == ReloadHelperState::error,
            "overall expiry interrupts a hold, releases its key and does not count the interrupted pair");
    }
    {
        Fixture fixture;
        ReloadHelperRunner runner;
        auto timing = fast_timing();
        timing.stage_gap_ms = 1000;
        timing.overall_timeout_ms = 300;
        runner.start(fixture.platform(), timing);
        check(wait_for([&] { return !runner.running(); }) && fixture.capture_count() == 1 &&
            runner.sent_count() == 4 && runner.state() == ReloadHelperState::error,
            "the overall budget also bounds fixed stage gaps without a late snapshot");
    }
}
}  // namespace

int main() {
    lifecycle_and_production_gate();
    exactly_four_snapshots_and_sixteen_pairs();
    configured_timing_and_humanization();
    random_sampling_failures();
    invalid_stage_snapshots();
    cancellation_and_activation();
    delivery_errors_and_deadlines();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
