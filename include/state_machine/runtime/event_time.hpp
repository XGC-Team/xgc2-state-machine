#pragma once

#include <state_machine/state_machine.hpp>

namespace state_machine {
namespace runtime {

inline double eventTimestampOr(const Event& event, double fallback_sec) {
    return event.timestamp > 0.0 ? event.timestamp : fallback_sec;
}

} // namespace runtime
} // namespace state_machine
