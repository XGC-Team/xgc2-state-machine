#pragma once

#include <cstdint>
#include <optional>

namespace state_machine {

// Mechanical one-shot arm state for the existing native host. Domain conversion,
// due-now behavior, input reasons and periodic phase remain with the module.
class SyncRuntimeTickAdapter {
  public:
    template <class WakeAt> void arm(std::optional<int64_t> next, WakeAt&& wake_at) {
        if (!next) {
            if (armed_) {
                wake_at(-1);
                armed_.reset();
            }
            return;
        }
        if (!armed_ || *armed_ != *next) {
            wake_at(*next);
            armed_ = next;
        }
    }
    // Called only for the host's existing DEADLINE reason, before the owner turn.
    void consumed() { armed_.reset(); }

  private:
    std::optional<int64_t> armed_;
};

} // namespace state_machine
