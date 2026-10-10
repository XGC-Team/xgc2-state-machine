#pragma once

#include <state_machine/state_machine.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <mutex>
#include <typeinfo>
#include <unordered_map>

namespace state_machine {

struct StateMachine::Impl {
    struct FaultInput {
        EventId event_id{0};
        uint64_t event_sequence{0};
        std::optional<StateId> state;
        std::optional<TransitionId> transition;
        CallbackKind callback{CallbackKind::kRuntime};
        std::string message;
        CorrelationId correlation{0};
        std::string exception_type;
    };

    static FaultInput faultInput(const Event* event, std::optional<StateId> state,
                                 std::optional<TransitionId> transition, CallbackKind callback, std::string message,
                                 std::string exception_type = {}) {
        FaultInput input;
        if (event) {
            input.event_id = event->id;
            input.event_sequence = event->sequence;
            input.correlation = event->correlation_id;
        }
        input.state = state;
        input.transition = transition;
        input.callback = callback;
        input.message = std::move(message);
        input.exception_type = std::move(exception_type);
        return input;
    }

    struct RegionEntry;

    struct StateEntry {
        StateConfig config;
        std::unique_ptr<State> state;
        TimePoint entered_at{};
        bool active{false};
        // Derived from the configuration by rebuildDerived(); constant while running.
        std::vector<StateId> path_to_root;       // outermost ancestor first, this state last
        std::vector<RegionEntry*> child_regions; // regions owned by this state, in region_order
        std::vector<TransitionRule*> rules;      // rules leaving this state, in evaluation order
    };

    struct RegionEntry {
        RegionConfig config;
        StateId active_leaf{0};
        uint64_t registration_order{0};
        // The active states of this region: its active leaf, then the states of the child
        // regions of that leaf, depth first. Valid while chain_epoch == Impl::active_epoch.
        mutable std::vector<const StateEntry*> chain;
        mutable uint64_t chain_epoch{0};
    };

    struct InternalEventEntry {
        Event event;
        RegionId producer_region{0};
        StateId producer_state{0};
        size_t first_visible_region_index{0};
        size_t producer_region_index{0};
    };

    struct TaskEntry {
        TaskHandle handle;
        TaskCancelPolicy policy{TaskCancelPolicy::kCancelOnStateExit};
    };

    RuntimeOptions options;
    std::shared_ptr<Clock> clock;
    mutable std::mutex inbox_mutex;
    std::deque<Event> inbox;
    std::deque<Event> pending_internal_events;
    uint64_t next_event_sequence{1};
    size_t generated_events{0};

    mutable std::recursive_mutex state_mutex;
    MachineLifecycle lifecycle{MachineLifecycle::kConfiguring};
    std::optional<std::thread::id> owner_thread;
    bool update_in_progress{false};
    bool stop_requested{false};
    uint64_t update_index{0};
    std::unordered_map<StateId, StateEntry> states;
    std::map<RegionId, RegionEntry> regions;
    std::vector<RegionId> region_order;
    std::vector<RegionEntry*> top_level_regions; // regions without an owner state, in region_order
    uint64_t active_epoch{1};                    // bumped whenever any region's active leaf changes
    bool derived_valid{false};                   // see ensureDerived()
    // Scratch buffers of update(), kept so that a steady-state tick allocates nothing.
    std::vector<const StateEntry*> update_chain;       // active states of the region being processed
    std::vector<Event> input_batch;                    // external events taken from the inbox
    std::vector<Event> initial_internal_batch;         // internal events deferred by the previous update
    std::vector<const Event*> visible_base;            // both batches, in sequence order
    std::vector<const Event*> visible_with_internal;   // visible_base plus internal events of this update
    std::vector<uint64_t> consumed_internal_sequences; // internal events that triggered a transition
    std::vector<StateId> exit_roots;
    std::vector<StateId> enter_suffix;
    std::vector<TransitionRule> transitions;
    TransitionId next_transition_id{1};
    uint64_t next_transition_registration_order{1};
    uint64_t next_region_registration_order{1};
    TaskId next_task_id{1};
    FaultId next_fault_id{1};
    size_t fault_depth{0};
    std::unordered_map<TaskId, TaskEntry> tasks;
    // The records of the running (or last) update are kept in a compact form that does not hold
    // a copy of the event they describe, only a reference to it; currentEvents() and
    // currentTrace() build the public records, copying the event, when they are called. Every
    // referenced event lives until the next update() starts: the two batches,
    // current_internal_events, current_output_events and pending_internal_events.
    struct EventRef {
        const Event* event{nullptr}; // null, with neither flag set: no event
        bool output{false};          // the event is current_output_events[index]
        bool condition{false};       // a transition without an event: the synthesized "condition" event
        size_t index{0};
    };
    struct ProcessedSlot {
        EventRef ref;
        bool triggered_transition{false};
        RegionId region{0};
        StateId from_state{0};
        StateId to_state{0};
        std::optional<TransitionId> transition;
        int priority{0};
    };
    struct TraceSlot {
        EventTraceRecord::Kind kind{EventTraceRecord::Kind::kEventConsumed};
        EventRef ref;
        RegionId producer_region{0};
        StateId producer_state{0};
        RegionId consumer_region{0};
        StateId from_state{0};
        StateId to_state{0};
        std::optional<TransitionId> transition;
        int priority{0};
    };
    std::vector<ProcessedSlot> current_events;
    std::vector<TraceSlot> current_trace;
    std::vector<Event> current_output_events;
    // Callbacks receive pointers into this container (ctx.event(), onEvent) and may post
    // more internal events while holding them; a deque keeps existing elements in place.
    std::deque<InternalEventEntry> current_internal_events;
    bool processing_region{false};
    size_t current_region_index{0};
    size_t internal_event_first_visible_region_index{0};
    std::deque<EventLogRecord> event_log;
    std::deque<FaultRecord> fault_log;
    StateMachine* machine{nullptr};

