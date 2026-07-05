#pragma once

#include <state_machine/state_machine.hpp>

#include <string>
#include <type_traits>
#include <utility>

namespace state_machine {
namespace runtime {
namespace detail {

template <typename Sink, typename = void> struct HasBoolConversion : std::false_type {};

template <typename Sink>
struct HasBoolConversion<Sink, std::void_t<decltype(static_cast<bool>(std::declval<const Sink&>()))>> : std::true_type {
};

template <typename Sink> bool sinkConfigured(const Sink& sink) {
    if constexpr (HasBoolConversion<Sink>::value) {
        return static_cast<bool>(sink);
    }
    return true;
}

struct IgnoreEventPostFailure {
    void operator()(const Status&, EventId, const std::string&) const {}
};

} // namespace detail

inline Event makeEvent(EventId event_id, EventCategory category, std::string source, double timestamp_sec) {
    Event event(event_id, EventTimestamp{timestamp_sec});
    event.category = category;
    event.source = std::move(source);
    return event;
}

template <typename Sink, typename FailureHandler, typename... SinkArgs>
Status postEventWithFailureHandler(Sink& sink, EventId event_id, EventCategory category, std::string source,
                                   double timestamp_sec, FailureHandler&& on_failure, SinkArgs&&... sink_args) {
    if (!detail::sinkConfigured(sink)) {
        auto status = Status::error(ErrorCode::kInvalidArgument, "event sink is not configured");
        std::forward<FailureHandler>(on_failure)(status, event_id, source);
        return status;
    }

    auto event = makeEvent(event_id, category, source, timestamp_sec);
    const auto status = sink(std::move(event), std::forward<SinkArgs>(sink_args)...);
    if (!status.ok()) {
        std::forward<FailureHandler>(on_failure)(status, event_id, source);
    }
    return status;
}

template <typename Sink, typename... SinkArgs>
Status postEvent(Sink& sink, EventId event_id, EventCategory category, std::string source, double timestamp_sec,
                 SinkArgs&&... sink_args) {
    return postEventWithFailureHandler(sink, event_id, category, std::move(source), timestamp_sec,
                                       detail::IgnoreEventPostFailure{}, std::forward<SinkArgs>(sink_args)...);
}

template <typename Sink, typename FailureHandler, typename... SinkArgs>
Status postInputEventWithFailureHandler(Sink& sink, EventId event_id, std::string source, double timestamp_sec,
                                        FailureHandler&& on_failure, SinkArgs&&... sink_args) {
    return postEventWithFailureHandler(sink, event_id, EventCategory::kInput, std::move(source), timestamp_sec,
                                       std::forward<FailureHandler>(on_failure), std::forward<SinkArgs>(sink_args)...);
}

template <typename Sink, typename... SinkArgs>
Status postInputEvent(Sink& sink, EventId event_id, std::string source, double timestamp_sec, SinkArgs&&... sink_args) {
    return postEvent(sink, event_id, EventCategory::kInput, std::move(source), timestamp_sec,
                     std::forward<SinkArgs>(sink_args)...);
}

} // namespace runtime
} // namespace state_machine
