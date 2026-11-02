#pragma once

#include "state_machine/algebra.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace state_machine {
namespace adt {

// ============================================================================
// 1. Transition<StateVariant, RejectReason>: Algebraic Transition Result
// ============================================================================
// Represents the codomain of a state transition morphism:
//   (State x Event) -> Stay + Move(NextState, forward_event) + Reject(Reason)
template <typename StateVariant, typename RejectReason = std::string_view>
struct Transition {
    struct Stay {};
    struct Move {
        StateVariant next_state;
        bool forward_event_to_next{false};
    };
    struct Reject {
        RejectReason reason{};
    };

    std::variant<Stay, Move, Reject> action{Stay{}};

    constexpr static Transition stay() noexcept { return Transition{Stay{}}; }

    template <typename NextState>
    constexpr static Transition to(NextState&& next, bool forward_event = false) {
        return Transition{Move{StateVariant{std::forward<NextState>(next)}, forward_event}};
    }

    constexpr static Transition reject(RejectReason reason) {
        return Transition{Reject{std::move(reason)}};
    }

    constexpr bool isStay() const noexcept { return std::holds_alternative<Stay>(action); }
    constexpr bool isMove() const noexcept { return std::holds_alternative<Move>(action); }
    constexpr bool isReject() const noexcept { return std::holds_alternative<Reject>(action); }
};

// ============================================================================
// 2. Fixed-Size Transition Trace Record (Zero Heap Allocation)
// ============================================================================
struct TransitionTrace {
    uint64_t epoch{0};
    std::size_t from_index{0};
    std::size_t to_index{0};
    double timestamp_sec{0.0};
    bool auto_guard{false};
};

// ============================================================================
// 3. Default Spec Hook Fallbacks (SFINAE / C++17 Optional Hooks)
// ============================================================================
namespace detail {

template <typename Spec, typename State, typename Context, typename OutputBuffer, typename = void>
struct HasOnEnter : std::false_type {};

template <typename Spec, typename State, typename Context, typename OutputBuffer>
struct HasOnEnter<Spec,
                  State,
                  Context,
                  OutputBuffer,
                  std::void_t<decltype(Spec::onEnter(std::declval<State&>(),
                                                     std::declval<Context&>(),
                                                     0.0,
                                                     std::declval<OutputBuffer&>()))>>
    : std::true_type {};

template <typename Spec, typename State, typename Context, typename OutputBuffer, typename = void>
struct HasOnExit : std::false_type {};

template <typename Spec, typename State, typename Context, typename OutputBuffer>
struct HasOnExit<Spec,
                 State,
                 Context,
                 OutputBuffer,
                 std::void_t<decltype(Spec::onExit(std::declval<State&>(),
                                                   std::declval<Context&>(),
                                                   0.0,
                                                   std::declval<OutputBuffer&>()))>>
    : std::true_type {};

template <typename Spec, typename State, typename Context, typename OutputBuffer, typename = void>
struct HasCheckGuards : std::false_type {};

template <typename Spec, typename State, typename Context, typename OutputBuffer>
struct HasCheckGuards<Spec,
                      State,
                      Context,
                      OutputBuffer,
                      std::void_t<decltype(Spec::checkGuards(std::declval<State&>(),
                                                             std::declval<Context&>(),
                                                             0.0,
                                                             0.0,
                                                             std::declval<OutputBuffer&>()))>>
    : std::true_type {};

template <typename Spec, typename State, typename Context, typename OutputBuffer, typename = void>
struct HasOnTick : std::false_type {};

template <typename Spec, typename State, typename Context, typename OutputBuffer>
struct HasOnTick<Spec,
                 State,
                 Context,
                 OutputBuffer,
                 std::void_t<decltype(Spec::onTick(std::declval<State&>(),
                                                   std::declval<Context&>(),
                                                   0.0,
                                                   0.0,
                                                   std::declval<OutputBuffer&>()))>>
    : std::true_type {};

template <typename Spec, typename State, typename Context, typename = void>
struct HasTickPolicy : std::false_type {};

template <typename Spec, typename State, typename Context>
struct HasTickPolicy<Spec,
                     State,
                     Context,
                     std::void_t<decltype(Spec::tickPolicy(
                         std::declval<const State&>(), std::declval<const Context&>(), 0.0, 0.0))>>
    : std::true_type {};

template <typename Spec, typename State, typename = void>
struct HasStateName : std::false_type {};

template <typename Spec, typename State>
struct HasStateName<
    Spec,
    State,
    std::void_t<decltype(Spec::stateName(std::declval<const State&>()))>> : std::true_type {};

} // namespace detail

// ============================================================================
// 4. AdtMachine<Spec, MaxOutputs, MaxTrace, MaxAutoHops>
// ============================================================================
// Value-semantics Algebraic Data Type state machine engine.
//
// Key properties:
//   1. Illegal states are unrepresentable: each alternative in `StateVariant`
//      owns its phase-specific fields by value. Transitioning destroys the old
//      state and move-constructs the new state in place.
//   2. Zero heap allocation: events (`EventVariant`), outputs (`OutputVariant`),
//      and transition traces live in inline storage.
//   3. Synchronous admission: `dispatch()` immediately evaluates the transition
//      and returns `Expected<DispatchReceipt, RejectReason>`.
//   4. Single-pass atomic `step()`: evaluates guard transitions and then invokes
//      `onTick` on the settled state in a single call, returning the active
//      state's `TickPolicy` for demand-driven scheduling.
template <typename Spec,
          std::size_t MaxOutputs = 16,
          std::size_t MaxTrace = 32,
          std::size_t MaxAutoHops = 8>
class AdtMachine {
  public:
    using StateVariant = typename Spec::StateVariant;
    using EventVariant = typename Spec::EventVariant;
    using OutputVariant = typename Spec::OutputVariant;
    using Context = typename Spec::Context;
    using RejectReason = typename Spec::RejectReason;
    using Trans = Transition<StateVariant, RejectReason>;
    using OutputBuffer = BoundedVector<OutputVariant, MaxOutputs>;
    using TraceBuffer = FixedRingBuffer<TransitionTrace, MaxTrace>;

