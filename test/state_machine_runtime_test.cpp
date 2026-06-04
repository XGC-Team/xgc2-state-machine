#include <state_machine/state_machine.hpp>

#include <gtest/gtest.h>

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace sm = state_machine;

namespace {

class ManualClock final : public sm::Clock {
  public:
    sm::TimePoint now() const override { return now_; }
    void advance(std::chrono::milliseconds delta) { now_ += delta; }

  private:
    sm::TimePoint now_{};
};

class RecordingState final : public sm::State {
  public:
    RecordingState(std::string name, std::vector<std::string>& log) : name_(std::move(name)), log_(log) {}

    std::string name() const override { return name_; }

    sm::ActionResult onEnter(sm::StateContext&) override {
        if (throw_on_enter_std) {
            throw std::runtime_error("enter exploded");
        }
        if (throw_on_enter_unknown) {
            throw 7;
        }
        log_.push_back("enter:" + name_);
        return enter_status;
    }

    sm::ActionResult onExit(sm::StateContext&) override {
        if (throw_on_exit_std) {
            throw std::runtime_error("exit exploded");
        }
        if (throw_on_exit_unknown) {
            throw 7;
        }
        log_.push_back("exit:" + name_);
        return exit_status;
    }

    sm::ActionResult onTick(sm::StateContext& ctx) override {
        if (throw_on_tick_std) {
            throw std::runtime_error("tick exploded");
        }
        if (throw_on_tick_unknown) {
            throw 7;
        }
        log_.push_back("tick:" + name_);
        if (reenter_machine) {
            auto result = reenter_machine->update({64, 64, false});
            assert(!result.ok());
            reenter_machine = nullptr;
        }
        if (post_on_tick != 0) {
            ctx.postEvent(sm::Event(post_on_tick));
        }
        if (start_task_on_tick) {
            auto result = ctx.startTask(task_policy, task_correlation);
            assert(result.ok());
            last_task = result.value;
            if (cancel_task_on_tick) {
                assert(ctx.cancelTask(last_task).ok());
            }
            start_task_on_tick = false;
        }
        if (touch_context_on_tick) {
            (void)ctx.now();
            (void)ctx.elapsed(ctx.state());
            (void)ctx.currentState(ctx.region());
            (void)ctx.snapshot();
            (void)ctx.generatedEvents();
        }
        return tick_status;
    }

    sm::ActionResult onEvent(sm::StateContext& ctx, const sm::Event& event) override {
        if (throw_on_event_std) {
            throw std::runtime_error("event exploded");
        }
        if (throw_on_event_unknown) {
            throw 7;
        }
        log_.push_back("event:" + name_ + ":" + std::to_string(event.id));
        for (auto id : post_on_event) {
            ctx.postEvent(sm::Event(id));
        }
        return event_status;
    }

    sm::EventId post_on_tick{0};
    std::vector<sm::EventId> post_on_event;
    bool start_task_on_tick{false};
    sm::TaskCancelPolicy task_policy{sm::TaskCancelPolicy::kCancelOnStateExit};
    sm::CorrelationId task_correlation{0};
    sm::TaskHandle last_task;
    sm::StateMachine* reenter_machine{nullptr};
    bool cancel_task_on_tick{false};
    bool touch_context_on_tick{false};
    bool throw_on_enter_std{false};
    bool throw_on_enter_unknown{false};
    bool throw_on_exit_std{false};
    bool throw_on_exit_unknown{false};
    bool throw_on_tick_std{false};
    bool throw_on_tick_unknown{false};
    bool throw_on_event_std{false};
    bool throw_on_event_unknown{false};
    sm::Status enter_status;
    sm::Status exit_status;
    sm::Status tick_status;
    sm::Status event_status;

  private:
    std::string name_;
    std::vector<std::string>& log_;
};

void addState(sm::StateMachine& machine, sm::StateId id, sm::RegionId region, std::optional<sm::StateId> parent,
              std::unique_ptr<sm::State> state) {
    assert(machine.addState(sm::StateConfig{id, parent, region}, std::move(state)).ok());
}

void testFifoAndSnapshot() {
    std::vector<std::string> log;
    sm::StateMachine machine("fifo");
    auto state = std::make_unique<RecordingState>("A", log);
    state->post_on_event = {3, 4};
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::move(state));
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    assert(machine.start().ok());
    assert(machine.postEvent(sm::Event(1)).ok());
    assert(machine.postEvent(sm::Event(2)).ok());
    auto first = machine.update({64, 64, false});
    assert(first.ok());
    assert(first.value.events_taken == 2);
    assert(first.value.events_processed == 2);
    assert(log[1] == "event:A:1");
    assert(log[2] == "event:A:2");
    auto second = machine.update({64, 64, false});
    assert(second.value.events_taken == 4);
    assert(log[3] == "event:A:3");
    assert(log[4] == "event:A:4");
    assert(log[5] == "event:A:3");
    assert(log[6] == "event:A:4");
}

void testThreadedSequence() {
    std::vector<std::string> log;
    sm::StateMachine machine("threaded");
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::make_unique<RecordingState>("A", log));
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    assert(machine.start().ok());
    std::thread t1([&] {
        assert(machine.postEvent(sm::Event(10)).ok());
    });
    std::thread t2([&] {
        assert(machine.postEvent(sm::Event(20)).ok());
    });
    t1.join();
    t2.join();
    sm::Event old_timestamp(30, sm::EventTimestamp{-1000.0});
    sm::Event new_timestamp(40, sm::EventTimestamp{1000.0});
    assert(machine.postEvent(old_timestamp).ok());
    assert(machine.postEvent(new_timestamp).ok());
    auto result = machine.update({64, 64, false});
    assert(result.value.events_taken == 4);
    auto records = machine.currentEvents();
    assert(records.size() == 4);
    for (size_t i = 1; i < records.size(); ++i) {
        assert(records[i - 1].event.sequence < records[i].event.sequence);
    }
    assert(records[2].event.id == 30);
    assert(records[3].event.id == 40);
}

