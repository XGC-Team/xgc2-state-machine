#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace state_machine {
namespace adt {

// ============================================================================
// 1. Sum-Type Elimination Combinator (Case Analysis: A + B -> C)
// ============================================================================
// By the universal property of the coproduct (sum type), a morphism out of
// A + B is uniquely determined by a pair of morphisms (A -> C) x (B -> C).
template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};
template <class... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

// ============================================================================
// 2. Expected<T, E>: Synchronous Admission & Result Sum Type (T + E)
// ============================================================================
template <typename E>
class Unexpected {
  public:
    constexpr explicit Unexpected(const E& error) : error_(error) {}
    constexpr explicit Unexpected(E&& error) : error_(std::move(error)) {}

    constexpr const E& error() const& noexcept { return error_; }
    constexpr E& error() & noexcept { return error_; }
    constexpr E&& error() && noexcept { return std::move(error_); }

  private:
    E error_;
};

template <typename E>
constexpr Unexpected<std::decay_t<E>> unexpected(E&& error) {
    return Unexpected<std::decay_t<E>>(std::forward<E>(error));
}

template <typename T, typename E>
class Expected {
  public:
    using value_type = T;
    using error_type = E;

    constexpr Expected(const T& value) : storage_(std::in_place_index<0>, value) {}
    constexpr Expected(T&& value) : storage_(std::in_place_index<0>, std::move(value)) {}
    constexpr Expected(const Unexpected<E>& err) : storage_(std::in_place_index<1>, err.error()) {}
    constexpr Expected(Unexpected<E>&& err)
        : storage_(std::in_place_index<1>, std::move(err).error()) {}

    constexpr bool hasValue() const noexcept { return storage_.index() == 0; }
    constexpr bool ok() const noexcept { return hasValue(); }
    constexpr explicit operator bool() const noexcept { return hasValue(); }

    constexpr const T& value() const& { return std::get<0>(storage_); }
    constexpr T& value() & { return std::get<0>(storage_); }
    constexpr T&& value() && { return std::move(std::get<0>(storage_)); }

    constexpr const T* operator->() const noexcept { return &std::get<0>(storage_); }
    constexpr T* operator->() noexcept { return &std::get<0>(storage_); }
    constexpr const T& operator*() const& noexcept { return std::get<0>(storage_); }
    constexpr T& operator*() & noexcept { return std::get<0>(storage_); }

    constexpr const E& error() const& { return std::get<1>(storage_); }
    constexpr E& error() & { return std::get<1>(storage_); }
    constexpr E&& error() && { return std::move(std::get<1>(storage_)); }

    template <typename U>
    constexpr T valueOr(U&& fallback) const& {
        return hasValue() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

    template <typename F>
    constexpr auto map(F&& fn) const& -> Expected<decltype(fn(std::declval<const T&>())), E> {
        using NextT = decltype(fn(std::declval<const T&>()));
        if (hasValue()) {
            return Expected<NextT, E>(fn(std::get<0>(storage_)));
        }
        return unexpected(std::get<1>(storage_));
    }

  private:
    std::variant<T, E> storage_;
};

template <typename E>
class Expected<void, E> {
  public:
    using value_type = void;
    using error_type = E;

    constexpr Expected() : storage_(std::in_place_index<0>, std::monostate{}) {}
    constexpr Expected(const Unexpected<E>& err) : storage_(std::in_place_index<1>, err.error()) {}
    constexpr Expected(Unexpected<E>&& err)
        : storage_(std::in_place_index<1>, std::move(err).error()) {}

    constexpr bool hasValue() const noexcept { return storage_.index() == 0; }
    constexpr bool ok() const noexcept { return hasValue(); }
    constexpr explicit operator bool() const noexcept { return hasValue(); }

    constexpr const E& error() const& { return std::get<1>(storage_); }
    constexpr E& error() & { return std::get<1>(storage_); }
    constexpr E&& error() && { return std::move(std::get<1>(storage_)); }

