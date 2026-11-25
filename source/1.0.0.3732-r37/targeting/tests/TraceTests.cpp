#include <cstdio>
#include "../src/TargetTrace.h"

int main() {
    using namespace axiom::trace;
    unsigned checks=0, failures=0;
    const auto check=[&](bool ok,const char* label) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL %s\n",label); }
    };
    Sample sample{};
    check(Outcome(sample)==Result::NoQuery,"outer update may skip both paths");
    sample.path=Path::Primary;
    sample.mode=2;
    check(Outcome(sample)==Result::NoQuery,"primary entry may skip its physics call");
    float start[3]{0,0,0};
    float end[3]{50000,0,0};
    check(RecordQuery(sample,0x70,start,end,0) && Outcome(sample)==Result::NoPhysicsHit &&
          sample.rayLength==50000.f,"primary ray miss has length and zero hits");
    sample.count=sample.converted=1;
    sample.raw=true;
    sample.hit[0]=96.f;
    sample.candidates[0]={{96.f,0.f,0.f},1,37,false};
    check(PublishedHit(sample).source==RaySource::WithoutBody &&
          PublishedHit(sample).material==37,"matching unbound ray hit is identified");
    sample.candidates[0].body=true;
    check(PublishedHit(sample).source==RaySource::WithBody,"matching native body is visible");
    sample.count=sample.converted=2;
    sample.candidates[1]=sample.candidates[0];
    check(PublishedHit(sample).source==RaySource::Unknown,"duplicate position gives no attribution");
    sample.converted=1;
    check(PublishedHit(sample).source==RaySource::Unknown,"incomplete conversion gives no attribution");
    sample={};
    sample.path=Path::Alternate;
    sample.mode=2;
    RecordSweep(sample,0,0x0003,start,end);
    check(sample.queried && sample.count==0 && sample.queryKind==3 &&
          sample.rayLength==0.f && Outcome(sample)==Result::NoPhysicsHit,
          "sweep miss has sweep coordinates and zero hits");
    RecordSweep(sample,2,0,start,end);
    check(sample.count==0 && sample.queryKind==3,"first query wins");
    sample={};
    sample.path=Path::Alternate;
    sample.mode=2;
    RecordSweep(sample,3,0,start,end);
    check(Outcome(sample)==Result::Rejected && sample.queryKind==0,"sweep hit without accepted target");
    sample.mode=1;
    check(Outcome(sample)==Result::Accepted,"alternate accepted mode");
    sample.mode=0;
    check(Outcome(sample)==Result::Rejected,"alternate path does not inherit primary acceptance");
    sample={};
    sample.mode=2;
    sample.beforeMode=0;
    check(CursorCleared(sample),"an accepted primary target can clear");
    sample.beforeMode=1;
    check(CursorCleared(sample),"an accepted alternate target can clear");
    sample.beforeMode=2;
    sample.beforeRaw=true;
    check(CursorCleared(sample),"a raw-only target can clear");
    sample.raw=true;
    check(!CursorCleared(sample),"a raw-only target that remains is not a clear");
    sample.beforeRaw=false;
    sample.raw=false;
    sample.reset=true;
    check(!CursorCleared(sample),"a pre-query reset remains distinct from a visible-state transition");
    sample.beforeMode=0;
    sample.mode=1;
    check(!CursorCleared(sample),"replacement in the same update is not a clear");
    sample.beforeMode=255;
    sample.mode=2;
    check(!CursorCleared(sample),"invalid before snapshot cannot prove a clear");
    sample.path=Path::Primary;
    sample.mode=0;
    check(Outcome(sample)==Result::NoQuery,"retained target does not imply a new physics query");
    sample.queried=true;
    sample.mode=255;
    check(Outcome(sample)==Result::StateUnavailable,"unread final state cannot prove rejection");
    std::printf("Axiom cursor trace: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}

