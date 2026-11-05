#pragma once

#include <type_traits>
#include <utility>
#include <variant>

namespace state_machine::adt {

struct Stay {};

template <class State> struct Enter {
    // The destination value is consumed by apply().
    State value;
};
template <class State> Enter(State) -> Enter<State>;

template <class... States> using Transition = std::variant<Stay, Enter<States>...>;

// One owner calls dispatch/apply and reads state(). There is no ingress queue,
// clock, lock or callback registry here. Spec::handle must cover every active
// alternative for each dispatched event, with one explicit result type.
// A parent may own another AdtMachine: a child transition leaves its parent alive.
template <class Spec> class AdtMachine {
    template <std::size_t... I> static constexpr bool safe_states(std::index_sequence<I...>) {
        return ((std::is_nothrow_move_constructible_v<std::variant_alternative_t<I, typename Spec::State>> &&
                 std::is_nothrow_destructible_v<std::variant_alternative_t<I, typename Spec::State>>)
                // Every alternative needs both lifetime properties.
                &&...);
    }

  public:
    using State = typename Spec::State;
    using Time = typename Spec::Time;
    static_assert(safe_states(std::make_index_sequence<std::variant_size_v<State>>{}),
                  "ADT states must have nonthrowing move construction and destruction");
    static_assert(std::is_nothrow_copy_constructible_v<Time> && std::is_nothrow_copy_assignable_v<Time>,
                  "ADT entry time must be a nonthrowing value");

    template <class Initial>
    explicit AdtMachine(Initial initial, Time now) noexcept
        : state_(std::in_place_type<Initial>, std::move(initial)), entered_at_(now) {}

    AdtMachine(const AdtMachine&) = delete;
    AdtMachine& operator=(const AdtMachine&) = delete;
    AdtMachine(AdtMachine&&) noexcept = default;
    AdtMachine& operator=(AdtMachine&&) = delete;

    const State& state() const noexcept { return state_; }
    Time entered_at() const noexcept { return entered_at_; }

    template <class Event, class... Context> decltype(auto) dispatch(const Event& event, Context&... context) {
        return std::visit(
            [&](auto& active) -> decltype(auto) {
                return Spec::handle(active, event, context...);
            },
            state_);
    }

    void apply(Stay, Time) noexcept {}

    template <class Next> void apply(Enter<Next> next, Time now) noexcept {
        // Assignment would preserve the lifetime on same-kind entry. Emplacement
        // deliberately ends the old instance, including all its local resources.
        state_.template emplace<Next>(std::move(next.value));
        entered_at_ = now;
    }

    template <class... Next> void apply(Transition<Next...> next, Time now) noexcept {
        std::visit(
            [&](auto& outcome) {
                apply(std::move(outcome), now);
            },
            next);
    }

  private:
    State state_;
    Time entered_at_;
};

} // namespace state_machine::adt
