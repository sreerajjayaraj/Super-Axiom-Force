#include "../src/AxiomTargeting.h"
#include <cstdio>

int main() {
    using namespace saf::embedded;
    const auto state=CurrentTargetingState();
    const bool dormant=!state.support && state.range==0.f &&
        SyncTargeting({},nullptr)==TargetingResult::Inactive;
    const bool refused=SyncTargeting({1,50000.f},nullptr)==TargetingResult::Unavailable;
    std::printf("%s embedded targeting rejects the fixture host and stays inactive without ownership\n",
                dormant&&refused?"PASS":"FAIL");
    return dormant&&refused?0:1;
}