    struct DispatchReceipt {
        bool transitioned{false};
        std::size_t from_index{0};
        std::size_t to_index{0};
        uint64_t state_epoch{0};
        TickPolicy tick_policy{TickIdle{}};
    };

    struct StepReceipt {
        std::size_t auto_transitions{0};
        std::size_t active_index{0};
        uint64_t state_epoch{0};
        TickPolicy tick_policy{TickIdle{}};
    };

    explicit AdtMachine(StateVariant initial_state = StateVariant{}, double now_sec = 0.0)
        : state_(std::move(initial_state)), entered_at_sec_(now_sec), last_step_sec_(now_sec) {}

    // Resets the machine to `initial_state` and runs `onEnter` with `ctx`.
    void reset(StateVariant initial_state, Context& ctx, double now_sec = 0.0) {
        outputs_.clear();
        trace_.clear();
        state_ = std::move(initial_state);
        entered_at_sec_ = now_sec;
        last_step_sec_ = now_sec;
        ++state_epoch_;
        invokeOnEnter(ctx, now_sec);
    }

    // Runs `onEnter` for the initial state without incrementing epoch.
    void start(Context& ctx, double now_sec = 0.0) {
        entered_at_sec_ = now_sec;
        last_step_sec_ = now_sec;
        invokeOnEnter(ctx, now_sec);
    }

