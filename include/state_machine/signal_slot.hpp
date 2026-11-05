#pragma once

#include <optional>
#include <utility>

namespace state_machine {

// Owner-local replaceable fact. External synchronization, if needed, belongs
// to the actual handoff. This cell adds no freshness, validity or wake policy.
template <class T> class SignalSlot {
  public:
    void replace(T value) { value_ = std::move(value); }
    const T* value() const noexcept { return value_ ? &*value_ : nullptr; }
    std::optional<T> take() {
        std::optional<T> taken;
        taken.swap(value_);
        return taken;
    }

  private:
    std::optional<T> value_;
};

} // namespace state_machine