void testOwnerThreadAndReentrantUpdate() {
    std::vector<std::string> log;
    sm::StateMachine machine("owner");
    auto state = std::make_unique<RecordingState>("A", log);
    state->reenter_machine = &machine;
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::move(state));
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    assert(machine.start().ok());
    std::thread other([&] {
        auto result = machine.update({64, 64, false});
        assert(!result.ok());
        assert(result.status.code == sm::ErrorCode::kWrongOwnerThread);
    });
    other.join();
    auto result = machine.update({64, 64, true});
    assert(result.ok());
    assert(!machine.faultLog().empty());
}

void testBoundedSnapshotLimit() {
    std::vector<std::string> log;
    sm::StateMachine machine("bounded");
    addState(machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("A", log));
    assert(machine.setInitialState({1, 1}).ok());
    assert(machine.start().ok());
    assert(machine.postEvent(sm::Event(1)).ok());
    assert(machine.postEvent(sm::Event(2)).ok());
    auto result = machine.update({1, 64, false});
    assert(result.value.events_taken == 1);
    assert(result.value.hit_event_limit);
    assert(machine.snapshot().inbox_size == 1);
}

void testTransitionLimitRequeuesUnprocessedEvents() {
    std::vector<std::string> log;
    sm::StateMachine machine("transition_limit");
    addState(machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("A", log));
    addState(machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("B", log));
    addState(machine, 3, 1, std::nullopt, std::make_unique<RecordingState>("C", log));
    sm::TransitionRule to_b;
    to_b.from = 1;
    to_b.target = 2;
    to_b.event = 1;
    assert(machine.addTransition(to_b).ok());
    sm::TransitionRule to_c;
    to_c.from = 2;
    to_c.target = 3;
    to_c.event = 2;
    assert(machine.addTransition(to_c).ok());
    assert(machine.setInitialState({1, 1}).ok());
    assert(machine.start().ok());
    assert(machine.postEvent(sm::Event(1)).ok());
    assert(machine.postEvent(sm::Event(2)).ok());
    auto first = machine.update({64, 1, false});
    assert(first.ok());
    assert(first.value.hit_transition_limit);
    assert(first.value.transitions_committed == 1);
    assert(machine.currentState(1) == 2);
    assert(machine.snapshot().inbox_size == 1);
    auto second = machine.update({64, 1, false});
    assert(second.ok());
    assert(second.value.transitions_committed == 1);
    assert(machine.currentState(1) == 3);
    assert(machine.snapshot().inbox_size == 0);
}

void testHierarchyAndExternalSelf() {
    std::vector<std::string> log;
    sm::StateMachine machine("hierarchy");
    addState(machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("Root", log));
    addState(machine, 2, 1, 1, std::make_unique<RecordingState>("A", log));
    addState(machine, 3, 1, 2, std::make_unique<RecordingState>("A1", log));
    addState(machine, 4, 1, 2, std::make_unique<RecordingState>("A2", log));
    assert(machine.setInitialState({1, 3}).ok());
    sm::TransitionRule to_a2;
    to_a2.from = 3;
    to_a2.target = 4;
    to_a2.event = 7;
    assert(machine.addTransition(to_a2).ok());
    sm::TransitionRule self;
    self.from = 4;
    self.target = 4;
    self.event = 8;
    self.type = sm::TransitionType::kExternalSelf;
    assert(machine.addTransition(self).ok());
    assert(machine.start().ok());
    log.clear();
    machine.postEvent(sm::Event(7));
    machine.update({64, 64, false});
    assert((log == std::vector<std::string>{"exit:A1", "enter:A2"}));
    log.clear();
    machine.postEvent(sm::Event(8));
    machine.update({64, 64, false});
    assert((log == std::vector<std::string>{"exit:A2", "enter:A2"}));
}

void testParallelAndGlobal() {
    std::vector<std::string> log;
    sm::StateMachine machine("parallel");
    assert(machine.addRegion({1, "control", 1}).ok());
    assert(machine.addRegion({2, "comm", 10}).ok());
    addState(machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("Manual", log));
    addState(machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("Auto", log));
    addState(machine, 10, 2, std::nullopt, std::make_unique<RecordingState>("Down", log));
    addState(machine, 11, 2, std::nullopt, std::make_unique<RecordingState>("Up", log));
    addState(machine, 99, 1, std::nullopt, std::make_unique<RecordingState>("Emergency", log));
    sm::TransitionRule control;
    control.from = 1;
    control.target = 2;
    control.event = 30;
    control.region = 1;
    assert(machine.addTransition(control).ok());
    sm::TransitionRule comm;
    comm.from = 10;
    comm.target = 11;
    comm.event = 30;
    comm.region = 2;
    assert(machine.addTransition(comm).ok());
    sm::TransitionRule global;
    global.from = 2;
    global.target = 99;
    global.event = 31;
    global.global = true;
    global.priority = 100;
    assert(machine.addTransition(global).ok());
    assert(machine.start().ok());
    log.clear();
    machine.postEvent(sm::Event(30));
    machine.update({64, 64, false});
    assert(machine.currentState(1) == 2);
    assert(machine.currentState(2) == 11);
    log.clear();
    machine.postEvent(sm::Event(31));
    machine.update({64, 64, false});
    assert(machine.currentState(1) == 99);
    assert(machine.currentState(2) == 0);
    assert(std::find(log.begin(), log.end(), "exit:Up") != log.end());

    sm::StateMachine inactive_global("inactive_global");
    std::vector<std::string> inactive_log;
    addState(inactive_global, 1, 1, std::nullopt, std::make_unique<RecordingState>("Idle", inactive_log));
    addState(inactive_global, 2, 1, std::nullopt, std::make_unique<RecordingState>("Active", inactive_log));
    addState(inactive_global, 3, 1, std::nullopt, std::make_unique<RecordingState>("Emergency", inactive_log));
    assert(inactive_global.setInitialState({1, 1}).ok());
    sm::TransitionRule to_active;
    to_active.from = 1;
    to_active.target = 2;
    to_active.event = 1;
    assert(inactive_global.addTransition(to_active).ok());
    sm::TransitionRule stale_global;
    stale_global.from = 1;
    stale_global.target = 3;
    stale_global.event = 2;
    stale_global.global = true;
    assert(inactive_global.addTransition(stale_global).ok());
    assert(inactive_global.start().ok());
    inactive_global.postEvent(sm::Event(1));
    inactive_global.update({64, 64, false});
    assert(inactive_global.currentState(1) == 2);
    inactive_global.postEvent(sm::Event(2));
    inactive_global.update({64, 64, false});
    assert(inactive_global.currentState(1) == 2);
}

