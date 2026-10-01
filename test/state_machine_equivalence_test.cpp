// Behaviour lock for the state-machine runtime.
//
// Replays fixed, seeded scenarios against the library and records every
// observable output after every step: the return value of each API call, every
// UpdateResult field, snapshot(), eventLog(), faultLog(), currentEvents(),
// currentTrace(), currentOutputEvents(), the state queries, the clock reads the
// runtime makes, and the order and arguments of every callback, guard and action
// the runtime invokes. The text of all of that is folded into 64-bit FNV-1a
// hashes that are compared with a golden file recorded from the revision before
// the per-tick optimizations (6f31e52). A refactor that changes anything a
// caller can see therefore fails here, at a step number.
//
// Two kinds of scenario:
//   controller_flight  a scripted flight through the controller-shaped machine
//                      (test/support); its compact trace is stored verbatim in
//                      the golden file so a reviewer can read what is locked.
//   random.N           machines with random region/state trees, transition
//                      tables, callbacks that post internal and output events,
//                      start tasks, fail and throw, driven by random events and
//                      update options; stored as checkpoint hashes.
//
// Usage:
//   state_machine_equivalence_test [restrictions] <golden>            verify (ctest)
//   state_machine_equivalence_test [restrictions] --update <golden>   rewrite it
//   state_machine_equivalence_test [restrictions] [--steps N] [--hashes] --dump <scenario>
//       prints the full text trace of controller_flight, random.N or seed:S, or
//       with --hashes one hash per step; diff two builds of the library by hand
// restrictions: --full or any of --internal-anywhere --interleaved-rules
// --many-tasks lift the restrictions described at struct Restrictions.

#include "support/controller_like_machine.hpp"

#include <state_machine/state_machine.hpp>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace sm = state_machine;

namespace {

// ---------------------------------------------------------------------------
// Deterministic random numbers: SplitMix64 on explicit integers only, so a seed
// means the same sequence on every compiler and standard library.
// ---------------------------------------------------------------------------
class Rng {
  public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    std::uint32_t below(std::uint32_t bound) {
        return bound == 0 ? 0U : static_cast<std::uint32_t>((next() >> 20) % bound);
    }

    bool chance(std::uint32_t percent) { return below(100) < percent; }

    template <typename T, std::size_t N> T pick(const T (&values)[N]) {
        return values[below(static_cast<std::uint32_t>(N))];
    }

  private:
    std::uint64_t state_;
};

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string
fmt(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    const int needed = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    const std::size_t length = needed > 0 ? static_cast<std::size_t>(needed) : 0;
    std::string out(length + 1, '\0');
    std::vsnprintf(&out[0], out.size(), format, args);
    va_end(args);
    out.resize(length);
    return out;
}

unsigned u(std::uint64_t value) {
    return static_cast<unsigned>(value);
}

unsigned long long ull(std::uint64_t value) {
    return static_cast<unsigned long long>(value);
}

// Every line that describes an observable is fed to a running FNV-1a hash.
// note() additionally keeps the line, for the human-readable golden trace.
class Observer {
  public:
    void line(const std::string& text) { feed(text); }

    void note(const std::string& text) {
        feed(text);
        summary_.push_back(text);
    }

    void keepText(bool keep) { keep_text_ = keep; }

    std::uint64_t hash() const { return hash_; }
    const std::vector<std::string>& summary() const { return summary_; }
    const std::string& text() const { return text_; }

  private:
    void feed(const std::string& text) {
        for (const char c : text) {
            mix(static_cast<unsigned char>(c));
        }
        mix(static_cast<unsigned char>('\n'));
        if (keep_text_) {
            text_ += text;
            text_ += '\n';
        }
    }

    void mix(unsigned char byte) {
        hash_ ^= byte;
        hash_ *= 1099511628211ULL;
    }

    std::uint64_t hash_{1469598103934665603ULL};
    bool keep_text_{false};
    std::string text_;
    std::vector<std::string> summary_;
};

// A clock that advances by one microsecond on every read, so the number and the
// order of reads the runtime makes are part of the observable behaviour.
class StepClock final : public sm::Clock {
  public:
    sm::TimePoint now() const override {
        ++reads_;
        return sm::TimePoint(
            std::chrono::duration_cast<sm::Duration>(std::chrono::nanoseconds(1000000 + reads_ * 1000)));
    }

    std::uint64_t reads() const { return reads_; }

