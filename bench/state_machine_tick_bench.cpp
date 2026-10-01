// Per-tick cost of the state-machine runtime on a machine shaped like the
// multirotor controller that runs it at 1 kHz: three parallel top-level
// regions (health / flight / debug), a nested flight hierarchy, ~45
// transitions with std::function guards, a debug state that emits one output
// event on every tick, and input events arriving at ROS-sensor rates.
//
// Only the public API is used, so the same source builds against any revision
// of the library and the numbers can be compared old vs new in one run.
//
// Reported per scenario over --reps repetitions of --ticks ticks each:
//   min/med ns    wall clock per tick of post + update (+ consumer reads); the
//                 minimum is the least noisy estimate on a busy machine
//   allocs/tick   global operator new calls (deterministic, noise free)
//   bytes/tick    bytes requested through operator new
//
// Usage: state_machine_tick_bench [--ticks N] [--reps R] [scenario ...]
// Scenarios: idle sensors consumer internal transitions harness_events

#include "controller_like_machine.hpp"

#include <state_machine/state_machine.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

std::uint64_t g_alloc_calls = 0;
std::uint64_t g_alloc_bytes = 0;

} // namespace

// Counting replacements of the global allocation functions. GCC cannot see that
// the replaced operator delete pairs with the replaced operator new (both sit on
// malloc/free) once it is inlined into the library headers' make_unique.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    ++g_alloc_calls;
    g_alloc_bytes += size;
    void* ptr = std::malloc(size == 0 ? 1 : size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif

namespace sm = state_machine;
using controller_like::buildControllerLikeMachine;
using controller_like::Flags;
namespace input_event = controller_like::input_event;
namespace output_event = controller_like::output_event;
namespace region_id = controller_like::region_id;
namespace state_id = controller_like::state_id;

namespace {

struct Harness {
    Flags flags;
    std::unique_ptr<sm::StateMachine> machine;

    Harness() {
        machine = buildControllerLikeMachine(flags);
        if (!machine->start().ok()) {
            std::fprintf(stderr, "start failed\n");
            std::abort();
        }
    }

    void tickOnce() {
        ++flags.tick;
        flags.now += 0.001;
        if (!machine->update().ok()) {
            std::fprintf(stderr, "update failed\n");
            std::abort();
        }
    }

    void post(sm::EventId id, const char* source) {
        sm::Event event(id, sm::EventTimestamp{flags.now});
        event.source = source;
        if (!machine->postEvent(std::move(event)).ok()) {
            std::fprintf(stderr, "post failed\n");
            std::abort();
        }
    }

    // Walk the same path a real flight does: SelfCheck -> Ready -> Takeoff* -> Hover.
    void reachHover() {
        tickOnce();
        post(input_event::kTakeoffRequested, "command");
        tickOnce();
        post(input_event::kAltctlReady, "mavros/state");
        tickOnce();
        post(input_event::kOffboardReady, "mavros/state");
        tickOnce();
        post(input_event::kArmReady, "mavros/state");
        tickOnce();
        post(input_event::kAltitudeReached, "takeoff");
        tickOnce();
        if (machine->currentState(region_id::kControl) != state_id::kHover) {
            std::fprintf(stderr, "failed to reach Hover, state=%u\n",
                         static_cast<unsigned>(machine->currentState(region_id::kControl)));
            std::abort();
        }
    }
};

// Sensor input streams: {event, source, period in ticks at 1 kHz}.
struct Stream {
    sm::EventId id;
    const char* source;
    int period;
};

const Stream kStreams[] = {
    {input_event::kLocalPosition, "mavros/local_position/pose", 20},
    {input_event::kLocalVelocity, "mavros/local_position/velocity_body", 20},
    {input_event::kImu, "mavros/imu/data", 5},
    {input_event::kStateEstimate, "alg/state_estimator/state", 5},
    {input_event::kVrpnPose, "pose", 10},
    {input_event::kMpcTrajectory, "alg/setpoint_raw/local", 10},
};

volatile std::size_t g_sink = 0;

enum class Scenario { kIdle, kSensors, kConsumer, kInternal, kTransitions, kHarnessEvents };

struct Sample {
    double min_ns_per_tick{0.0};
    double median_ns_per_tick{0.0};
    double allocs_per_tick{0.0};
    double bytes_per_tick{0.0};
};

void postSensorEvents(Harness& h) {
    for (const Stream& stream : kStreams) {
        if (h.flags.tick % stream.period == 0) {
            h.post(stream.id, stream.source);
        }
    }
}

void consumerReads(Harness& h, std::vector<std::uint32_t>& ids) {
    // DroneRosNode::controlLoopCallback + DebugOutputConsumer::handle.
    const auto outputs = h.machine->currentOutputEvents();
    for (const sm::Event& event : outputs) {
        g_sink = g_sink + event.id;
        if (event.id == output_event::kPublishStateMachineEvents) {
            const auto records = h.machine->currentEvents();
            ids.clear();
            for (const auto& record : records) {
                ids.push_back(record.event.id);
            }
            g_sink = g_sink + ids.size();
        }
    }
}

void runTicks(Scenario scenario, Harness& h, int ticks, std::vector<std::uint32_t>& ids) {
    for (int i = 0; i < ticks; ++i) {
        switch (scenario) {
        case Scenario::kIdle:
            h.tickOnce();
            break;
        case Scenario::kSensors:
            postSensorEvents(h);
            h.tickOnce();
            break;
        case Scenario::kConsumer:
            postSensorEvents(h);
            h.tickOnce();
            consumerReads(h, ids);
            break;
        case Scenario::kInternal:
            h.flags.internal_every = 4;
            postSensorEvents(h);
            h.tickOnce();
            consumerReads(h, ids);
            break;
        case Scenario::kTransitions:
            h.flags.toggle = (h.flags.tick % 2) == 0;
            h.tickOnce();
            break;
        case Scenario::kHarnessEvents:
            ++h.flags.tick;
            for (const Stream& stream : kStreams) {
                if (h.flags.tick % stream.period == 0) {
                    sm::Event event(stream.id, sm::EventTimestamp{h.flags.now});
                    event.source = stream.source;
                    g_sink = g_sink + event.source.size();
                }
            }
            break;
        }
    }
}

Sample measure(Scenario scenario, int ticks, int reps) {
    std::vector<double> ns;
    Sample sample;
    std::vector<std::uint32_t> ids;
    ids.reserve(64);
    for (int rep = 0; rep < reps; ++rep) {
        Harness h;
        h.reachHover();
        runTicks(scenario, h, 2000, ids); // warm-up: grow every reusable buffer
        const std::uint64_t allocs_before = g_alloc_calls;
        const std::uint64_t bytes_before = g_alloc_bytes;
        const auto begin = std::chrono::steady_clock::now();
        runTicks(scenario, h, ticks, ids);
        const auto end = std::chrono::steady_clock::now();
        ns.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()) /
                     ticks);
        sample.allocs_per_tick = static_cast<double>(g_alloc_calls - allocs_before) / ticks;
        sample.bytes_per_tick = static_cast<double>(g_alloc_bytes - bytes_before) / ticks;
    }
    std::sort(ns.begin(), ns.end());
    sample.min_ns_per_tick = ns.front();
    sample.median_ns_per_tick = ns[ns.size() / 2];
    return sample;
}