void testTaskStaleFaultStopAndLimits() {
    std::vector<std::string> log;
    sm::StateMachine machine("task");
    auto state = std::make_unique<RecordingState>("A", log);
    state->start_task_on_tick = true;
    state->task_correlation = 42;
    auto* state_ptr = state.get();
    addState(machine, 1, 1, std::nullopt, std::move(state));
    addState(machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("B", log));
    assert(machine.setInitialState({1, 1}).ok());
    sm::TransitionRule to_b;
    to_b.from = 1;
    to_b.target = 2;
    to_b.event = 50;
    assert(machine.addTransition(to_b).ok());
    assert(machine.start().ok());
    machine.update({0, 64, true});
    const auto task = state_ptr->last_task;
    machine.postEvent(sm::Event(50));
    machine.update({64, 64, false});
    assert(machine.currentState(1) == 2);
    assert(machine.postTaskResult(task, sm::TaskStatus::kCompleted).ok());
    assert(machine.snapshot().inbox_size == 0);

    for (auto status : {sm::TaskStatus::kFailed, sm::TaskStatus::kCancelled, sm::TaskStatus::kTimeout}) {
        sm::StateMachine status_machine("task_status");
        std::vector<std::string> status_log;
        auto status_state = std::make_unique<RecordingState>("TaskStatus", status_log);
        status_state->start_task_on_tick = true;
        auto* status_state_ptr = status_state.get();
        addState(status_machine, 1, 1, std::nullopt, std::move(status_state));
        assert(status_machine.setInitialState({1, 1}).ok());
        assert(status_machine.start().ok());
        status_machine.update({0, 64, true});
        assert(status_machine.postTaskResult(status_state_ptr->last_task, status).ok());
        assert(status_machine.snapshot().inbox_size == 1);
        status_machine.update({64, 64, false});
        const auto records = status_machine.currentEvents();
        assert(!records.empty());
        assert(records.front().event.id == sm::kTaskResultEvent);
        assert(std::get<int64_t>(records.front().event.payload.at("task_status")) ==
               static_cast<int64_t>(static_cast<int>(status)));
    }

    sm::StateMachine stop_task_machine("stop_task");
    std::vector<std::string> stop_log;
    auto stop_state = std::make_unique<RecordingState>("StopTask", stop_log);
    stop_state->start_task_on_tick = true;
    stop_state->task_policy = sm::TaskCancelPolicy::kCancelOnMachineStop;
    addState(stop_task_machine, 1, 1, std::nullopt, std::move(stop_state));
    assert(stop_task_machine.setInitialState({1, 1}).ok());
    assert(stop_task_machine.start().ok());
    stop_task_machine.update({0, 64, true});
    std::thread stop_thread([&] {
        assert(stop_task_machine.stop().ok());
    });
    stop_thread.join();
    assert(stop_task_machine.lifecycle() == sm::MachineLifecycle::kStopping);
    stop_task_machine.update({64, 64, false});
    assert(stop_task_machine.lifecycle() == sm::MachineLifecycle::kStopped);
    auto event_log = stop_task_machine.eventLog();
    assert(std::any_of(event_log.begin(), event_log.end(), [](const sm::EventLogRecord& record) {
        return record.kind == sm::EventLogRecord::Kind::kTaskCancelled && record.message == "cancelled on machine stop";
    }));

    auto bad = std::make_unique<RecordingState>("Bad", log);
    bad->tick_status = sm::Status::error(sm::ErrorCode::kFaulted, "tick failed");
    sm::StateMachine fault_machine("fault");
    addState(fault_machine, 1, 1, std::nullopt, std::move(bad));
    assert(fault_machine.setInitialState({1, 1}).ok());
    assert(fault_machine.start().ok());
    auto fault_result = fault_machine.update({64, 64, true});
    assert(fault_result.value.faults_recorded == 1);
    assert(!fault_machine.faultLog().empty());

    assert(machine.stop().ok());
    auto stop_result = machine.update({64, 64, false});
    assert(stop_result.value.lifecycle == sm::MachineLifecycle::kStopped);
}