  private:
    mutable std::uint64_t reads_{0};
};

long long nanos(sm::TimePoint time) {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

long long nanos(sm::Duration duration) {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

// ---------------------------------------------------------------------------
// Formatting of library values.
// ---------------------------------------------------------------------------
std::string describe(const sm::PayloadValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return fmt("i%lld", static_cast<long long>(*integer));
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return fmt("d%a", *real);
    }
    if (const auto* flag = std::get_if<bool>(&value)) {
        return *flag ? "btrue" : "bfalse";
    }
    return "s'" + std::get<std::string>(value) + "'";
}

std::string describe(const sm::Event& event) {
    std::string out =
        fmt("{id=%u seq=%llu cat=%d ts=%a corr=%llu src='%s' payload=[", u(event.id), ull(event.sequence),
            static_cast<int>(event.category), event.timestamp, ull(event.correlation_id), event.source.c_str());
    for (const auto& entry : event.payload) {
        out += entry.first + "=" + describe(entry.second) + ";";
    }
    return out + "]}";
}

std::string describe(const sm::Event* event) {
    return event == nullptr ? "null" : describe(*event);
}

std::string describe(const std::vector<sm::StateId>& path) {
    std::string out = "[";
    for (const auto id : path) {
        out.append(fmt("%u,", u(id)));
    }
    return out + "]";
}

std::string describe(const sm::Status& status) {
    return fmt("%d:'%s'", static_cast<int>(status.code), status.message.c_str());
}

template <typename T> std::string describe(const std::optional<T>& value) {
    return value ? fmt("%llu", ull(static_cast<std::uint64_t>(*value))) : "-";
}

// typeid names are compiler specific; keep only what identifies the exception.
std::string describeExceptionType(const std::string& name) {
    if (name.empty()) {
        return "-";
    }
    if (name.find("runtime_error") != std::string::npos) {
        return "runtime_error";
    }
    if (name.find("ScriptError") != std::string::npos) {
        return "ScriptError";
    }
    return "other";
}

std::string describe(const sm::FaultRecord& fault) {
    return fmt("{id=%llu ts=%lld evseq=%llu ev=%u state=%s tr=%s cb=%d sev=%d msg='%s' type=%s corr=%llu}",
               ull(fault.id), nanos(fault.timestamp), ull(fault.event_sequence), u(fault.triggering_event),
               describe(fault.state).c_str(), describe(fault.transition).c_str(), static_cast<int>(fault.callback_kind),
               static_cast<int>(fault.severity), fault.message.c_str(),
               describeExceptionType(fault.exception_type).c_str(), ull(fault.correlation_id));
}

std::string describe(const sm::EventLogRecord& record) {
    return fmt("{kind=%d seq=%llu ev=%u region=%u from=%u to=%u tr=%s msg='%s'}", static_cast<int>(record.kind),
               ull(record.sequence), u(record.event_id), u(record.region), u(record.from_state), u(record.to_state),
               describe(record.transition).c_str(), record.message.c_str());
}

std::string describe(const sm::ProcessedEventRecord& record) {
    return fmt("{ev=%s trig=%d region=%u from=%u to=%u tr=%s prio=%d}", describe(record.event).c_str(),
               record.triggered_transition ? 1 : 0, u(record.region), u(record.from_state), u(record.to_state),
               describe(record.transition).c_str(), record.priority);
}

std::string describe(const sm::EventTraceRecord& record) {
    return fmt("{kind=%d ev=%s prodr=%u prods=%u consr=%u from=%u to=%u tr=%s prio=%d}", static_cast<int>(record.kind),
               describe(record.event).c_str(), u(record.producer_region), u(record.producer_state),
               u(record.consumer_region), u(record.from_state), u(record.to_state), describe(record.transition).c_str(),
               record.priority);
}

std::string describe(const sm::UpdateResult& result) {
    return fmt("{status=%s taken=%zu processed=%zu remaining=%zu transitions=%zu generated=%zu faults=%zu "
               "evlimit=%d trlimit=%d lc=%d}",
               describe(result.status).c_str(), result.events_taken, result.events_processed, result.events_remaining,
               result.transitions_committed, result.generated_events, result.faults_recorded,
               result.hit_event_limit ? 1 : 0, result.hit_transition_limit ? 1 : 0, static_cast<int>(result.lifecycle));
}

std::string describe(const sm::TaskHandle& handle) {
    return fmt("{id=%llu owner=%u region=%u corr=%llu at=%lld}", ull(handle.id), u(handle.owner_state),
               u(handle.owner_region), ull(handle.correlation_id), nanos(handle.started_at));
}

// ---------------------------------------------------------------------------
// The scripted world shared by every callback of one scenario.
// ---------------------------------------------------------------------------
constexpr sm::EventId kEventPool[] = {100, 101, 102, 103, 104, 105, 106, 107};

struct ScriptError : std::exception {
    const char* what() const noexcept override { return "scripted failure"; }
};

// Scenario restrictions. The defaults keep every observable independent of the
// standard library and free of undefined behaviour in older revisions of the
// runtime, so the golden file is valid for all of them:
//   tick_only_internal  internal events are posted only from onTick() and start();
//                       posting from other callbacks while the runtime holds
//                       pointers into its per-tick storage is a use-after-free in
//                       revisions that keep that storage in a std::vector
//   grouped_rules       the rules of one source state are registered together; the
//                       runtime's rule comparator is a strict weak ordering only
//                       then, otherwise the order is whatever std::stable_sort
//                       leaves (libstdc++ and libc++ differ)
//   single_task         at most one task per machine; cancellation order follows
//                       std::unordered_map iteration
struct Restrictions {
    bool tick_only_internal{true};
    bool grouped_rules{true};
    bool single_task{true};

    static Restrictions none() { return Restrictions{false, false, false}; }
};

struct World {
    World(std::uint64_t seed, const Restrictions& restrictions) : rng(seed), limits(restrictions) {}

    Rng rng;
    Observer obs;
    Restrictions limits;
    StepClock* clock{nullptr};
    sm::StateMachine* machine{nullptr};
    bool in_start{
        false}; // callbacks run by start(): internal events go to the pending queue, never to per-tick storage
    std::uint32_t max_tasks{1};
    std::uint32_t tasks_started{0};
    std::vector<sm::TaskHandle> handles;
    int callbacks{0};

    sm::EventId pickEventId() {
        switch (rng.below(12)) {
        case 9:
            return sm::kTaskResultEvent;
        case 10:
            return sm::kFaultEvent;
        case 11:
            return sm::kStopRequestedEvent;
        default:
            return rng.pick(kEventPool);
        }
    }

    sm::Event makeEvent(sm::EventCategory category = sm::EventCategory::kInput) {
        sm::Event event(pickEventId());
        static const char* const kSources[] = {"",
                                               "a",
                                               "health",
                                               "mavros/local_position/pose",
                                               "alg/state_estimator/state",
                                               "a-source-name-that-is-longer-than-any-sso"};
        event.category = category;
        event.source = rng.pick(kSources);
        if (rng.chance(70)) {
            event.timestamp = static_cast<double>(rng.below(100000)) / 64.0;
        }
        event.correlation_id = rng.chance(30) ? rng.below(5) : 0;
        event.sequence = rng.below(1000); // the runtime must overwrite it
        const std::uint32_t entries = rng.below(4);
        for (std::uint32_t i = 0; i < entries; ++i) {
            const std::string key = fmt("k%u", rng.below(5));
            switch (rng.below(4)) {
            case 0:
                event.payload[key] = static_cast<std::int64_t>(rng.below(1000)) - 500;
                break;
            case 1:
                event.payload[key] = static_cast<double>(rng.below(1000)) / 8.0;
                break;
            case 2:
                event.payload[key] = rng.chance(50);
                break;
            default:
                event.payload[key] = std::string(rng.chance(50) ? "v" : "a-payload-string-longer-than-sso");
                break;
            }
        }
        return event;
    }

    sm::EventPayload makePayload() {
        sm::EventPayload payload;
        const std::uint32_t entries = rng.below(3);
        for (std::uint32_t i = 0; i < entries; ++i) {
            payload[fmt("p%u", rng.below(4))] = static_cast<std::int64_t>(rng.below(100));
        }
        return payload;
    }
};

enum class Tag { kEnter, kExit, kTick, kEvent, kAction, kGuard };

const char* tagName(Tag tag) {
    switch (tag) {
    case Tag::kEnter:
        return "enter";
    case Tag::kExit:
        return "exit";
    case Tag::kTick:
        return "tick";
    case Tag::kEvent:
        return "event";
    case Tag::kAction:
        return "action";
    case Tag::kGuard:
        return "guard";
    }
    return "?";
}

// Per-callback behaviour: percentages, drawn once per state or rule.
struct Behavior {
    std::uint32_t p_error{0};
    std::uint32_t p_throw{0};
    std::uint32_t p_internal{0};
    std::uint32_t p_output{0};
    std::uint32_t p_task{0};
    std::uint32_t p_query{0};
    std::uint32_t p_external{0};
    std::uint32_t p_reenter{0};
    std::uint32_t p_stop{0};
    std::uint32_t p_cancel{0};
};

Behavior randomBehavior(Rng& rng, std::uint32_t fault_level) {
    Behavior b;
    b.p_error = fault_level == 0 ? 0 : rng.below(2 * fault_level + 1);
    b.p_throw = fault_level == 0 ? 0 : rng.below(fault_level + 1);
    b.p_internal = rng.chance(40) ? rng.below(25) : 0;
    b.p_output = rng.chance(50) ? rng.below(40) : 0;
    b.p_task = rng.chance(30) ? rng.below(8) : 0;
    b.p_query = rng.below(40);
    b.p_external = rng.chance(20) ? rng.below(6) : 0;
    b.p_reenter = rng.chance(15) ? rng.below(4) : 0;
    b.p_stop = rng.chance(5) ? rng.below(2) : 0;
    b.p_cancel = rng.chance(20) ? rng.below(10) : 0;
    return b;
}

[[noreturn]] void throwRandom(World& w) {
    switch (w.rng.below(3)) {
    case 0:
        throw std::runtime_error("scripted runtime_error");
    case 1:
        throw ScriptError();
    default:
        throw 42; // not derived from std::exception
    }
}

// What a scripted callback does besides recording that it ran. Every random
// draw happens in a fixed order, so the sequence is a pure function of the
// seed and of the order in which the runtime invokes callbacks.
sm::Status act(World& w, const Behavior& b, sm::StateContext& ctx, Tag tag, unsigned who) {
    if (w.rng.chance(b.p_query)) {
        const sm::MachineSnapshot snapshot = ctx.snapshot();
        std::string leaves;
        for (const auto& entry : snapshot.active_leaf_states) {
            leaves.append(fmt("%u:%u,", u(entry.first), u(entry.second)));
        }
        // One clock read per statement: argument evaluation order is unspecified.
        const long long elapsed = nanos(ctx.elapsed(ctx.state()));
        const long long now = nanos(ctx.now());
        w.obs.line(fmt("  query lc=%d upd=%llu inbox=%zu leaves=%s cur=%u elapsed=%lld gen=%zu now=%lld",
                       static_cast<int>(snapshot.lifecycle), ull(snapshot.update_index), snapshot.inbox_size,
                       leaves.c_str(), u(ctx.currentState(ctx.region())), elapsed, ctx.generatedEvents(), now));
    }
    if (w.rng.chance(b.p_internal) && (!w.limits.tick_only_internal || tag == Tag::kTick || w.in_start)) {
        const sm::Status status = ctx.postInternalEvent(w.makeEvent());
        w.obs.line("  post_internal " + describe(status));
    }
    if (w.rng.chance(b.p_output)) {
        const sm::Status status = ctx.emitOutput(w.makeEvent());
        w.obs.line("  emit_output " + describe(status));
    }
    if (w.rng.chance(b.p_task) && w.tasks_started < w.max_tasks) {
        static const sm::TaskCancelPolicy kPolicies[] = {
            sm::TaskCancelPolicy::kKeepRunning, sm::TaskCancelPolicy::kCancelOnStateExit,
            sm::TaskCancelPolicy::kCancelOnRegionExit, sm::TaskCancelPolicy::kCancelOnMachineStop};
        const sm::TaskCancelPolicy policy = w.rng.pick(kPolicies);
        const sm::CorrelationId correlation = w.rng.chance(50) ? w.rng.below(4) : 0;
        const auto result = ctx.startTask(policy, correlation);
        w.obs.line("  start_task " + describe(result.status) + " " + describe(result.value));
        if (result.ok()) {
            ++w.tasks_started;
            w.handles.push_back(result.value);
        }
    }
    if (w.rng.chance(b.p_cancel) && !w.handles.empty()) {
        const sm::TaskHandle handle = w.handles[w.rng.below(static_cast<std::uint32_t>(w.handles.size()))];
        w.obs.line("  cancel_task " + describe(ctx.cancelTask(handle)));
    }
    if (w.rng.chance(b.p_external) && w.machine != nullptr) {
        w.obs.line("  post_external " + describe(w.machine->postEvent(w.makeEvent())));
    }
    if (w.rng.chance(b.p_reenter) && w.machine != nullptr) {
        const auto result = w.machine->update();
        w.obs.line("  reenter_update " + describe(result.status) + " " + describe(result.value));
    }
    if (w.rng.chance(b.p_stop) && w.machine != nullptr) {
        w.obs.line("  stop_from_callback " + describe(w.machine->stop()));
    }
    if (w.rng.chance(b.p_throw)) {
        w.obs.line(fmt("  throw (%s #%u)", tagName(tag), who));
        throwRandom(w);
    }
    if (w.rng.chance(b.p_error)) {
        static const sm::ErrorCode kCodes[] = {sm::ErrorCode::kInvalidArgument, sm::ErrorCode::kNotFound,
                                               sm::ErrorCode::kTransitionRejected, sm::ErrorCode::kLimitReached};
        w.obs.line(fmt("  fail (%s #%u)", tagName(tag), who));
        return sm::Status::error(w.rng.pick(kCodes), fmt("scripted failure in %s of %u", tagName(tag), who));
    }
    return {};
}

class ScriptedState final : public sm::State {
  public:
    ScriptedState(World& world, sm::StateId id, std::string name, Behavior behavior)
        : world_(world), id_(id), name_(std::move(name)), behavior_(behavior) {}

    std::string name() const override { return name_; }

    sm::ActionResult onEnter(sm::StateContext& ctx) override { return run(ctx, Tag::kEnter, nullptr); }
    sm::ActionResult onExit(sm::StateContext& ctx) override { return run(ctx, Tag::kExit, nullptr); }
    sm::ActionResult onTick(sm::StateContext& ctx) override { return run(ctx, Tag::kTick, nullptr); }
    sm::ActionResult onEvent(sm::StateContext& ctx, const sm::Event& event) override {
        return run(ctx, Tag::kEvent, &event);
    }

  private:
    sm::ActionResult run(sm::StateContext& ctx, Tag tag, const sm::Event* event) {
        ++world_.callbacks;
        world_.obs.line(fmt("cb %s state=%u ctx.region=%u ctx.state=%u gen=%zu ctx.event=%s arg=%s", tagName(tag),
                            u(id_), u(ctx.region()), u(ctx.state()), ctx.generatedEvents(),
                            describe(ctx.event()).c_str(), describe(event).c_str()));
        return act(world_, behavior_, ctx, tag, u(id_));
    }

    World& world_;
    sm::StateId id_;
    std::string name_;
    Behavior behavior_;
};

// ---------------------------------------------------------------------------
// Random machine: a tree of regions and states, parallel and nested, plus a
// random transition table.
// ---------------------------------------------------------------------------
struct TState;

struct TRegion {
    sm::RegionId id{0}; // 0 = implicit child region created by the builder
    int order{0};
    std::vector<TState> states;
    std::size_t initial{0};
};

struct TState {
    sm::StateId id{0};
    std::vector<TRegion> children; // empty: leaf; one region with id 0: implicit; several: parallel regions
    std::uint32_t name_mode{0};
};

struct Topology {
    std::vector<TRegion> top;
    std::vector<sm::StateId> all_states;
    std::map<sm::StateId, std::size_t> top_index;        // state -> index into `top`
    std::map<sm::StateId, sm::RegionId> declared_region; // explicit region id or 0
};

class TopologyGenerator {
  public:
    explicit TopologyGenerator(Rng& rng) : rng_(rng) {}

    Topology generate() {
        Topology topology;
        const std::uint32_t top_regions = 1 + rng_.below(3);
        static const sm::RegionId kTopIds[] = {1, 2, 3, 7, 9};
        std::vector<sm::RegionId> ids(std::begin(kTopIds), std::end(kTopIds));
        for (std::uint32_t i = 0; i < top_regions; ++i) {
            TRegion region;
            const auto pick = rng_.below(static_cast<std::uint32_t>(ids.size()));
            region.id = ids[pick];
            ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(pick));
            static const int kOrders[] = {0, 5, 10, 10, 20};
            region.order = rng_.pick(kOrders);
            fillRegion(region, 0, 1 + rng_.below(4));
            topology.top.push_back(std::move(region));
        }
        for (std::size_t i = 0; i < topology.top.size(); ++i) {
            collect(topology, topology.top[i], i, topology.top[i].id);
        }
        return topology;
    }

  private:
    void fillRegion(TRegion& region, int depth, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) {
            TState state;
            state.id = next_state_++;
            state.name_mode = rng_.below(4);
            if (depth < 2 && rng_.chance(35)) {
                if (rng_.chance(60)) {
                    TRegion child;
                    fillRegion(child, depth + 1, 1 + rng_.below(3));
                    state.children.push_back(std::move(child));
                } else {
                    const std::uint32_t parallel = 2 + rng_.below(2);
                    for (std::uint32_t r = 0; r < parallel; ++r) {
                        TRegion child;
                        child.id = next_region_++;
                        static const int kOrders[] = {0, 3, 3, 8};
                        child.order = rng_.pick(kOrders);
                        fillRegion(child, depth + 1, 1 + rng_.below(3));
                        state.children.push_back(std::move(child));
                    }
                }
            }
            region.states.push_back(std::move(state));
        }
        region.initial = rng_.below(count);
    }

