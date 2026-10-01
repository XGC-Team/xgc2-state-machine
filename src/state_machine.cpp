#include <state_machine/state_machine.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <typeinfo>
#include <unordered_map>

namespace state_machine {
namespace {

constexpr RegionId kImplicitRegionBase = 0x80000000u;

template <typename T> void pushBounded(std::deque<T>& buffer, T value, size_t capacity) {
    const size_t normalized = std::max<size_t>(capacity, 1);
    buffer.push_back(std::move(value));
    while (buffer.size() > normalized) {
        buffer.pop_front();
    }
}

std::string exceptionMessage(const std::exception& ex) {
    return ex.what();
}

std::string exceptionMessage(...) {
    return "unknown exception";
}

class PassiveState final : public State {
  public:
    explicit PassiveState(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }

  private:
    std::string name_;
};

} // namespace

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
        bool active{true};
        bool cancel_requested{false};
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

    explicit Impl(const RuntimeOptions& runtime_options, std::shared_ptr<Clock> runtime_clock)
        : options(runtime_options), clock(std::move(runtime_clock)) {
        if (!clock) {
            clock = std::make_shared<SteadyClock>();
        }
        if (options.event_log_capacity == 0) {
            options.event_log_capacity = 1;
        }
        if (options.fault_log_capacity == 0) {
            options.fault_log_capacity = 1;
        }
        if (options.max_fault_depth == 0) {
            options.max_fault_depth = 1;
        }
    }

    TimePoint now() const { return clock->now(); }

    Status ensureConfiguring(const char* op) const {
        if (lifecycle != MachineLifecycle::kConfiguring) {
            return Status::error(ErrorCode::kInvalidLifecycle, std::string(op) + " is only allowed while configuring");
        }
        return Status{};
    }

    Status ensureOwnerBound() {
        const auto current = std::this_thread::get_id();
        if (!owner_thread) {
            owner_thread = current;
            return Status{};
        }
        if (*owner_thread != current) {
            return Status::error(ErrorCode::kWrongOwnerThread, "operation called from non-owner thread");
        }
        return Status{};
    }

    void log(EventLogRecord record) { pushBounded(event_log, std::move(record), options.event_log_capacity); }

    void sortRegionOrder() {
        std::stable_sort(region_order.begin(), region_order.end(), [&](RegionId lhs, RegionId rhs) {
            const auto& lhs_region = regions.at(lhs);
            const auto& rhs_region = regions.at(rhs);
            if (lhs_region.config.execution_order != rhs_region.config.execution_order) {
                return lhs_region.config.execution_order < rhs_region.config.execution_order;
            }
            return lhs_region.registration_order < rhs_region.registration_order;
        });
    }

    RegionId stateRegion(StateId state) const {
        const auto it = states.find(state);
        return it == states.end() ? 0 : it->second.config.region;
    }

    RegionId topLevelRegionOfRegion(RegionId region) const {
        auto region_it = regions.find(region);
        if (region_it == regions.end()) {
            return 0;
        }
        while (true) {
            const auto owner_state = region_it->second.config.owner_state.value_or(0);
            if (owner_state == 0) {
                break;
            }
            const auto state_it = states.find(owner_state);
            if (state_it == states.end()) {
                return 0;
            }
            region_it = regions.find(state_it->second.config.region);
            if (region_it == regions.end()) {
                return 0;
            }
        }
        return region_it->first;
    }

    RegionId topLevelRegionOfState(StateId state) const { return topLevelRegionOfRegion(stateRegion(state)); }

