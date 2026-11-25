#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"
#include <limits>

int main() {
    constexpr float budget=50000.f;
    ApplyConfig cfg{true,true,true,1.f,1000.f};
    unsigned failures=0;
    const auto check=[&](bool pass,const char* test) {
        std::printf("%s %s\n", pass?"PASS":"FAIL",test);
        failures+=!pass;
    };
    InterlockedExchange(&SAFMenuUnlimitedReachV1,0);
    check(EffectiveAxiomRange(cfg)==budget,"native aim works with a one-unit saved range and no Menu request");
    cfg.range=90000.f;
    check(EffectiveAxiomRange(cfg)==budget,"a high saved value cannot change the native crosshair budget");
    cfg.range=(std::numeric_limits<float>::quiet_NaN)();
    InterlockedExchange(&SAFMenuUnlimitedReachV1,1);
    check(EffectiveAxiomRange(cfg)==budget,"an obsolete value never changes dynamic or Menu-owned targeting");
    return failures?1:0;
}