void testLifecycleErrorsAndAccessors() {
    sm::RuntimeOptions tiny_options;
    tiny_options.event_log_capacity = 0;
    tiny_options.fault_log_capacity = 0;
    tiny_options.max_fault_depth = 0;
    sm::StateMachine machine("lifecycle", tiny_options, nullptr);
    assert(machine.name() == "lifecycle");
    assert(machine.bindOwnerThread().ok());
    assert(!machine.addRegion({0, "bad", 0}).ok());
    assert(machine.addRegion({7, "r7", 0}).ok());
    assert(machine.currentStateName(7).empty());
    assert(!machine.addRegion({7, "dup", 0}).ok());
    assert(!machine.addState(sm::StateConfig{0, std::nullopt, 7}, nullptr).ok());
    std::vector<std::string> log;
    addState(machine, 1, 7, std::nullopt, std::make_unique<RecordingState>("A", log));
    assert(!machine.addState(sm::StateConfig{1, std::nullopt, 7}, std::make_unique<RecordingState>("Dup", log)).ok());
    assert(!machine.addState(sm::StateConfig{2, 99, 7}, std::make_unique<RecordingState>("Child", log)).ok());
    assert(!machine.addTransition(sm::TransitionRule{}).ok());
    sm::TransitionRule missing_target;
    missing_target.from = 1;
    missing_target.target = 44;
    assert(!machine.addTransition(missing_target).ok());
    assert(!machine.setInitialState({7, 44}).ok());
    assert(machine.setInitialState({7, 1}).ok());
    assert(machine.start().ok());
    assert(machine.lifecycle() == sm::MachineLifecycle::kRunning);
    assert(machine.elapsed(99) == sm::Duration::zero());
    (void)machine.now();
    assert(machine.isActiveInPath({7, 1}));
    assert(!machine.isActiveInPath({77, 1}));
    assert(machine.currentState(77) == 0);
    assert(machine.currentStateName(7) == "A");
    assert(machine.currentStateName(77).empty());
    assert(!machine.addRegion({9, "frozen", 0}).ok());
    assert(
        !machine.addState(sm::StateConfig{3, std::nullopt, 8}, std::make_unique<RecordingState>("Frozen", log)).ok());
    assert(!machine.addTransition(missing_target).ok());
    assert(!machine.setInitialState({7, 1}).ok());
    assert(machine.postEvent(sm::Event(91)).ok());
    assert(machine.postEvent(sm::Event(92)).ok());
    machine.update({64, 64, false});
    assert(machine.eventLog().size() == 1);
    assert(machine.stop().ok());
    auto stopped_update = machine.update({64, 64, false});
    assert(stopped_update.value.lifecycle == sm::MachineLifecycle::kStopped);
    assert(!machine.postEvent(sm::Event(93)).ok());
    assert(!machine.update({64, 64, false}).ok());

    sm::StateMachine configuring("configuring");
    assert(!configuring.postEvent(sm::Event(1)).ok());
    assert(!configuring.update({64, 64, false}).ok());
    assert(configuring.stop().ok());
    assert(configuring.stop().ok());
    assert(!configuring.update({64, 64, false}).ok());

    sm::StateMachine no_initial("no_initial");
    assert(no_initial.addRegion({1, "empty", 0}).ok());
    assert(!no_initial.start().ok());

    sm::StateMachine bad_enter("bad_enter");
    auto bad = std::make_unique<RecordingState>("BadEnter", log);
    bad->enter_status = sm::Status::error(sm::ErrorCode::kFaulted, "enter failed");
    addState(bad_enter, 1, 1, std::nullopt, std::move(bad));
    assert(bad_enter.setInitialState({1, 1}).ok());
    assert(!bad_enter.start().ok());
    assert(bad_enter.lifecycle() == sm::MachineLifecycle::kFaulted);
    assert(!bad_enter.update({64, 64, false}).ok());
    assert(!bad_enter.postEvent(sm::Event(1)).ok());

    sm::StateMachine cross_region_initial("cross_region_initial");
    addState(cross_region_initial, 1, 1, std::nullopt, std::make_unique<RecordingState>("A", log));
    assert(cross_region_initial.setInitialState({8, 1}).ok());

    sm::StateMachine double_start("double_start");
    addState(double_start, 1, 1, std::nullopt, std::make_unique<RecordingState>("A", log));
    assert(double_start.setInitialState({1, 1}).ok());
    assert(double_start.start().ok());
    assert(!double_start.start().ok());

    sm::StateMachine wrong_owner_start("wrong_owner_start");
    assert(wrong_owner_start.bindOwnerThread().ok());
    std::thread start_thread([&] {
        assert(!wrong_owner_start.start().ok());
    });
    start_thread.join();

    sm::RuntimeOptions fault_ring_options;
    fault_ring_options.fault_log_capacity = 1;
    sm::StateMachine fault_ring("fault_ring", fault_ring_options);
    auto faulting = std::make_unique<RecordingState>("Faulting", log);
    faulting->event_status = sm::Status::error(sm::ErrorCode::kFaulted, "event failed");
    addState(fault_ring, 1, 1, std::nullopt, std::move(faulting));
    assert(fault_ring.setInitialState({1, 1}).ok());
    assert(fault_ring.start().ok());
    fault_ring.postEvent(sm::Event(1));
    fault_ring.postEvent(sm::Event(2));
    fault_ring.update({64, 64, false});
    assert(fault_ring.faultLog().size() == 1);
}

