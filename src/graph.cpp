#include "state_machine_impl.hpp"

#include <set>

namespace state_machine {

void StateMachine::Impl::sortRegionOrder() {
    std::stable_sort(region_order.begin(), region_order.end(), [&](RegionId lhs, RegionId rhs) {
        const auto& lhs_region = regions.at(lhs);
        const auto& rhs_region = regions.at(rhs);
        if (lhs_region.config.execution_order != rhs_region.config.execution_order) {
            return lhs_region.config.execution_order < rhs_region.config.execution_order;
        }
        return lhs_region.registration_order < rhs_region.registration_order;
    });
}

RegionId StateMachine::Impl::stateRegion(StateId state) const {
    const auto it = states.find(state);
    return it == states.end() ? 0 : it->second.config.region;
}

RegionId StateMachine::Impl::topLevelRegionOfRegion(RegionId region) const {
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

RegionId StateMachine::Impl::topLevelRegionOfState(StateId state) const {
    return topLevelRegionOfRegion(stateRegion(state));
}

std::vector<StateId> StateMachine::Impl::computePathToRoot(StateId state) const {
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

const std::vector<StateId>& StateMachine::Impl::pathOf(StateId state) const {
    static const std::vector<StateId> kNoPath;
    const auto it = states.find(state);
    return it == states.end() ? kNoPath : it->second.path_to_root;
}

void StateMachine::Impl::markDerivedStale() {
    derived_valid = false;
}

void StateMachine::Impl::ensureDerived() {
    if (!derived_valid) {
        rebuildDerived();
    }
}

void StateMachine::Impl::rebuildDerived() {
    derived_valid = true;
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

void StateMachine::Impl::setActiveLeaf(RegionEntry& region, StateId leaf) {
    region.active_leaf = leaf;
    ++active_epoch;
}

void StateMachine::Impl::collectChain(const RegionEntry& region, std::vector<const StateEntry*>& out) const {
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

const std::vector<const StateMachine::Impl::StateEntry*>& StateMachine::Impl::chainOf(const RegionEntry& region) const {
    if (region.chain_epoch != active_epoch) {
        region.chain.clear();
        collectChain(region, region.chain);
        region.chain_epoch = active_epoch;
    }
    return region.chain;
}

std::vector<StateId> StateMachine::Impl::activeStatesInRegion(RegionId region) const {
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

const StateMachine::Impl::StateEntry* StateMachine::Impl::activeLeafEntry(RegionId region) const {
    const auto it = regions.find(region);
    if (it == regions.end()) {
        return nullptr;
    }
    const auto& chain = chainOf(it->second);
    return chain.empty() ? nullptr : chain.back();
}

StateId StateMachine::Impl::activeLeafOf(RegionId region) const {
    const StateEntry* leaf = activeLeafEntry(region);
    return leaf == nullptr ? 0 : leaf->config.id;
}

bool StateMachine::Impl::activeInPath(StateSelection selection) const {
    const auto it = regions.find(selection.region);
    if (it == regions.end()) {
        return false;
    }
    const auto& chain = chainOf(it->second);
    return std::any_of(chain.begin(), chain.end(), [&](const StateEntry* entry) {
        return entry->config.id == selection.state;
    });
}

size_t StateMachine::Impl::commonPrefix(const std::vector<StateId>& lhs, const std::vector<StateId>& rhs) const {
    size_t prefix = 0;
    while (prefix < lhs.size() && prefix < rhs.size() && lhs[prefix] == rhs[prefix]) {
        ++prefix;
    }
    return prefix;
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
    impl_->markDerivedStale();
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
    impl_->markDerivedStale();
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
    impl_->markDerivedStale();
    return Status{};
}

} // namespace state_machine