    static std::string exceptionMessage(const std::exception& ex);
    static std::string exceptionMessage(...);

    Impl(const RuntimeOptions& runtime_options, std::shared_ptr<Clock> runtime_clock);
    TimePoint now() const;
    Status ensureConfiguring(const char* op) const;
    Status ensureOwnerBound();
    void log(EventLogRecord record);
    void sortRegionOrder();
    RegionId stateRegion(StateId state) const;
    RegionId topLevelRegionOfRegion(RegionId region) const;
    RegionId topLevelRegionOfState(StateId state) const;
    std::vector<StateId> computePathToRoot(StateId state) const;
    const std::vector<StateId>& pathOf(StateId state) const;
    void markDerivedStale();
    void ensureDerived();
    void rebuildDerived();
    void setActiveLeaf(RegionEntry& region, StateId leaf);
    void collectChain(const RegionEntry& region, std::vector<const StateEntry*>& out) const;
    const std::vector<const StateEntry*>& chainOf(const RegionEntry& region) const;
    std::vector<StateId> activeStatesInRegion(RegionId region) const;
    const StateEntry* activeLeafEntry(RegionId region) const;
    StateId activeLeafOf(RegionId region) const;
    bool activeInPath(StateSelection selection) const;
    size_t commonPrefix(const std::vector<StateId>& lhs, const std::vector<StateId>& rhs) const;
    void cancelTasksForStateExit(StateId state);
    void cancelTasksForRegionExit(RegionId region);
    void cancelTasksForMachineStop();
    Status enterState(StateId state, const Event* event, bool expand_defaults);
    Status exitState(StateId state, const Event* event);
    Status exitRegion(RegionId region, const Event* event);
    Status enterRegionDefault(RegionId region, const Event* event);
    Event eventOf(const EventRef& ref) const;
    ProcessedSlot& recordProcessed(const EventRef& ref);
    TraceSlot& recordTrace(EventTraceRecord::Kind kind, const EventRef& ref);
    Status postInputEvent(Event event, const EventLogRecord* accepted_log = nullptr);
    Status enqueueEvent(Event event, bool bypass_capacity = false, const EventLogRecord* accepted_log = nullptr);
    Status enqueueInternalEvent(Event event, StateSelection producer);
    Status enqueueOutputEvent(Event event, StateSelection producer);
    FaultRecord recordFault(FaultInput input);

    template <typename Call>
    Status callStateCallback(const StateEntry& entry, CallbackKind kind, const Event* event, Call&& call) {
        if (!entry.state) {
            return Status::error(ErrorCode::kNotFound, "state callback target not found");
        }
        const StateId state = entry.config.id;
        StateContext ctx(*machine, StateContext::Config{{entry.config.region, state}, event, generated_events});
        try {
            auto status = call(*entry.state, ctx);
            if (!status.ok()) {
                recordFault(faultInput(event, state, std::nullopt, kind, status.message));
                return status;
            }
            return Status{};
        } catch (const std::exception& ex) {
            const auto message = exceptionMessage(ex);
            recordFault(faultInput(event, state, std::nullopt, kind, message, typeid(ex).name()));
            return Status::error(ErrorCode::kFaulted, message);
        } catch (...) {
            const auto message = exceptionMessage();
            recordFault(faultInput(event, state, std::nullopt, kind, message));
            return Status::error(ErrorCode::kFaulted, message);
        }
    }

    template <typename Call>
    Status callStateCallback(StateId state, CallbackKind kind, const Event* event, Call&& call) {
        const auto it = states.find(state);
        if (it == states.end()) {
            return Status::error(ErrorCode::kNotFound, "state callback target not found");
        }
        return callStateCallback(it->second, kind, event, std::forward<Call>(call));
    }

    template <typename T> static void clearAndTrim(std::vector<T>& values) {
        constexpr size_t kRetainedCapacity = 256;
        values.clear();
        if (values.capacity() > kRetainedCapacity) {
            std::vector<T>().swap(values);
        }
    }
    bool evaluateGuard(TransitionRule& rule, const Event* event, UpdateResult& result);
    void commitTransition(TransitionRule& rule, RegionId region_id, const Event* event, UpdateResult& result);
};

} // namespace state_machine