void testContextTasksAndRegionCancel() {
    std::vector<std::string> log;
    auto clock = std::make_shared<ManualClock>();
    sm::StateMachine machine("context", {}, clock);
    auto active = std::make_unique<RecordingState>("Active", log);
    active->start_task_on_tick = true;
    active->task_policy = sm::TaskCancelPolicy::kKeepRunning;
    active->touch_context_on_tick = true;
    auto* active_ptr = active.get();
    addState(machine, 1, 1, std::nullopt, std::move(active));
    addState(machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("Other", log));
    assert(machine.setInitialState({1, 1}).ok());
    assert(machine.start().ok());
    clock->advance(std::chrono::milliseconds(5));
    machine.update({0, 64, true});
    const auto task = active_ptr->last_task;
    assert(machine.elapsed(1) > sm::Duration::zero());
    assert(machine.postTaskResult(task, sm::TaskStatus::kCompleted, {{"value", int64_t{5}}}).ok());
    assert(machine.snapshot().inbox_size == 1);
    machine.update({64, 64, false});
    const auto records = machine.currentEvents();
    assert(!records.empty());
    assert(records.front().event.id == sm::kTaskResultEvent);
    sm::TaskHandle missing_task = task;
    missing_task.id = 99999;
    assert(!machine.cancelTask(missing_task).ok());

    sm::StateMachine cancel_machine("cancel_region");
    std::vector<std::string> cancel_log;
    addState(cancel_machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("Main", cancel_log));
    auto region_state = std::make_unique<RecordingState>("RegionTask", cancel_log);
    region_state->start_task_on_tick = true;
    region_state->task_policy = sm::TaskCancelPolicy::kCancelOnRegionExit;
    addState(cancel_machine, 10, 2, std::nullopt, std::move(region_state));
    addState(cancel_machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("Safe", cancel_log));
    assert(cancel_machine.setInitialState({1, 1}).ok());
    assert(cancel_machine.setInitialState({2, 10}).ok());
    sm::TransitionRule global;
    global.from = 1;
    global.target = 2;
    global.event = 9;
    global.region = 1;
    global.global = true;
    assert(cancel_machine.addTransition(global).ok());
    assert(cancel_machine.start().ok());
    cancel_machine.update({0, 64, true});
    cancel_machine.postEvent(sm::Event(9));
    cancel_machine.update({64, 64, false});
    cancel_machine.update({0, 64, true});
    const auto event_log = cancel_machine.eventLog();
    assert(std::any_of(event_log.begin(), event_log.end(), [](const sm::EventLogRecord& record) {
        return record.kind == sm::EventLogRecord::Kind::kTaskCancelled && record.message == "cancelled on region exit";
    }));

    sm::StateMachine inactive_region_task("inactive_region_task");
    std::vector<std::string> inactive_log;
    addState(inactive_region_task, 1, 1, std::nullopt, std::make_unique<RecordingState>("Main", inactive_log));
    auto inactive_state = std::make_unique<RecordingState>("InactiveRegionTask", inactive_log);
    inactive_state->start_task_on_tick = true;
    inactive_state->task_policy = sm::TaskCancelPolicy::kCancelOnRegionExit;
    auto* inactive_state_ptr = inactive_state.get();
    addState(inactive_region_task, 10, 2, std::nullopt, std::move(inactive_state));
    addState(inactive_region_task, 2, 1, std::nullopt, std::make_unique<RecordingState>("Safe", inactive_log));
    assert(inactive_region_task.setInitialState({1, 1}).ok());
    assert(inactive_region_task.setInitialState({2, 10}).ok());
    sm::TransitionRule inactive_global;
    inactive_global.from = 1;
    inactive_global.target = 2;
    inactive_global.event = 10;
    inactive_global.region = 1;
    inactive_global.global = true;
    assert(inactive_region_task.addTransition(inactive_global).ok());
    assert(inactive_region_task.start().ok());
    inactive_region_task.update({0, 64, true});
    assert(inactive_region_task.cancelTask(inactive_state_ptr->last_task).ok());
    inactive_region_task.postEvent(sm::Event(10));
    inactive_region_task.update({64, 64, false});

    sm::StateMachine cancel_context("cancel_context");
    std::vector<std::string> context_log;
    auto cancel_state = std::make_unique<RecordingState>("CancelInContext", context_log);
    cancel_state->start_task_on_tick = true;
    cancel_state->cancel_task_on_tick = true;
    addState(cancel_context, 1, 1, std::nullopt, std::move(cancel_state));
    assert(cancel_context.setInitialState({1, 1}).ok());
    assert(cancel_context.start().ok());
    cancel_context.update({0, 64, true});
}

