#pragma once

#include <optional>
#include <utility>

namespace state_machine {

// Requests coalesce, not input data. The caller retains its owner/lock and queue.
class QueuedTurn {
  public:
    template <class Enqueue> void request(Enqueue&& enqueue) {
        if (queued_)
            return;
        queued_ = true;
        std::forward<Enqueue>(enqueue)();
    }
    void beginTurn() { queued_ = false; }
    // Removal is not a join or a restart; retain the existing queued state.
    template <class Remove> void cancel(Remove&& remove) { std::forward<Remove>(remove)(); }

  private:
    bool queued_{false};
};

// One owner's installed deadline in one clock domain. Effects run before the
// bookkeeping changes, so a throwing conversion/installation leaves the old arm.
template <class Time> class DeadlineArm {
  public:
    template <class Arm> void arm(const Time& deadline, Arm&& install) {
        if (armed_ && *armed_ == deadline)
            return;
        std::forward<Arm>(install)(deadline);
        armed_ = deadline;
    }
    template <class Cancel> void cancel(Cancel&& remove) {
        if (armed_) {
            std::forward<Cancel>(remove)();
            armed_.reset();
        }
    }
    template <class Arm, class Cancel> void update(const std::optional<Time>& next, Arm&& install, Cancel&& remove) {
        if (next)
            arm(*next, std::forward<Arm>(install));
        else
            cancel(std::forward<Cancel>(remove));
    }
    void consumed() { armed_.reset(); }

  private:
    std::optional<Time> armed_;
};

} // namespace state_machine
