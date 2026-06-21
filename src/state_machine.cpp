#include <state_machine/state_machine.hpp>

#include <algorithm>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace state_machine {
namespace {

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

    std::vector<StateId> pathToRoot(StateId leaf) const {
        std::vector<StateId> path;
        StateId current = leaf;
        std::set<StateId> seen;
        while (current != 0 && seen.insert(current).second) {
            const auto it = states.find(current);
            if (it == states.end()) {
                break; // LCOV_EXCL_LINE: public configuration rejects active leaves without a state entry.
            }
            path.push_back(current);
            current = it->second.config.parent.value_or(0);
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    bool activeInPath(StateSelection selection) const {
        const auto region_it = regions.find(selection.region);
        if (region_it == regions.end()) {
            return false;
        }
        const auto path = pathToRoot(region_it->second.active_leaf);
        return std::find(path.begin(), path.end(), selection.state) != path.end();
    }

    RegionId stateRegion(StateId state) const {
        const auto it = states.find(state);
        return it == states.end() ? 0 : it->second.config.region;
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

    Status callStateCallback(StateId state, RegionId region, CallbackKind kind, const Event* event,
                             const std::function<ActionResult(State&, StateContext&)>& call) {
        auto it = states.find(state);
        if (it == states.end() || !it->second.state) {
            return Status::error(
                ErrorCode::kNotFound,
                "state callback target not found"); // LCOV_EXCL_LINE: guarded by state graph validation.
        }
        StateContext ctx(*machine, StateContext::Config{{region, state}, event, generated_events});
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

    StateMachine* machine{nullptr};
};

// cppcheck-suppress passedByValue ; keep public constructor ABI stable and copy into the pimpl.
StateMachine::StateMachine(std::string name, RuntimeOptions options, std::shared_ptr<Clock> clock)
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
    RegionId id = config.id;
    impl_->regions[id] =
        StateMachine::Impl::RegionEntry{std::move(config), 0, impl_->next_region_registration_order++};
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
        RegionConfig region;
        region.id = config.region;
        region.name = "region_" + std::to_string(config.region);
        impl_->regions[config.region] =
            StateMachine::Impl::RegionEntry{region, 0, impl_->next_region_registration_order++};
        impl_->region_order.push_back(config.region);
        impl_->sortRegionOrder();
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
    if (rule.target && impl_->states.count(*rule.target) == 0) {
        return Status::error(ErrorCode::kNotFound, "transition target state is not registered");
    }
    if (rule.id == 0) {
        rule.id = impl_->next_transition_id++;
    }
    rule.registration_order = impl_->next_transition_registration_order++;
    if (rule.region == 0) {
        rule.region = impl_->stateRegion(rule.from);
    }
    impl_->transitions.push_back(std::move(rule));
    std::stable_sort(impl_->transitions.begin(), impl_->transitions.end(), [](const auto& lhs, const auto& rhs) {
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

Status StateMachine::setInitialState(StateSelection selection) {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    auto status = impl_->ensureConfiguring("setInitialState");
    if (!status.ok()) {
        return status;
    }
    const auto state_it = impl_->states.find(selection.state);
    if (state_it == impl_->states.end()) {
        return Status::error(ErrorCode::kNotFound, "initial state is not registered");
    }
    if (impl_->regions.count(selection.region) == 0) {
        RegionConfig config;
        config.id = selection.region;
        config.name = "region_" + std::to_string(selection.region);
        config.initial_state = selection.state;
        impl_->regions[selection.region] =
            StateMachine::Impl::RegionEntry{config, 0, impl_->next_region_registration_order++};
        impl_->region_order.push_back(selection.region);
        impl_->sortRegionOrder();
    }
    auto& entry = impl_->regions[selection.region];
    entry.config.initial_state = selection.state;
    entry.active_leaf = selection.state;
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
    for (auto& region_pair : impl_->regions) {
        auto& region = region_pair.second;
        if (region.config.initial_state == 0) {
            return Status::error(ErrorCode::kInvalidArgument, "every region needs an initial state");
        }
        region.active_leaf = region.config.initial_state;
    }
    impl_->lifecycle = MachineLifecycle::kRunning;
    for (RegionId region_id : impl_->region_order) {
        const auto& region = impl_->regions[region_id];
        const auto path = impl_->pathToRoot(region.active_leaf);
        for (StateId state : path) {
            impl_->states[state].active = true;
            impl_->states[state].entered_at = impl_->now();
            auto cb_status = impl_->callStateCallback(state, region_id, CallbackKind::kOnEnter, nullptr,
                                                      [](State& active_state, StateContext& ctx) {
                                                          return active_state.onEnter(ctx);
                                                      });
            if (!cb_status.ok()) {
                impl_->lifecycle = MachineLifecycle::kFaulted;
                return cb_status;
            }
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
        for (const auto& event : input_batch) {
            visible.push_back(&event);
        }
        for (const auto& event : initial_internal_batch) {
            visible.push_back(&event);
        }
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

    auto evaluate_guard = [&](TransitionRule& rule, const Event& event) -> bool {
        if (!rule.guard) {
            return true;
        }
        try {
            GuardContext ctx(*this, event);
            return rule.guard(ctx);
        } catch (const std::exception& ex) {
            ++result.faults_recorded;
            impl_->recordFault(StateMachine::Impl::faultInput(&event, rule.from, rule.id, CallbackKind::kGuard,
                                                              exceptionMessage(ex), typeid(ex).name()));
            return false;
        } catch (...) {
            ++result.faults_recorded;
            impl_->recordFault(StateMachine::Impl::faultInput(&event, rule.from, rule.id, CallbackKind::kGuard,
                                                              exceptionMessage()));
            return false;
        }
    };

    auto commit_transition = [&](TransitionRule& rule, RegionId region_id, const Event& event) {
        StateId from_leaf = impl_->regions[region_id].active_leaf;
        StateId to_leaf = rule.target.value_or(from_leaf);
        const bool no_exit_enter =
            rule.type == TransitionType::kInternal || rule.type == TransitionType::kTargetless;
        std::vector<StateId> exit_path;
        std::vector<StateId> enter_path;
        if (!no_exit_enter) {
            const auto from_path = impl_->pathToRoot(from_leaf);
            const auto to_path = impl_->pathToRoot(to_leaf);
            size_t prefix = rule.type == TransitionType::kExternalSelf && from_leaf == to_leaf
                                ? from_path.size() - 1
                                : impl_->commonPrefix(from_path, to_path);
            for (size_t i = from_path.size(); i > prefix; --i) {
                exit_path.push_back(from_path[i - 1]);
            }
            for (size_t i = prefix; i < to_path.size(); ++i) {
                enter_path.push_back(to_path[i]);
            }
        }

        for (StateId state : exit_path) {
            impl_->cancelTasksForStateExit(state);
            auto status = impl_->callStateCallback(state, region_id, CallbackKind::kOnExit, &event,
                                                   [](State& active_state, StateContext& ctx) {
                                                       return active_state.onExit(ctx);
                                                   });
            impl_->states[state].active = false;
            if (!status.ok()) {
                ++result.faults_recorded;
            }
        }

        if (rule.action) {
            try {
                StateContext ctx(*this, StateContext::Config{{region_id, rule.from}, &event, impl_->generated_events});
                auto status = rule.action(ctx);
                if (!status.ok()) {
                    ++result.faults_recorded;
                    impl_->recordFault(StateMachine::Impl::faultInput(&event, rule.from, rule.id,
                                                                      CallbackKind::kAction, status.message));
                }
            } catch (const std::exception& ex) {
                ++result.faults_recorded;
                impl_->recordFault(StateMachine::Impl::faultInput(&event, rule.from, rule.id, CallbackKind::kAction,
                                                                  exceptionMessage(ex), typeid(ex).name()));
            } catch (...) {
                ++result.faults_recorded;
                impl_->recordFault(StateMachine::Impl::faultInput(&event, rule.from, rule.id, CallbackKind::kAction,
                                                                  exceptionMessage()));
            }
        }

        if (!no_exit_enter) {
            if (rule.global) {
                for (RegionId id : impl_->region_order) {
                    if (id != region_id) {
                        auto other_path = impl_->pathToRoot(impl_->regions[id].active_leaf);
                        for (auto it = other_path.rbegin(); it != other_path.rend(); ++it) {
                            impl_->cancelTasksForStateExit(*it);
                            auto status = impl_->callStateCallback(*it, id, CallbackKind::kOnExit, &event,
                                                                   [](State& active_state, StateContext& ctx) {
                                                                       return active_state.onExit(ctx);
                                                                   });
                            if (!status.ok()) {
                                ++result.faults_recorded;
                            }
                            impl_->states[*it].active = false;
                        }
                        impl_->cancelTasksForRegionExit(id);
                        impl_->regions[id].active_leaf = 0;
                    }
                }
            }
            impl_->regions[region_id].active_leaf = to_leaf;
            for (StateId state : enter_path) {
                impl_->states[state].active = true;
                impl_->states[state].entered_at = impl_->now();
                auto status = impl_->callStateCallback(state, region_id, CallbackKind::kOnEnter, &event,
                                                       [](State& active_state, StateContext& ctx) {
                                                           return active_state.onEnter(ctx);
                                                       });
                if (!status.ok()) {
                    ++result.faults_recorded;
                }
            }
        }

        ++result.transitions_committed;
        ++result.events_processed;
        ProcessedEventRecord processed;
        processed.event = event;
        processed.triggered_transition = true;
        processed.region = region_id;
        processed.from_state = from_leaf;
        processed.to_state = no_exit_enter ? from_leaf : to_leaf;
        processed.transition = rule.id;
        processed.priority = rule.priority;
        impl_->current_events.push_back(processed);

        EventLogRecord record;
        record.kind = EventLogRecord::Kind::kTransitionCommitted;
        record.sequence = event.sequence;
        record.event_id = event.id;
        record.region = region_id;
        record.from_state = from_leaf;
        record.to_state = processed.to_state;
        record.transition = rule.id;
        impl_->log(record);
    };

    {
        std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
        for (size_t region_index = 0; region_index < impl_->region_order.size(); ++region_index) {
            if (result.transitions_committed >= options.max_transitions_per_update) {
                result.hit_transition_limit = true;
                EventLogRecord record;
                record.kind = EventLogRecord::Kind::kLimitReached;
                record.message = "transition limit reached";
                impl_->log(record);
                break;
            }

            const RegionId region_id = impl_->region_order[region_index];
            auto region_it = impl_->regions.find(region_id);
            if (region_it == impl_->regions.end() || region_it->second.active_leaf == 0) {
                continue;
            }

            impl_->processing_region = true;
            impl_->current_region_index = region_index;

            if (options.run_tick) {
                StateId leaf = region_it->second.active_leaf;
                const auto status = impl_->callStateCallback(leaf, region_id, CallbackKind::kOnTick, nullptr,
                                                             [](State& state, StateContext& ctx) {
                                                                 return state.onTick(ctx);
                                                             });
                if (!status.ok()) {
                    ++result.faults_recorded;
                }
            }

            const auto visible = visible_events_for_region(region_index);

            struct Candidate {
                TransitionRule* rule{nullptr};
                const Event* event{nullptr};
                RegionId region{0};
                size_t state_depth{0};
            };

            const auto active_path = impl_->pathToRoot(region_it->second.active_leaf);
            std::vector<Candidate> candidates;
            for (const Event* event : visible) {
                if (!event || event->category == EventCategory::kOutput) {
                    continue;
                }
                for (auto& rule : impl_->transitions) {
                    if (rule.event != 0 && rule.event != event->id) {
                        continue;
                    }
                    const RegionId rule_region = rule.region == 0 ? impl_->stateRegion(rule.from) : rule.region;
                    if (rule_region != region_id) {
                        continue;
                    }
                    if (rule.target && impl_->stateRegion(*rule.target) != region_id) {
                        continue;
                    }
                    const auto state_it = std::find(active_path.begin(), active_path.end(), rule.from);
                    if (state_it == active_path.end()) {
                        continue;
                    }
                    const size_t depth = static_cast<size_t>(std::distance(active_path.begin(), state_it));
                    candidates.push_back(Candidate{&rule, event, region_id, depth});
                }
            }

            std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
                if (lhs.rule->priority != rhs.rule->priority) {
                    return lhs.rule->priority > rhs.rule->priority;
                }
                if (lhs.rule->evaluation_order != rhs.rule->evaluation_order) {
                    return lhs.rule->evaluation_order < rhs.rule->evaluation_order;
                }
                if (lhs.state_depth != rhs.state_depth) {
                    return lhs.state_depth > rhs.state_depth;
                }
                if (lhs.rule->registration_order != rhs.rule->registration_order) {
                    return lhs.rule->registration_order < rhs.rule->registration_order;
                }
                return lhs.event->sequence < rhs.event->sequence;
            });

            Candidate* selected = nullptr;
            for (auto& candidate : candidates) {
                if (evaluate_guard(*candidate.rule, *candidate.event)) {
                    selected = &candidate;
                    break;
                }
            }

            if (selected) {
                commit_transition(*selected->rule, selected->region, *selected->event);
            } else if (region_it->second.active_leaf != 0) {
                StateId leaf = region_it->second.active_leaf;
                for (const Event* event : visible) {
                    if (!event || event->category == EventCategory::kOutput) {
                        continue;
                    }
                    auto status = impl_->callStateCallback(leaf, region_id, CallbackKind::kOnEvent, event,
                                                           [&](State& state, StateContext& ctx) {
                                                               return state.onEvent(ctx, *event);
                                                           });
                    if (!status.ok()) {
                        ++result.faults_recorded;
                    }
                    ++result.events_processed;
                    ProcessedEventRecord processed;
                    processed.event = *event;
                    processed.region = region_id;
                    processed.from_state = leaf;
                    processed.to_state = leaf;
                    impl_->current_events.push_back(processed);
                }
            }

            impl_->processing_region = false;
        }

        const size_t next_tick_index = impl_->region_order.size();
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
            for (RegionId region_id : impl_->region_order) {
                auto& region = impl_->regions[region_id];
                auto path = impl_->pathToRoot(region.active_leaf);
                for (auto it = path.rbegin(); it != path.rend(); ++it) {
                    impl_->cancelTasksForStateExit(*it);
                    auto status = impl_->callStateCallback(*it, region_id, CallbackKind::kOnExit, nullptr,
                                                           [](State& active_state, StateContext& ctx) {
                                                               return active_state.onExit(ctx);
                                                           });
                    if (!status.ok()) {
                        ++result.faults_recorded;
                    }
                    impl_->states[*it].active = false;
                }
                region.active_leaf = 0;
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
        snapshot.active_leaf_states[region_pair.first] = region_pair.second.active_leaf;
        snapshot.active_state_paths[region_pair.first] = impl_->pathToRoot(region_pair.second.active_leaf);
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
    auto it = impl_->regions.find(region);
    return it == impl_->regions.end() ? 0 : it->second.active_leaf;
}

std::string StateMachine::currentStateName(RegionId region) const {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    const auto region_it = impl_->regions.find(region);
    if (region_it == impl_->regions.end()) {
        return {};
    }
    const auto state_it = impl_->states.find(region_it->second.active_leaf);
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

GuardContext::GuardContext(const StateMachine& machine, const Event& event) : machine_(machine), event_(event) {}

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
