#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"

#ifdef SAF_BASELINE_TEST
static void SetMenuUnlimitedRequestForTest(bool) {}
static void ServiceMenuRange() {}
#else
static void SetMenuUnlimitedRequestForTest(bool on) {
    InterlockedExchange(&SAFMenuUnlimitedReachV1,on?1:0);
}
#endif

int main() {
    using std::numeric_limits;
    ApplyConfig cfg{true,true,true,1.f,1000.f};
    unsigned failures=0;
    const auto check=[&](bool pass,const char* label) {
        if (!pass) ++failures;
        std::printf("%s %s\n", pass?"PASS":"FAIL", label);
    };
    SetMenuUnlimitedRequestForTest(false);
    check(EffectiveAxiomRange(cfg)==50000.f,"saved one-unit range does not limit native aiming");
    cfg.range=90000.f;
    check(EffectiveAxiomRange(cfg)==50000.f,"saved higher range does not change native aim budget");
    cfg.range=(numeric_limits<float>::quiet_NaN)();
    SetMenuUnlimitedRequestForTest(true);
    check(EffectiveAxiomRange(cfg)==50000.f,"obsolete range and Menu request cannot change the native aim budget");
    return failures?1:0;
}