void testTransitionVariantsAndFaults() {
    std::vector<std::string> log;
    sm::StateMachine machine("variants");
    addState(machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("Root", log));
    addState(machine, 2, 1, 1, std::make_unique<RecordingState>("Leaf", log));
    addState(machine, 3, 1, 1, std::make_unique<RecordingState>("Target", log));
    assert(machine.setInitialState({1, 2}).ok());
    sm::TransitionRule parent;
    parent.from = 1;
    parent.target = 3;
    parent.event = 1;
    assert(machine.addTransition(parent).ok());
    sm::TransitionRule internal;
    internal.from = 3;
    internal.event = 2;
    internal.type = sm::TransitionType::kInternal;
    internal.action = [&](sm::StateContext&) {
        log.emplace_back("action:internal");
        return sm::Status{};
    };
    assert(machine.addTransition(internal).ok());
    sm::TransitionRule targetless;
    targetless.from = 3;
    targetless.event = 3;
    targetless.type = sm::TransitionType::kTargetless;
    targetless.action = [&](sm::StateContext&) {
        log.emplace_back("action:targetless");
        return sm::Status{};
    };
    assert(machine.addTransition(targetless).ok());
    assert(machine.start().ok());
    log.clear();
    machine.postEvent(sm::Event(1));
    machine.update({64, 64, false});
    assert(machine.currentState(1) == 3);
    assert((log == std::vector<std::string>{"exit:Leaf", "enter:Target"}));
    log.clear();
    machine.postEvent(sm::Event(2));
    machine.postEvent(sm::Event(3));
    machine.update({64, 64, false});
    assert((log == std::vector<std::string>{"action:internal", "action:targetless"}));

    sm::StateMachine limit_machine("limit");
    std::vector<std::string> limit_log;
    addState(limit_machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("A", limit_log));
    addState(limit_machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("B", limit_log));
    addState(limit_machine, 10, 2, std::nullopt, std::make_unique<RecordingState>("C", limit_log));
    addState(limit_machine, 11, 2, std::nullopt, std::make_unique<RecordingState>("D", limit_log));
    assert(limit_machine.setInitialState({1, 1}).ok());
    assert(limit_machine.setInitialState({2, 10}).ok());
    sm::TransitionRule a;
    a.from = 1;
    a.target = 2;
    a.event = 5;
    assert(limit_machine.addTransition(a).ok());
    sm::TransitionRule c;
    c.from = 10;
    c.target = 11;
    c.event = 5;
    assert(limit_machine.addTransition(c).ok());
    assert(limit_machine.start().ok());
    auto zero_limit = limit_machine.update({0, 0, false});
    assert(zero_limit.ok());
    limit_machine.postEvent(sm::Event(5));
    limit_machine.postEvent(sm::Event(5));
    auto limited = limit_machine.update({64, 1, false});
    assert(limited.value.hit_transition_limit);

    sm::StateMachine skip_machine("skip");
    std::vector<std::string> skip_log;
    addState(skip_machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("A", skip_log));
    addState(skip_machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("B", skip_log));
    addState(skip_machine, 10, 2, std::nullopt, std::make_unique<RecordingState>("C", skip_log));
    assert(skip_machine.setInitialState({1, 1}).ok());
    assert(skip_machine.setInitialState({2, 10}).ok());
    sm::TransitionRule skipped_global;
    skipped_global.from = 1;
    skipped_global.target = 2;
    skipped_global.event = 123;
    skipped_global.global = true;
    assert(skip_machine.addTransition(skipped_global).ok());
    sm::TransitionRule wrong_region;
    wrong_region.from = 1;
    wrong_region.target = 2;
    wrong_region.event = 20;
    wrong_region.region = 2;
    assert(skip_machine.addTransition(wrong_region).ok());
    sm::TransitionRule cross_region;
    cross_region.from = 1;
    cross_region.target = 10;
    cross_region.event = 20;
    cross_region.region = 1;
    assert(skip_machine.addTransition(cross_region).ok());
    assert(skip_machine.start().ok());
    skip_machine.postEvent(sm::Event(20));
    skip_machine.update({64, 64, false});

    sm::StateMachine enter_status_machine("enter_status");
    std::vector<std::string> enter_status_log;
    addState(enter_status_machine, 1, 1, std::nullopt, std::make_unique<RecordingState>("A", enter_status_log));
    auto enter_bad = std::make_unique<RecordingState>("EnterBad", enter_status_log);
    enter_bad->enter_status = sm::Status::error(sm::ErrorCode::kFaulted, "enter failed during transition");
    addState(enter_status_machine, 2, 1, std::nullopt, std::move(enter_bad));
    assert(enter_status_machine.setInitialState({1, 1}).ok());
    sm::TransitionRule enter_bad_transition;
    enter_bad_transition.from = 1;
    enter_bad_transition.target = 2;
    enter_bad_transition.event = 2;
    assert(enter_status_machine.addTransition(enter_bad_transition).ok());
    assert(enter_status_machine.start().ok());
    enter_status_machine.postEvent(sm::Event(2));
    assert(enter_status_machine.update({64, 64, false}).value.faults_recorded == 1);

    sm::StateMachine fault_machine("fault_variants");
    std::vector<std::string> fault_log;
    auto bad_event = std::make_unique<RecordingState>("BadEvent", fault_log);
    bad_event->event_status = sm::Status::error(sm::ErrorCode::kFaulted, "event failed");
    addState(fault_machine, 1, 1, std::nullopt, std::move(bad_event));
    addState(fault_machine, 2, 1, std::nullopt, std::make_unique<RecordingState>("Target", fault_log));
    assert(fault_machine.setInitialState({1, 1}).ok());
    sm::TransitionRule guard_std;
    guard_std.from = 1;
    guard_std.target = 2;
    guard_std.event = 10;
    guard_std.priority = 4;
    guard_std.guard = [](const sm::GuardContext& ctx) -> bool {
        (void)ctx.now();
        (void)ctx.elapsed(1);
        (void)ctx.snapshot();
        (void)ctx.event();
        throw std::runtime_error("guard std");
    };
    assert(fault_machine.addTransition(guard_std).ok());
    sm::TransitionRule guard_unknown = guard_std;
    guard_unknown.id = 0;
    guard_unknown.priority = 3;
    guard_unknown.guard = [](const sm::GuardContext&) -> bool {
        throw 7;
    };
    assert(fault_machine.addTransition(guard_unknown).ok());
    sm::TransitionRule action_bad;
    action_bad.from = 1;
    action_bad.event = 11;
    action_bad.type = sm::TransitionType::kInternal;
    action_bad.action = [](sm::StateContext&) {
        return sm::Status::error(sm::ErrorCode::kFaulted, "action failed");
    };
    assert(fault_machine.addTransition(action_bad).ok());
    sm::TransitionRule action_std = action_bad;
    action_std.id = 0;
    action_std.event = 12;
    action_std.action = [](sm::StateContext&) -> sm::Status {
        throw std::runtime_error("action std");
    };
    assert(fault_machine.addTransition(action_std).ok());
    sm::TransitionRule action_unknown = action_bad;
    action_unknown.id = 0;
    action_unknown.event = 13;
    action_unknown.action = [](sm::StateContext&) -> sm::Status {
        throw 7;
    };
    assert(fault_machine.addTransition(action_unknown).ok());
    assert(fault_machine.start().ok());
    fault_machine.postEvent(sm::Event(10));
    fault_machine.postEvent(sm::Event(11));
    fault_machine.postEvent(sm::Event(12));
    fault_machine.postEvent(sm::Event(13));
    auto faults = fault_machine.update({64, 64, false});
    assert(faults.value.faults_recorded >= 5);

    sm::RuntimeOptions one_depth;
    one_depth.max_fault_depth = 1;
    sm::StateMachine breaker("breaker", one_depth);
    auto bad_tick = std::make_unique<RecordingState>("BadTick", fault_log);
    bad_tick->throw_on_tick_unknown = true;
    addState(breaker, 1, 1, std::nullopt, std::move(bad_tick));
    assert(breaker.setInitialState({1, 1}).ok());
    assert(breaker.start().ok());
    breaker.update({64, 64, true});
    breaker.update({64, 64, true});
    assert(breaker.lifecycle() == sm::MachineLifecycle::kFaulted);
}

