// Heap allocations per tick of the runtime on a controller-shaped machine.
//
// update() runs at 1 kHz per robot, so a steady-state tick must not touch the
// heap. This test counts the calls to the global operator new made by the
// library while it runs and fails if a tick that carries no event allocates at
// all, or if posting and dispatching events allocates more than the one copy of
// the event's source string the event log keeps (plus amortized container
// nodes). Timing is not asserted: it is too noisy for a test; see
// bench/state_machine_tick_bench.cpp for the numbers.

#include "support/controller_like_machine.hpp"

#include <state_machine/state_machine.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace {

std::uint64_t g_alloc_calls = 0;
std::int64_t g_live_allocations = 0;
bool g_counting = false;

// Counting is only on while the library runs, not while the test builds events.
class CountingScope {
  public:
    CountingScope() { g_counting = true; }
    ~CountingScope() { g_counting = false; }
    CountingScope(const CountingScope&) = delete;
    CountingScope& operator=(const CountingScope&) = delete;
};

} // namespace

// GCC cannot see that the replaced operator delete pairs with the replaced
// operator new (both sit on malloc/free) once it is inlined into the headers.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    if (g_counting) {
        ++g_alloc_calls;
    }
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    ++g_live_allocations;
    return ptr;
}

void operator delete(void* ptr) noexcept {
    if (ptr != nullptr) {
        --g_live_allocations;
    }
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    if (ptr != nullptr) {
        --g_live_allocations;
    }
    std::free(ptr);
}

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif

namespace sm = state_machine;
namespace cl = controller_like;

namespace {

struct Rig {
    cl::Flags flags;
    std::unique_ptr<sm::StateMachine> machine;

    Rig() {
        machine = cl::buildControllerLikeMachine(flags);
        machine->start();
        tick();
        post(cl::input_event::kTakeoffRequested, "command");
        tick();
        post(cl::input_event::kAltctlReady, "mavros/state");
        tick();
        post(cl::input_event::kOffboardReady, "mavros/state");
        tick();
        post(cl::input_event::kArmReady, "mavros/state");
        tick();
        post(cl::input_event::kAltitudeReached, "takeoff");
        tick();
    }

    void tick() {
        ++flags.tick;
        flags.now += 0.001;
        machine->update();
    }

    void post(sm::EventId id, const std::string& source) {
        sm::Event event(id, sm::EventTimestamp{flags.now});
        event.source = source;
        machine->postEvent(std::move(event));
    }
};

bool check(const char* what, double measured, double bound) {
    const bool ok = measured <= bound;
    std::printf("%-44s %8.3f (bound %.3f) %s\n", what, measured, bound, ok ? "ok" : "FAILED");
    return ok;
}

class TaskCycle final : public sm::State {
  public:
    std::string name() const override { return "TaskCycle"; }
    sm::Status onTick(sm::StateContext& ctx) override {
        auto started = ctx.startTask();
        handle = started.value;
        return started.status;
    }
    sm::TaskHandle handle;
};

enum class TaskEnd { kComplete, kCancel, kStateExit };

bool taskStorageIsBounded(TaskEnd ending) {
    auto state = std::make_unique<TaskCycle>();
    auto* observed = state.get();
    sm::RuntimeOptions options;
    options.event_log_capacity = 32;
    auto builder = sm::StateMachine::builder("task-storage", options);
    builder.region(1)
        .initial(1)
        .state(1)
        .impl(std::move(state))
        .endRegion()
        .transition()
        .from(1)
        .to(1)
        .on(77)
        .type(sm::TransitionType::kExternalSelf);
    auto built = builder.build();
    if (!built.ok()) {
        return false;
    }
    auto machine = std::move(built.value);
    if (!machine->start().ok()) {
        return false;
    }
    const auto cycle = [&]() {
        if (!machine->update().ok()) {
            return false;
        }
        sm::Status ended;
        if (ending == TaskEnd::kComplete) {
            ended = machine->postTaskResult(observed->handle, sm::TaskStatus::kCompleted);
        } else if (ending == TaskEnd::kCancel) {
            ended = machine->cancelTask(observed->handle);
        } else {
            ended = machine->postEvent(sm::Event(77));
        }
        return ended.ok() && machine->update({64, 64, false}).ok();
    };
    for (int i = 0; i < 4000; ++i) {
        if (!cycle()) {
            return false;
        }
    }
    const auto before = g_live_allocations;
    for (int i = 0; i < 4000; ++i) {
        if (!cycle()) {
            return false;
        }
    }
    // Bounded logs/deque storage can differ by a few blocks between snapshots.
    // Retaining 4,000 ended task nodes would exceed this bound by orders of magnitude.
    const auto growth = g_live_allocations - before;
    const char* label = ending == TaskEnd::kComplete ? "completed task storage growth"
                                                     : ending == TaskEnd::kCancel ? "cancelled task storage growth"
                                                                                  : "state-exit task storage growth";
    return check(label, static_cast<double>(growth), 8.0);
}

} // namespace

int main() {
    constexpr int kWarmup = 4000;
    constexpr int kTicks = 2000;
    bool ok = true;

    {
        Rig rig;
        if (rig.machine->currentState(cl::region_id::kControl) != cl::state_id::kHover) {
            std::fprintf(stderr, "the flight did not reach Hover\n");
            return 2;
        }
        for (int i = 0; i < kWarmup; ++i) {
            rig.tick();
        }
        g_alloc_calls = 0;
        {
            const CountingScope counting;
            for (int i = 0; i < kTicks; ++i) {
                rig.tick();
            }
        }
        ok = check("idle tick, allocations per tick", static_cast<double>(g_alloc_calls) / kTicks, 0.0) && ok;
    }

    {
        // Sensor events at ROS rates, long topic names as sources.
        Rig rig;
        const std::string sources[] = {"mavros/local_position/pose", "mavros/imu/data", "alg/state_estimator/state",
                                       "mavros/local_position/velocity_body"};
        for (int i = 0; i < kWarmup; ++i) {
            rig.post(cl::input_event::kImu, sources[static_cast<std::size_t>(i) % 4]);
            rig.tick();
        }
        g_alloc_calls = 0;
        int posted = 0;
        for (int i = 0; i < kTicks; ++i) {
            sm::Event event(cl::input_event::kStateEstimate, sm::EventTimestamp{rig.flags.now});
            event.source = sources[static_cast<std::size_t>(i) % 4];
            {
                const CountingScope counting;
                rig.machine->postEvent(std::move(event));
                ++rig.flags.tick;
                rig.flags.now += 0.001;
                rig.machine->update();
            }
            ++posted;
        }
        ok = check("one event per tick, allocations per event", static_cast<double>(g_alloc_calls) / posted, 2.5) && ok;
    }

    {
        // Hover <-> Custom1 every tick: exit, enter, action-less transition, records and log.
        Rig rig;
        for (int i = 0; i < kWarmup; ++i) {
            rig.flags.toggle = (i % 2) == 0;
            rig.tick();
        }
        g_alloc_calls = 0;
        {
            const CountingScope counting;
            for (int i = 0; i < kTicks; ++i) {
                rig.flags.toggle = (i % 2) == 0;
                rig.tick();
            }
        }
        ok = check("a transition every tick, allocations per tick", static_cast<double>(g_alloc_calls) / kTicks, 0.5) &&
             ok;
    }

    ok = taskStorageIsBounded(TaskEnd::kComplete) && ok;
    ok = taskStorageIsBounded(TaskEnd::kCancel) && ok;
    ok = taskStorageIsBounded(TaskEnd::kStateExit) && ok;
    return ok ? 0 : 1;
}
