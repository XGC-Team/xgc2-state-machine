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

template <typename T> void pushBounded(std::deque<T>& buffer, const T& value, size_t capacity) {
    const size_t normalized = std::max<size_t>(capacity, 1);
    buffer.push_back(value);
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

    struct StateEntry {
        StateConfig config;
        std::unique_ptr<State> state;
        TimePoint entered_at{};
        bool active{false};
    };

    struct RegionEntry {
        RegionConfig config;
        StateId active_leaf{0};
        uint64_t registration_order{0};
    };

    struct InternalEventEntry {
        Event event;
        size_t first_visible_region_index{0};
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
    std::vector<TransitionRule> transitions;
    TransitionId next_transition_id{1};
    uint64_t next_transition_registration_order{1};
    uint64_t next_region_registration_order{1};
    TaskId next_task_id{1};
    FaultId next_fault_id{1};
    size_t fault_depth{0};
    std::unordered_map<TaskId, TaskEntry> tasks;
    std::vector<ProcessedEventRecord> current_events;
    std::vector<Event> current_output_events;
    std::vector<InternalEventEntry> current_internal_events;
    bool processing_region{false};
    size_t current_region_index{0};
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

    void log(const EventLogRecord& record) { pushBounded(event_log, record, options.event_log_capacity); }

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

    std::vector<RegionId> topLevelRegions() const {
        std::vector<RegionId> ids;
        for (RegionId id : region_order) {
            const auto it = regions.find(id);
            if (it != regions.end() && !it->second.config.owner_state) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    std::vector<RegionId> childRegions(StateId state) const {
        std::vector<RegionId> ids;
        for (RegionId id : region_order) {
            const auto it = regions.find(id);
            if (it != regions.end() && it->second.config.owner_state == state) {
                ids.push_back(id);
            }
        }
        return ids;
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
        while (region_it->second.config.owner_state) {
            const auto owner_state = *region_it->second.config.owner_state;
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

    std::vector<StateId> pathToRoot(StateId state) const {
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

    void collectActiveStatesInRegion(RegionId region, std::vector<StateId>& out) const {
        const auto region_it = regions.find(region);
        if (region_it == regions.end() || region_it->second.active_leaf == 0) {
            return;
        }
        const StateId active = region_it->second.active_leaf;
        out.push_back(active);
        for (RegionId child_region : childRegions(active)) {
            collectActiveStatesInRegion(child_region, out);
        }
    }

    std::vector<StateId> activeStatesInRegion(RegionId region) const {
        std::vector<StateId> active;
        collectActiveStatesInRegion(region, active);
        return active;
    }

    bool activeInPath(StateSelection selection) const {
        const auto active = activeStatesInRegion(selection.region);
        return std::find(active.begin(), active.end(), selection.state) != active.end();
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

    Status callStateCallback(StateId state, CallbackKind kind, const Event* event,
                             const std::function<ActionResult(State&, StateContext&)>& call) {
        auto it = states.find(state);
        if (it == states.end() || !it->second.state) {
            return Status::error(ErrorCode::kNotFound, "state callback target not found");
        }
        StateContext ctx(*machine, StateContext::Config{{it->second.config.region, state}, event, generated_events});
        try {
            auto status = call(*it->second.state, ctx);
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

    Status enterState(StateId state, const Event* event, bool expand_defaults) {
        auto state_it = states.find(state);
        if (state_it == states.end()) {
            return Status::error(ErrorCode::kNotFound, "state not found");
        }
        auto region_it = regions.find(state_it->second.config.region);
        if (region_it == regions.end()) {
            return Status::error(ErrorCode::kNotFound, "state region not found");
        }
        region_it->second.active_leaf = state;
        state_it->second.active = true;
        state_it->second.entered_at = now();
        auto status =
            callStateCallback(state, CallbackKind::kOnEnter, event, [](State& active_state, StateContext& ctx) {
                return active_state.onEnter(ctx);
            });
        if (!status.ok()) {
            return status;
        }
        if (expand_defaults) {
            for (RegionId child_region : childRegions(state)) {
                auto child_it = regions.find(child_region);
                if (child_it == regions.end() || child_it->second.config.initial_state == 0) {
                    return Status::error(ErrorCode::kInvalidArgument, "child region needs an initial state");
                }
                status = enterState(child_it->second.config.initial_state, event, true);
                if (!status.ok()) {
                    return status;
                }
            }
        }
        return Status{};
    }

    Status exitState(StateId state, const Event* event) {
        for (auto child_regions = childRegions(state); !child_regions.empty();) {
            const RegionId child_region = child_regions.back();
            child_regions.pop_back();
            const auto region_it = regions.find(child_region);
            if (region_it != regions.end() && region_it->second.active_leaf != 0) {
                auto status = exitState(region_it->second.active_leaf, event);
                if (!status.ok()) {
                    return status;
                }
                cancelTasksForRegionExit(child_region);
                region_it->second.active_leaf = 0;
            }
        }

        auto state_it = states.find(state);
        if (state_it == states.end()) {
            return Status::error(ErrorCode::kNotFound, "state not found");
        }
        cancelTasksForStateExit(state);
        auto status =
            callStateCallback(state, CallbackKind::kOnExit, event, [](State& active_state, StateContext& ctx) {
                return active_state.onExit(ctx);
            });
        state_it->second.active = false;
        auto region_it = regions.find(state_it->second.config.region);
        if (region_it != regions.end() && region_it->second.active_leaf == state) {
            region_it->second.active_leaf = 0;
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
        region_it->second.active_leaf = 0;
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
        inbox.push_back(event);
        ++generated_events;
        EventLogRecord record;
        record.kind = EventLogRecord::Kind::kEventEnqueued;
        record.sequence = event.sequence;
        record.event_id = event.id;
        record.message = event.source;
        log(record);
        return Status{};
    }

    Status enqueueInternalEvent(Event event) {
        event.category = EventCategory::kInternal;
        event.sequence = next_event_sequence++;
        ++generated_events;
        if (update_in_progress && processing_region) {
            current_internal_events.push_back(InternalEventEntry{std::move(event), current_region_index + 1});
        } else {
            pending_internal_events.push_back(std::move(event));
        }
        return Status{};
    }

    Status enqueueOutputEvent(Event event) {
        event.category = EventCategory::kOutput;
        event.sequence = next_event_sequence++;
        ++generated_events;
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
    std::optional<TransitionRule> pending_transition;
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
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->kind == ScopeKind::kState) {
                return it->state;
            }
        }
        return 0;
    }

    RegionId currentRegion() const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            if (it->kind == ScopeKind::kRegion) {
                return it->region;
            }
        }
        return 0;
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
    impl_->pending_transition = TransitionRule{};
    return *this;
}

StateMachine::Builder& StateMachine::Builder::from(StateId state) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->from = state;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::to(StateId state) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->target = state;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::on(EventId event) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->event = event;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::when(std::function<bool(const GuardContext&)> guard) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->guard = std::move(guard);
    return *this;
}

StateMachine::Builder& StateMachine::Builder::priority(int priority) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->priority = priority;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::evaluationOrder(int evaluation_order) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->evaluation_order = evaluation_order;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::type(TransitionType type) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->type = type;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::global(bool enabled) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->global = enabled;
    return *this;
}

StateMachine::Builder& StateMachine::Builder::action(std::function<ActionResult(StateContext&)> action) {
    if (!impl_->pending_transition) {
        impl_->pending_transition = TransitionRule{};
    }
    impl_->pending_transition->action = std::move(action);
    return *this;
}

Result<std::unique_ptr<StateMachine>> StateMachine::Builder::build() {
    impl_->finalizeTransition();
    if (impl_->regions.empty()) {
        impl_->error("state machine requires at least one region");
    }

    for (const auto& region_pair : impl_->regions) {
        const auto& region = region_pair.second.config;
        if (region.initial_state == 0) {
            impl_->error("region " + std::to_string(region.id) + " is missing an initial state");
            continue;
        }
        const auto state_it = impl_->states.find(region.initial_state);
        if (state_it == impl_->states.end()) {
            impl_->error("region " + std::to_string(region.id) + " initial state is not registered");
            continue;
        }
        if (state_it->second.config.region != region.id) {
            impl_->error("region " + std::to_string(region.id) + " initial state is not a direct child");
        }
        if (region.owner_state && impl_->states.count(*region.owner_state) == 0) {
            impl_->error("region " + std::to_string(region.id) + " owner state is not registered");
        }
    }

    for (const StateId id : impl_->state_order) {
        auto& state = impl_->states[id];
        if (impl_->regions.count(state.config.region) == 0) {
            impl_->error("state " + std::to_string(id) + " region is not registered");
        }
        if (state.config.parent && impl_->states.count(*state.config.parent) == 0) {
            impl_->error("state " + std::to_string(id) + " parent is not registered");
        }
        if (!state.state) {
            state.state = std::make_unique<PassiveState>(state.name);
        }
    }

    for (auto& transition : impl_->transitions) {
        if (transition.from == 0 || impl_->states.count(transition.from) == 0) {
            impl_->error("transition source state is not registered");
            continue;
        }
        if (!transition.event && !transition.guard) {
            impl_->error("condition-only transition requires when(guard)");
        }
        if (transition.target && impl_->states.count(*transition.target) == 0) {
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
    for (auto& transition : impl_->transitions) {
        auto status = machine->addTransition(std::move(transition));
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
    impl_->regions[id] = StateMachine::Impl::RegionEntry{std::move(config), 0, impl_->next_region_registration_order++};
    impl_->region_order.push_back(id);
    impl_->sortRegionOrder();
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
    if (impl_->states.count(config.id) > 0) {
        return Status::error(ErrorCode::kAlreadyExists, "state already exists");
    }
    if (config.parent && impl_->states.count(*config.parent) == 0) {
        return Status::error(ErrorCode::kNotFound, "parent state is not registered");
    }
    if (impl_->regions.count(config.region) == 0) {
        return Status::error(ErrorCode::kNotFound, "state region is not registered");
    }
    impl_->states[config.id] = StateMachine::Impl::StateEntry{config, std::move(state), {}, false};
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
    for (RegionId region_id : impl_->topLevelRegions()) {
        status = impl_->enterRegionDefault(region_id, nullptr);
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
    size_t generated_before = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
        generated_before = impl_->generated_events;
    }
    if (options.max_transitions_per_update == 0) {
        options.max_transitions_per_update = 1;
    }

    std::vector<Event> input_batch;
    std::vector<Event> initial_internal_batch;
    {
        std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
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
        impl_->current_output_events.clear();
        impl_->current_internal_events.clear();
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
        std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
        impl_->update_in_progress = false;
        impl_->processing_region = false;
        result.lifecycle = impl_->lifecycle;
        return Result<UpdateResult>{result.status, result};
    };

    auto visible_events_for_region = [&](size_t region_index) {
        std::vector<const Event*> visible;
        visible.reserve(input_batch.size() + initial_internal_batch.size() + impl_->current_internal_events.size());
        std::transform(input_batch.begin(), input_batch.end(), std::back_inserter(visible), [](const Event& event) {
            return &event;
        });
        std::transform(initial_internal_batch.begin(), initial_internal_batch.end(), std::back_inserter(visible),
                       [](const Event& event) {
                           return &event;
                       });
        for (const auto& entry : impl_->current_internal_events) {
            if (entry.first_visible_region_index <= region_index) {
                visible.push_back(&entry.event);
            }
        }
        std::stable_sort(visible.begin(), visible.end(), [](const Event* lhs, const Event* rhs) {
            return lhs->sequence < rhs->sequence;
        });
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
        const auto current_active = impl_->activeStatesInRegion(region_id);
        const StateId from_leaf = current_active.empty() ? 0 : current_active.back();
        const bool no_exit_enter = rule.type == TransitionType::kInternal || rule.type == TransitionType::kTargetless;
        StateId to_state = rule.target.value_or(rule.from);
        std::vector<StateId> exit_roots;
        std::vector<StateId> enter_suffix;

        if (!no_exit_enter) {
            const auto from_path = impl_->pathToRoot(rule.from);
            const auto to_path = impl_->pathToRoot(to_state);
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
                for (RegionId id : impl_->topLevelRegions()) {
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
        }
        ProcessedEventRecord processed;
        if (event) {
            processed.event = *event;
        } else {
            processed.event.category = EventCategory::kInternal;
            processed.event.source = "condition";
        }
        processed.triggered_transition = true;
        processed.region = region_id;
        processed.from_state = from_leaf;
        processed.to_state = no_exit_enter ? from_leaf : to_state;
        processed.transition = rule.id;
        processed.priority = rule.priority;
        impl_->current_events.push_back(processed);

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
        impl_->log(record);
    };

    {
        std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
        const auto top_regions = impl_->topLevelRegions();
        for (size_t region_index = 0; region_index < top_regions.size(); ++region_index) {
            if (result.transitions_committed >= options.max_transitions_per_update) {
                result.hit_transition_limit = true;
                EventLogRecord record;
                record.kind = EventLogRecord::Kind::kLimitReached;
                record.message = "transition limit reached";
                impl_->log(record);
                break;
            }

            const RegionId region_id = top_regions[region_index];
            auto region_it = impl_->regions.find(region_id);
            if (region_it == impl_->regions.end() || region_it->second.active_leaf == 0) {
                continue;
            }

            impl_->processing_region = true;
            impl_->current_region_index = region_index;

            if (options.run_tick) {
                const auto active_for_tick = impl_->activeStatesInRegion(region_id);
                for (StateId state : active_for_tick) {
                    const auto status = impl_->callStateCallback(state, CallbackKind::kOnTick, nullptr,
                                                                 [](State& active_state, StateContext& ctx) {
                                                                     return active_state.onTick(ctx);
                                                                 });
                    if (!status.ok()) {
                        ++result.faults_recorded;
                    }
                }
            }

            const auto visible = visible_events_for_region(region_index);
            const auto active_for_transition = impl_->activeStatesInRegion(region_id);
            TransitionRule* selected_rule = nullptr;
            const Event* selected_event = nullptr;
            for (StateId state : active_for_transition) {
                for (auto& rule : impl_->transitions) {
                    if (rule.from != state || rule.region != region_id) {
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
                const auto active_for_events = impl_->activeStatesInRegion(region_id);
                for (const Event* event : visible) {
                    if (!event || event->category == EventCategory::kOutput) {
                        continue;
                    }
                    for (StateId state : active_for_events) {
                        auto status = impl_->callStateCallback(state, CallbackKind::kOnEvent, event,
                                                               [&](State& active_state, StateContext& ctx) {
                                                                   return active_state.onEvent(ctx, *event);
                                                               });
                        if (!status.ok()) {
                            ++result.faults_recorded;
                        }
                        ++result.events_processed;
                        ProcessedEventRecord processed;
                        processed.event = *event;
                        processed.region = region_id;
                        processed.from_state = state;
                        processed.to_state = state;
                        impl_->current_events.push_back(processed);
                    }
                }
            }

            impl_->processing_region = false;
        }

        const size_t next_tick_index = top_regions.size();
        for (auto& entry : impl_->current_internal_events) {
            if (entry.first_visible_region_index >= next_tick_index) {
                impl_->pending_internal_events.push_back(std::move(entry.event));
            }
        }
        impl_->current_internal_events.clear();
    }

    {
        std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
        if (impl_->stop_requested) {
            for (RegionId region_id : impl_->topLevelRegions()) {
                auto status = impl_->exitRegion(region_id, nullptr);
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

    {
        std::lock_guard<std::mutex> inbox_lock(impl_->inbox_mutex);
        result.generated_events = impl_->generated_events - generated_before;
    }
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
        const auto active = impl_->activeStatesInRegion(region_pair.first);
        snapshot.active_leaf_states[region_pair.first] = active.empty() ? 0 : active.back();
        snapshot.active_state_paths[region_pair.first] = active;
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
    return impl_->current_events;
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
    const auto active = impl_->activeStatesInRegion(region);
    return active.empty() ? 0 : active.back();
}

std::vector<StateId> StateMachine::currentStatePath(RegionId region) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->activeStatesInRegion(region);
}

std::string StateMachine::currentStateName(RegionId region) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    const auto active = impl_->activeStatesInRegion(region);
    if (active.empty()) {
        return {};
    }
    const auto state_it = impl_->states.find(active.back());
    if (state_it == impl_->states.end() || !state_it->second.state) {
        return {};
    }
    return state_it->second.state->name();
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
    return impl.enqueueInternalEvent(std::move(event));
}

Status StateContext::emitOutput(Event event) {
    auto& impl = *machine_.impl_;
    std::lock_guard<std::recursive_mutex> lock(impl.state_mutex);
    return impl.enqueueOutputEvent(std::move(event));
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