void testCallbackFailures() {
    std::vector<std::string> log;
    sm::StateMachine enter_throw("enter_throw");
    auto enter = std::make_unique<RecordingState>("EnterThrow", log);
    enter->throw_on_enter_std = true;
    addState(enter_throw, 1, 1, std::nullopt, std::move(enter));
    assert(enter_throw.setInitialState({1, 1}).ok());
    assert(!enter_throw.start().ok());

    sm::StateMachine exit_throw("exit_throw");
    auto exit_state = std::make_unique<RecordingState>("ExitThrow", log);
    exit_state->throw_on_exit_unknown = true;
    addState(exit_throw, 1, 1, std::nullopt, std::move(exit_state));
    addState(exit_throw, 2, 1, std::nullopt, std::make_unique<RecordingState>("B", log));
    assert(exit_throw.setInitialState({1, 1}).ok());
    sm::TransitionRule to_b;
    to_b.from = 1;
    to_b.target = 2;
    to_b.event = 1;
    assert(exit_throw.addTransition(to_b).ok());
    assert(exit_throw.start().ok());
    exit_throw.postEvent(sm::Event(1));
    assert(exit_throw.update({64, 64, false}).value.faults_recorded == 1);

    sm::StateMachine event_throw("event_throw");
    auto event_state = std::make_unique<RecordingState>("EventThrow", log);
    event_state->throw_on_event_std = true;
    addState(event_throw, 1, 1, std::nullopt, std::move(event_state));
    assert(event_throw.setInitialState({1, 1}).ok());
    assert(event_throw.start().ok());
    event_throw.postEvent(sm::Event(9));
    assert(event_throw.update({64, 64, false}).value.faults_recorded == 1);

    sm::StateMachine global_exit_fault("global_exit_fault");
    auto other_exit_bad = std::make_unique<RecordingState>("OtherExitBad", log);
    other_exit_bad->exit_status = sm::Status::error(sm::ErrorCode::kFaulted, "other exit failed");
    addState(global_exit_fault, 1, 1, std::nullopt, std::make_unique<RecordingState>("Main", log));
    addState(global_exit_fault, 2, 1, std::nullopt, std::make_unique<RecordingState>("Safe", log));
    addState(global_exit_fault, 10, 2, std::nullopt, std::move(other_exit_bad));
    assert(global_exit_fault.setInitialState({1, 1}).ok());
    assert(global_exit_fault.setInitialState({2, 10}).ok());
    sm::TransitionRule global;
    global.from = 1;
    global.target = 2;
    global.event = 20;
    global.global = true;
    global.region = 1;
    assert(global_exit_fault.addTransition(global).ok());
    assert(global_exit_fault.start().ok());
    global_exit_fault.postEvent(sm::Event(20));
    assert(global_exit_fault.update({64, 64, false}).value.faults_recorded == 1);

    sm::StateMachine stop_exit_fault("stop_exit_fault");
    auto stop_bad = std::make_unique<RecordingState>("StopExitBad", log);
    stop_bad->exit_status = sm::Status::error(sm::ErrorCode::kFaulted, "stop exit failed");
    addState(stop_exit_fault, 1, 1, std::nullopt, std::move(stop_bad));
    assert(stop_exit_fault.setInitialState({1, 1}).ok());
    assert(stop_exit_fault.start().ok());
    assert(stop_exit_fault.stop().ok());
    assert(stop_exit_fault.update({64, 64, false}).value.faults_recorded == 1);
}

void testEventQueueCapacitySmoke() {
    std::vector<std::string> log;
    sm::RuntimeOptions options;
    options.max_pending_events = 2;
    sm::StateMachine machine("capacity_smoke", options);
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::make_unique<RecordingState>("A", log));
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    assert(machine.start().ok());

    assert(machine.postEvent(sm::Event(10)).ok());
    assert(machine.postEvent(sm::Event(20)).ok());
    auto overflow = machine.postEvent(sm::Event(30));
    assert(!overflow.ok());
    assert(overflow.code == sm::ErrorCode::kLimitReached);

    auto snapshot = machine.snapshot();
    assert(snapshot.inbox_size == 2);
    auto result = machine.update({64, 64, false});
    assert(result.ok());
    assert(result.value.events_processed == 2);
}

void testConcurrentPostSmoke() {
    std::vector<std::string> log;
    sm::RuntimeOptions options;
    options.event_log_capacity = 512;
    options.max_pending_events = 512;
    sm::StateMachine machine("concurrent_post_smoke", options);
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::make_unique<RecordingState>("A", log));
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    assert(machine.start().ok());

    constexpr int kThreads = 8;
    constexpr int kEventsPerThread = 32;
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            for (int i = 0; i < kEventsPerThread; ++i) {
                sm::Event event(static_cast<sm::EventId>(1000 + thread_index * kEventsPerThread + i));
                if (machine.postEvent(event).ok()) {
                    ++accepted;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    assert(accepted == kThreads * kEventsPerThread);
    auto snapshot = machine.snapshot();
    assert(snapshot.inbox_size == static_cast<size_t>(accepted));
    auto result = machine.update({512, 512, false});
    assert(result.ok());
    assert(result.value.events_processed == static_cast<size_t>(accepted));
}

void testFaultEventBypassesPendingCapacitySmoke() {
    std::vector<std::string> log;
    sm::RuntimeOptions options;
    options.max_pending_events = 1;
    options.max_fault_depth = 4;
    sm::StateMachine machine("fault_event_capacity", options);
    auto faulting = std::make_unique<RecordingState>("Faulting", log);
    faulting->tick_status = sm::Status::error(sm::ErrorCode::kFaulted, "tick fault");
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::move(faulting));
    addState(machine, 2, sm::kDefaultRegion, std::nullopt, std::make_unique<RecordingState>("Fault", log));
    sm::TransitionRule to_fault;
    to_fault.from = 1;
    to_fault.target = 2;
    to_fault.event = sm::kFaultEvent;
    assert(machine.addTransition(to_fault).ok());
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    assert(machine.start().ok());
    assert(machine.postEvent(sm::Event(42)).ok());

    auto tick_result = machine.update({0, 64, true});
    assert(tick_result.ok());
    assert(tick_result.value.faults_recorded == 1);
    assert(machine.snapshot().inbox_size == 2);

    auto event_result = machine.update({64, 64, false});
    assert(event_result.ok());
    assert(event_result.value.events_processed == 2);
    assert(event_result.value.transitions_committed == 1);
    assert(machine.currentState(sm::kDefaultRegion) == 2);
}

