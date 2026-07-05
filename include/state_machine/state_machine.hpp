#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace state_machine {

using StateId = uint32_t;
using RegionId = uint32_t;
using EventId = uint32_t;
using TransitionId = uint64_t;
using TaskId = uint64_t;
using CorrelationId = uint64_t;
using FaultId = uint64_t;
using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;
using PayloadValue = std::variant<int64_t, double, bool, std::string>;
using EventPayload = std::map<std::string, PayloadValue>;
constexpr RegionId kDefaultRegion = 1;
constexpr EventId kFaultEvent = 0xFFFF0001u;
constexpr EventId kStopRequestedEvent = 0xFFFF0002u;
constexpr EventId kTaskResultEvent = 0xFFFF0003u;

enum class ErrorCode {
    kOk,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kInvalidLifecycle,
    kNotStarted,
    kStopped,
    kFaulted,
    kWrongOwnerThread,
    kUpdateAlreadyInProgress,
    kTransitionRejected,
    kLimitReached,
};

struct Status {
    ErrorCode code{ErrorCode::kOk};
    std::string message;
    static Status error(ErrorCode code_value, std::string message_value) {
        return Status{code_value, std::move(message_value)};
    }
    bool ok() const { return code == ErrorCode::kOk; }
    explicit operator bool() const { return ok(); }
};

template <typename T> struct Result {
    Status status;
    T value{};
    static Result<T> ok(T value) { return Result<T>{Status{}, std::move(value)}; }
    static Result<T> error(ErrorCode code, std::string message) {
        return Result<T>{Status::error(code, std::move(message)), T{}};
    }
    bool ok() const { return status.ok(); }
};

enum class MachineLifecycle { kConfiguring, kRunning, kStopping, kStopped, kFaulted };
enum class TransitionType { kExternal, kExternalSelf, kInternal, kTargetless };
enum class CallbackKind { kGuard, kAction, kOnEnter, kOnExit, kOnTick, kOnEvent, kTask, kRuntime };
enum class FaultSeverity { kWarning, kError, kFatal };
enum class TaskStatus { kCompleted, kFailed, kCancelled, kTimeout, kStale };
enum class TaskCancelPolicy { kKeepRunning, kCancelOnStateExit, kCancelOnRegionExit, kCancelOnMachineStop };
enum class EventCategory { kInput, kInternal, kOutput };

struct EventTimestamp {
    double seconds{0.0};
    explicit EventTimestamp(double value) : seconds(value) {}
};

struct Event {
    EventId id{0};
    double timestamp{0.0};
    std::string source;
    CorrelationId correlation_id{0};
    EventPayload payload;
    uint64_t sequence{0};
    EventCategory category{EventCategory::kInput};
    Event() = default;
    explicit Event(EventId event_id) : id(event_id) {}
    Event(EventId event_id, EventTimestamp event_time) : id(event_id), timestamp(event_time.seconds) {}
};

struct FaultRecord {
    FaultId id{0};
    TimePoint timestamp{};
    uint64_t event_sequence{0};
    EventId triggering_event{0};
    std::optional<StateId> state;
    std::optional<TransitionId> transition;
    CallbackKind callback_kind{CallbackKind::kRuntime};
    FaultSeverity severity{FaultSeverity::kError};
    std::string message;
    std::string exception_type;
    CorrelationId correlation_id{0};
};

struct TaskHandle {
    TaskId id{0};
    StateId owner_state{0};
    RegionId owner_region{0};
    CorrelationId correlation_id{0};
    TimePoint started_at{};
};

struct RuntimeOptions {
    size_t event_log_capacity{1024};
    size_t fault_log_capacity{128};
    size_t max_pending_events{4096};
    bool allow_prestart_events{false};
    size_t max_fault_depth{2};
};

struct StateSelection {
    RegionId region{kDefaultRegion};
    StateId state{0};
};

struct UpdateOptions {
    size_t max_events_per_update{64};
    size_t max_transitions_per_update{64};
    bool run_tick{true};
};

struct UpdateResult {
    Status status{Status{}};
    size_t events_taken{0};
    size_t events_processed{0};
    size_t events_remaining{0};
    size_t transitions_committed{0};
    size_t generated_events{0};
    size_t faults_recorded{0};
    bool hit_event_limit{false};
    bool hit_transition_limit{false};
    MachineLifecycle lifecycle{MachineLifecycle::kConfiguring};
};

struct MachineSnapshot {
    MachineLifecycle lifecycle{MachineLifecycle::kConfiguring};
    uint64_t update_index{0};
    std::map<RegionId, StateId> active_leaf_states;
    std::map<RegionId, std::vector<StateId>> active_state_paths;
    size_t inbox_size{0};
    std::vector<FaultRecord> recent_faults;
};

struct EventLogRecord {
    enum class Kind {
        kEventEnqueued,
        kEventProcessed,
        kTransitionSelected,
        kTransitionCommitted,
        kCallbackFault,
        kTaskStarted,
        kTaskCancelled,
        kTaskResultReceived,
        kEventDropped,
        kLimitReached,
        kLifecycle,
    };
    Kind kind{Kind::kLifecycle};
    uint64_t sequence{0};
    EventId event_id{0};
    RegionId region{0};
    StateId from_state{0};
    StateId to_state{0};
    std::optional<TransitionId> transition;
    std::string message;
};

struct ProcessedEventRecord {
    Event event;
    bool triggered_transition{false};
    RegionId region{0};
    StateId from_state{0};
    StateId to_state{0};
    std::optional<TransitionId> transition;
    int priority{0};
};

struct EventTraceRecord {
    enum class Kind {
        kInternalEventGenerated,
        kOutputEventGenerated,
        kEventConsumed,
        kTransitionCommitted,
        kInternalEventDeferred,
    };

