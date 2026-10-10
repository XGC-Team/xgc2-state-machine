#include "state_machine_impl.hpp"

namespace state_machine {

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
        StateMachine::Impl::clearAndTrim(input_batch);
        StateMachine::Impl::clearAndTrim(initial_internal_batch);
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
            !impl_->current_internal_events.empty() &&
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
                            if (impl_->evaluateGuard(rule, event, result)) {
                                selected_rule = &rule;
                                selected_event = event;
                                break;
                            }
                        }
                    } else if (impl_->evaluateGuard(rule, nullptr, result)) {
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
                impl_->commitTransition(*selected_rule, region_id, selected_event, result);
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

} // namespace state_machine
