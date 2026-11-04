#include <state_machine/adt_machine.hpp>
namespace adt = state_machine::adt;
struct A {};
struct B {
    B() = default;
#if CONTRACT_CASE == 2
    B(B&&) noexcept(false) {}
#elif CONTRACT_CASE == 3
    ~B() noexcept(false) {}
#endif
};
struct Event {};
struct Spec {
    using State = std::variant<A, B>;
    using Time = int;
    static adt::Stay handle(A&, const Event&) { return {}; }
#if CONTRACT_CASE != 1
    static adt::Stay handle(B&, const Event&) { return {}; }
#endif
};
int main() {
    adt::AdtMachine<Spec> machine(A{}, 0);
    machine.apply(machine.dispatch(Event{}), 1);
#if CONTRACT_CASE == 4
    machine.apply(adt::Enter{Event{}}, 2);
#endif
}
