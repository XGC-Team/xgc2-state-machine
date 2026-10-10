#include "state_machine_impl.hpp"

namespace state_machine {

void StateMachine::Impl::cancelTasksForStateExit(StateId state) {
    for (auto it = tasks.begin(); it != tasks.end();) {
        const auto& task = it->second;
        if (task.handle.owner_state == state && task.policy == TaskCancelPolicy::kCancelOnStateExit) {
            EventLogRecord record;
            record.kind = EventLogRecord::Kind::kTaskCancelled;
            record.from_state = state;
            record.message = "cancelled on state exit";
            log(record);
            it = tasks.erase(it);
        } else {
            ++it;
        }
    }
}

void StateMachine::Impl::cancelTasksForRegionExit(RegionId region) {
    for (auto it = tasks.begin(); it != tasks.end();) {
        const auto& task = it->second;
        if (task.handle.owner_region == region && task.policy == TaskCancelPolicy::kCancelOnRegionExit) {
            EventLogRecord record;
            record.kind = EventLogRecord::Kind::kTaskCancelled;
            record.region = region;
            record.message = "cancelled on region exit";
            log(record);
            it = tasks.erase(it);
        } else {
            ++it;
        }
    }
}

void StateMachine::Impl::cancelTasksForMachineStop() {
    for (auto it = tasks.begin(); it != tasks.end();) {
        const auto& task = it->second;
        if (task.policy == TaskCancelPolicy::kCancelOnMachineStop) {
            EventLogRecord record;
            record.kind = EventLogRecord::Kind::kTaskCancelled;
            record.region = task.handle.owner_region;
            record.from_state = task.handle.owner_state;
            record.message = "cancelled on machine stop";
            log(record);
            it = tasks.erase(it);
        } else {
            ++it;
        }
    }
}

Status StateMachine::postTaskResult(TaskHandle handle, TaskStatus status, EventPayload payload) {
    // Worker results and their admission log share the state lock with the update.
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    const auto it = impl_->tasks.find(handle.id);
    const bool active_path = impl_->activeInPath({handle.owner_region, handle.owner_state});
    const bool stale =
        it == impl_->tasks.end() || it->second.handle.correlation_id != handle.correlation_id || !active_path;
    if (stale) {
        EventLogRecord record;
        record.kind = EventLogRecord::Kind::kTaskResultReceived;
        record.region = handle.owner_region;
        record.from_state = handle.owner_state;
        record.message = "stale task result ignored";
        impl_->log(record);
        // A matching result ends a task even when its owner has already left.
        if (it != impl_->tasks.end() && it->second.handle.correlation_id == handle.correlation_id && !active_path) {
            impl_->tasks.erase(it);
        }
        return Status{};
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
    // Keep the task pending until its result is actually in the inbox. The
    // admission log is written by the inbox after capacity/lifecycle checks.
    auto posted = impl_->postInputEvent(std::move(event), &record);
    if (posted.ok()) {
        impl_->tasks.erase(it);
    }
    return posted;
}

Status StateMachine::cancelTask(const TaskHandle& handle) {
    std::lock_guard<std::recursive_mutex> lock(impl_->state_mutex);
    auto it = impl_->tasks.find(handle.id);
    if (it == impl_->tasks.end() && (handle.id == 0 || handle.id >= impl_->next_task_id)) {
        return Status::error(ErrorCode::kNotFound, "task not found");
    }
    // Issued IDs are monotonic and never reused: cancellation remains
    // idempotent without retaining completed/cancelled task records.
    if (it != impl_->tasks.end()) {
        impl_->tasks.erase(it);
    }
    EventLogRecord record;
    record.kind = EventLogRecord::Kind::kTaskCancelled;
    record.region = handle.owner_region;
    record.from_state = handle.owner_state;
    record.message = "cancel requested";
    impl_->log(record);
    return Status{};
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
    impl.tasks[handle.id] = StateMachine::Impl::TaskEntry{handle, policy};
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

} // namespace state_machine
