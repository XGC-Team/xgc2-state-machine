#include <state_machine/adt_machine.hpp>

#include <array>
#include <gtest/gtest.h>
#include <memory>

namespace {
namespace sm = state_machine::adt;
struct Exits {
    std::array<int, 8> ids{};
    std::size_t count = 0;
};
struct Resource {
    Resource(int value, Exits& log) : id(value), exits(log) {}
    int id;
    Exits& exits;
    ~Resource() noexcept { exits.ids[exits.count++] = id; }
};
struct First {
    std::unique_ptr<Resource> resource;
};
struct Second {};
struct ChildSpec {
    using State = std::variant<First, Second>;
    using Time = long long;
    struct Advance {};
    static sm::Transition<First, Second> handle(First&, const Advance&) { return sm::Enter{Second{}}; }
    static sm::Transition<First, Second> handle(Second&, const Advance&) { return sm::Stay{}; }
};
struct Parent {
    std::unique_ptr<Resource> resource;
    sm::AdtMachine<ChildSpec> child;
};
struct Outside {};
struct Spec {
    using State = std::variant<Outside, Parent>;
    using Time = long long;
    static int handle(Outside&, const ChildSpec::Advance&, Time&) { return 0; }
    static int handle(Parent& p, const ChildSpec::Advance& e, Time& now) {
        p.child.apply(p.child.dispatch(e), now);
        return 1;
    }
};

TEST(AdtMachine, ChildEntryPreservesParentAndStayPreservesEntryTime) {
    Exits exits;
    sm::AdtMachine<Spec> machine(Parent{std::make_unique<Resource>(1, exits),
                                        sm::AdtMachine<ChildSpec>(First{std::make_unique<Resource>(2, exits)}, 7)},
                                 5);
    auto* parent = std::get<Parent>(machine.state()).resource.get();
    long long now = 11;
    EXPECT_EQ(machine.dispatch(ChildSpec::Advance{}, now), 1);
    EXPECT_EQ(exits.count, 1u);
    EXPECT_EQ(exits.ids[0], 2);
    EXPECT_EQ(std::get<Parent>(machine.state()).resource.get(), parent);
    EXPECT_EQ(machine.entered_at(), 5);
    EXPECT_EQ(std::get<Parent>(machine.state()).child.entered_at(), 11);
    now = 19;
    machine.dispatch(ChildSpec::Advance{}, now);
    EXPECT_EQ(std::get<Parent>(machine.state()).child.entered_at(), 11);
    machine.apply(sm::Enter{Outside{}}, now);
    EXPECT_EQ(exits.count, 2u);
    EXPECT_EQ(exits.ids[0], 2);
    EXPECT_EQ(exits.ids[1], 1);
    EXPECT_EQ(machine.entered_at(), 19);
}

TEST(AdtMachine, SameKindEntryDestroysThePreviousInstance) {
    Exits exits;
    sm::AdtMachine<ChildSpec> machine(First{std::make_unique<Resource>(3, exits)}, 2);
    auto next = First{std::make_unique<Resource>(4, exits)};
    machine.apply(sm::Enter{std::move(next)}, 3);
    EXPECT_EQ(exits.count, 1u);
    EXPECT_EQ(exits.ids[0], 3);
    EXPECT_EQ(std::get<First>(machine.state()).resource->id, 4);
    EXPECT_EQ(machine.entered_at(), 3);
    machine.apply(sm::Stay{}, 20);
    EXPECT_EQ(exits.count, 1u);
    EXPECT_EQ(exits.ids[0], 3);
    EXPECT_EQ(machine.entered_at(), 3);
}
} // namespace