    std::vector<StateId> computePathToRoot(StateId state) const {
        std::vector<StateId> path;
        StateId current = state;
        std::set<StateId> seen;
        while (current != 0 && seen.insert(current).second) {
            const auto state_it = states.find(current);
            if (state_it == states.end()) {
                break;
            }
            path.push_back(current);
            current = state_it->second.config.parent.value_or(0);
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    const std::vector<StateId>& pathOf(StateId state) const {
        static const std::vector<StateId> kNoPath;
        const auto it = states.find(state);
        return it == states.end() ? kNoPath : it->second.path_to_root;
    }

    // Recomputes everything that is derived from regions, states and transitions. The graph
    // is only mutated while configuring, so the tables are constant once the machine runs.
    void rebuildDerived() {
        top_level_regions.clear();
        for (const RegionId id : region_order) {
            const auto it = regions.find(id);
            if (it != regions.end() && !it->second.config.owner_state) {
                top_level_regions.push_back(&it->second);
            }
        }
        for (auto& entry : states) {
            entry.second.child_regions.clear();
            entry.second.rules.clear();
            entry.second.path_to_root = computePathToRoot(entry.first);
        }
        for (TransitionRule& rule : transitions) {
            const auto source = states.find(rule.from);
            if (source != states.end()) {
                source->second.rules.push_back(&rule);
            }
        }
        for (const RegionId id : region_order) {
            const auto it = regions.find(id);
            if (it == regions.end() || !it->second.config.owner_state) {
                continue;
            }
            const auto owner = states.find(*it->second.config.owner_state);
            if (owner != states.end()) {
                owner->second.child_regions.push_back(&it->second);
            }
        }
        ++active_epoch;
    }

    void setActiveLeaf(RegionEntry& region, StateId leaf) {
        region.active_leaf = leaf;
        ++active_epoch;
    }

    void collectChain(const RegionEntry& region, std::vector<const StateEntry*>& out) const {
        if (region.active_leaf == 0) {
            return;
        }
        const auto state_it = states.find(region.active_leaf);
        if (state_it == states.end()) {
            return;
        }
        out.push_back(&state_it->second);
        for (const RegionEntry* child : state_it->second.child_regions) {
            collectChain(*child, out);
        }
    }

    // Active states of a region, cached until any active leaf changes.
    const std::vector<const StateEntry*>& chainOf(const RegionEntry& region) const {
        if (region.chain_epoch != active_epoch) {
            region.chain.clear();
            collectChain(region, region.chain);
            region.chain_epoch = active_epoch;
        }
        return region.chain;
    }

    std::vector<StateId> activeStatesInRegion(RegionId region) const {
        std::vector<StateId> active;
        const auto it = regions.find(region);
        if (it == regions.end()) {
            return active;
        }
        const auto& chain = chainOf(it->second);
        active.reserve(chain.size());
        std::transform(chain.begin(), chain.end(), std::back_inserter(active), [](const StateEntry* entry) {
            return entry->config.id;
        });
        return active;
    }

    // Last of the active states (the leaf of the last parallel branch), or nullptr.
    const StateEntry* activeLeafEntry(RegionId region) const {
        const auto it = regions.find(region);
        if (it == regions.end()) {
            return nullptr;
        }
        const auto& chain = chainOf(it->second);
        return chain.empty() ? nullptr : chain.back();
    }

    StateId activeLeafOf(RegionId region) const {
        const StateEntry* leaf = activeLeafEntry(region);
        return leaf == nullptr ? 0 : leaf->config.id;
    }

    bool activeInPath(StateSelection selection) const {
        const auto it = regions.find(selection.region);
        if (it == regions.end()) {
            return false;
        }
        const auto& chain = chainOf(it->second);
        return std::any_of(chain.begin(), chain.end(), [&](const StateEntry* entry) {
            return entry->config.id == selection.state;
        });
    }

    size_t commonPrefix(const std::vector<StateId>& lhs, const std::vector<StateId>& rhs) const {
        size_t prefix = 0;
        while (prefix < lhs.size() && prefix < rhs.size() && lhs[prefix] == rhs[prefix]) {
            ++prefix;
        }
        return prefix;
    }

    void cancelTasksForStateExit(StateId state) {
        for (auto& entry : tasks) {
            auto& task = entry.second;
            if (!task.active) {
                continue;
            }
            if (task.handle.owner_state == state && task.policy == TaskCancelPolicy::kCancelOnStateExit) {
                task.active = false;
                task.cancel_requested = true;
                EventLogRecord record;
                record.kind = EventLogRecord::Kind::kTaskCancelled;
                record.from_state = state;
                record.message = "cancelled on state exit";
                log(record);
            }
        }
    }

    void cancelTasksForRegionExit(RegionId region) {
        for (auto& entry : tasks) {
            auto& task = entry.second;
            if (!task.active) {
                continue;
            }
            if (task.handle.owner_region == region && task.policy == TaskCancelPolicy::kCancelOnRegionExit) {
                task.active = false;
                task.cancel_requested = true;
                EventLogRecord record;
                record.kind = EventLogRecord::Kind::kTaskCancelled;
                record.region = region;
                record.message = "cancelled on region exit";
                log(record);
            }
        }
    }

    void cancelTasksForMachineStop() {
        for (auto& entry : tasks) {
            auto& task = entry.second;
            if (task.active && task.policy == TaskCancelPolicy::kCancelOnMachineStop) {
                task.active = false;
                task.cancel_requested = true;
                EventLogRecord record;
                record.kind = EventLogRecord::Kind::kTaskCancelled;
                record.region = task.handle.owner_region;
                record.from_state = task.handle.owner_state;
                record.message = "cancelled on machine stop";
                log(record);
            }
        }
    }

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

    Status enterState(StateId state, const Event* event, bool expand_defaults) {
        auto state_it = states.find(state);
        if (state_it == states.end()) {
            return Status::error(ErrorCode::kNotFound, "state not found");
        }
        auto region_it = regions.find(state_it->second.config.region);
        if (region_it == regions.end()) {
            return Status::error(ErrorCode::kNotFound, "state region not found");
        }
        setActiveLeaf(region_it->second, state);
        state_it->second.active = true;
        state_it->second.entered_at = now();
        auto status = callStateCallback(state_it->second, CallbackKind::kOnEnter, event,
                                        [](State& active_state, StateContext& ctx) {
                                            return active_state.onEnter(ctx);
                                        });
        if (!status.ok()) {
            return status;
        }
        if (expand_defaults) {
            for (const RegionEntry* child : state_it->second.child_regions) {
                if (child->config.initial_state == 0) {
                    return Status::error(ErrorCode::kInvalidArgument, "child region needs an initial state");
                }
                status = enterState(child->config.initial_state, event, true);
                if (!status.ok()) {
                    return status;
                }
            }
        }
        return Status{};
    }

    Status exitState(StateId state, const Event* event) {
        auto state_it = states.find(state);
        if (state_it == states.end()) {
            return Status::error(ErrorCode::kNotFound, "state not found");
        }
        const auto& child_regions = state_it->second.child_regions;
        for (auto child = child_regions.rbegin(); child != child_regions.rend(); ++child) {
            RegionEntry& child_region = **child;
            if (child_region.active_leaf != 0) {
                auto status = exitState(child_region.active_leaf, event);
                if (!status.ok()) {
                    return status;
                }
                cancelTasksForRegionExit(child_region.config.id);
                setActiveLeaf(child_region, 0);
            }
        }

        cancelTasksForStateExit(state);
        auto status = callStateCallback(state_it->second, CallbackKind::kOnExit, event,
                                        [](State& active_state, StateContext& ctx) {
                                            return active_state.onExit(ctx);
                                        });
        state_it->second.active = false;
        auto region_it = regions.find(state_it->second.config.region);
        if (region_it != regions.end() && region_it->second.active_leaf == state) {
            setActiveLeaf(region_it->second, 0);
        }
        return status;
    }

    Status exitRegion(RegionId region, const Event* event) {
        const auto region_it = regions.find(region);
        if (region_it == regions.end() || region_it->second.active_leaf == 0) {
            return Status{};
        }
        auto status = exitState(region_it->second.active_leaf, event);
        cancelTasksForRegionExit(region);
        if (!status.ok()) {
            return status;
        }
        setActiveLeaf(region_it->second, 0);
        return Status{};
    }

    Status enterRegionDefault(RegionId region, const Event* event) {
        const auto region_it = regions.find(region);
        if (region_it == regions.end()) {
            return Status::error(ErrorCode::kNotFound, "region not found");
        }
        if (region_it->second.config.initial_state == 0) {
            return Status::error(ErrorCode::kInvalidArgument, "region needs an initial state");
        }
        return enterState(region_it->second.config.initial_state, event, true);
    }

    // A copy of the event a reference stands for.
    Event eventOf(const EventRef& ref) const {
        if (ref.output) {
            return current_output_events[ref.index];
        }
        if (ref.event != nullptr) {
            return *ref.event;
        }
        Event event;
        if (ref.condition) {
            event.category = EventCategory::kInternal;
            event.source = "condition";
        }
        return event;
    }

    ProcessedSlot& recordProcessed(const EventRef& ref) {
        ProcessedSlot& slot = current_events.emplace_back();
        slot.ref = ref;
        return slot;
    }

    TraceSlot& recordTrace(EventTraceRecord::Kind kind, const EventRef& ref) {
        TraceSlot& slot = current_trace.emplace_back();
        slot.kind = kind;
        slot.ref = ref;
        return slot;
    }

    Status enqueueEvent(Event event, bool bypass_capacity = false) {
        event.category = EventCategory::kInput;
        std::lock_guard<std::mutex> inbox_lock(inbox_mutex);
        if (!bypass_capacity && options.max_pending_events > 0 && inbox.size() >= options.max_pending_events) {
            EventLogRecord record;
            record.kind = EventLogRecord::Kind::kEventDropped;
            record.event_id = event.id;
            record.message = "pending event capacity reached";
            log(record);
            return Status::error(ErrorCode::kLimitReached, "pending event capacity reached");
        }
        event.sequence = next_event_sequence++;
        ++generated_events;
        EventLogRecord record;
        record.kind = EventLogRecord::Kind::kEventEnqueued;
        record.sequence = event.sequence;
        record.event_id = event.id;
        record.message = event.source;
        inbox.push_back(std::move(event));
        log(std::move(record));
        return Status{};
    }

    Status enqueueInternalEvent(Event event, StateSelection producer) {
        event.category = EventCategory::kInternal;
        event.sequence = next_event_sequence++;
        ++generated_events;
        const Event* stored = nullptr;
        if (update_in_progress && processing_region) {
            current_internal_events.push_back(InternalEventEntry{std::move(event), producer.region, producer.state,
                                                                 internal_event_first_visible_region_index,
                                                                 current_region_index});
            stored = &current_internal_events.back().event;
        } else {
            pending_internal_events.push_back(std::move(event));
            stored = &pending_internal_events.back();
        }
        TraceSlot& trace = recordTrace(EventTraceRecord::Kind::kInternalEventGenerated, EventRef{stored});
        trace.producer_region = producer.region;
        trace.producer_state = producer.state;
        return Status{};
    }

    Status enqueueOutputEvent(Event event, StateSelection producer) {
        event.category = EventCategory::kOutput;
        event.sequence = next_event_sequence++;
        ++generated_events;
        EventRef ref;
        ref.output = true;
        ref.index = current_output_events.size();
        TraceSlot& trace = recordTrace(EventTraceRecord::Kind::kOutputEventGenerated, ref);
        trace.producer_region = producer.region;
        trace.producer_state = producer.state;
        current_output_events.push_back(std::move(event));
        return Status{};
    }

    FaultRecord recordFault(FaultInput input) {
        FaultRecord fault;
        fault.id = next_fault_id++;
        fault.timestamp = now();
        fault.event_sequence = input.event_sequence;
        fault.triggering_event = input.event_id;
        fault.state = input.state;
        fault.transition = input.transition;
        fault.callback_kind = input.callback;
        fault.message = std::move(input.message);
        fault.exception_type = std::move(input.exception_type);
        fault.correlation_id = input.correlation;
        pushBounded(fault_log, fault, options.fault_log_capacity);
        EventLogRecord log_record;
        log_record.kind = EventLogRecord::Kind::kCallbackFault;
        log_record.sequence = fault.event_sequence;
        log_record.event_id = fault.triggering_event;
        log_record.transition = fault.transition;
        log_record.from_state = fault.state.value_or(0);
        log_record.message = fault.message;
        log(log_record);
        return fault;
    }
};

struct StateMachine::Builder::Impl {
    enum class ScopeKind { kRegion, kState };
    struct Scope {
        ScopeKind kind{ScopeKind::kRegion};
        RegionId region{0};
        StateId state{0};
        bool child_region_open{false};
    };

    struct RegionDef {
        RegionConfig config;
        uint64_t registration_order{0};
    };

    struct StateDef {
        StateConfig config;
        std::unique_ptr<State> state;
        std::string name;
        uint64_t registration_order{0};
    };

    std::string machine_name;
    RuntimeOptions options;
    std::shared_ptr<Clock> clock;
    std::unordered_map<RegionId, RegionDef> regions;
    std::vector<RegionId> region_order;
    std::unordered_map<StateId, StateDef> states;
    std::vector<StateId> state_order;
    std::vector<TransitionRule> transitions;
    std::vector<Scope> scopes;
    std::unordered_map<StateId, RegionId> implicit_region_by_state;
    std::unique_ptr<TransitionRule> pending_transition;
    RegionId next_implicit_region{kImplicitRegionBase};
    uint64_t next_region_registration_order{1};
    uint64_t next_state_registration_order{1};
    uint64_t next_transition_registration_order{1};
    std::vector<std::string> errors;

    Impl(std::string name, const RuntimeOptions& runtime_options, std::shared_ptr<Clock> runtime_clock)
        : machine_name(std::move(name)), options(runtime_options), clock(std::move(runtime_clock)) {}

    void error(std::string message) { errors.push_back(std::move(message)); }

    void finalizeTransition() {
        if (!pending_transition) {
            return;
        }
        pending_transition->registration_order = next_transition_registration_order++;
        transitions.push_back(std::move(*pending_transition));
        pending_transition.reset();
    }

    TransitionRule& ensurePendingTransition() {
        if (!pending_transition) {
            pending_transition = std::make_unique<TransitionRule>();
        }
        return *pending_transition;
    }

    void closeLeafStates() {
        while (!scopes.empty() && scopes.back().kind == ScopeKind::kState && !scopes.back().child_region_open) {
            scopes.pop_back();
        }
    }

    RegionId allocateRegionId() {
        while (regions.count(next_implicit_region) > 0) {
            ++next_implicit_region;
        }
        return next_implicit_region++;
    }

    RegionId ensureImplicitRegion(StateId owner_state) {
        const auto existing = implicit_region_by_state.find(owner_state);
        if (existing != implicit_region_by_state.end()) {
            return existing->second;
        }
        const RegionId region_id = allocateRegionId();
        RegionConfig config;
        config.id = region_id;
        config.name = "state_" + std::to_string(owner_state) + "_children";
        config.owner_state = owner_state;
        regions.emplace(region_id, RegionDef{config, next_region_registration_order++});
        region_order.push_back(region_id);
        implicit_region_by_state[owner_state] = region_id;
        if (!scopes.empty() && scopes.back().kind == ScopeKind::kState && scopes.back().state == owner_state) {
            scopes.back().child_region_open = true;
        }
        return region_id;
    }

    RegionId currentContainerRegion() {
        if (scopes.empty()) {
            error("state() requires a region()");
            return 0;
        }
        closeLeafStates();
        if (scopes.empty()) {
            error("state() requires a region()");
            return 0;
        }
        const auto& scope = scopes.back();
        if (scope.kind == ScopeKind::kRegion) {
            return scope.region;
        }
        return ensureImplicitRegion(scope.state);
    }

    StateId currentState() const {
        const auto it = std::find_if(scopes.rbegin(), scopes.rend(), [](const Scope& scope) {
            return scope.kind == ScopeKind::kState;
        });
        return it == scopes.rend() ? 0 : it->state;
    }

    RegionId currentRegion() const {
        const auto it = std::find_if(scopes.rbegin(), scopes.rend(), [](const Scope& scope) {
            return scope.kind == ScopeKind::kRegion;
        });
        return it == scopes.rend() ? 0 : it->region;
    }

    std::optional<StateId> ownerOfRegion(RegionId region) const {
        const auto it = regions.find(region);
        if (it == regions.end()) {
            return std::nullopt;
        }
        return it->second.config.owner_state;
    }

    std::string firstError() const {
        if (errors.empty()) {
            return {};
        }
        std::ostringstream stream;
        for (size_t i = 0; i < errors.size(); ++i) {
            if (i != 0) {
                stream << "; ";
            }
            stream << errors[i];
        }
        return stream.str();
    }
};

StateMachine::Builder::Builder(std::string name, const RuntimeOptions& options, std::shared_ptr<Clock> clock)
    : impl_(std::make_unique<Impl>(std::move(name), options, std::move(clock))) {}

StateMachine::Builder::Builder(Builder&&) noexcept = default;
StateMachine::Builder& StateMachine::Builder::operator=(Builder&&) noexcept = default;
StateMachine::Builder::~Builder() = default;

StateMachine::Builder& StateMachine::Builder::region(RegionId id) {
    impl_->finalizeTransition();
    if (id == 0) {
        impl_->error("region id must be non-zero");
        return *this;
    }
    const StateId owner = impl_->currentState();
    if (impl_->regions.count(id) > 0) {
        impl_->error("duplicate region id " + std::to_string(id));
        return *this;
    }
    RegionConfig config;
    config.id = id;
    config.name = "region_" + std::to_string(id);
    if (owner != 0) {
        config.owner_state = owner;
        if (!impl_->scopes.empty() && impl_->scopes.back().kind == Impl::ScopeKind::kState) {
            impl_->scopes.back().child_region_open = true;
        }
    }
    impl_->regions.emplace(id, Impl::RegionDef{config, impl_->next_region_registration_order++});
    impl_->region_order.push_back(id);
    impl_->scopes.push_back(Impl::Scope{Impl::ScopeKind::kRegion, id, 0, false});
    return *this;
}

StateMachine::Builder& StateMachine::Builder::name(std::string name) {
    impl_->finalizeTransition();
    if (impl_->scopes.empty()) {
        impl_->machine_name = std::move(name);
        return *this;
    }
    const auto& scope = impl_->scopes.back();
    if (scope.kind == Impl::ScopeKind::kRegion) {
        impl_->regions[scope.region].config.name = std::move(name);
    } else {
        impl_->states[scope.state].name = std::move(name);
    }
    return *this;
}

StateMachine::Builder& StateMachine::Builder::order(int execution_order) {
    if (impl_->pending_transition) {
        impl_->pending_transition->evaluation_order = execution_order;
        return *this;
    }
    if (impl_->scopes.empty() || impl_->scopes.back().kind != Impl::ScopeKind::kRegion) {
        impl_->error("order() applies to the current region or transition");
        return *this;
    }
    impl_->regions[impl_->scopes.back().region].config.execution_order = execution_order;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::initial(StateId state) {
    impl_->finalizeTransition();
    if (state == 0) {
        impl_->error("initial state id must be non-zero");
        return *this;
    }
    if (impl_->scopes.empty()) {
        impl_->error("initial() requires a region or state scope");
        return *this;
    }
    auto& scope = impl_->scopes.back();
    RegionId region_id = 0;
    if (scope.kind == Impl::ScopeKind::kRegion) {
        region_id = scope.region;
    } else {
        region_id = impl_->ensureImplicitRegion(scope.state);
        scope.child_region_open = true;
    }
    impl_->regions[region_id].config.initial_state = state;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::state(StateId id) {
    impl_->finalizeTransition();
    if (id == 0) {
        impl_->error("state id must be non-zero");
        return *this;
    }
    const RegionId region_id = impl_->currentContainerRegion();
    if (region_id == 0) {
        return *this;
    }
    if (impl_->states.count(id) > 0) {
        impl_->error("duplicate state id " + std::to_string(id));
        return *this;
    }
    StateConfig config;
    config.id = id;
    config.region = region_id;
    config.parent = impl_->ownerOfRegion(region_id);
    impl_->states.emplace(
        id, Impl::StateDef{config, nullptr, "state_" + std::to_string(id), impl_->next_state_registration_order++});
    impl_->state_order.push_back(id);
    impl_->scopes.push_back(Impl::Scope{Impl::ScopeKind::kState, 0, id, false});
    return *this;
}

StateMachine::Builder& StateMachine::Builder::impl(std::unique_ptr<State> state) {
    impl_->finalizeTransition();
    if (impl_->scopes.empty() || impl_->scopes.back().kind != Impl::ScopeKind::kState) {
        impl_->error("impl() requires a current state");
        return *this;
    }
    if (!state) {
        impl_->error("state implementation must be non-null");
        return *this;
    }
    const StateId id = impl_->scopes.back().state;
    if (impl_->states[id].name == "state_" + std::to_string(id)) {
        impl_->states[id].name = state->name();
    }
    impl_->states[id].state = std::move(state);
    return *this;
}

StateMachine::Builder& StateMachine::Builder::endState() {
    impl_->finalizeTransition();
    bool popped_leaf = false;
    while (!impl_->scopes.empty()) {
        auto scope = impl_->scopes.back();
        impl_->scopes.pop_back();
        if (scope.kind != Impl::ScopeKind::kState) {
            impl_->error("endState() without a state scope");
            return *this;
        }
        if (scope.child_region_open) {
            return *this;
        }
        popped_leaf = true;
        if (impl_->scopes.empty() || impl_->scopes.back().kind == Impl::ScopeKind::kRegion) {
            return *this;
        }
    }
    if (!popped_leaf) {
        impl_->error("endState() without a state scope");
    }
    return *this;
}

StateMachine::Builder& StateMachine::Builder::endRegion() {
    impl_->finalizeTransition();
    while (!impl_->scopes.empty()) {
        auto scope = impl_->scopes.back();
        impl_->scopes.pop_back();
        if (scope.kind == Impl::ScopeKind::kRegion) {
            return *this;
        }
    }
    impl_->error("endRegion() without a region scope");
    return *this;
}

StateMachine::Builder& StateMachine::Builder::transition() {
    impl_->finalizeTransition();
    impl_->pending_transition = std::make_unique<TransitionRule>();
    return *this;
}

StateMachine::Builder& StateMachine::Builder::from(StateId state) {
    impl_->ensurePendingTransition().from = state;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::to(StateId state) {
    impl_->ensurePendingTransition().target = state;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::on(EventId event) {
    impl_->ensurePendingTransition().event = event;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::when(std::function<bool(const GuardContext&)> guard) {
    impl_->ensurePendingTransition().guard = std::move(guard);
    return *this;
}

StateMachine::Builder& StateMachine::Builder::priority(int priority) {
    impl_->ensurePendingTransition().priority = priority;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::evaluationOrder(int evaluation_order) {
    impl_->ensurePendingTransition().evaluation_order = evaluation_order;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::type(TransitionType type) {
    impl_->ensurePendingTransition().type = type;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::global(bool enabled) {
    impl_->ensurePendingTransition().global = enabled;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::action(std::function<ActionResult(StateContext&)> action) {
    impl_->ensurePendingTransition().action = std::move(action);
    return *this;
}

Result<std::unique_ptr<StateMachine>> StateMachine::Builder::build() {
    impl_->finalizeTransition();
    if (impl_->regions.empty()) {
        impl_->error("state machine requires at least one region");
    }

    for (const auto& region_pair : impl_->regions) {
        const auto& region_config = region_pair.second.config;
        if (region_config.initial_state == 0) {
            impl_->error("region " + std::to_string(region_config.id) + " is missing an initial state");
            continue;
        }
        const auto state_it = impl_->states.find(region_config.initial_state);
        if (state_it == impl_->states.end()) {
            impl_->error("region " + std::to_string(region_config.id) + " initial state is not registered");
            continue;
        }
        if (state_it->second.config.region != region_config.id) {
            impl_->error("region " + std::to_string(region_config.id) + " initial state is not a direct child");
        }
        if (region_config.owner_state && impl_->states.count(*region_config.owner_state) == 0) {
            impl_->error("region " + std::to_string(region_config.id) + " owner state is not registered");
        }
    }

    for (const StateId id : impl_->state_order) {
        auto& state_def = impl_->states[id];
        state_def.config.name = state_def.name;
        if (impl_->regions.count(state_def.config.region) == 0) {
            impl_->error("state " + std::to_string(id) + " region is not registered");
        }
        if (state_def.config.parent && impl_->states.count(*state_def.config.parent) == 0) {
            impl_->error("state " + std::to_string(id) + " parent is not registered");
        }
        if (!state_def.state) {
            state_def.state = std::make_unique<PassiveState>(state_def.name);
        }
    }

    for (const auto& rule : impl_->transitions) {
        if (rule.from == 0 || impl_->states.count(rule.from) == 0) {
            impl_->error("transition source state is not registered");
            continue;
        }
        if (!rule.event && !rule.guard) {
            impl_->error("condition-only transition requires when(guard)");
        }
        if (rule.target && impl_->states.count(*rule.target) == 0) {
            impl_->error("transition target state is not registered");
        }
    }

    if (!impl_->errors.empty()) {
        return Result<std::unique_ptr<StateMachine>>::error(ErrorCode::kInvalidArgument, impl_->firstError());
    }

    auto machine = std::make_unique<StateMachine>(impl_->machine_name, impl_->options, impl_->clock);
    for (RegionId id : impl_->region_order) {
        auto status = machine->addRegion(impl_->regions[id].config);
        if (!status.ok()) {
            return Result<std::unique_ptr<StateMachine>>{status, nullptr};
        }
    }
    for (StateId id : impl_->state_order) {
        auto status = machine->addState(impl_->states[id].config, std::move(impl_->states[id].state));
        if (!status.ok()) {
            return Result<std::unique_ptr<StateMachine>>{status, nullptr};
        }
    }
    for (auto& rule : impl_->transitions) {
        auto status = machine->addTransition(std::move(rule));
        if (!status.ok()) {
            return Result<std::unique_ptr<StateMachine>>{status, nullptr};
        }
    }
    return Result<std::unique_ptr<StateMachine>>::ok(std::move(machine));
}

StateMachine::Builder StateMachine::builder(std::string name, const RuntimeOptions& options,
                                            std::shared_ptr<Clock> clock) {
    return Builder(std::move(name), options, std::move(clock));
}

StateMachine::StateMachine(std::string name, const RuntimeOptions& options, std::shared_ptr<Clock> clock)
    : impl_(std::make_unique<Impl>(options, std::move(clock))), name_(std::move(name)) {
    impl_->machine = this;
}

StateMachine::~StateMachine() = default;

Status StateMachine::bindOwnerThread() {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->ensureOwnerBound();
}

Status StateMachine::addRegion(RegionConfig config) {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    auto status = impl_->ensureConfiguring("addRegion");
    if (!status.ok()) {
        return status;
    }
    if (config.id == 0) {
        return Status::error(ErrorCode::kInvalidArgument, "region id must be non-zero");
    }
    if (impl_->regions.count(config.id) > 0) {
        return Status::error(ErrorCode::kAlreadyExists, "region already exists");
    }
    const RegionId id = config.id;
    StateMachine::Impl::RegionEntry entry;
    entry.config = std::move(config);
    entry.registration_order = impl_->next_region_registration_order++;
    impl_->regions[id] = std::move(entry);
    impl_->region_order.push_back(id);
    impl_->sortRegionOrder();
    impl_->rebuildDerived();
    return Status{};
}

Status StateMachine::addState(StateConfig config, std::unique_ptr<State> state) {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    auto status = impl_->ensureConfiguring("addState");
    if (!status.ok()) {
        return status;
    }
    if (!state || config.id == 0) {
        return Status::error(ErrorCode::kInvalidArgument, "state id and state object are required");
    }
    if (config.name.empty()) {
        config.name = state->name();
    }
    if (impl_->states.count(config.id) > 0) {
        return Status::error(ErrorCode::kAlreadyExists, "state already exists");
    }
    if (config.parent && impl_->states.count(*config.parent) == 0) {
        return Status::error(ErrorCode::kNotFound, "parent state is not registered");
    }
    if (impl_->regions.count(config.region) == 0) {
        return Status::error(ErrorCode::kNotFound, "state region is not registered");
    }
    StateMachine::Impl::StateEntry entry;
    entry.config = config;
    entry.state = std::move(state);
    impl_->states[config.id] = std::move(entry);
    impl_->rebuildDerived();
    return Status{};
}

Status StateMachine::addTransition(TransitionRule rule) {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    auto status = impl_->ensureConfiguring("addTransition");
    if (!status.ok()) {
        return status;
    }
    if (rule.from == 0 || impl_->states.count(rule.from) == 0) {
        return Status::error(ErrorCode::kNotFound, "transition source state is not registered");
    }
    if (!rule.event && !rule.guard) {
        return Status::error(ErrorCode::kInvalidArgument, "condition-only transition requires a guard");
    }
    if (rule.target && impl_->states.count(*rule.target) == 0) {
        return Status::error(ErrorCode::kNotFound, "transition target state is not registered");
    }
    if (rule.target && impl_->topLevelRegionOfState(rule.from) != impl_->topLevelRegionOfState(*rule.target)) {
        return Status::error(ErrorCode::kInvalidArgument, "transition target must stay in the same top-level region");
    }
    if (rule.id == 0) {
        rule.id = impl_->next_transition_id++;
    }
    rule.registration_order = impl_->next_transition_registration_order++;
    rule.region = impl_->topLevelRegionOfState(rule.from);
    impl_->transitions.push_back(std::move(rule));
    std::stable_sort(impl_->transitions.begin(), impl_->transitions.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.from != rhs.from) {
            return lhs.registration_order < rhs.registration_order;
        }
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        if (lhs.evaluation_order != rhs.evaluation_order) {
            return lhs.evaluation_order < rhs.evaluation_order;
        }
        return lhs.registration_order < rhs.registration_order;
    });
    impl_->rebuildDerived();
    return Status{};
}

Status StateMachine::start() {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    auto owner_status = impl_->ensureOwnerBound();
    if (!owner_status.ok()) {
        return owner_status;
    }
    auto status = impl_->ensureConfiguring("start");
    if (!status.ok()) {
        return status;
    }
    for (const auto& region_pair : impl_->regions) {
        const auto& region = region_pair.second.config;
        if (region.initial_state == 0) {
            return Status::error(ErrorCode::kInvalidArgument, "every region needs an initial state");
        }
        const auto state_it = impl_->states.find(region.initial_state);
        if (state_it == impl_->states.end() || state_it->second.config.region != region.id) {
            return Status::error(ErrorCode::kInvalidArgument, "region initial state must be a direct child");
        }
    }
    impl_->lifecycle = MachineLifecycle::kRunning;
    for (const StateMachine::Impl::RegionEntry* region : impl_->top_level_regions) {
        status = impl_->enterRegionDefault(region->config.id, nullptr);
        if (!status.ok()) {
            impl_->lifecycle = MachineLifecycle::kFaulted;
            return status;
        }
    }
    EventLogRecord record;
    record.kind = EventLogRecord::Kind::kLifecycle;
    record.message = "started";
    impl_->log(record);
    return Status{};
}

Status StateMachine::stop() {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    if (impl_->lifecycle == MachineLifecycle::kStopped || impl_->lifecycle == MachineLifecycle::kFaulted) {
        return Status{};
    }
    if (impl_->lifecycle == MachineLifecycle::kConfiguring) {
        impl_->lifecycle = MachineLifecycle::kStopped;
        return Status{};
    }
    impl_->stop_requested = true;
    impl_->lifecycle = MachineLifecycle::kStopping;
    return Status{};
}

Status StateMachine::postEvent(Event event) {
    std::lock_guard<std::recursive_mutex> state_lock(impl_->state_mutex);
    if (event.category != EventCategory::kInput) {
        return Status::error(ErrorCode::kInvalidArgument, "external postEvent only accepts input events");
    }
    if (impl_->lifecycle == MachineLifecycle::kConfiguring && !impl_->options.allow_prestart_events) {
        EventLogRecord record;
        record.kind = EventLogRecord::Kind::kEventDropped;
        record.event_id = event.id;
        record.message = "prestart event rejected";
        impl_->log(record);
        return Status::error(ErrorCode::kNotStarted, "prestart events are disabled");
    }
    if (impl_->lifecycle == MachineLifecycle::kStopped || impl_->lifecycle == MachineLifecycle::kFaulted) {
        return Status::error(impl_->lifecycle == MachineLifecycle::kStopped ? ErrorCode::kStopped : ErrorCode::kFaulted,
                             "event rejected by lifecycle");
    }
    return impl_->enqueueEvent(std::move(event));
}

Status StateMachine::postTaskResult(TaskHandle handle, TaskStatus status, EventPayload payload) {
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
        const auto it = impl_->tasks.find(handle.id);
        const bool active_path = impl_->activeInPath({handle.owner_region, handle.owner_state});
        const bool stale = it == impl_->tasks.end() || !it->second.active ||
                           it->second.handle.correlation_id != handle.correlation_id || !active_path;
        if (stale) {
            EventLogRecord record;
            record.kind = EventLogRecord::Kind::kTaskResultReceived;
            record.region = handle.owner_region;
            record.from_state = handle.owner_state;
            record.message = "stale task result ignored";
            impl_->log(record);
            return Status{};
        }
        impl_->tasks[handle.id].active = false;
    }
    Event event(kTaskResultEvent);
    event.correlation_id = handle.correlation_id;
    event.payload = std::move(payload);
    event.payload["task_id"] = static_cast<int64_t>(handle.id);
    event.payload["task_status"] = static_cast<int64_t>(static_cast<int>(status));
    EventLogRecord record;
    record.kind = EventLogRecord::Kind::kTaskResultReceived;
    record.region = handle.owner_region;
    record.from_state = handle.owner_state;
    record.message = "task result accepted";
    impl_->log(record);
    return postEvent(std::move(event));
}

Status StateMachine::cancelTask(const TaskHandle& handle) {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    auto it = impl_->tasks.find(handle.id);
    if (it == impl_->tasks.end()) {
        return Status::error(ErrorCode::kNotFound, "task not found");
    }
    it->second.active = false;
    it->second.cancel_requested = true;
    EventLogRecord record;
    record.kind = EventLogRecord::Kind::kTaskCancelled;
    record.region = handle.owner_region;
    record.from_state = handle.owner_state;
    record.message = "cancel requested";
    impl_->log(record);
    return Status{};
}

Result<UpdateResult> StateMachine::update(UpdateOptions options) {
    UpdateResult result;
    // One critical section for the whole update. Taking and releasing the lock around each
    // phase let other threads in between phases; holding it only removes interleavings.
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    const size_t generated_before = impl_->generated_events;
    if (options.max_transitions_per_update == 0) {
        options.max_transitions_per_update = 1;
    }

    std::vector<Event>& input_batch = impl_->input_batch;
    std::vector<Event>& initial_internal_batch = impl_->initial_internal_batch;
    {
        auto owner_status = impl_->ensureOwnerBound();
        if (!owner_status.ok()) {
            result.status = owner_status;
            result.lifecycle = impl_->lifecycle;
            return Result<UpdateResult>{owner_status, result};
        }
        if (impl_->update_in_progress) {
            auto fault = impl_->recordFault(StateMachine::Impl::faultInput(
                nullptr, std::nullopt, std::nullopt, CallbackKind::kRuntime, "update is non-reentrant"));
            Event fault_event(kFaultEvent);
            fault_event.correlation_id = fault.correlation_id;
            impl_->enqueueEvent(fault_event, true);
            result.status = Status::error(ErrorCode::kUpdateAlreadyInProgress, "update is non-reentrant");
            result.lifecycle = impl_->lifecycle;
            return Result<UpdateResult>{result.status, result};
        }
        if (impl_->lifecycle == MachineLifecycle::kConfiguring) {
            result.status = Status::error(ErrorCode::kNotStarted, "runtime is not started");
            result.lifecycle = impl_->lifecycle;
            return Result<UpdateResult>{result.status, result};
        }
        if (impl_->lifecycle == MachineLifecycle::kStopped) {
            result.status = Status::error(ErrorCode::kStopped, "runtime is stopped");
            result.lifecycle = impl_->lifecycle;
            return Result<UpdateResult>{result.status, result};
        }
        if (impl_->lifecycle == MachineLifecycle::kFaulted) {
            result.status = Status::error(ErrorCode::kFaulted, "runtime is faulted");
            result.lifecycle = impl_->lifecycle;
            return Result<UpdateResult>{result.status, result};
        }
        impl_->update_in_progress = true;
        impl_->processing_region = false;
        impl_->current_events.clear();
        impl_->current_trace.clear();
        impl_->current_output_events.clear();
        impl_->current_internal_events.clear();
        // update() is not re-entrant (checked above), so these members are free to reuse.
        input_batch.clear();
        initial_internal_batch.clear();
        impl_->consumed_internal_sequences.clear();
        while (!impl_->pending_internal_events.empty()) {
            initial_internal_batch.push_back(std::move(impl_->pending_internal_events.front()));
            impl_->pending_internal_events.pop_front();
        }
        ++impl_->update_index;
    }

    {
        std::lock_guard<std::mutex> inbox_lock(impl_->inbox_mutex);
        const size_t take = std::min(options.max_events_per_update, impl_->inbox.size());
        for (size_t i = 0; i < take; ++i) {
            input_batch.push_back(std::move(impl_->inbox.front()));
            impl_->inbox.pop_front();
        }
        result.events_taken = input_batch.size();
        result.events_remaining = impl_->inbox.size();
        result.hit_event_limit = !impl_->inbox.empty();
    }

    auto finish = [&]() {
        impl_->update_in_progress = false;
        impl_->processing_region = false;
        result.lifecycle = impl_->lifecycle;
        return Result<UpdateResult>{result.status, result};
    };
    // The events the states of a region can see, ordered by sequence: the batch taken from the
    // inbox, the internal events deferred from the previous update, and the internal events
    // generated so far in this update by earlier regions (or by the region's own tick).
    const auto by_sequence = [](const Event* lhs, const Event* rhs) {
        return lhs->sequence < rhs->sequence;
    };
    const auto sort_by_sequence = [&](std::vector<const Event*>& events) {
        if (!std::is_sorted(events.begin(), events.end(), by_sequence)) {
            std::stable_sort(events.begin(), events.end(), by_sequence);
        }
    };
    bool visible_base_built = false;
    auto visible_events_for_region = [&](size_t region_index) -> const std::vector<const Event*>& {
        std::vector<const Event*>& base = impl_->visible_base;
        if (!visible_base_built) {
            base.clear();
            std::transform(input_batch.begin(), input_batch.end(), std::back_inserter(base), [](const Event& event) {
                return &event;
            });
            std::transform(initial_internal_batch.begin(), initial_internal_batch.end(), std::back_inserter(base),
                           [](const Event& event) {
                               return &event;
                           });
            sort_by_sequence(base);
            visible_base_built = true;
        }
        const bool has_internal =
            std::any_of(impl_->current_internal_events.begin(), impl_->current_internal_events.end(),
                        [region_index](const StateMachine::Impl::InternalEventEntry& entry) {
                            return entry.first_visible_region_index <= region_index;
                        });
        if (!has_internal) {
            return base;
        }
        std::vector<const Event*>& visible = impl_->visible_with_internal;
        visible.assign(base.begin(), base.end());
        for (const auto& entry : impl_->current_internal_events) {
            if (entry.first_visible_region_index <= region_index) {
                visible.push_back(&entry.event);
            }
        }
        sort_by_sequence(visible);
        return visible;
    };

    auto evaluate_guard = [&](TransitionRule& rule, const Event* event) -> bool {
        if (!rule.guard) {
            return true;
        }
        try {
            GuardContext ctx(*this, event);
            return rule.guard(ctx);
        } catch (const std::exception& ex) {
            ++result.faults_recorded;
            impl_->recordFault(StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kGuard,
                                                              exceptionMessage(ex), typeid(ex).name()));
            return false;
        } catch (...) {
            ++result.faults_recorded;
            impl_->recordFault(
                StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kGuard, exceptionMessage()));
            return false;
        }
    };

    auto commit_transition = [&](TransitionRule& rule, RegionId region_id, const Event* event) {
        const StateId from_leaf = impl_->activeLeafOf(region_id);
        const bool no_exit_enter = rule.type == TransitionType::kInternal || rule.type == TransitionType::kTargetless;
        StateId to_state = rule.target.value_or(rule.from);
        std::vector<StateId>& exit_roots = impl_->exit_roots;
        std::vector<StateId>& enter_suffix = impl_->enter_suffix;
        exit_roots.clear();
        enter_suffix.clear();

        if (!no_exit_enter) {
            const auto& from_path = impl_->pathOf(rule.from);
            const auto& to_path = impl_->pathOf(to_state);
            size_t prefix = rule.type == TransitionType::kExternalSelf && rule.from == to_state && !from_path.empty()
                                ? from_path.size() - 1
                                : impl_->commonPrefix(from_path, to_path);
            for (size_t i = from_path.size(); i > prefix; --i) {
                exit_roots.push_back(from_path[i - 1]);
            }
            for (size_t i = prefix; i < to_path.size(); ++i) {
                enter_suffix.push_back(to_path[i]);
            }
        }

        for (StateId state : exit_roots) {
            auto status = impl_->exitState(state, event);
            if (!status.ok()) {
                ++result.faults_recorded;
            }
        }

        if (rule.action) {
            try {
                StateContext ctx(
                    *this,
                    StateContext::Config{{impl_->stateRegion(rule.from), rule.from}, event, impl_->generated_events});
                auto status = rule.action(ctx);
                if (!status.ok()) {
                    ++result.faults_recorded;
                    impl_->recordFault(StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kAction,
                                                                      status.message));
                }
            } catch (const std::exception& ex) {
                ++result.faults_recorded;
                impl_->recordFault(StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kAction,
                                                                  exceptionMessage(ex), typeid(ex).name()));
            } catch (...) {
                ++result.faults_recorded;
                impl_->recordFault(StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kAction,
                                                                  exceptionMessage()));
            }
        }

        if (!no_exit_enter) {
            if (rule.global) {
                for (const StateMachine::Impl::RegionEntry* top : impl_->top_level_regions) {
                    const RegionId id = top->config.id;
                    if (id != region_id) {
                        auto status = impl_->exitRegion(id, event);
                        if (!status.ok()) {
                            ++result.faults_recorded;
                        }
                    }
                }
            }
            for (size_t i = 0; i < enter_suffix.size(); ++i) {
                const bool expand_defaults = i + 1 == enter_suffix.size();
                auto status = impl_->enterState(enter_suffix[i], event, expand_defaults);
                if (!status.ok()) {
                    ++result.faults_recorded;
                }
            }
        }

        ++result.transitions_committed;
        if (event) {
            ++result.events_processed;
            if (event->category == EventCategory::kInternal) {
                impl_->consumed_internal_sequences.push_back(event->sequence);
            }
        }
        StateMachine::Impl::EventRef ref;
        ref.event = event;
        ref.condition = event == nullptr;
        StateMachine::Impl::ProcessedSlot& processed = impl_->recordProcessed(ref);
        processed.triggered_transition = true;
        processed.region = region_id;
        processed.from_state = from_leaf;
        processed.to_state = no_exit_enter ? from_leaf : to_state;
        processed.transition = rule.id;
        processed.priority = rule.priority;

        StateMachine::Impl::TraceSlot& trace = impl_->recordTrace(EventTraceRecord::Kind::kTransitionCommitted, ref);
        trace.consumer_region = region_id;
        trace.from_state = from_leaf;
        trace.to_state = processed.to_state;
        trace.transition = rule.id;
        trace.priority = rule.priority;

        EventLogRecord record;
        record.kind = EventLogRecord::Kind::kTransitionCommitted;
        if (event) {
            record.sequence = event->sequence;
            record.event_id = event->id;
        }
        record.region = region_id;
        record.from_state = from_leaf;
        record.to_state = processed.to_state;
        record.transition = rule.id;
        impl_->log(std::move(record));
    };

    {
        const auto& top_regions = impl_->top_level_regions;
        for (size_t region_index = 0; region_index < top_regions.size(); ++region_index) {
            if (result.transitions_committed >= options.max_transitions_per_update) {
                result.hit_transition_limit = true;
                EventLogRecord record;
                record.kind = EventLogRecord::Kind::kLimitReached;
                record.message = "transition limit reached";
                impl_->log(record);
                break;
            }

            const StateMachine::Impl::RegionEntry& region = *top_regions[region_index];
            const RegionId region_id = region.config.id;
            if (region.active_leaf == 0) {
                continue;
            }

            impl_->processing_region = true;
            impl_->current_region_index = region_index;
            impl_->internal_event_first_visible_region_index = region_index + 1;

            // Callbacks cannot change which states are active (only transitions, start() and
            // stop() do), so one snapshot of them serves the tick, the rule scan and the event
            // dispatch of this region.
            const auto& cached_chain = impl_->chainOf(region);
            impl_->update_chain.assign(cached_chain.begin(), cached_chain.end());
            const auto& active = impl_->update_chain;

            if (options.run_tick) {
                impl_->internal_event_first_visible_region_index = region_index;
                for (const StateMachine::Impl::StateEntry* entry : active) {
                    const auto status = impl_->callStateCallback(*entry, CallbackKind::kOnTick, nullptr,
                                                                 [](State& active_state, StateContext& ctx) {
                                                                     return active_state.onTick(ctx);
                                                                 });
                    if (!status.ok()) {
                        ++result.faults_recorded;
                    }
                }
                impl_->internal_event_first_visible_region_index = region_index + 1;
            }

            const std::vector<const Event*>& visible = visible_events_for_region(region_index);
            TransitionRule* selected_rule = nullptr;
            const Event* selected_event = nullptr;
            for (const StateMachine::Impl::StateEntry* entry : active) {
                for (TransitionRule* candidate : entry->rules) {
                    TransitionRule& rule = *candidate;
                    if (rule.region != region_id) {
                        continue;
                    }
                    if (rule.event) {
                        for (const Event* event : visible) {
                            if (!event || event->category == EventCategory::kOutput || event->id != *rule.event) {
                                continue;
                            }
                            if (evaluate_guard(rule, event)) {
                                selected_rule = &rule;
                                selected_event = event;
                                break;
                            }
                        }
                    } else if (evaluate_guard(rule, nullptr)) {
                        selected_rule = &rule;
                        selected_event = nullptr;
                    }
                    if (selected_rule != nullptr) {
                        break;
                    }
                }
                if (selected_rule != nullptr) {
                    break;
                }
            }

            if (selected_rule != nullptr) {
                commit_transition(*selected_rule, region_id, selected_event);
            } else {
                for (const Event* event : visible) {
                    if (!event || event->category == EventCategory::kOutput) {
                        continue;
                    }
                    for (const StateMachine::Impl::StateEntry* entry : active) {
                        const StateId state = entry->config.id;
                        auto status = impl_->callStateCallback(*entry, CallbackKind::kOnEvent, event,
                                                               [&](State& active_state, StateContext& ctx) {
                                                                   return active_state.onEvent(ctx, *event);
                                                               });
                        if (!status.ok()) {
                            ++result.faults_recorded;
                        }
                        ++result.events_processed;
                        StateMachine::Impl::ProcessedSlot& processed =
                            impl_->recordProcessed(StateMachine::Impl::EventRef{event});
                        processed.region = region_id;
                        processed.from_state = state;
                        processed.to_state = state;

                        StateMachine::Impl::TraceSlot& trace = impl_->recordTrace(
                            EventTraceRecord::Kind::kEventConsumed, StateMachine::Impl::EventRef{event});
                        trace.consumer_region = region_id;
                        trace.from_state = state;
                        trace.to_state = state;
                    }
                }
            }

            impl_->processing_region = false;
        }

        const size_t next_tick_index = top_regions.size();
        for (auto& entry : impl_->current_internal_events) {
            const auto& consumed = impl_->consumed_internal_sequences;
            const bool event_consumed =
                std::find(consumed.begin(), consumed.end(), entry.event.sequence) != consumed.end();
            if (!event_consumed &&
                (entry.first_visible_region_index >= next_tick_index || entry.producer_region_index > 0)) {
                StateMachine::Impl::TraceSlot& trace = impl_->recordTrace(
                    EventTraceRecord::Kind::kInternalEventDeferred, StateMachine::Impl::EventRef{&entry.event});
                trace.producer_region = entry.producer_region;
                trace.producer_state = entry.producer_state;
                impl_->pending_internal_events.push_back(entry.event); // the trace still refers to entry.event
            }
        }
    }

    {
        if (impl_->stop_requested) {
            for (const StateMachine::Impl::RegionEntry* top : impl_->top_level_regions) {
                auto status = impl_->exitRegion(top->config.id, nullptr);
                if (!status.ok()) {
                    ++result.faults_recorded;
                }
            }
            impl_->cancelTasksForMachineStop();
            impl_->lifecycle = MachineLifecycle::kStopped;
            impl_->stop_requested = false;
        }

        if (result.faults_recorded > 0) {
            ++impl_->fault_depth;
            Event fault_event(kFaultEvent);
            fault_event.source = "runtime";
            impl_->enqueueEvent(fault_event, true);
            if (impl_->fault_depth > impl_->options.max_fault_depth) {
                impl_->lifecycle = MachineLifecycle::kFaulted;
            }
        } else {
            impl_->fault_depth = 0;
        }
    }

    result.generated_events = impl_->generated_events - generated_before;
    impl_->visible_base.clear();
    impl_->visible_with_internal.clear();
    return finish();
}

MachineSnapshot StateMachine::snapshot() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    MachineSnapshot snapshot;
    snapshot.lifecycle = impl_->lifecycle;
    snapshot.update_index = impl_->update_index;
    {
        std::lock_guard<std::mutex> inbox_lock(impl_->inbox_mutex);
        snapshot.inbox_size = impl_->inbox.size();
    }
    snapshot.inbox_size += impl_->pending_internal_events.size();
    for (const auto& region_pair : impl_->regions) {
        auto active = impl_->activeStatesInRegion(region_pair.first);
        snapshot.active_leaf_states[region_pair.first] = active.empty() ? 0 : active.back();
        snapshot.active_state_paths[region_pair.first] = std::move(active);
    }
    snapshot.recent_faults.assign(impl_->fault_log.begin(), impl_->fault_log.end());
    return snapshot;
}

std::vector<EventLogRecord> StateMachine::eventLog() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return {impl_->event_log.begin(), impl_->event_log.end()};
}

std::vector<FaultRecord> StateMachine::faultLog() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return {impl_->fault_log.begin(), impl_->fault_log.end()};
}