struct NamedScenario {
    const char* name;
    Scenario scenario;
};

const NamedScenario kScenarios[] = {
    {"idle", Scenario::kIdle},
    {"sensors", Scenario::kSensors},
    {"consumer", Scenario::kConsumer},
    {"internal", Scenario::kInternal},
    {"transitions", Scenario::kTransitions},
    {"harness_events", Scenario::kHarnessEvents},
};

} // namespace

int main(int argc, char** argv) {
    int ticks = 20000;
    int reps = 9;
    std::vector<std::string> selected;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            ticks = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            reps = std::atoi(argv[++i]);
        } else {
            selected.emplace_back(argv[i]);
        }
    }
    if (ticks <= 0 || reps <= 0) {
        std::fprintf(stderr, "usage: %s [--ticks N] [--reps R] [scenario ...]\n", argv[0]);
        return 2;
    }
    for (const NamedScenario& named : kScenarios) {
        if (!selected.empty() && std::find(selected.begin(), selected.end(), named.name) == selected.end()) {
            continue;
        }
        const Sample sample = measure(named.scenario, ticks, reps);
        std::printf("%-15s min_ns/tick=%8.1f  med_ns/tick=%8.1f  allocs/tick=%7.3f  bytes/tick=%8.1f\n", named.name,
                    sample.min_ns_per_tick, sample.median_ns_per_tick, sample.allocs_per_tick, sample.bytes_per_tick);
    }
    return 0;
}
