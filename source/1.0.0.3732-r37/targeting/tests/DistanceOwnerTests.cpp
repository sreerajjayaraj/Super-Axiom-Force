
#include "../src/DistanceOwner.h"
#include <cstdio>
#include <cstring>
#include <limits>

uint32_t Bits(float value) {
    uint32_t bits=0;
    memcpy(&bits,&value,sizeof(bits));
    return bits;
}

int main() {
    unsigned checks=0,failed=0;
    const auto check=[&](bool ok,const char* test) {
        ++checks;
        if (!ok) { ++failed; std::printf("FAIL %s\n",test); }
    };
    const uint32_t native=Bits(4.0f), first=Bits(4000.0f), second=Bits(5500.0f);
    axiom::DistanceOwner owner;
    const axiom::Settings off{0,0,4000}, on{1,0,4000};
    const auto apply=[&](uint32_t current,const axiom::Settings& cfg,bool ready) {
        const auto operation=owner.Decide(current,cfg,ready);
        owner.Complete(operation,true);
        return operation;
    };
    check(!apply(native,off,true).write,"off performs no write");
    check(!apply(native,on,false).write,"missing or unresolved SAF stays passive");
    check(!apply(0,on,true).write,"uninitialized target stays passive");
    check(!apply(Bits(181),on,true).write,"unknown target remains untouched");
    auto a=apply(native,on,true);
    check(a.write && a.expected==native && a.desired==first && owner.Owned(),"exact native 4 acquires one remote catch scalar");
    check(!apply(first,on,true).write,"same setting no repeated write");
    a=apply(first,{1,0,5500},true);
    check(a.write && a.desired==second,"Menu slider drives the remote catch scalar");
    a=apply(second,{1,1,5500},true);
    check(a.write && a.desired==Bits(50000),"Menu unlimited uses its original 50000 limit");
    a=apply(Bits(50000),off,true);
    check(a.write && a.desired==native && !owner.Owned(),"toggle off restores native 4 once");
    check(!apply(native,off,true).write,"repeated off stays passive");
    a=apply(native,{1,0,std::numeric_limits<float>::quiet_NaN()},true);
    check(!a.write,"unknown malformed settings fail closed");
    a=apply(native,{1,0,100001},true);
    check(a.write && a.desired==Bits(100),"numeric range fallback matches Menu");
    a=apply(Bits(100),on,false);
    check(a.write && a.desired==native,"lost SAF proof restores the bridge's own value");

    a=owner.Decide(native,on,true);
    check(a.write,"explicit retry starts with native");
    owner.Complete(a,false);
    check(!owner.Owned(),"failed acquisition never becomes owned");
    a=apply(native,on,true);
    check(a.write,"second acquisition succeeds");
    a=apply(Bits(181),off,true);
    check(!a.write && a.stop && !owner.Owned(),"foreign writer aborts without overwrite");
    check(!apply(native,on,true).write,"foreign writer requires a restart");
    std::printf("Axiom distance ownership %u checks, %u failures\n",checks,failed);
    return failed?1:0;
}