std::vector<ProcessedEventRecord> StateMachine::currentEvents() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    std::vector<ProcessedEventRecord> records;
    records.reserve(impl_->current_events.size());
    std::transform(impl_->current_events.begin(), impl_->current_events.end(), std::back_inserter(records),
                   [this](const Impl::ProcessedSlot& slot) {
                       return ProcessedEventRecord{impl_->eventOf(slot.ref),
                                                   slot.triggered_transition,
                                                   slot.region,
                                                   slot.from_state,
                                                   slot.to_state,
                                                   slot.transition,
                                                   slot.priority};
                   });
    return records;
}

std::vector<EventTraceRecord> StateMachine::currentTrace() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    std::vector<EventTraceRecord> records;
    records.reserve(impl_->current_trace.size());
    std::transform(impl_->current_trace.begin(), impl_->current_trace.end(), std::back_inserter(records),
                   [this](const Impl::TraceSlot& slot) {
                       return EventTraceRecord{slot.kind,           impl_->eventOf(slot.ref), slot.producer_region,
                                               slot.producer_state, slot.consumer_region,     slot.from_state,
                                               slot.to_state,       slot.transition,          slot.priority};
                   });
    return records;
}

std::vector<Event> StateMachine::currentOutputEvents() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->current_output_events;
}

