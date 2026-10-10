#include "state_machine_impl.hpp"

namespace state_machine {

Status StateMachine::Impl::enterState(StateId state, const Event* event, bool expand_defaults) {
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
    auto status =
        callStateCallback(state_it->second, CallbackKind::kOnEnter, event, [](State& active_state, StateContext& ctx) {
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

Status StateMachine::Impl::exitState(StateId state, const Event* event) {
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
    auto status =
        callStateCallback(state_it->second, CallbackKind::kOnExit, event, [](State& active_state, StateContext& ctx) {
            return active_state.onExit(ctx);
        });
    state_it->second.active = false;
    auto region_it = regions.find(state_it->second.config.region);
    if (region_it != regions.end() && region_it->second.active_leaf == state) {
        setActiveLeaf(region_it->second, 0);
    }
    return status;
}

Status StateMachine::Impl::exitRegion(RegionId region, const Event* event) {
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

Status StateMachine::Impl::enterRegionDefault(RegionId region, const Event* event) {
    const auto region_it = regions.find(region);
    if (region_it == regions.end()) {
        return Status::error(ErrorCode::kNotFound, "region not found");
    }
    if (region_it->second.config.initial_state == 0) {
        return Status::error(ErrorCode::kInvalidArgument, "region needs an initial state");
    }
    return enterState(region_it->second.config.initial_state, event, true);
}

bool StateMachine::Impl::evaluateGuard(TransitionRule& rule, const Event* event, UpdateResult& result) {
    if (!rule.guard) {
        return true;
    }
    try {
        GuardContext ctx(*machine, event);
        return rule.guard(ctx);
    } catch (const std::exception& ex) {
        ++result.faults_recorded;
        recordFault(StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kGuard,
                                                   exceptionMessage(ex), typeid(ex).name()));
        return false;
    } catch (...) {
        ++result.faults_recorded;
        recordFault(
            StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kGuard, exceptionMessage()));
        return false;
    }
}

void StateMachine::Impl::commitTransition(TransitionRule& rule, RegionId region_id, const Event* event,
                                          UpdateResult& result) {
    const StateId from_leaf = activeLeafOf(region_id);
    const bool no_exit_enter = rule.type == TransitionType::kInternal || rule.type == TransitionType::kTargetless;
    StateId to_state = rule.target.value_or(rule.from);
    exit_roots.clear();
    enter_suffix.clear();

    if (!no_exit_enter) {
        const auto& from_path = pathOf(rule.from);
        const auto& to_path = pathOf(to_state);
        size_t prefix = rule.type == TransitionType::kExternalSelf && rule.from == to_state && !from_path.empty()
                            ? from_path.size() - 1
                            : commonPrefix(from_path, to_path);
        for (size_t i = from_path.size(); i > prefix; --i) {
            exit_roots.push_back(from_path[i - 1]);
        }
        for (size_t i = prefix; i < to_path.size(); ++i) {
            enter_suffix.push_back(to_path[i]);
        }
    }

    for (StateId state : exit_roots) {
        auto status = exitState(state, event);
        if (!status.ok()) {
            ++result.faults_recorded;
        }
    }

    if (rule.action) {
        try {
            StateContext ctx(*machine,
                             StateContext::Config{{stateRegion(rule.from), rule.from}, event, generated_events});
            auto status = rule.action(ctx);
            if (!status.ok()) {
                ++result.faults_recorded;
                recordFault(
                    StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kAction, status.message));
            }
        } catch (const std::exception& ex) {
            ++result.faults_recorded;
            recordFault(StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kAction,
                                                       exceptionMessage(ex), typeid(ex).name()));
        } catch (...) {
            ++result.faults_recorded;
            recordFault(
                StateMachine::Impl::faultInput(event, rule.from, rule.id, CallbackKind::kAction, exceptionMessage()));
        }
    }

    if (!no_exit_enter) {
        if (rule.global) {
            for (const StateMachine::Impl::RegionEntry* top : top_level_regions) {
                const RegionId id = top->config.id;
                if (id != region_id) {
                    auto status = exitRegion(id, event);
                    if (!status.ok()) {
                        ++result.faults_recorded;
                    }
                }
            }
        }
        for (size_t i = 0; i < enter_suffix.size(); ++i) {
            const bool expand_defaults = i + 1 == enter_suffix.size();
            auto status = enterState(enter_suffix[i], event, expand_defaults);
            if (!status.ok()) {
                ++result.faults_recorded;
            }
        }
    }

    ++result.transitions_committed;
    if (event) {
        ++result.events_processed;
        if (event->category == EventCategory::kInternal) {
            consumed_internal_sequences.push_back(event->sequence);
        }
    }
    StateMachine::Impl::EventRef ref;
    ref.event = event;
    ref.condition = event == nullptr;
    StateMachine::Impl::ProcessedSlot& processed = recordProcessed(ref);
    processed.triggered_transition = true;
    processed.region = region_id;
    processed.from_state = from_leaf;
    processed.to_state = no_exit_enter ? from_leaf : to_state;
    processed.transition = rule.id;
    processed.priority = rule.priority;

    StateMachine::Impl::TraceSlot& trace = recordTrace(EventTraceRecord::Kind::kTransitionCommitted, ref);
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
    log(std::move(record));
}

} // namespace state_machine