    void collect(Topology& topology, const TRegion& region, std::size_t top, sm::RegionId declared) {
        for (const TState& state : region.states) {
            topology.all_states.push_back(state.id);
            topology.top_index[state.id] = top;
            topology.declared_region[state.id] = declared;
            for (const TRegion& child : state.children) {
                collect(topology, child, top, child.id);
            }
        }
    }

    Rng& rng_;
    sm::StateId next_state_{10};
    sm::RegionId next_region_{20};
};

std::function<bool(const sm::GuardContext&)> makeGuard(World& world_ref, unsigned index, std::uint32_t p_true,
                                                       const Behavior& behavior) {
    World* world = &world_ref;
    return [world, index, p_true, behavior](const sm::GuardContext& guard) {
        ++world->callbacks;
        world->obs.line(fmt("guard rule=%u ev=%s", index, describe(guard.event()).c_str()));
        if (world->rng.chance(behavior.p_query)) {
            const sm::MachineSnapshot snapshot = guard.snapshot();
            const long long now = nanos(guard.now());
            const long long elapsed = nanos(guard.elapsed(10));
            world->obs.line(fmt("  guard_query lc=%d upd=%llu now=%lld elapsed=%lld",
                                static_cast<int>(snapshot.lifecycle), ull(snapshot.update_index), now, elapsed));
        }
        if (world->rng.chance(behavior.p_throw)) {
            world->obs.line("  guard_throw");
            throwRandom(*world);
        }
        const bool result = world->rng.chance(p_true);
        world->obs.line(fmt("  guard_result %d", result ? 1 : 0));
        return result;
    };
}