void testFaultFuseRejectsEventsSmoke() {
    std::vector<std::string> log;
    sm::RuntimeOptions options;
    options.max_fault_depth = 1;
    sm::StateMachine machine("fault_fuse_rejects_events", options);
    auto faulting = std::make_unique<RecordingState>("Faulting", log);
    faulting->tick_status = sm::Status::error(sm::ErrorCode::kFaulted, "tick fault");
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::move(faulting));
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    assert(machine.start().ok());

    auto first_fault = machine.update({64, 64, true});
    assert(first_fault.ok());
    assert(first_fault.value.faults_recorded == 1);
    assert(machine.lifecycle() == sm::MachineLifecycle::kRunning);

    auto fused_fault = machine.update({64, 64, true});
    assert(fused_fault.ok());
    assert(fused_fault.value.faults_recorded == 1);
    assert(machine.lifecycle() == sm::MachineLifecycle::kFaulted);

    auto rejected = machine.postEvent(sm::Event(99));
    assert(!rejected.ok());
    assert(rejected.code == sm::ErrorCode::kFaulted);
    assert(machine.snapshot().inbox_size == 1);
}

void testTickGeneratedEventTwoStageUpdateSmoke() {
    std::vector<std::string> log;
    sm::StateMachine machine("tick_event_two_stage");
    auto state = std::make_unique<RecordingState>("A", log);
    state->post_on_tick = 7;
    addState(machine, 1, sm::kDefaultRegion, std::nullopt, std::move(state));
    addState(machine, 2, sm::kDefaultRegion, std::nullopt, std::make_unique<RecordingState>("B", log));
    assert(machine.setInitialState({sm::kDefaultRegion, 1}).ok());
    sm::TransitionRule to_b;
    to_b.from = 1;
    to_b.target = 2;
    to_b.event = 7;
    assert(machine.addTransition(to_b).ok());
    assert(machine.start().ok());
    log.clear();

    auto tick_stage = machine.update({64, 64, true});
    assert(tick_stage.ok());
    assert(tick_stage.value.events_taken == 0);
    assert(tick_stage.value.generated_events == 1);
    assert(machine.currentState(sm::kDefaultRegion) == 1);
    assert(machine.snapshot().inbox_size == 1);
    assert((log == std::vector<std::string>{"tick:A"}));

    auto event_stage = machine.update({64, 64, false});
    assert(event_stage.ok());
    assert(event_stage.value.events_taken == 1);
    assert(event_stage.value.events_processed == 1);
    assert(event_stage.value.transitions_committed == 1);
    assert(machine.currentState(sm::kDefaultRegion) == 2);
    assert((log == std::vector<std::string>{"tick:A", "exit:A", "enter:B"}));
}

} // namespace

TEST(StateMachineRuntime, FifoAndSnapshot) {
    testFifoAndSnapshot();
}

TEST(StateMachineRuntime, ThreadedSequence) {
    testThreadedSequence();
}

TEST(StateMachineRuntime, OwnerThreadAndReentrantUpdate) {
    testOwnerThreadAndReentrantUpdate();
}

TEST(StateMachineRuntime, BoundedSnapshotLimit) {
    testBoundedSnapshotLimit();
}

TEST(StateMachineRuntime, TransitionLimitRequeuesUnprocessedEvents) {
    testTransitionLimitRequeuesUnprocessedEvents();
}

TEST(StateMachineRuntime, HierarchyAndExternalSelf) {
    testHierarchyAndExternalSelf();
}

TEST(StateMachineRuntime, ParallelRegionsAndGlobalTransition) {
    testParallelAndGlobal();
}

TEST(StateMachineRuntime, TaskFaultStopAndLimits) {
    testTaskStaleFaultStopAndLimits();
}

TEST(StateMachineRuntime, LifecycleErrorsAndAccessors) {
    testLifecycleErrorsAndAccessors();
}

TEST(StateMachineRuntime, ContextTasksAndRegionCancel) {
    testContextTasksAndRegionCancel();
}

TEST(StateMachineRuntime, TransitionVariantsAndFaults) {
    testTransitionVariantsAndFaults();
}

TEST(StateMachineRuntime, CallbackFailures) {
    testCallbackFailures();
}

TEST(StateMachineSmoke, EventQueueCapacity) {
    testEventQueueCapacitySmoke();
}

TEST(StateMachineSmoke, ConcurrentPost) {
    testConcurrentPostSmoke();
}

TEST(StateMachineSmoke, FaultEventBypassesPendingCapacity) {
    testFaultEventBypassesPendingCapacitySmoke();
}

TEST(StateMachineSmoke, FaultFuseRejectsEvents) {
    testFaultFuseRejectsEventsSmoke();
}

TEST(StateMachineSmoke, TickGeneratedEventTwoStageUpdate) {
    testTickGeneratedEventTwoStageUpdateSmoke();
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
