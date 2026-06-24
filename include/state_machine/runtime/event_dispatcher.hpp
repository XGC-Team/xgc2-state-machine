#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <state_machine/state_machine.hpp>

namespace state_machine {
namespace runtime {

struct EventDispatchFailure {
    Event event;
    std::string consumer_name;
    std::string message;
};

struct EventDispatchResult {
    size_t events_total{0};
    size_t events_handled{0};
    size_t events_unhandled{0};
    size_t consumer_failures{0};
    std::vector<Event> unhandled_events;
    std::vector<EventDispatchFailure> failures;
};

class EventConsumer {
  public:
    virtual ~EventConsumer() = default;
    virtual std::string name() const = 0;
    virtual bool handle(const Event& event) = 0;
};

class EventDispatcher {
  public:
    void addConsumer(std::unique_ptr<EventConsumer> consumer) {
        if (consumer) {
            consumers_.push_back(std::move(consumer));
        }
    }

    EventDispatchResult dispatch(const std::vector<Event>& events) {
        EventDispatchResult result;
        result.events_total = events.size();

        for (const auto& event : events) {
            bool handled = false;
            for (auto& consumer : consumers_) {
                if (!consumer) {
                    continue;
                }
                try {
                    if (consumer->handle(event)) {
                        handled = true;
                        ++result.events_handled;
                        break;
                    }
                } catch (const std::exception& ex) {
                    ++result.consumer_failures;
                    result.failures.push_back(EventDispatchFailure{event, consumerName(*consumer), ex.what()});
                } catch (...) {
                    ++result.consumer_failures;
                    result.failures.push_back(
                        EventDispatchFailure{event, consumerName(*consumer), "unknown exception"});
                }
            }

            if (!handled) {
                ++result.events_unhandled;
                result.unhandled_events.push_back(event);
            }
        }

        return result;
    }

  private:
    static std::string consumerName(const EventConsumer& consumer) {
        try {
            return consumer.name();
        } catch (...) {
            return "<unknown>";
        }
    }

    std::vector<std::unique_ptr<EventConsumer>> consumers_;
};

} // namespace runtime
} // namespace state_machine