class MachineFactory {
  public:
    MachineFactory(World& world, std::uint32_t fault_level) : world_(world), fault_level_(fault_level) {}

    // Returns nullptr (and notes why) if the builder rejects the machine.
    std::unique_ptr<sm::StateMachine> build(const Topology& topology, const sm::RuntimeOptions& options,
                                            const std::shared_ptr<sm::Clock>& clock) {
        auto builder = sm::StateMachine::builder("random", options, clock);
        for (const TRegion& region : topology.top) {
            builder.region(region.id).order(region.order);
            if (world_.rng.chance(50)) {
                builder.name(fmt("top%u", u(region.id)));
            }
            emitRegion(builder, region);
            builder.endRegion();
        }
        addTransitions(builder, topology);
        auto result = builder.build();
        if (!result.ok()) {
            world_.obs.note("build failed: " + describe(result.status));
            return nullptr;
        }
        return std::move(result.value);
    }

  private:
    void emitRegion(sm::StateMachine::Builder& builder, const TRegion& region) {
        builder.initial(region.states[region.initial].id);
        for (const TState& state : region.states) {
            emitState(builder, state);
        }
    }

    void emitState(sm::StateMachine::Builder& builder, const TState& state) {
        builder.state(state.id);
        if (state.name_mode == 1) {
            builder.name(fmt("S%u", u(state.id)));
        }
        builder.impl(std::make_unique<ScriptedState>(world_, state.id, fmt("impl%u", u(state.id)),
                                                     randomBehavior(world_.rng, fault_level_)));
        if (state.name_mode == 2) {
            builder.name(fmt("Renamed%u", u(state.id)));
        }
        if (state.children.empty()) {
            return;
        }
        if (state.children.size() == 1 && state.children[0].id == 0) {
            emitRegion(builder, state.children[0]);
        } else {
            for (const TRegion& child : state.children) {
                builder.region(child.id).order(child.order);
                emitRegion(builder, child);
                builder.endRegion();
            }
        }
        builder.endState();
    }

    struct RuleSpec {
        unsigned index{0};
        sm::StateId from{0};
        std::optional<sm::StateId> to;
        std::optional<sm::EventId> event;
        std::optional<std::uint32_t> guard_percent;
        Behavior guard_behavior;
        int priority{0};
        std::optional<int> evaluation_order;
        sm::TransitionType type{sm::TransitionType::kExternal};
        bool global{false};
        std::optional<Behavior> action_behavior;
    };

