// Producers on several threads post input events while the owner thread runs
// update() and another thread keeps taking snapshots. Every event must be
// delivered exactly once, in the order its producer posted it, and the
// runtime-assigned sequence numbers must increase in delivery order.
//
// Build with -fsanitize=thread to check the locking, e.g.
//   cmake -DCMAKE_CXX_FLAGS=-fsanitize=thread ...

#include <state_machine/state_machine.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sm = state_machine;

namespace {

constexpr sm::RegionId kRegion = 1;
constexpr sm::StateId kCounting = 10;
constexpr sm::EventId kSensor = 100;
constexpr int kProducers = 4;
constexpr int kEventsPerProducer = 20000;

class Counting final : public sm::State {
  public:
    Counting() : next_index_(kProducers, 0) {}

    std::string name() const override { return "Counting"; }

    sm::ActionResult onEvent(sm::StateContext&, const sm::Event& event) override {
        const auto producer = static_cast<size_t>(std::get<int64_t>(event.payload.at("producer")));
        const int64_t index = std::get<int64_t>(event.payload.at("index"));
        if (event.sequence <= last_sequence_) {
            ++sequence_violations;
        }
        last_sequence_ = event.sequence;
        if (index != next_index_[producer]) {
            ++order_violations;
        }
        next_index_[producer] = index + 1;
        ++delivered;
        return sm::Status{};
    }

    int delivered{0};
    int order_violations{0};
    int sequence_violations{0};

  private:
    std::vector<int64_t> next_index_;
    uint64_t last_sequence_{0};
};

} // namespace

int main() {
    auto counting = std::make_unique<Counting>();
    Counting* counter = counting.get();
    auto builder = sm::StateMachine::builder("concurrency");
    builder.region(kRegion).initial(kCounting).state(kCounting).impl(std::move(counting)).endRegion();
    auto built = builder.build();
    if (!built.ok()) {
        std::fprintf(stderr, "build failed: %s\n", built.status.message.c_str());
        return 2;
    }
    std::unique_ptr<sm::StateMachine> machine = std::move(built.value);
    if (!machine->start().ok()) {
        std::fprintf(stderr, "start failed\n");
        return 2;
    }

    std::atomic<bool> producing{true};
    std::atomic<int> producer_errors{0};
    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&machine, &producer_errors, producer] {
            for (int index = 0; index < kEventsPerProducer; ++index) {
                for (;;) {
                    sm::Event event(kSensor);
                    event.source = "producer";
                    event.payload["producer"] = static_cast<int64_t>(producer);
                    event.payload["index"] = static_cast<int64_t>(index);
                    const sm::Status status = machine->postEvent(std::move(event));
                    if (status.ok()) {
                        break;
                    }
                    if (status.code != sm::ErrorCode::kLimitReached) {
                        std::fprintf(stderr, "post failed: %s\n", status.message.c_str());
                        ++producer_errors;
                        return;
                    }
                    std::this_thread::yield(); // inbox full: wait for the owner to drain it
                }
            }
        });
    }
    std::thread observer([&machine, &producing] {
        while (producing.load()) {
            (void)machine->snapshot();
            (void)machine->currentState();
            std::this_thread::yield();
        }
    });

    constexpr int kTotal = kProducers * kEventsPerProducer;
    size_t taken = 0;
    size_t processed = 0;
    int updates = 0;
    while (counter->delivered < kTotal && producer_errors.load() == 0) {
        const auto result = machine->update();
        if (!result.ok()) {
            std::fprintf(stderr, "update failed: %s\n", result.status.message.c_str());
            producing = false;
            for (auto& thread : producers) {
                thread.join();
            }
            observer.join();
            return 1;
        }
        taken += result.value.events_taken;
        processed += result.value.events_processed;
        ++updates;
    }
    for (auto& thread : producers) {
        thread.join();
    }
    producing = false;
    observer.join();

    const auto last = machine->update();
    taken += last.value.events_taken;
    const bool ok = producer_errors.load() == 0 && counter->delivered == kTotal && counter->order_violations == 0 &&
                    counter->sequence_violations == 0 && taken == static_cast<size_t>(kTotal) &&
                    processed == static_cast<size_t>(kTotal) && machine->snapshot().inbox_size == 0;
    std::printf("concurrency: %d events from %d producers in %d updates; delivered=%d taken=%zu processed=%zu "
                "order_violations=%d sequence_violations=%d -> %s\n",
                kTotal, kProducers, updates, counter->delivered, taken, processed, counter->order_violations,
                counter->sequence_violations, ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