    // Synchronously dispatches a discrete event against the active state.
    Expected<DispatchReceipt, RejectReason> dispatch(Context& ctx,
                                                     const EventVariant& event,
                                                     double now_sec) {
        const std::size_t orig_index = state_.index();
        Trans trans = std::visit(
            [&](auto& current_state, const auto& concrete_event) -> Trans {
                return Spec::onEvent(current_state, ctx, concrete_event, now_sec, outputs_);
            },
            state_,
            event);

        if (const auto* rej = std::get_if<typename Trans::Reject>(&trans.action)) {
            return unexpected(rej->reason);
        }

        bool transitioned = false;
        if (auto* mv = std::get_if<typename Trans::Move>(&trans.action)) {
            const bool forward_event = mv->forward_event_to_next;
            commitTransition(std::move(mv->next_state), ctx, now_sec, false);
            transitioned = true;
            if (forward_event) {
                // Forward the triggering event to the newly entered state.
                Trans follow = std::visit(
                    [&](auto& entered_state, const auto& concrete_event) -> Trans {
                        return Spec::onEvent(entered_state, ctx, concrete_event, now_sec, outputs_);
                    },
                    state_,
                    event);
                if (auto* mv2 = std::get_if<typename Trans::Move>(&follow.action)) {
                    commitTransition(std::move(mv2->next_state), ctx, now_sec, false);
                }
            }
        }

        // Also evaluate any immediate automatic guard transitions unlocked by the event.
        for (std::size_t hop = 0; hop < MaxAutoHops; ++hop) {
            if (!evaluateOneGuardHop(ctx, now_sec)) {
                break;
            }
            transitioned = true;
        }

        return DispatchReceipt{
            transitioned,
            orig_index,
            state_.index(),
            state_epoch_,
            currentTickPolicy(ctx, now_sec),
        };
    }

    // Single-pass atomic step:
    //   1. Evaluates automatic guard transitions up to MaxAutoHops.
    //   2. Invokes `onTick` on the settled active state.
    //   3. Evaluates post-tick guard transitions if `onTick` updated state invariants.
    //   4. Returns `StepReceipt` containing the active state's `TickPolicy`.
    StepReceipt step(Context& ctx, double now_sec) {
        std::size_t hops = 0;
        while (hops < MaxAutoHops && evaluateOneGuardHop(ctx, now_sec)) {
            ++hops;
        }

        const double elapsed_sec = now_sec - entered_at_sec_;
        std::visit(
            [&](auto& s) {
                using S = std::decay_t<decltype(s)>;
                if constexpr (detail::HasOnTick<Spec, S, Context, OutputBuffer>::value) {
                    Spec::onTick(s, ctx, now_sec, elapsed_sec, outputs_);
                }
            },
            state_);

        while (hops < MaxAutoHops && evaluateOneGuardHop(ctx, now_sec)) {
            ++hops;
        }

        last_step_sec_ = now_sec;
        return StepReceipt{
            hops,
            state_.index(),
            state_epoch_,
            currentTickPolicy(ctx, now_sec),
        };
    }

    // Directly forces a transition to `next_state` (e.g. emergency fail-closed fault).
    template <typename NextState>
    void forceTransition(NextState&& next_state, Context& ctx, double now_sec) {
        commitTransition(StateVariant{std::forward<NextState>(next_state)}, ctx, now_sec, true);
    }

    TickPolicy currentTickPolicy(const Context& ctx, double now_sec) const noexcept {
        return std::visit(
            [&](const auto& s) -> TickPolicy {
                using S = std::decay_t<decltype(s)>;
                if constexpr (detail::HasTickPolicy<Spec, S, Context>::value) {
                    return Spec::tickPolicy(s, ctx, now_sec, entered_at_sec_);
                } else {
                    return TickIdle{};
                }
            },
            state_);
    }

    std::string_view currentStateName() const noexcept {
        return std::visit(
            [](const auto& s) -> std::string_view {
                using S = std::decay_t<decltype(s)>;
                if constexpr (detail::HasStateName<Spec, S>::value) {
                    return Spec::stateName(s);
                } else {
                    return "State";
                }
            },
            state_);
    }