MachineLifecycle StateMachine::lifecycle() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->lifecycle;
}

StateId StateMachine::currentState(RegionId region) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->activeLeafOf(region);
}

std::vector<StateId> StateMachine::currentStatePath(RegionId region) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->activeStatesInRegion(region);
}

std::string StateMachine::currentStateName(RegionId region) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    const StateMachine::Impl::StateEntry* leaf = impl_->activeLeafEntry(region);
    return leaf == nullptr ? std::string{} : leaf->config.name;
}

Duration StateMachine::elapsed(StateId state) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    const auto it = impl_->states.find(state);
    if (it == impl_->states.end() || !it->second.active) {
        return Duration::zero();
    }
    return impl_->now() - it->second.entered_at;
}

TimePoint StateMachine::now() const {
    return impl_->now();
}

bool StateMachine::isActiveInPath(StateSelection selection) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->activeInPath(selection);
}

size_t StateMachine::generatedEventCount() const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->generated_events;
}

GuardContext::GuardContext(const StateMachine& machine, const Event* event) : machine_(machine), event_(event) {}

TimePoint GuardContext::now() const {
    return machine_.now();
}

Duration GuardContext::elapsed(StateId state) const {
    return machine_.elapsed(state);
}

MachineSnapshot GuardContext::snapshot() const {
    return machine_.snapshot();
}