    // With grouped_rules each source state's rules are registered together (see
    // Restrictions); without it the interleaving locks the exact order the
    // library at hand produces.
    void addTransitions(sm::StateMachine::Builder& builder, const Topology& topology) {
        Rng& rng = world_.rng;
        std::map<std::size_t, std::vector<sm::StateId>> by_top;
        for (const sm::StateId id : topology.all_states) {
            by_top[topology.top_index.at(id)].push_back(id);
        }
        std::vector<RuleSpec> specs;
        const std::size_t rules = topology.all_states.size() * (1 + rng.below(3));
        for (std::size_t index = 0; index < rules; ++index) {
            RuleSpec spec;
            spec.index = static_cast<unsigned>(index);
            spec.from = topology.all_states[rng.below(static_cast<std::uint32_t>(topology.all_states.size()))];
            const auto& siblings = by_top.at(topology.top_index.at(spec.from));
            if (rng.chance(80)) {
                spec.to = siblings[rng.below(static_cast<std::uint32_t>(siblings.size()))];
            }
            if (rng.chance(60)) {
                spec.event = world_.pickEventId();
            }
            if (!spec.event || rng.chance(50)) {
                spec.guard_percent = rng.chance(30) ? 100 : rng.below(60);
                spec.guard_behavior.p_query = rng.below(30);
                spec.guard_behavior.p_throw = fault_level_ == 0 ? 0 : rng.below(fault_level_ + 1);
            }
            static const int kPriorities[] = {0, 0, 10, 20, 50, 90, 100};
            spec.priority = rng.pick(kPriorities);
            if (rng.chance(40)) {
                spec.evaluation_order = static_cast<int>(rng.below(4));
            }
            static const sm::TransitionType kTypes[] = {sm::TransitionType::kExternal, sm::TransitionType::kExternal,
                                                        sm::TransitionType::kExternalSelf,
                                                        sm::TransitionType::kInternal, sm::TransitionType::kTargetless};
            spec.type = rng.pick(kTypes);
            spec.global = rng.chance(10);
            if (rng.chance(45)) {
                spec.action_behavior = randomBehavior(rng, fault_level_);
            }
            specs.push_back(spec);
        }
        if (world_.limits.grouped_rules) {
            std::vector<RuleSpec> grouped;
            std::vector<bool> taken(specs.size(), false);
            for (std::size_t i = 0; i < specs.size(); ++i) {
                if (taken[i]) {
                    continue;
                }
                for (std::size_t j = i; j < specs.size(); ++j) {
                    if (!taken[j] && specs[j].from == specs[i].from) {
                        taken[j] = true;
                        grouped.push_back(specs[j]);
                    }
                }
            }
            specs = std::move(grouped);
        }
        for (const RuleSpec& spec : specs) {
            builder.transition().from(spec.from);
            if (spec.to) {
                builder.to(*spec.to);
            }
            if (spec.event) {
                builder.on(*spec.event);
            }
            if (spec.guard_percent) {
                builder.when(makeGuard(world_, spec.index, *spec.guard_percent, spec.guard_behavior));
            }
            builder.priority(spec.priority);
            if (spec.evaluation_order) {
                builder.evaluationOrder(*spec.evaluation_order);
            }
            builder.type(spec.type);
            if (spec.global) {
                builder.global();
            }
            if (spec.action_behavior) {
                builder.action(makeAction(spec.index, *spec.action_behavior));
            }
        }
    }

    std::function<sm::ActionResult(sm::StateContext&)> makeAction(unsigned index, const Behavior& behavior) {
        World* world = &world_;
        return [world, index, behavior](sm::StateContext& ctx) {
            ++world->callbacks;
            world->obs.line(fmt("cb action rule=%u ctx.region=%u ctx.state=%u gen=%zu ctx.event=%s", index,
                                u(ctx.region()), u(ctx.state()), ctx.generatedEvents(), describe(ctx.event()).c_str()));
            return act(*world, behavior, ctx, Tag::kAction, 1000 + index);
        };
    }

    World& world_;
    std::uint32_t fault_level_;
};

// ---------------------------------------------------------------------------
// Observation of the whole machine: everything the public API can report.
// ---------------------------------------------------------------------------
struct LifeInfo {
    std::vector<sm::StateId> states;
    std::map<sm::StateId, sm::RegionId> top_region;
    std::map<sm::StateId, sm::RegionId> declared_region;
};

void dumpMachine(World& w, const sm::StateMachine& machine, const LifeInfo& info, bool deep) {
    Observer& obs = w.obs;
    const sm::MachineSnapshot snapshot = machine.snapshot();
    obs.line(fmt("snap lc=%d upd=%llu inbox=%zu faults=%zu", static_cast<int>(snapshot.lifecycle),
                 ull(snapshot.update_index), snapshot.inbox_size, snapshot.recent_faults.size()));
    for (const auto& entry : snapshot.active_leaf_states) {
        const auto path = snapshot.active_state_paths.find(entry.first);
        obs.line(fmt("snap region=%u leaf=%u path=%s", u(entry.first), u(entry.second),
                     path == snapshot.active_state_paths.end() ? "missing" : describe(path->second).c_str()));
    }
    for (const auto& fault : snapshot.recent_faults) {
        obs.line("snap fault " + describe(fault));
    }
    const long long now = nanos(machine.now());
    obs.line(fmt("m name=%s lc=%d generated=%zu clock=%llu now=%lld", machine.name().c_str(),
                 static_cast<int>(machine.lifecycle()), machine.generatedEventCount(), ull(w.clock->reads()), now));
    for (const auto& entry : snapshot.active_leaf_states) {
        obs.line(fmt("q region=%u cur=%u name='%s' path=%s", u(entry.first), u(machine.currentState(entry.first)),
                     machine.currentStateName(entry.first).c_str(),
                     describe(machine.currentStatePath(entry.first)).c_str()));
    }
    obs.line(fmt("q default cur=%u name='%s' path=%s", u(machine.currentState()), machine.currentStateName().c_str(),
                 describe(machine.currentStatePath()).c_str()));
    for (const sm::StateId state : info.states) {
        const sm::RegionId top = info.top_region.at(state);
        const sm::RegionId own = info.declared_region.at(state);
        std::string line =
            fmt("q state=%u top=%d own=%d elapsed=%lld", u(state), machine.isActiveInPath({top, state}) ? 1 : 0,
                machine.isActiveInPath({own, state}) ? 1 : 0, nanos(machine.elapsed(state)));
        if (deep) {
            for (const auto& entry : snapshot.active_leaf_states) {
                line.append(machine.isActiveInPath({entry.first, state}) ? "1" : "0");
            }
        }
        obs.line(line);
    }
    for (const auto& record : machine.eventLog()) {
        obs.line("log " + describe(record));
    }
    for (const auto& fault : machine.faultLog()) {
        obs.line("faultlog " + describe(fault));
    }
    for (const auto& record : machine.currentEvents()) {
        obs.line("processed " + describe(record));
    }
    for (const auto& record : machine.currentTrace()) {
        obs.line("trace " + describe(record));
    }
    for (const auto& event : machine.currentOutputEvents()) {
        obs.line("output " + describe(event));
    }
}