  private:
    std::variant<std::monostate, E> storage_;
};

// ============================================================================
// 3. TickPolicy Monoid: State-Driven Demand Scheduling
// ============================================================================
// Every state declares its temporal demand:
//   - TickIdle:     purely event-driven (0 Hz timer; wakes only on external event)
//   - TickPeriodic: periodic cadence with period_sec > 0
//   - TickDeadline: one-shot wakeup at absolute timestamp wake_at_sec
// Under product composition of state machines (S1 x S2), TickPolicy forms a
// commutative monoid with identity TickIdle{} and binary operation mergeTickPolicy.
struct TickIdle {
    constexpr bool operator==(const TickIdle&) const noexcept { return true; }
    constexpr bool operator!=(const TickIdle&) const noexcept { return false; }
};

struct TickPeriodic {
    double period_sec{0.01};

    constexpr bool operator==(const TickPeriodic& other) const noexcept {
        return period_sec == other.period_sec;
    }
    constexpr bool operator!=(const TickPeriodic& other) const noexcept {
        return !(*this == other);
    }
};

struct TickDeadline {
    double wake_at_sec{0.0};

    constexpr bool operator==(const TickDeadline& other) const noexcept {
        return wake_at_sec == other.wake_at_sec;
    }
    constexpr bool operator!=(const TickDeadline& other) const noexcept {
        return !(*this == other);
    }
};

using TickPolicy = std::variant<TickIdle, TickPeriodic, TickDeadline>;

namespace Policy {
constexpr TickPolicy idle() noexcept { return TickIdle{}; }
constexpr TickPolicy periodic(double period_sec) noexcept { return TickPeriodic{period_sec}; }
constexpr TickPolicy rateHz(double hz) noexcept {
    return (hz > 0.0) ? TickPolicy{TickPeriodic{1.0 / hz}} : TickPolicy{TickIdle{}};
}
constexpr TickPolicy deadline(double wake_at_sec) noexcept { return TickDeadline{wake_at_sec}; }
} // namespace Policy

// Computes the delay in seconds from `now_sec` until the next required step(),
// given that the last step ran at `last_step_sec`. Returns std::nullopt for TickIdle.
inline std::optional<double> nextWakeDelaySec(const TickPolicy& policy,
                                              double now_sec,
                                              double last_step_sec) noexcept {
    return std::visit(
        Overloaded{
            [](const TickIdle&) -> std::optional<double> { return std::nullopt; },
            [now_sec, last_step_sec](const TickPeriodic& p) -> std::optional<double> {
                if (!(p.period_sec > 0.0) || !std::isfinite(p.period_sec)) {
                    return std::nullopt;
                }
                if (!std::isfinite(last_step_sec) || last_step_sec <= 0.0) {
                    return p.period_sec;
                }
                const double target = last_step_sec + p.period_sec;
                return std::max(0.0, target - now_sec);
            },
            [now_sec](const TickDeadline& d) -> std::optional<double> {
                if (!std::isfinite(d.wake_at_sec)) {
                    return std::nullopt;
                }
                return std::max(0.0, d.wake_at_sec - now_sec);
            },
        },
        policy);
}

// Monoid operation on TickPolicy for product states (S1 x S2):
// TickIdle is the identity; two Periodic policies combine to min(period);
// a Periodic and a Deadline combine to whichever wakes earlier from `now_sec`.
inline TickPolicy mergeTickPolicy(const TickPolicy& lhs,
                                  const TickPolicy& rhs,
                                  double now_sec = 0.0) noexcept {
    return std::visit(
        Overloaded{
            [](const TickIdle&, const TickIdle&) -> TickPolicy { return TickIdle{}; },
            [&](const TickIdle&, const auto&) -> TickPolicy { return rhs; },
            [&](const auto&, const TickIdle&) -> TickPolicy { return lhs; },
            [](const TickPeriodic& a, const TickPeriodic& b) -> TickPolicy {
                if (!(a.period_sec > 0.0)) return b;
                if (!(b.period_sec > 0.0)) return a;
                return TickPeriodic{std::min(a.period_sec, b.period_sec)};
            },
            [](const TickDeadline& a, const TickDeadline& b) -> TickPolicy {
                return TickDeadline{std::min(a.wake_at_sec, b.wake_at_sec)};
            },
            [now_sec](const TickPeriodic& p, const TickDeadline& d) -> TickPolicy {
                if (!(p.period_sec > 0.0)) return d;
                const double periodic_wake = now_sec + p.period_sec;
                if (d.wake_at_sec <= periodic_wake) {
                    return d;
                }
                return p;
            },
            [now_sec](const TickDeadline& d, const TickPeriodic& p) -> TickPolicy {
                if (!(p.period_sec > 0.0)) return d;
                const double periodic_wake = now_sec + p.period_sec;
                if (d.wake_at_sec <= periodic_wake) {
                    return d;
                }
                return p;
            },
        },
        lhs,
        rhs);
}

// ============================================================================
// 4. Zero-Allocation Fixed-Capacity Containers
// ============================================================================
template <typename T, std::size_t Capacity>
class BoundedVector {
  public:
    static_assert(Capacity > 0, "BoundedVector capacity must be positive");

