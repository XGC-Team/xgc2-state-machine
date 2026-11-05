#pragma once

#include <optional>
#include <utility>

namespace state_machine {

// An absolute existing obligation in one clock domain. Period/phase and
// eligibility stay in the owner; the edge merely arms this resulting demand.
template <class Time> class NextWake {
  public:
    void at(Time time) {
        if (!deadline_ || time < *deadline_)
            deadline_ = std::move(time);
    }
    void include(const NextWake& other) {
        if (other.deadline_)
            at(*other.deadline_);
    }
    const std::optional<Time>& deadline() const noexcept { return deadline_; }

  private:
    std::optional<Time> deadline_;
};

} // namespace state_machine