std::string pathsSummary(const sm::MachineSnapshot& snapshot) {
    std::string out;
    for (const auto& entry : snapshot.active_state_paths) {
        out += fmt("%u:%s ", u(entry.first), describe(entry.second).c_str());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Scenario: random machines.
// ---------------------------------------------------------------------------
struct RandomRun {
    std::vector<std::string> checkpoints;
    std::uint64_t final_hash{0};
    std::string text;
    std::vector<std::uint64_t> step_hashes;
    std::uint64_t callbacks{0};
};

sm::UpdateOptions randomUpdateOptions(Rng& rng) {
    static const std::size_t kEvents[] = {0, 1, 2, 3, 64, 64};
    static const std::size_t kTransitions[] = {0, 1, 2, 64, 64};
    sm::UpdateOptions options;
    options.max_events_per_update = rng.pick(kEvents);
    options.max_transitions_per_update = rng.pick(kTransitions);
    options.run_tick = rng.chance(80);
    return options;
}

sm::RuntimeOptions randomRuntimeOptions(Rng& rng) {
    static const std::size_t kEventLog[] = {0, 1, 3, 8, 24, 64};
    static const std::size_t kFaultLog[] = {0, 1, 2, 5, 16};
    static const std::size_t kPending[] = {0, 1, 3, 6, 4096};
    static const std::size_t kDepth[] = {0, 1, 2, 3, 8};
    sm::RuntimeOptions options;
    options.event_log_capacity = rng.pick(kEventLog);
    options.fault_log_capacity = rng.pick(kFaultLog);
    options.max_pending_events = rng.pick(kPending);
    options.allow_prestart_events = rng.chance(30);
    options.max_fault_depth = rng.pick(kDepth);
    return options;
}

class RandomDriver {
  public:
    RandomDriver(std::uint64_t seed, const Restrictions& restrictions, int steps, int checkpoint_every, bool keep_text)
        : world_(seed, restrictions), steps_(steps), checkpoint_every_(checkpoint_every) {
        world_.obs.keepText(keep_text);
    }

    RandomRun run() {
        RandomRun out;
        int life = 0;
        while (step_ < steps_) {
            runLife(life++, out);
        }
        out.final_hash = world_.obs.hash();
        out.text = world_.obs.text();
        out.callbacks = static_cast<std::uint64_t>(world_.callbacks);
        return out;
    }

  private:
    void endStep(RandomRun& out) {
        ++step_;
        out.step_hashes.push_back(world_.obs.hash());
        if (checkpoint_every_ > 0 && step_ % checkpoint_every_ == 0) {
            out.checkpoints.push_back(fmt("step %d hash 0x%016llx", step_, ull(world_.obs.hash())));
        }
    }

    void runLife(int life, RandomRun& out) {
        Rng& rng = world_.rng;
        World& w = world_;
        w.obs.line(fmt("== life %d", life));
        w.handles.clear();
        w.tasks_started = 0;
        w.max_tasks = w.limits.single_task ? 1 : 12;

        static const std::uint32_t kFaultLevels[] = {0, 0, 1, 3};
        const std::uint32_t fault_level = rng.pick(kFaultLevels);
        TopologyGenerator generator(rng);
        const Topology topology = generator.generate();
        const sm::RuntimeOptions options = randomRuntimeOptions(rng);
        w.obs.line(fmt("options evlog=%zu faultlog=%zu pending=%zu prestart=%d depth=%zu states=%zu faults=%u",
                       options.event_log_capacity, options.fault_log_capacity, options.max_pending_events,
                       options.allow_prestart_events ? 1 : 0, options.max_fault_depth, topology.all_states.size(),
                       fault_level));
        auto clock = std::make_shared<StepClock>();
        w.clock = clock.get();
        MachineFactory factory(w, fault_level);
        auto machine = factory.build(topology, options, clock);
        if (!machine) {
            w.machine = nullptr;
            endStep(out);
            return;
        }
        w.machine = machine.get();

        LifeInfo info;
        info.states = topology.all_states;
        for (const sm::StateId id : topology.all_states) {
            info.top_region[id] = topology.top[topology.top_index.at(id)].id;
            info.declared_region[id] = topology.declared_region.at(id);
        }
        dumpMachine(w, *machine, info, true);
        endStep(out);

        if (rng.chance(30)) {
            w.obs.line("prestart: post " + describe(machine->postEvent(w.makeEvent())));
            const auto prestart_update = machine->update();
            w.obs.line("prestart: update " + describe(prestart_update.status) + " " + describe(prestart_update.value));
            dumpMachine(w, *machine, info, false);
            endStep(out);
        }
        if (rng.chance(10)) {
            w.obs.line("bind " + describe(machine->bindOwnerThread()));
        }
        if (rng.chance(4)) {
            w.obs.line("stop before start " + describe(machine->stop()));
        }
        w.in_start = true;
        w.obs.line("start " + describe(machine->start()));
        w.in_start = false;
        dumpMachine(w, *machine, info, true);
        endStep(out);
        if (rng.chance(10)) {
            w.obs.line("start again " + describe(machine->start()));
        }

        const int life_steps = 60 + static_cast<int>(rng.below(240));
        int terminal_steps = 0;
        for (int i = 0; i < life_steps && step_ < steps_ && terminal_steps < 8; ++i) {
            const sm::MachineLifecycle lifecycle = machine->lifecycle();
            if (lifecycle == sm::MachineLifecycle::kStopped || lifecycle == sm::MachineLifecycle::kFaulted) {
                ++terminal_steps;
            }
            stepOnce(*machine, i);
            dumpMachine(w, *machine, info, (step_ % 8) == 0);
            endStep(out);
        }
        w.machine = nullptr;
    }

    void stepOnce(sm::StateMachine& machine, int index) {
        World& w = world_;
        Rng& rng = w.rng;
        const std::uint32_t choice = rng.below(100);
        if (choice < 35) {
            const sm::Event event =
                w.makeEvent(rng.chance(4) ? (rng.chance(50) ? sm::EventCategory::kInternal : sm::EventCategory::kOutput)
                                          : sm::EventCategory::kInput);
            w.obs.line("op post " + describe(event));
            w.obs.line("  -> " + describe(machine.postEvent(event)));
        } else if (choice < 70) {
            doUpdate(machine, randomUpdateOptions(rng));
        } else if (choice < 78) {
            const std::uint32_t burst = 2 + rng.below(8);
            w.obs.line(fmt("op burst %u", burst));
            for (std::uint32_t i = 0; i < burst; ++i) {
                w.obs.line("  -> " + describe(machine.postEvent(w.makeEvent())));
            }
        } else if (choice < 83) {
            if (!w.handles.empty()) {
                sm::TaskHandle handle = w.handles[rng.below(static_cast<std::uint32_t>(w.handles.size()))];
                if (rng.chance(20)) {
                    handle.correlation_id += 1;
                }
                if (rng.chance(10)) {
                    handle.owner_state += 1;
                }
                static const sm::TaskStatus kStatuses[] = {sm::TaskStatus::kCompleted, sm::TaskStatus::kFailed,
                                                           sm::TaskStatus::kCancelled, sm::TaskStatus::kTimeout,
                                                           sm::TaskStatus::kStale};
                w.obs.line("op task_result " + describe(handle));
                const sm::TaskStatus status = rng.pick(kStatuses);
                w.obs.line("  -> " + describe(machine.postTaskResult(handle, status, w.makePayload())));
            }
        } else if (choice < 85) {
            if (!w.handles.empty()) {
                sm::TaskHandle handle = w.handles[rng.below(static_cast<std::uint32_t>(w.handles.size()))];
                if (rng.chance(15)) {
                    handle.id += 1000;
                }
                w.obs.line("op cancel_task " + describe(handle));
                w.obs.line("  -> " + describe(machine.cancelTask(handle)));
            }
        } else if (choice < 87) {
            foreignThreadOp(machine);
        } else if (choice < 88) {
            if (index > 10) {
                w.obs.line("op stop " + describe(machine.stop()));
            }
        } else if (choice < 89) {
            w.obs.line("op bind " + describe(machine.bindOwnerThread()));
        } else if (choice < 90) {
            w.obs.line("op observe");
        } else {
            sm::UpdateOptions options;
            options.max_events_per_update = 4096;
            options.max_transitions_per_update = 4096;
            doUpdate(machine, options);
        }
    }

    void doUpdate(sm::StateMachine& machine, const sm::UpdateOptions& options) {
        World& w = world_;
        w.obs.line(fmt("op update events=%zu transitions=%zu tick=%d", options.max_events_per_update,
                       options.max_transitions_per_update, options.run_tick ? 1 : 0));
        const auto result = machine.update(options);
        w.obs.line("  -> " + describe(result.status) + " " + describe(result.value));
    }

    void foreignThreadOp(sm::StateMachine& machine) {
        World& w = world_;
        const std::uint32_t which = w.rng.below(4);
        std::string outcome;
        std::thread worker([&] {
            switch (which) {
            case 0: {
                const auto result = machine.update();
                outcome = "update " + describe(result.status) + " " + describe(result.value);
                break;
            }
            case 1:
                outcome = "bind " + describe(machine.bindOwnerThread());
                break;
            case 2:
                outcome = "start " + describe(machine.start());
                break;
            default:
                outcome = "snapshot upd=" + fmt("%llu", ull(machine.snapshot().update_index));
                break;
            }
        });
        worker.join();
        w.obs.line("op foreign " + outcome);
    }

    World world_;
    int steps_;
    int checkpoint_every_;
    int step_{0};
};

// ---------------------------------------------------------------------------
// Scenario: scripted flight through the controller-shaped machine.
// ---------------------------------------------------------------------------
namespace cl = controller_like;

struct FlightRun {
    std::vector<std::string> summary;
    std::uint64_t full_hash{0};
    std::string text;
};

FlightRun runControllerFlight(bool keep_text) {
    World w(1, Restrictions{});
    w.obs.keepText(keep_text);
    auto clock = std::make_shared<StepClock>();
    w.clock = clock.get();
    cl::Flags flags;
    flags.setpoint_every = 3;
    auto machine = cl::buildControllerLikeMachine(flags, clock);
    w.machine = machine.get();

    LifeInfo info;
    info.states = {cl::state_id::kSelfCheck,       cl::state_id::kReady,      cl::state_id::kTakeoffInit,
                   cl::state_id::kTakeoffOffboard, cl::state_id::kTakeoffArm, cl::state_id::kTakeoffAscending,
                   cl::state_id::kHover,           cl::state_id::kNormal,     cl::state_id::kLanding,
                   cl::state_id::kTakeoff,         cl::state_id::kCustom1,    cl::state_id::kHealthMonitor,
                   cl::state_id::kDebugMonitor};
    for (const sm::StateId id : info.states) {
        info.top_region[id] = cl::region_id::kControl;
        info.declared_region[id] = cl::region_id::kControl;
    }
    info.top_region[cl::state_id::kHealthMonitor] = cl::region_id::kHealth;
    info.declared_region[cl::state_id::kHealthMonitor] = cl::region_id::kHealth;
    info.top_region[cl::state_id::kDebugMonitor] = cl::region_id::kDebug;
    info.declared_region[cl::state_id::kDebugMonitor] = cl::region_id::kDebug;

    int step = 0;
    const auto observe = [&](const std::string& what, const sm::Status& status, bool verbose) {
        const auto snapshot = machine->snapshot();
        w.obs.note(
            fmt("%02d %s -> %s | %s", step, what.c_str(), describe(status).c_str(), pathsSummary(snapshot).c_str()));
        if (verbose) {
            std::string events;
            for (const auto& record : machine->currentEvents()) {
                events +=
                    fmt("%u@%u:%u>%u", u(record.event.id), u(record.region), u(record.from_state), u(record.to_state));
                if (record.triggered_transition) {
                    events += fmt("(t%llu)", ull(record.transition.value_or(0)));
                }
                events += ' ';
            }
            std::string outputs;
            for (const auto& event : machine->currentOutputEvents()) {
                outputs.append(fmt("%u ", u(event.id)));
            }
            w.obs.note("     processed: " + events);
            w.obs.note("     outputs: " + outputs);
        }
        dumpMachine(w, *machine, info, false);
        ++step;
    };
    const auto update = [&](const std::string& what, sm::UpdateOptions options = sm::UpdateOptions{}) {
        ++flags.tick;
        flags.now += 0.001;
        const auto result = machine->update(options);
        observe(what + " " + describe(result.value), result.status, true);
    };
    const auto post = [&](sm::EventId id, const char* source, const std::string& what) {
        sm::Event event(id, sm::EventTimestamp{flags.now});
        event.source = source;
        observe("post " + what, machine->postEvent(std::move(event)), false);
    };

    observe("start", machine->start(), true);
    update("update");
    update("update");
    post(cl::input_event::kLocalPosition, "mavros/local_position/pose", "pose");
    post(cl::input_event::kImu, "mavros/imu/data", "imu");
    post(cl::input_event::kStateEstimate, "alg/state_estimator/state", "estimate");
    update("update with sensor events");
    post(cl::input_event::kTakeoffRequested, "command", "takeoff");
    update("takeoff");
    update("settle");
    post(cl::input_event::kAltctlReady, "mavros/state", "altctl");
    update("altctl");
    post(cl::input_event::kOffboardReady, "mavros/state", "offboard");
    update("offboard");
    post(cl::input_event::kArmReady, "mavros/state", "arm");
    update("arm");
    post(cl::input_event::kAltitudeReached, "takeoff", "altitude");
    update("hover");
    post(cl::input_event::kTrajectoryTrackingRequested, "command", "tracking");
    update("tracking");
    for (int i = 0; i < 6; ++i) {
        post(cl::input_event::kMpcTrajectory, "alg/setpoint_raw/local", "mpc");
        post(cl::input_event::kVrpnPose, "pose", "vrpn");
    }
    sm::UpdateOptions limited;
    limited.max_events_per_update = 5;
    update("limited to 5 events", limited);
    update("limited to 5 events", limited);
    update("drain");
    flags.internal_every = 2;
    update("health posts an unused internal event");
    update("health posts an unused internal event");
    flags.internal_every = 0;
    flags.safety_event = 40; // SAFE_GEOFENCE_VIOLATION: Normal -> Landing, raised in an earlier region
    update("geofence violation");
    update("landing");
    post(cl::input_event::kTouchdown, "landing", "touchdown");
    update("touchdown");
    flags.distance_exceeded = true;
    post(cl::input_event::kTakeoffRequested, "command", "takeoff while too far");
    update("takeoff rejected by guard");
    flags.distance_exceeded = false;
    sm::UpdateOptions no_tick;
    no_tick.run_tick = false;
    update("no tick", no_tick);
    sm::UpdateOptions one_transition;
    one_transition.max_transitions_per_update = 1;
    post(cl::input_event::kTakeoffRequested, "command", "takeoff again");
    update("one transition", one_transition);
    observe("stop", machine->stop(), false);
    update("exit all");
    update("after stop");
    observe("post after stop", machine->postEvent(sm::Event(cl::input_event::kImu)), false);

    FlightRun run;
    run.summary = w.obs.summary();
    run.full_hash = w.obs.hash();
    run.text = w.obs.text();
    return run;
}

// ---------------------------------------------------------------------------
// Scenario table, golden file, driver.
// ---------------------------------------------------------------------------
constexpr int kRandomSteps = 700;
constexpr int kCheckpointEvery = 50;

std::vector<std::uint64_t> randomSeeds() {
    std::vector<std::uint64_t> out;
    for (std::uint64_t i = 1; i <= 24; ++i) {
        out.push_back(i * 7919ULL);
    }
    return out;
}

// Golden lines carry no trailing blanks, so the file passes `git diff --check`.
std::string rtrim(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

std::vector<std::string> computeGolden(const Restrictions& restrictions) {
    std::vector<std::string> lines;
    lines.emplace_back("# xgc2 state_machine equivalence golden v1; regenerate with --update");
    lines.push_back(fmt("# restrictions tick_only_internal=%d grouped_rules=%d single_task=%d",
                        restrictions.tick_only_internal ? 1 : 0, restrictions.grouped_rules ? 1 : 0,
                        restrictions.single_task ? 1 : 0));
    const FlightRun flight = runControllerFlight(false);
    lines.emplace_back("[controller_flight]");
    lines.insert(lines.end(), flight.summary.begin(), flight.summary.end());
    lines.push_back(fmt("full_hash 0x%016llx", ull(flight.full_hash)));
    const std::vector<std::uint64_t> seeds = randomSeeds();
    for (std::size_t i = 0; i < seeds.size(); ++i) {
        RandomDriver driver(seeds[i], restrictions, kRandomSteps, kCheckpointEvery, false);
        const RandomRun run = driver.run();
        lines.push_back(fmt("[random.%zu seed=%llu steps=%d]", i + 1, ull(seeds[i]), kRandomSteps));
        lines.insert(lines.end(), run.checkpoints.begin(), run.checkpoints.end());
        lines.push_back(fmt("final 0x%016llx callbacks %llu", ull(run.final_hash), ull(run.callbacks)));
    }
    std::transform(lines.begin(), lines.end(), lines.begin(), rtrim);
    return lines;
}

bool readLines(const std::string& path, std::vector<std::string>& lines) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return true;
}

int verify(const std::string& path, const Restrictions& restrictions) {
    std::vector<std::string> expected;
    if (!readLines(path, expected)) {
        std::fprintf(stderr, "cannot read golden file %s\n", path.c_str());
        return 2;
    }
    const std::vector<std::string> actual = computeGolden(restrictions);
    const std::size_t common = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) {
            std::fprintf(stderr, "golden mismatch at line %zu\n  expected: %s\n  actual:   %s\n", i + 1,
                         expected[i].c_str(), actual[i].c_str());
            return 1;
        }
    }
    if (expected.size() != actual.size()) {
        std::fprintf(stderr, "golden length mismatch: expected %zu lines, got %zu\n", expected.size(), actual.size());
        return 1;
    }
    std::printf("equivalence: %zu golden lines match\n", actual.size());
    return 0;
}