StateContext::StateContext(StateMachine& machine, const Config& config)
    : machine_(machine), selection_(config.selection), event_(config.event),
      generated_events_before_(config.generated_events_before) {}

Status StateContext::postInternalEvent(Event event) {
    auto& impl = *machine_.impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.state_mutex);
    return impl.enqueueInternalEvent(std::move(event), selection_);
}

Status StateContext::emitOutput(Event event) {
    auto& impl = *machine_.impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.state_mutex);
    return impl.enqueueOutputEvent(std::move(event), selection_);
}

TimePoint StateContext::now() const {
    return machine_.now();
}

Duration StateContext::elapsed(StateId state) const {
    return machine_.elapsed(state);
}

Result<TaskHandle> StateContext::startTask(TaskCancelPolicy policy, CorrelationId correlation_id) {
    auto& impl = *machine_.impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.state_mutex);
    TaskHandle handle;
    handle.id = impl.next_task_id++;
    handle.owner_state = selection_.state;
    handle.owner_region = selection_.region;
    handle.correlation_id = correlation_id == 0 ? handle.id : correlation_id;
    handle.started_at = impl.now();
    impl.tasks[handle.id] = StateMachine::Impl::TaskEntry{handle, policy, true, false};
    EventLogRecord record;
    record.kind = EventLogRecord::Kind::kTaskStarted;
    record.region = selection_.region;
    record.from_state = selection_.state;
    record.message = "task started";
    impl.log(record);
    return Result<TaskHandle>::ok(handle);
}

Status StateContext::cancelTask(const TaskHandle& handle) {
    return machine_.cancelTask(handle);
}

StateId StateContext::currentState(RegionId region) const {
    return machine_.currentState(region);
}

MachineSnapshot StateContext::snapshot() const {
    return machine_.snapshot();
}

size_t StateContext::generatedEvents() const {
    return machine_.generatedEventCount() - generated_events_before_;
}

} // namespace state_machine
