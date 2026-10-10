#include "state_machine_impl.hpp"

#include <iterator>

namespace state_machine {

std::string StateMachine::Impl::exceptionMessage(const std::exception& ex) {
    return ex.what();
}

std::string StateMachine::Impl::exceptionMessage(...) {
    return "unknown exception";
}

StateMachine::Impl::Impl(const RuntimeOptions& runtime_options, std::shared_ptr<Clock> runtime_clock)
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

TimePoint StateMachine::Impl::now() const {
    return clock->now();
}

Status StateMachine::Impl::ensureConfiguring(const char* op) const {
    if (lifecycle != MachineLifecycle::kConfiguring) {
        return Status::error(ErrorCode::kInvalidLifecycle, std::string(op) + " is only allowed while configuring");
    }
    return Status{};
}

Status StateMachine::Impl::ensureOwnerBound() {
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

StateMachine::StateMachine(std::string name, const RuntimeOptions& options, std::shared_ptr<Clock> clock)
    : impl_(std::make_unique<Impl>(options, std::move(clock))), name_(std::move(name)) {
    impl_->machine = this;
}

StateMachine::~StateMachine() = default;

Status StateMachine::bindOwnerThread() {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->ensureOwnerBound();
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
    impl_->ensureDerived();
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