int writeGolden(const std::string& path, const Restrictions& restrictions) {
    const std::vector<std::string> lines = computeGolden(restrictions);
    std::ofstream out(path);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return 2;
    }
    for (const std::string& line : lines) {
        out << line << '\n';
    }
    std::printf("wrote %zu lines to %s\n", lines.size(), path.c_str());
    return 0;
}

int dump(const std::string& name, const Restrictions& restrictions, int steps_override, bool hashes_only) {
    if (name == "controller_flight") {
        const FlightRun run = runControllerFlight(true);
        std::fputs(run.text.c_str(), stdout);
        std::printf("full_hash 0x%016llx\n", ull(run.full_hash));
        return 0;
    }
    unsigned long long parsed = 0;
    std::uint64_t seed = 0;
    int steps = kRandomSteps;
    if (std::sscanf(name.c_str(), "seed:%llu", &parsed) == 1) {
        seed = parsed; // explicit seed, any step count
    } else if (std::sscanf(name.c_str(), "random.%llu", &parsed) == 1) {
        const std::vector<std::uint64_t> seeds = randomSeeds();
        if (parsed == 0 || parsed > seeds.size()) {
            std::fprintf(stderr, "unknown scenario %s\n", name.c_str());
            return 2;
        }
        seed = seeds[parsed - 1];
    } else {
        std::fprintf(stderr, "unknown scenario %s\n", name.c_str());
        return 2;
    }
    if (steps_override > 0) {
        steps = steps_override;
    }
    RandomDriver driver(seed, restrictions, steps, 0, !hashes_only);
    const RandomRun run = driver.run();
    if (hashes_only) {
        for (std::size_t i = 0; i < run.step_hashes.size(); ++i) {
            std::printf("%zu 0x%016llx\n", i + 1, ull(run.step_hashes[i]));
        }
    } else {
        std::fputs(run.text.c_str(), stdout);
    }
    std::printf("final 0x%016llx callbacks %llu\n", ull(run.final_hash), ull(run.callbacks));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Restrictions restrictions;
    bool hashes_only = false;
    int steps = 0;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--full") == 0) {
            restrictions = Restrictions::none();
        } else if (std::strcmp(argv[i], "--internal-anywhere") == 0) {
            restrictions.tick_only_internal = false;
        } else if (std::strcmp(argv[i], "--interleaved-rules") == 0) {
            restrictions.grouped_rules = false;
        } else if (std::strcmp(argv[i], "--many-tasks") == 0) {
            restrictions.single_task = false;
        } else if (std::strcmp(argv[i], "--hashes") == 0) {
            hashes_only = true;
        } else if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
            steps = std::atoi(argv[++i]);
        } else {
            args.emplace_back(argv[i]);
        }
    }
    if (args.size() == 2 && args[0] == "--update") {
        return writeGolden(args[1], restrictions);
    }
    if (args.size() == 2 && args[0] == "--dump") {
        return dump(args[1], restrictions, steps, hashes_only);
    }
    if (args.size() == 1) {
        return verify(args[0], restrictions);
    }
    std::fprintf(stderr,
                 "usage: %s <golden> | --update <golden> |\n"
                 "       [--full | --internal-anywhere --interleaved-rules --many-tasks] [--steps N] [--hashes]\n"
                 "       --dump <controller_flight|random.N|seed:S>\n",
                 argv[0]);
    return 2;
}
