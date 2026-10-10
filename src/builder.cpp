#include "state_machine_impl.hpp"

#include <sstream>

namespace state_machine {

namespace {
constexpr RegionId kImplicitRegionBase = 0x80000000u;

class PassiveState final : public State {
  public:
    explicit PassiveState(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }

  private:
    std::string name_;
};
} // namespace

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

} // namespace state_machine