    Kind kind{Kind::kEventConsumed};
    Event event;
    RegionId producer_region{0};
    StateId producer_state{0};
    RegionId consumer_region{0};
    StateId from_state{0};
    StateId to_state{0};
    std::optional<TransitionId> transition;
    int priority{0};
};

class Clock {
  public:
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;
};

class SteadyClock final : public Clock {
  public:
    TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

class StateMachine;

class GuardContext {
  public:
    GuardContext(const StateMachine& machine, const Event* event);
    TimePoint now() const;
    Duration elapsed(StateId state) const;
    MachineSnapshot snapshot() const;
    const Event* event() const { return event_; }

  private:
    const StateMachine& machine_;
    const Event* event_{nullptr};
};

class StateContext {
  public:
    struct Config {
        StateSelection selection;
        const Event* event{nullptr};
        size_t generated_events_before{0};
    };

    StateContext(StateMachine& machine, const Config& config);
    Status postInternalEvent(Event event);
    Status emitOutput(Event event);
    TimePoint now() const;
    Duration elapsed(StateId state) const;
    Result<TaskHandle> startTask(TaskCancelPolicy policy = TaskCancelPolicy::kCancelOnStateExit,
                                 CorrelationId correlation_id = 0);
    Status cancelTask(const TaskHandle& handle);
    StateId currentState(RegionId region) const;
    MachineSnapshot snapshot() const;
    RegionId region() const { return selection_.region; }
    StateId state() const { return selection_.state; }
    const Event* event() const { return event_; }
    size_t generatedEvents() const;

  private:
    StateMachine& machine_;
    StateSelection selection_;
    const Event* event_{nullptr};
    size_t generated_events_before_{0};
};

using ActionResult = Status;

class State {
  public:
    virtual ~State() = default;
    virtual std::string name() const = 0;
    virtual ActionResult onEnter(StateContext&) { return Status{}; }
    virtual ActionResult onExit(StateContext&) { return Status{}; }
    virtual ActionResult onTick(StateContext&) { return Status{}; }
    virtual ActionResult onEvent(StateContext&, const Event&) { return Status{}; }
};

struct TransitionRule {
    TransitionId id{0};
    StateId from{0};
    std::optional<StateId> target;
    std::optional<EventId> event;
    RegionId region{0};
    int priority{0};
    int evaluation_order{0};
    TransitionType type{TransitionType::kExternal};
    bool global{false};
    std::function<bool(const GuardContext&)> guard;
    std::function<ActionResult(StateContext&)> action;
    uint64_t registration_order{0};
};

struct StateConfig {
    StateId id{0};
    std::string name;
    std::optional<StateId> parent;
    RegionId region{kDefaultRegion};
};

struct RegionConfig {
    RegionId id{kDefaultRegion};
    std::string name;
    StateId initial_state{0};
    int execution_order{0};
    std::optional<StateId> owner_state;
};

class StateMachine {
  public:
    class Builder {
      public:
        explicit Builder(std::string name, const RuntimeOptions& options = {},
                         std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());
        Builder(Builder&&) noexcept;
        Builder& operator=(Builder&&) noexcept;
        Builder(const Builder&) = delete;
        Builder& operator=(const Builder&) = delete;
        ~Builder();

        Builder& region(RegionId id);
        Builder& name(std::string name);
        Builder& order(int execution_order);
        Builder& initial(StateId state);
        Builder& state(StateId id);
        Builder& impl(std::unique_ptr<State> state);
        Builder& endState();
        Builder& endRegion();

        Builder& transition();
        Builder& from(StateId state);
        Builder& to(StateId state);
        Builder& on(EventId event);
        Builder& when(std::function<bool(const GuardContext&)> guard);
        Builder& priority(int priority);
        Builder& evaluationOrder(int evaluation_order);
        Builder& type(TransitionType type);
        Builder& global(bool enabled = true);
        Builder& action(std::function<ActionResult(StateContext&)> action);

        Result<std::unique_ptr<StateMachine>> build();

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    static Builder builder(std::string name, const RuntimeOptions& options = {},
                           std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());

    explicit StateMachine(std::string name, const RuntimeOptions& options = {},
                          std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());
    ~StateMachine();
    StateMachine(const StateMachine&) = delete;
    StateMachine& operator=(const StateMachine&) = delete;
    Status bindOwnerThread();
    Status start();
    Status stop();
    Result<UpdateResult> update(UpdateOptions options = {});
    Status postEvent(Event event);
    Status postTaskResult(TaskHandle handle, TaskStatus status, EventPayload payload = {});
    Status cancelTask(const TaskHandle& handle);
    MachineSnapshot snapshot() const;
    std::vector<EventLogRecord> eventLog() const;
    std::vector<FaultRecord> faultLog() const;
    std::vector<ProcessedEventRecord> currentEvents() const;
    std::vector<EventTraceRecord> currentTrace() const;
    std::vector<Event> currentOutputEvents() const;
    MachineLifecycle lifecycle() const;
    StateId currentState(RegionId region = kDefaultRegion) const;
    std::vector<StateId> currentStatePath(RegionId region = kDefaultRegion) const;
    std::string currentStateName(RegionId region = kDefaultRegion) const;
    Duration elapsed(StateId state) const;
    TimePoint now() const;
    std::string name() const { return name_; }
    bool isActiveInPath(StateSelection selection) const;
    size_t generatedEventCount() const;

  private:
    friend class Builder;
    friend class GuardContext;
    friend class StateContext;
    struct Impl;
    Status addRegion(RegionConfig config);
    Status addState(StateConfig config, std::unique_ptr<State> state);
    Status addTransition(TransitionRule rule);
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

} // namespace state_machine