    constexpr BoundedVector() = default;

    constexpr bool push_back(const T& item) {
        if (size_ >= Capacity) {
            ++dropped_;
            return false;
        }
        items_[size_++] = item;
        return true;
    }

    constexpr bool push_back(T&& item) {
        if (size_ >= Capacity) {
            ++dropped_;
            return false;
        }
        items_[size_++] = std::move(item);
        return true;
    }

    template <typename... Args>
    constexpr bool emplace_back(Args&&... args) {
        if (size_ >= Capacity) {
            ++dropped_;
            return false;
        }
        items_[size_++] = T{std::forward<Args>(args)...};
        return true;
    }

    constexpr void clear() noexcept { size_ = 0; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr static std::size_t capacity() noexcept { return Capacity; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr bool full() const noexcept { return size_ >= Capacity; }
    constexpr std::size_t droppedCount() const noexcept { return dropped_; }

    constexpr const T& operator[](std::size_t index) const noexcept { return items_[index]; }
    constexpr T& operator[](std::size_t index) noexcept { return items_[index]; }

    constexpr const T* data() const noexcept { return items_.data(); }
    constexpr T* data() noexcept { return items_.data(); }
    constexpr const T* begin() const noexcept { return items_.data(); }
    constexpr const T* end() const noexcept { return items_.data() + size_; }
    constexpr T* begin() noexcept { return items_.data(); }
    constexpr T* end() noexcept { return items_.data() + size_; }

  private:
    std::array<T, Capacity> items_{};
    std::size_t size_{0};
    std::size_t dropped_{0};
};

template <typename T, std::size_t Capacity>
class FixedRingBuffer {
  public:
    static_assert(Capacity > 0, "FixedRingBuffer capacity must be positive");

    constexpr FixedRingBuffer() = default;

    constexpr void push(const T& item) noexcept {
        items_[write_pos_] = item;
        advance();
    }

    constexpr void push(T&& item) noexcept {
        items_[write_pos_] = std::move(item);
        advance();
    }

    constexpr void clear() noexcept {
        write_pos_ = 0;
        size_ = 0;
    }

    constexpr std::size_t size() const noexcept { return size_; }
    constexpr static std::size_t capacity() noexcept { return Capacity; }
    constexpr bool empty() const noexcept { return size_ == 0; }
    constexpr bool full() const noexcept { return size_ == Capacity; }

    // Logical index 0 is the oldest element currently in the ring.
    constexpr const T& operator[](std::size_t logical_index) const noexcept {
        const std::size_t start = (size_ == Capacity) ? write_pos_ : 0;
        return items_[(start + logical_index) % Capacity];
    }

    constexpr const T& back() const noexcept {
        const std::size_t last = (write_pos_ + Capacity - 1) % Capacity;
        return items_[last];
    }

  private:
    constexpr void advance() noexcept {
        write_pos_ = (write_pos_ + 1) % Capacity;
        if (size_ < Capacity) {
            ++size_;
        }
    }

    std::array<T, Capacity> items_{};
    std::size_t write_pos_{0};
    std::size_t size_{0};
};

} // namespace adt
} // namespace state_machine