    template <typename S>
    constexpr bool isIn() const noexcept {
        return std::holds_alternative<S>(state_);
    }

    template <typename S>
    constexpr const S* getIf() const noexcept {
        return std::get_if<S>(&state_);
    }

    template <typename S>
    constexpr S* getIfMutable() noexcept {
        return std::get_if<S>(&state_);
    }

    constexpr const StateVariant& state() const noexcept { return state_; }
    constexpr StateVariant& stateMutable() noexcept { return state_; }
    constexpr std::size_t stateIndex() const noexcept { return state_.index(); }
    constexpr uint64_t stateEpoch() const noexcept { return state_epoch_; }
    constexpr double enteredAtSec() const noexcept { return entered_at_sec_; }
    constexpr double lastStepSec() const noexcept { return last_step_sec_; }
    constexpr double stateElapsedSec(double now_sec) const noexcept {
        return now_sec - entered_at_sec_;
    }

    constexpr const OutputBuffer& outputs() const noexcept { return outputs_; }
    constexpr OutputBuffer& outputsMutable() noexcept { return outputs_; }
    constexpr void clearOutputs() noexcept { outputs_.clear(); }

    template <typename Fn>
    void drainOutputs(Fn&& consumer) {
        for (std::size_t i = 0; i < outputs_.size(); ++i) {
            consumer(outputs_[i]);
        }
        outputs_.clear();
    }

    constexpr const TraceBuffer& trace() const noexcept { return trace_; }
    constexpr void clearTrace() noexcept { trace_.clear(); }

  private:
    bool evaluateOneGuardHop(Context& ctx, double now_sec) {
        const double elapsed_sec = now_sec - entered_at_sec_;
        Trans guard_trans = std::visit(
            [&](auto& s) -> Trans {
                using S = std::decay_t<decltype(s)>;
                if constexpr (detail::HasCheckGuards<Spec, S, Context, OutputBuffer>::value) {
                    return Spec::checkGuards(s, ctx, now_sec, elapsed_sec, outputs_);
                } else {
                    return Trans::stay();
                }
            },
            state_);

        if (auto* mv = std::get_if<typename Trans::Move>(&guard_trans.action)) {
            commitTransition(std::move(mv->next_state), ctx, now_sec, true);
            return true;
        }
        return false;
    }

    void commitTransition(StateVariant&& next_state,
                          Context& ctx,
                          double now_sec,
                          bool is_auto_guard) {
        const std::size_t from_idx = state_.index();
        invokeOnExit(ctx, now_sec);
        state_ = std::move(next_state);
        entered_at_sec_ = now_sec;
        ++state_epoch_;
        trace_.push(TransitionTrace{
            state_epoch_,
            from_idx,
            state_.index(),
            now_sec,
            is_auto_guard,
        });
        invokeOnEnter(ctx, now_sec);
    }

    void invokeOnEnter(Context& ctx, double now_sec) {
        std::visit(
            [&](auto& s) {
                using S = std::decay_t<decltype(s)>;
                if constexpr (detail::HasOnEnter<Spec, S, Context, OutputBuffer>::value) {
                    Spec::onEnter(s, ctx, now_sec, outputs_);
                }
            },
            state_);
    }

    void invokeOnExit(Context& ctx, double now_sec) {
        std::visit(
            [&](auto& s) {
                using S = std::decay_t<decltype(s)>;
                if constexpr (detail::HasOnExit<Spec, S, Context, OutputBuffer>::value) {
                    Spec::onExit(s, ctx, now_sec, outputs_);
                }
            },
            state_);
    }

    StateVariant state_{};
    double entered_at_sec_{0.0};
    double last_step_sec_{0.0};
    uint64_t state_epoch_{1};
    OutputBuffer outputs_{};
    TraceBuffer trace_{};
};

} // namespace adt
} // namespace state_machine
