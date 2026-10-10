#include "state_machine_impl.hpp"

namespace state_machine {

namespace {
template <typename T> void pushBounded(std::deque<T>& buffer, T value, size_t capacity) {
    const size_t normalized = std::max<size_t>(capacity, 1);
    buffer.push_back(std::move(value));
    while (buffer.size() > normalized) {
        buffer.pop_front();
    }
}
} // namespace

void StateMachine::Impl::log(EventLogRecord record) {
    pushBounded(event_log, std::move(record), options.event_log_capacity);
}

Event StateMachine::Impl::eventOf(const EventRef& ref) const {
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

StateMachine::Impl::ProcessedSlot& StateMachine::Impl::recordProcessed(const EventRef& ref) {
    ProcessedSlot& slot = current_events.emplace_back();
    slot.ref = ref;
    return slot;
}

StateMachine::Impl::TraceSlot& StateMachine::Impl::recordTrace(EventTraceRecord::Kind kind, const EventRef& ref) {
    TraceSlot& slot = current_trace.emplace_back();
    slot.kind = kind;
    slot.ref = ref;
    return slot;
}

Status StateMachine::Impl::enqueueEvent(Event event, bool bypass_capacity, const EventLogRecord* accepted_log) {
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
    if (accepted_log != nullptr) {
        log(*accepted_log);
    }
    log(std::move(record));
    return Status{};
}

Status StateMachine::Impl::enqueueInternalEvent(Event event, StateSelection producer) {
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

Status StateMachine::Impl::enqueueOutputEvent(Event event, StateSelection producer) {
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

FaultRecord StateMachine::Impl::recordFault(FaultInput input) {
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

Status StateMachine::postEvent(Event event) {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    return impl_->postInputEvent(std::move(event));
}

Status StateMachine::Impl::postInputEvent(Event event, const EventLogRecord* accepted_log) {
    if (event.category != EventCategory::kInput) {
        return Status::error(ErrorCode::kInvalidArgument, "external postEvent only accepts input events");
    }
    if (lifecycle == MachineLifecycle::kConfiguring && !options.allow_prestart_events) {
        EventLogRecord record;
        record.kind = EventLogRecord::Kind::kEventDropped;
        record.event_id = event.id;
        record.message = "prestart event rejected";
        log(record);
        return Status::error(ErrorCode::kNotStarted, "prestart events are disabled");
    }
    if (lifecycle == MachineLifecycle::kStopped || lifecycle == MachineLifecycle::kFaulted) {
        return Status::error(lifecycle == MachineLifecycle::kStopped ? ErrorCode::kStopped : ErrorCode::kFaulted,
                             "event rejected by lifecycle");
    }
    return enqueueEvent(std::move(event), false, accepted_log);
}

} // namespace state_machine
