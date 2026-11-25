
#define AXIOM_TESTING 1
#include "../src/AxiomCoexist.cpp"
#include <fstream>

namespace {
std::atomic<SafState> testState{SafState::Missing};
std::atomic<float> effectiveSafRange{500.f};
uintptr_t fixtureSupport=0;
volatile LONG unlimitedRequest=0;
unsigned settingReads=0, settingPublishes=0;
bool seededEnabled=true;
constexpr float seedRange=700.f;
constexpr float seedPull=175.f;

int FixtureSaf(safcompat::Lease& lease) {
    lease.state.support=fixtureSupport;
    lease.state.rangeBits=axiom::FloatBits(effectiveSafRange.load());
    return static_cast<int>(testState.load());
}
bool FixtureGame(uintptr_t base) { return base==g.game; }
void FixtureReadSettings(void* object) {
    ++settingReads;
    const auto at=reinterpret_cast<uintptr_t>(object);
    Set(at+0x1C,seedRange);
    Set(at+0x20,seedPull);
    Set(at+0x24,uint8_t{1});
    Set(at+0x25,uint8_t{0});
}
void FixturePublishSettings(const void* fields) {
    ++settingPublishes;
    const auto at=reinterpret_cast<uintptr_t>(fields);
    float range=0;
    uint8_t unlimited=0;
    Get(at,range);
    Get(at+9,unlimited);
    Set(g.menu+g.profile->range,range);
    Set(g.menu+g.profile->unlimited,uint32_t{unlimited});
}
bool FixtureEnabled(const wchar_t* group,const wchar_t* name,bool fallback) {
    return !wcscmp(group,L"AxiomForce") && !wcscmp(name,L"Enabled") ? seededEnabled:fallback;
}

template<class F>
bool Eventually(F check) {
    const auto deadline=GetTickCount64()+2200;
    do {
        if (check()) return true;
        Sleep(10);
    } while (GetTickCount64()<deadline);
    return false;
}

template<class T>
T At(uintptr_t address) { T value{}; Get(address,value); return value; }

template<class F>
F Shim(const quiesced_patch::Patch& patch) {
    return reinterpret_cast<F>(patch.address);
}
}

std::array<void*,4> traceForwarded{};
const void* traceForwardedAim=nullptr;
bool traceNativeErrorPreserved=false;
std::array<uint8_t,0x40> traceQuery{};
std::array<uint8_t,0x10> traceCollector{};
uint32_t traceHits=0;
uint8_t traceRaw=0,traceMode=2;
std::array<uint8_t,0xA0> traceHit{};
std::array<uint8_t,0x58> traceCandidate{};
const float traceHitPosition[3]{-9984.f,20.f,30.f};
bool conversionArgs=false;
bool fixtureHasBody=false;

void FixtureConvertCandidate(void* context,const void* hit,void* result,uint8_t absolute) {
    conversionArgs=context==traceCollector.data() && hit==traceHit.data() &&
                   result==traceCandidate.data() && absolute==1 && GetLastError()==213;
    std::memcpy(traceCandidate.data()+8,traceHitPosition,sizeof(traceHitPosition));
    const uint32_t kind=1,material=37;
    const uintptr_t body=fixtureHasBody?1:0;
    std::memcpy(traceHit.data()+0x88,&kind,sizeof(kind));
    std::memcpy(traceCandidate.data()+0x48,&material,sizeof(material));
    std::memcpy(traceCandidate.data()+0x50,&body,sizeof(body));
    SetLastError(216);
}

void FixtureTraceDispatch(void* world,const void* query,void* collector,void* context) {
    traceNativeErrorPreserved=GetLastError()==212;
    traceForwarded={world,const_cast<void*>(query),collector,context};
    std::memcpy(traceCollector.data()+0xC,&traceHits,sizeof(traceHits));
    SetLastError(213);
}
void FixtureTraceTarget(void* object,const void* aim) {
    traceForwardedAim=aim;
    if (GetLastError()!=211) return;
    SetLastError(212);
    TraceDispatch(object,traceQuery.data(),traceCollector.data(),const_cast<void*>(aim));
    if (GetLastError()!=213) return;
    auto* bytes=static_cast<uint8_t*>(object);
    if (traceHits==1) {
        TraceCandidateConvert(traceCollector.data(),traceHit.data(),traceCandidate.data(),1);
        if (GetLastError()!=216) return;
        std::memcpy(bytes+0x148,traceHitPosition,sizeof(traceHitPosition));
    }
    bytes[0x140]=traceMode;
    bytes[0x144]=traceRaw;
    SetLastError(214);
}

unsigned surfaceQueries=0;
bool fixtureProbeBody=true;
bool fixtureCenterBody=false;
bool surfaceAbi=true;
const Point centerRay{-9760.f,20.f,30.f};

void SetProbeCandidate(NativeRayCandidate& entry,const Point& position,bool body) {
    const uintptr_t nativeBody=body?1:0;
    std::memcpy(entry.bytes.data()+8,position.data(),sizeof(position));
    std::memcpy(entry.bytes.data()+0x50,&nativeBody,sizeof(nativeBody));
}

bool FixtureSurfaceQuery(void* world,const void* query,uint8_t kind,void* output) {
    auto& vector=*static_cast<NativeRayOutput*>(output)->vector;
    ++surfaceQueries;
    Point start{},end{};
    std::memcpy(start.data(),static_cast<const uint8_t*>(query)+0x28,sizeof(start));
    std::memcpy(end.data(),static_cast<const uint8_t*>(query)+0x34,sizeof(end));
    surfaceAbi=surfaceAbi && kind==4 && world && start==Point{-10000.f,20.f,30.f} &&
        axiom::surface::Distance(start,end)<241.f && GetLastError()==216u;
    vector.count=1;
    SetProbeCandidate(vector.entries[0],Point{-9760.f,20.1f,30.f},fixtureProbeBody);
    SetLastError(223);
    return true;
}

struct ExtendedProbeFixture {
    NativeRayCandidate first{},second{};
    Point origin{};
    float reach=0.f;
    bool hasBlocker=false,forwarded=false,nativeInlineLayout=false;
    bool grow=false,nativeHit=true;
    NativeRayCandidate* grown=nullptr;
    unsigned releases=0;
} fixtureExtended;

void FixtureFreeExtended(void* allocation) {
    if (allocation!=fixtureExtended.grown) return;
    ++fixtureExtended.releases;
    delete[] fixtureExtended.grown;
    fixtureExtended.grown=nullptr;
}

bool FixtureExtendedProbe(void* world,const void* query,uint8_t kind,void* output) {
    ++surfaceQueries;
    auto& vector=*static_cast<NativeRayOutput*>(output)->vector;
    Point start{},end{};
    std::memcpy(start.data(),static_cast<const uint8_t*>(query)+0x28,sizeof(start));
    std::memcpy(end.data(),static_cast<const uint8_t*>(query)+0x34,sizeof(end));
    fixtureExtended.forwarded=world && kind==4 && start==fixtureExtended.origin &&
        axiom::surface::Distance(start,end)<=fixtureExtended.reach+0.001f &&
        axiom::surface::Distance(start,end)>145.f &&
        GetLastError()==216u;
    // Native resize compares old storage with vector + 0x10 before
    // taking the branch that can send an address to the game allocator.
    fixtureExtended.nativeInlineLayout=
        reinterpret_cast<uintptr_t>(vector.entries)==reinterpret_cast<uintptr_t>(&vector)+0x10 &&
        vector.capacity==8 && vector.count==0;
    if (fixtureExtended.grow) {
        if (!fixtureExtended.nativeInlineLayout) return false;
        // Reproduce the 8-to-17 capacity growth observed in the dump.
        const uint32_t capacity=vector.capacity*2+1;
        fixtureExtended.grown=new NativeRayCandidate[capacity]{};
        vector.entries=fixtureExtended.grown;
        vector.capacity=capacity;
    }
    vector.entries[0]=fixtureExtended.first;
    vector.entries[1]=fixtureExtended.second;
    vector.count=fixtureExtended.grow?9:fixtureExtended.hasBlocker?2:1;
    return fixtureExtended.nativeHit;
}

void FixtureSurfaceCenter(void*,const void*,void* result,uint8_t absolute) {
    surfaceAbi=surfaceAbi && absolute==1 && GetLastError()==221u;
    SetProbeCandidate(*static_cast<NativeRayCandidate*>(result),centerRay,fixtureCenterBody);
    SetLastError(216);
}

bool fixtureContinuedBlocker=false,fixtureContinuedQuery=false;
uint32_t fixtureRetryKind=0;
unsigned fixtureWindow=0;

void FixtureRetryConvert(void*,const void*,void*,uint8_t) {}
bool FixtureContinuedProbe(void* world,const void* query,uint8_t kind,void* output) {
    auto& vector=*static_cast<NativeRayOutput*>(output)->vector;
    Point start{},end{};
    std::memcpy(start.data(),static_cast<const uint8_t*>(query)+0x28,sizeof(start));
    std::memcpy(end.data(),static_cast<const uint8_t*>(query)+0x34,sizeof(end));
    ++surfaceQueries;
    if (fixtureWindow>=13 && kind==2 && end==Point{-9792.f,20.f,30.f}) {
        if (!world || start!=Point{-10000.f,20.f,30.f}) return false;
        vector.count=2;
        SetProbeCandidate(vector.entries[0],Point{-9940.f,20.f,30.f},fixtureWindow!=14);
        SetProbeCandidate(vector.entries[1],Point{-9820.f,20.f,30.f},true);
        return true;
    }
    if (fixtureWindow>=9) {
        if (!world || start!=Point{-10000.f,20.f,30.f}) return false;
        if (std::abs(end[0]+9472.f)>0.1f) return false;
        const bool offset=end[1]!=20.f || end[2]!=30.f;
        if (offset && kind!=2) return false;
        const auto onRay=[&](float x) {
            const auto fraction=x/(end[0]-start[0]);
            return Point{start[0]+x,start[1]+(end[1]-start[1])*fraction,
                                     start[2]+(end[2]-start[2])*fraction};
        };
        const bool foreground=offset && fixtureWindow==10;
        vector.count=offset && fixtureWindow!=11?2u:1u;
        SetProbeCandidate(vector.entries[0],onRay(foreground?112.f:144.f),foreground);
        if (!foreground) {
            const uint32_t gridTag=0x5FFFFFFFu, gridKind=1;
            std::memcpy(vector.entries[0].bytes.data()+0x48,&gridTag,sizeof(gridTag));
            std::array<uint8_t,0x8C> raw{};
            std::memcpy(raw.data()+0x88,&gridKind,sizeof(gridKind));
            TraceCandidateConvert(nullptr,raw.data(),vector.entries,1);
        }
        if (vector.count==2) {
            auto hit=onRay(296.f);
            if (fixtureWindow==12) hit[2]+=15.f;
            SetProbeCandidate(vector.entries[1],hit,true);
        }
        return true;
    }
    if (fixtureWindow>=5) {
        const Point camera{-10000.f,20.f,30.f};
        const Point crop{-9472.f,20.f,30.f};
        if (!world || start!=camera || end!=crop || (kind!=4 && kind!=2)) return false;
        const uint32_t gridTag=0x5FFFFFFFu;
        const bool alternate=kind==2;
        vector.count=alternate?(fixtureWindow==6?3u:2u):1u;
        SetProbeCandidate(vector.entries[0],Point{-9856.f,20.f,30.f},false);
        std::memcpy(vector.entries[0].bytes.data()+0x48,&gridTag,sizeof(gridTag));
        std::array<uint8_t,0x8C> raw{};
        const uint32_t gridKind=1;
        std::memcpy(raw.data()+0x88,&gridKind,sizeof(gridKind));
        TraceCandidateConvert(nullptr,raw.data(),vector.entries,1);
        if (alternate) {
            if (fixtureWindow==6) SetProbeCandidate(vector.entries[1],Point{-9810.f,20.f,30.f},false);
            SetProbeCandidate(vector.entries[vector.count-1],
                              Point{-9575.f,fixtureWindow==7?20.5f:20.f,30.f},fixtureWindow!=8);
        }
        return true;
    }
    if (fixtureWindow) {
        const bool drift=fixtureWindow==4;
        if (!world || kind!=4 || start!=Point{-10000.f,20.f,30.f} ||
            axiom::surface::Distance(end,Point{-9472.f,20.f,drift?30.04224f:30.f})>0.001f)
            return false;
        const bool leading=fixtureWindow==2 || fixtureWindow==3;
        vector.count=leading?2u:1u;
        SetProbeCandidate(vector.entries[leading?1u:0u],Point{-9490.f,20.f,30.f},true);
        if (leading) {
            const bool blocker=fixtureWindow==3;
            SetProbeCandidate(vector.entries[0],blocker?Point{-9900.f,20.f,30.f}:
                              Point{-9856.f,20.f,30.f},blocker);
            if (!blocker) {
                const uint32_t tag=0x5FFFFFFFu,kindValue=1;
                std::memcpy(vector.entries[0].bytes.data()+0x48,&tag,sizeof(tag));
                std::array<uint8_t,0x8C> raw{};
                std::memcpy(raw.data()+0x88,&kindValue,sizeof(kindValue));
                TraceCandidateConvert(nullptr,raw.data(),vector.entries,1);
            }
        }
        return true;
    }
    fixtureContinuedQuery=world && kind==4 && start==Point{-9855.96875f,20.f,30.f} &&
        end==Point{40000.f,20.f,30.f};
    vector.count=2;
    SetProbeCandidate(vector.entries[0],fixtureContinuedBlocker?Point{-9824.f,20.f,30.f}:
                      Point{-6200.f,20.f,30.f},!fixtureContinuedBlocker);
    SetProbeCandidate(vector.entries[1],Point{-6170.f,20.f,30.f},true);
    if (fixtureContinuedBlocker) {
        const uint32_t tag=0x5FFFFFFFu;
        std::memcpy(vector.entries[0].bytes.data()+0x48,&tag,sizeof(tag));
        std::array<uint8_t,0x8C> raw{};
        std::memcpy(raw.data()+0x88,&fixtureRetryKind,sizeof(fixtureRetryKind));
        TraceCandidateConvert(nullptr,raw.data(),vector.entries,1);
    }
    return fixtureContinuedQuery;
}

const void* fixtureCommitPacket=nullptr;
uint8_t fixtureCommitLane=255;
Point fixtureAnchor{-9760.f,20.1f,30.f};
bool fixtureCastForwarded=false;

bool FixtureCommitAnchor(void*,uint8_t lane,void* transform) {
    if (lane!=0) return false;
    std::memcpy(static_cast<uint8_t*>(transform)+0x1C,fixtureAnchor.data(),sizeof(fixtureAnchor));
    SetLastError(229);
    return true;
}

void* FixtureCastDispatch(void* owner,void* result,void* request,void* context,const void* event) {
    fixtureCastForwarded=owner && request && context && event && GetLastError()==225;
    SetLastError(226);
    return result;
}

void FixtureSurfaceCommit(void* object,uint8_t lane,const void* packet) {
    fixtureCommitPacket=packet;
    fixtureCommitLane=lane;
    std::array<uint8_t,40> transform{};
    if (g_testCommitAnchor) TraceCommitAnchor(object,lane,transform.data());
    auto* p=static_cast<uint8_t*>(object);
    const float nativeDistance=88.5f;
    std::memcpy(p+0x4C8+0x428*lane,&nativeDistance,sizeof(nativeDistance));
    p[0x168+0x428*lane]=1;
    p[0xA4C+0x1A0*lane]=3;
    SetLastError(227);
}

uint8_t tracePath=0;
bool traceReset=false, traceSweepHit=false;
uint8_t traceResetFlag=2;
float traceDelta=0.f;
std::array<uint8_t,0x94> sweepQuery{};
std::array<uint8_t,16> sweepStorage{};
std::array<void*,2> sweepResult{};
bool traceSweepArgs=false;

bool FixtureTraceSweep(void* world,const void* query,uint8_t kind,void* result,float epsilon) {
    traceSweepArgs=world==traceForwarded[0] && query==sweepQuery.data() &&
                   kind==4 && result==sweepResult.data() && epsilon==-1.f && GetLastError()==212;
    std::memcpy(sweepStorage.data()+8,&traceHits,sizeof(traceHits));
    SetLastError(213);
    return traceSweepHit;
}

void FixtureTraceReset(void* object) {
    auto* bytes=static_cast<uint8_t*>(object);
    bytes[0x140]=2;
    bytes[0x144]=0;
    SetLastError(215);
}

void FixtureTraceUpdate(void* object,float delta) {
    traceDelta=delta;
    traceForwarded[0]=object;
    if (GetLastError()!=211) return;
    if (tracePath==1) {
        TraceTarget(object,traceQuery.data());
        if (GetLastError()!=214) return;
    } else if (tracePath==2) {
        SetLastError(212);
        if (TraceSweep(object,sweepQuery.data(),4,sweepResult.data(),-1.f)!=traceSweepHit ||
            GetLastError()!=213) return;
        auto* bytes=static_cast<uint8_t*>(object);
        bytes[0x140]=traceMode;
        bytes[0x144]=traceRaw;
    }
    if (traceReset) {
        TracePreQueryReset(object,traceResetFlag);
        if (GetLastError()!=215) return;
    }
    SetLastError(214);
}


int main(int argc,char** argv) {
    g.profile=argc==2 && !std::strcmp(argv[1],"601") ? &kMenu601 : &kMenu60;
    unsigned checks=0, failed=0;
    const auto check=[&](bool good,const char* text) {
        ++checks;
        if (!good) { ++failed; std::printf("FAIL %s\n",text); }
    };
    check(DllMain(GetModuleHandleW(nullptr),DLL_PROCESS_ATTACH,nullptr) &&
          !g.stop && !g.worker && g_log==INVALID_HANDLE_VALUE,"foreign test host is inert at process attach");
    check(HashFile(L"..\\evidence\\Menu60-1927CF17.pe-data",kMenuSha) &&
          !HashFile(L"..\\evidence\\Menu60-1927CF17.pe-data",kSAFSha),"BCrypt exact digest accepts Menu and refuses a wrong identity");

    const auto menu604Path=L"evidence\\Menu604-2E248E02.pe-data";
    const auto menu604File=CreateFileW(menu604Path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    const auto menu604Section=menu604File==INVALID_HANDLE_VALUE ? nullptr :
        CreateFileMappingW(menu604File,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);
    const auto menu604Image=menu604Section ? MapViewOfFile(menu604Section,FILE_MAP_READ,0,0,0) : nullptr;
    const auto menu604Base=reinterpret_cast<uintptr_t>(menu604Image);
    check(menu604Image && !ExactMenu(menu604Base),"6.0.4 does not opt into legacy Menu writes");
    if (!menu604Image) return 2;
    constexpr uintptr_t menu604WorkerFlag=0x7B605C;
    const auto latch=reinterpret_cast<void*>(menu604Base+menu604WorkerFlag);
    DWORD latchProtection=0;
    if (!VirtualProtect(latch,4,PAGE_WRITECOPY,&latchProtection) ||
        !Set(menu604Base+menu604WorkerFlag,uint32_t{1})) return 2;
    DWORD latchIgnored=0;
    if (!VirtualProtect(latch,4,latchProtection,&latchIgnored)) return 2;
    const auto* providerPath=g.profile==&kMenu601 ? L"evidence\\Menu601-DA60636F.pe-data"
                                          : L"..\\evidence\\Menu60-1927CF17.pe-data";
    const auto file=CreateFileW(providerPath,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    const auto section=file==INVALID_HANDLE_VALUE ? nullptr :
        CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);
    // Map as inert PE data. No import binding,
    // TLS, entrypoint, or provider instructions execute.
    const auto image=section ? MapViewOfFile(section,FILE_MAP_READ,0,0,0) : nullptr;
    const auto base=reinterpret_cast<uintptr_t>(image);
    check(image && ExactMenu(base)==g.profile && HashFile(providerPath,g.profile->hash),
          "exact provider PE is admitted by SHA, header and image size");
    const auto& profile=*g.profile;
    const auto vt=base+profile.vtable;
    if (image) {
        // SEC_IMAGE applies relocations using Windows' shared image base before
        // MapViewOfFileEx. Check the three slots reconcile to one aligned base,
        // then materialize just those relocations at this fixture's view base.
        const auto relocated=At<uintptr_t>(vt+0x50)-profile.init.rva;
        const bool relocations=relocated && (relocated & 0xFFFF)==0 &&
            At<uintptr_t>(vt+0x58)==relocated+profile.shutdown.rva &&
            At<uintptr_t>(vt+0x90)==relocated+profile.toggle.rva;
        DWORD prior=0;
        const bool write=relocations && VirtualProtect(reinterpret_cast<void*>(vt),0x98,PAGE_WRITECOPY,&prior);
        check(write && Set(vt+0x50,base+profile.init.rva) &&
              Set(vt+0x58,base+profile.shutdown.rva) && Set(vt+0x90,base+profile.toggle.rva),
              "inert Menu vtable relocation matches all three exact methods");
        DWORD ignored=0;
        if (write) VirtualProtect(reinterpret_cast<void*>(vt),0x98,prior,&ignored);
    }
    check(image && MenuWitnesses(base), "production witness check recognizes inert mapped Menu");
    if (image) {
        const auto at=reinterpret_cast<uint8_t*>(base+g.profile->toggle.rva);
        DWORD previous=0;
        if (VirtualProtect(at,1,PAGE_EXECUTE_WRITECOPY,&previous)) {
            const auto original=*at;
            *at^=1;
            DWORD ignored=0;
            const bool changed=VirtualProtect(at,1,previous,&ignored)!=0;
            if (changed) check(!MenuWitnesses(base), "foreign change to exact Menu toggle blocks startup");
            else check(false,"original execute protection restored for mutation test");
            if (VirtualProtect(at,1,PAGE_EXECUTE_WRITECOPY,&ignored)) {
                *at=original;
                VirtualProtect(at,1,previous,&ignored);
            }
        } else check(false,"test can isolate a foreign Menu mutation");
        UnmapViewOfFile(image);
    }
    if (section) CloseHandle(section);
    if (file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    const auto allocate=[](size_t n,DWORD protect) {
        return reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,n,MEM_RESERVE|MEM_COMMIT,protect));
    };
    g.game=allocate(0x7250000,PAGE_READWRITE);
    g.menu=allocate(0x6E0000,PAGE_READWRITE);
    const auto object=allocate(4096,PAGE_READWRITE);
    const auto shim=allocate(4096,PAGE_EXECUTE_READWRITE);
    if (!g.game || !g.menu || !object || !shim) return 2;
    const auto traceContext=allocate(4096,PAGE_READWRITE);
    const auto traceRoot=allocate(4096,PAGE_READWRITE);
    if (!traceContext || !traceRoot) return 2;
    Set(g.game+0x72451C0,traceRoot);
    Set(traceRoot+0x30,traceContext);
    Set(traceContext+0x50,object);
    Set(traceContext+0x58,object);
    Set(object,g.game+0x5994CC8);
    Set(object+8,object);
    Set(object+0x94,uint8_t{1});
    Set(object+0xA0,object);
    g_traceTls=TlsAlloc();
    if (g_traceTls==TLS_OUT_OF_INDEXES) return 2;
    g_testTraceTarget=FixtureTraceTarget;
    g_testTraceUpdate=FixtureTraceUpdate;
    g_testTraceSweep=FixtureTraceSweep;
    g_testTraceReset=FixtureTraceReset;
    g_testTraceDispatch=FixtureTraceDispatch;
    g_testCandidateConvert=FixtureConvertCandidate;
    Set(object+0x140,uint8_t{2});
    g_initialized=true;
    g_traceUnlimited=true;
    const float endpoints[]{-10000.f,20.f,30.f,40000.f,20.f,30.f};
    const uint32_t mask=0x70;
    std::memcpy(traceQuery.data()+8,&mask,sizeof(mask));
    std::memcpy(traceQuery.data()+0x28,endpoints,sizeof(endpoints));
    const auto aim=traceQuery.data();
    tracePath=1;
    sweepResult[0]=sweepStorage.data();
    std::memcpy(sweepQuery.data()+8,&mask,sizeof(mask));
    std::memcpy(sweepQuery.data()+0x48,endpoints,sizeof(endpoints));
    const auto invoke=[&] {
        SetLastError(211);
        TraceUpdate(reinterpret_cast<void*>(object),0.25f);
        const auto last=GetLastError();
        return last==214 && TlsGetValue(g_traceTls)==nullptr && traceDelta==0.25f;
    };
    check(invoke() && traceNativeErrorPreserved && traceForwardedAim==aim &&
          traceForwarded==std::array<void*,4>{reinterpret_cast<void*>(object),
            traceQuery.data(),traceCollector.data(),aim},
          "target and dispatch forward original arguments and preserve Win32 last error");
    const auto getTrace=[&](Result result)->const TraceSlot& {
        return g_traceSlots[TraceIndex(axiom::trace::Path::Primary,result,true)];
    };
    check(getTrace(Result::NoPhysicsHit).pending &&
          getTrace(Result::NoPhysicsHit).sample.rayLength==50000.f,
          "one scoped query captures a native long ray miss");
    traceHits=2;
    check(invoke() && getTrace(Result::Rejected).pending &&
          getTrace(Result::Rejected).sample.count==2,
          "same-frame final rejection retains the physics count");
    traceRaw=1;
    traceHits=1;
    check(invoke() && conversionArgs && getTrace(Result::Raw).pending &&
          axiom::trace::PublishedHit(getTrace(Result::Raw).sample).source==axiom::trace::RaySource::WithoutBody &&
          axiom::trace::PublishedHit(getTrace(Result::Raw).sample).material==37 &&
          axiom::trace::PublishedHit(getTrace(Result::Raw).sample).kind==1,
          "native conversion forwards ABI and correlates a bodyless red hit");
    traceMode=0;
    fixtureHasBody=true;
    check(invoke() && getTrace(Result::Accepted).pending &&
          axiom::trace::PublishedHit(getTrace(Result::Accepted).sample).source==axiom::trace::RaySource::WithBody,
          "native conversion correlates a body-backed blue hit without changing cursor flags");
    DrainTargetTrace();
    check(!getTrace(Result::NoPhysicsHit).pending && !getTrace(Result::Accepted).pending,
          "worker drains captured rows outside of the game callback");
    check(invoke() && !getTrace(Result::Accepted).pending,"rapid repeated outcomes are throttled");
    const auto alternate=[&](Result result)->const TraceSlot& {
        return g_traceSlots[TraceIndex(axiom::trace::Path::Alternate,result,true)];
    };
    tracePath=2;
    traceMode=2;
    traceRaw=0;
    check(invoke() && traceSweepArgs && alternate(Result::NoPhysicsHit).pending &&
          alternate(Result::NoPhysicsHit).sample.count==0 &&
          alternate(Result::NoPhysicsHit).sample.beforeMode==0 &&
          alternate(Result::NoPhysicsHit).sample.to[0]==40000.f &&
          g_traceSlots[TraceClearIndex(true,axiom::trace::Path::Alternate)].pending,
          "alternate sweep forwards all five arguments and records miss with prior state");
    traceMode=1;
    traceHits=4;
    traceSweepHit=true;
    check(invoke() && alternate(Result::Accepted).pending &&
          alternate(Result::Accepted).sample.count==4,
          "alternate sweep records native count and accepted mode");
    tracePath=0;
    traceReset=true;
    using axiom::trace::ResetReason;
    auto* decisionPage=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (decisionPage) {
        // Give the relay the same RBP-relative byte as the native parent.
        constexpr std::array<uint8_t,11> entry{
            0x55,0x48,0x83,0xEC,0x70,0x48,0x8B,0xEC,0x88,0x4D,0x67};
        constexpr std::array<uint8_t,21> result{
            0x0F,0xB6,0x45,0x67,             // movzx eax, byte ptr [rbp+0x67]
            0x0F,0x95,0xC2,                  // setne dl (flags from the native compare)
            0x0F,0xB6,0xD2,0xC1,0xE2,0x08,   // movzx edx,dl; shl edx,8
            0x09,0xD0,                       // or eax,edx
            0x48,0x83,0xC4,0x70,0x5D,0xC3};  // add rsp,0x70; pop rbp; ret
        // The reset target jumps to a second, independently distinguishable return stub.
        auto resetResult=result;
        resetResult[12]=0x10; // Put the reset path's saved nonzero flag in bit 16.
        const auto relay=axiom::trace::ResetDecisionRelay(
            reinterpret_cast<uintptr_t>(decisionPage+128),reinterpret_cast<uintptr_t>(decisionPage+64));
        std::memcpy(decisionPage,entry.data(),entry.size());
        std::memcpy(decisionPage+entry.size(),relay.data(),relay.size());
        std::memcpy(decisionPage+64,result.data(),result.size());
        std::memcpy(decisionPage+128,resetResult.data(),resetResult.size());
        DWORD previousProtection=0;
        const bool executable=VirtualProtect(decisionPage,4096,PAGE_EXECUTE_READ,&previousProtection) &&
                              FlushInstructionCache(GetCurrentProcess(),decisionPage,4096);
        const auto decision=reinterpret_cast<uint32_t(*)(uint8_t)>(decisionPage);
        check(executable && decision(0)==0,
              "decision relay preserves zero branch, frame byte and compare flags");
        check(executable && decision(1)==0x10002,
              "decision relay preserves the reset branch and compare flags while tagging it");
        VirtualFree(decisionPage,0,MEM_RELEASE);
    } else {
        check(false,"decision relay fixture allocation");
    }
    const auto recordedReset=[&](ResetReason reason) {
        const auto& reset=g_traceSlots[TraceResetIndex(true,reason)];
        const auto& clear=g_traceSlots[TraceClearIndex(true,Path::None,reason)];
        return reset.pending && clear.pending && reset.sample.resetReason==reason &&
               clear.sample.resetReason==reason && axiom::trace::CursorCleared(clear.sample);
    };
    const auto parts=object+0x400;
    const auto action=object+0x500;
    const auto actionRoot=object+0x600;
    const auto actorState=object+0x700;
    Set(object+0x68,parts);
    Set(parts+0x40,action);
    Set(action+0x88,actionRoot);
    Set(actionRoot+0x10,actorState);
    Set(actorState+0xFA,uint8_t{5});
    check(invoke() && recordedReset(ResetReason::ActorMode) &&
          g_traceSlots[TraceResetIndex(true,ResetReason::ActorMode)].sample.sampledActorMode==5,
          "actor reset records the mode read from the controlled actor's verified chain");
    Set(object+0x140,uint8_t{0});
    Set(object+0x144,uint8_t{1});
    traceResetFlag=1;
    Set(actionRoot+0x10,uintptr_t{0});
    check(invoke() && recordedReset(ResetReason::ContextOverride) &&
          recordedReset(ResetReason::ActorMode) &&
          g_traceSlots[TraceResetIndex(true,ResetReason::ContextOverride)].sample.sampledActorMode==255,
          "context reset survives an unavailable mode chain and records it as unavailable");
    Set(object+0x140,uint8_t{0});
    Set(object+0x144,uint8_t{1});
    traceResetFlag=3;
    check(invoke() && recordedReset(ResetReason::Unknown),
          "unexpected native flag is logged without assigning a cause");
    DrainTargetTrace();
    traceReset=false;
    check(invoke() && g_traceSlots[TraceIndex(axiom::trace::Path::None,Result::NoQuery,true)].sample.mode==2,
          "outer update without a query reports final native state");
    DrainTargetTrace();
    Set(traceContext+0x58,uintptr_t{0});
    tracePath=2;
    traceMode=2;
    traceHits=2;
    check(invoke() && !alternate(Result::Rejected).pending,
          "foreign actor runs native sweep without publishing a diagnostic");
    Set(traceContext+0x58,object);
    Set(object+0x140,uint8_t{0});
    Set(object+0x144,uint8_t{1});
    TraceExternalReset(reinterpret_cast<void*>(object));
    check(GetLastError()==215 && g_traceSlots[TraceExternalResetIndex(true)].pending &&
          axiom::trace::CursorCleared(g_traceSlots[TraceExternalResetIndex(true)].sample),
          "external reset records the actual target clear outside the update");
    // Keep all physics and tether callbacks inside this inert fixture.
    g_testSurfaceQuery=FixtureSurfaceQuery;
    g_testSurfaceCommit=FixtureSurfaceCommit;
    g_testCommitAnchor=FixtureCommitAnchor;
    g_testCastDispatch=FixtureCastDispatch;
    g_commitTls=TlsAlloc();
    if (g_commitTls==TLS_OUT_OF_INDEXES) return 2;
    g_searchReady=true;
    g_effectiveRangeBits=axiom::FloatBits(50000.f);
    TargetFrame injected{};
    injected.component=reinterpret_cast<void*>(object);
    const auto surfaceCase=[&](bool centerBody,bool probeBody,bool enabled) {
        std::array<NativeRayCandidate,8> entries{};
        fixtureCenterBody=centerBody;
        fixtureProbeBody=probeBody;
        surfaceQueries=0;
        g_searchReady=enabled;
        g_surfaceHistory={};
        g_nextSurfaceProbe=0;
        injected={};
        injected.component=reinterpret_cast<void*>(object);
        injected.world=injected.component;
        injected.sample.path=Path::Primary;
        injected.sample.queried=true;
        injected.sample.tick=1000;
        injected.sample.count=1;
        injected.sample.rayLength=50000.f;
        injected.effectiveRange=50000.f;
        injected.expectedConversions=1;
        std::memcpy(injected.sample.from,endpoints,12);
        std::memcpy(injected.sample.to,endpoints+3,12);
        TlsSetValue(g_traceTls,&injected);
        std::memcpy(injected.searchQuery.data()+8,&mask,sizeof(mask));
        std::memcpy(injected.searchQuery.data()+0x28,endpoints,sizeof(endpoints));
        g_testCandidateConvert=FixtureSurfaceCenter;
        SetLastError(221);
        TraceCandidateConvert(nullptr,traceHit.data(),entries.data(),1);
        const bool preserved=GetLastError()==216 && surfaceAbi;
        TlsSetValue(g_traceTls,nullptr);
        return preserved && entries[0].HasBody()==(centerBody || (probeBody && enabled));
    };
    check(surfaceCase(false,true,true) && surfaceQueries==1 && injected.sample.probeOffered,
          "bodyless primary hit is replaced with a fresh native result for original game validation");
    check(surfaceCase(false,false,true) && surfaceQueries==1 && !injected.sample.probeOffered,
          "an unattachable first retry retains the original native result");
    check(surfaceCase(true,true,true) && surfaceQueries==0 && !injected.sample.probeTried,
          "native body candidates retain priority");
    check(surfaceCase(false,true,false) && surfaceQueries==0,
          "missing SAF range ownership prevents native retry");
    std::array<NativeRayCandidate,2> mixed{};
    SetProbeCandidate(mixed[0],Point{-9759.f,20.f,30.f},true);
    SetProbeCandidate(mixed[1],centerRay,false);
    injected.world=reinterpret_cast<void*>(object);
    injected.sample.count=2;
    injected.sample.candidates={};
    injected.sample.converted=2;
    injected.sample.mode=2;
    injected.sample.raw=true;
    std::memcpy(injected.sample.hit,centerRay.data(),sizeof(centerRay));
    std::memcpy(injected.sample.candidates[1].point,centerRay.data(),sizeof(centerRay));
    injected.unlimited=true;
    injected.sample.tick=GetTickCount64();
    injected.searchQuery={};
    std::memcpy(injected.searchQuery.data()+8,&mask,sizeof(mask));
    std::memcpy(injected.searchQuery.data()+0x28,endpoints,sizeof(endpoints));
    injected.sample.probeOffered=false;
    g_searchReady=true;
    g_surfaceHistory={};
    surfaceQueries=0;
    SetLastError(216);
    ProbeNearbySurface(injected,mixed.data());
    check(!surfaceQueries,"mixed ray preserves ordinary collision ordering without a confirmed red point");
    RememberRedHit(injected,true);
    ++injected.sample.tick;
    SetLastError(216);
    ProbeNearbySurface(injected,mixed.data());
    check(surfaceQueries==1 && mixed[0].Position()==Point{-9759.f,20.f,30.f} &&
          mixed[1].HasBody() && injected.sample.probeOffered && surfaceAbi,
          "same native red point gains a complete body while other targets stay in native order");
    SetProbeCandidate(mixed[1],centerRay,false);
    injected.effectiveRange=200.f;
    SetLastError(216);
    ProbeNearbySurface(injected,mixed.data());
    check(surfaceQueries==1 && !mixed[1].HasBody(),"a shorter effective range disqualifies the same collision");

    injected.effectiveRange=50000.f;
    injected.sample.tick=GetTickCount64();
    injected.offeredPoint=fixtureAnchor;
    NativeTargetKey nativeTarget{};
    nativeTarget[0]=1;
    nativeTarget[4]=17;
    nativeTarget[8]=51;
    nativeTarget[24]=71;
    Set(object+0xF0,nativeTarget);
    Set(object+0x140,uint8_t{0});
    auto* controlled=reinterpret_cast<void*>(object);
    std::array<uint8_t,0xF8> event{};
    event[0]=6;
    event[9]=2;
    std::memcpy(event.data()+16,nativeTarget.data()+8,16);
    event[32]=19;
    const auto cast=[&] {
        SetLastError(225);
        return TraceCastSurface(controlled,controlled,controlled,controlled,event.data())==controlled &&
               GetLastError()==226 && fixtureCastForwarded;
    };
    RememberRecovery(injected,true);
    event[16]++;
    check(cast() && !g_castCount,"changed cast identity is forwarded without recovered attribution");
    event[16]--;
    check(cast() && g_castCount==1 && g_castEvents[0].tick==injected.sample.tick,
          "native cast descriptor matches the recovered published target");
    fixtureAnchor[1]+=1.f;
    Set(object+0xA4C,uint8_t{2});
    TraceSurfaceCommit(controlled,0,event.data());
    check(GetLastError()==227 && fixtureCommitPacket==event.data() && fixtureCommitLane==0 && g_attachmentCount==1 &&
          g_attachmentEvents[0].distance==88.5f && g_attachmentEvents[0].kind==1 &&
          g_attachmentEvents[0].unlimited && g_attachmentEvents[0].anchorKnown &&
          !g_attachmentEvents[0].recoveredTick,"different native anchor records attachment without recovery attribution");
    fixtureAnchor=injected.offeredPoint;
    Set(object+0xA4C,uint8_t{2});
    TraceSurfaceCommit(controlled,0,event.data());
    check(g_attachmentCount==2 && g_attachmentEvents[1].castTick &&
          g_attachmentEvents[1].recoveredTick==injected.sample.tick &&
          g_attachmentEvents[1].anchor==fixtureAnchor && g_attachmentEvents[1].effectiveRange==50000.f,
          "matching native anchor ties the recovered cast to a real reel-to-surface transition");
    TraceSurfaceCommit(controlled,0,event.data());
    check(g_attachmentCount==2,"repeated commit observer does not double count");
    // World-body cursor descriptors can already be type 2.
    nativeTarget[0]=2;
    nativeTarget[4]=13;
    nativeTarget[24]=19;
    Set(object+0xF0,nativeTarget);
    event[12]=13;
    g_traceUnlimited=false;
    g_effectiveRangeBits=axiom::FloatBits(5000.f);
    injected.effectiveRange=5000.f;
    RememberRecovery(injected,false);
    check(cast() && g_castCount==2 && !g_castEvents[1].unlimited &&
          g_castEvents[1].range==5000.f,"Reach off dispatch uses the live SAF Range");
    Set(object+0xA4C,uint8_t{2});
    TraceSurfaceCommit(controlled,0,event.data());
    check(g_attachmentCount==3 && !g_attachmentEvents[2].unlimited &&
          g_attachmentEvents[2].castTick && g_attachmentEvents[2].effectiveRange==5000.f,
          "Reach off also correlates a recovered cast to the native tether");
    // A sorted native ray can return an unresolved grid hit before its
    // first body. Recover the whole native record without a second query.
    const Point grid{-9856.f,20.f,30.f};
    const Point frontBody{-9860.f,20.f,30.f};
    const auto nativeRay=[&](bool unlimited,float reach,bool blocker,bool secondRed,
                             bool nonGrid=false,float bodyX=-9848.f,float bodyY=20.f,bool warmed=true) {
        const Point farBody{bodyX,bodyY,30.f};
        std::array<NativeRayCandidate,3> records{};
        const Point red=nonGrid?Point{-9855.f,20.f,30.f}:grid;
        SetProbeCandidate(records[0],blocker?frontBody:red,blocker);
        SetProbeCandidate(records[1],blocker?grid:Point{-9852.f,20.f,30.f},false);
        SetProbeCandidate(records[2],farBody,true);
        if (!blocker && !secondRed) records[1]=records[2];
        const uint64_t handle=0x12345678;
        const size_t bodyIndex=blocker||secondRed?2:1;
        std::memcpy(records[bodyIndex].bytes.data()+0x40,&handle,sizeof(handle));
        injected={};
        injected.component=controlled;
        injected.world=controlled;
        injected.effectiveRange=reach;
        injected.unlimited=unlimited;
        injected.sample.tick=GetTickCount64();
        injected.sample.count=static_cast<uint32_t>(bodyIndex+1);
        injected.sample.converted=injected.sample.count;
        injected.sample.path=Path::Primary;
        injected.sample.queried=true;
        injected.sample.mode=2;
        injected.sample.raw=true;
        injected.sample.rayLength=50000.f;
        std::memcpy(injected.sample.from,endpoints,12);
        std::memcpy(injected.sample.to,endpoints+3,12);
        std::memcpy(injected.sample.hit,red.data(),sizeof(red));
        for (size_t n=0;n<injected.sample.count;++n) {
            auto& item=injected.sample.candidates[n];
            const Point point=records[n].Position();
            std::memcpy(item.point,point.data(),sizeof(point));
            item.body=records[n].HasBody();
            item.kind=1;
            item.material=item.body?0u:0x5FFFFFFFu;
            std::memcpy(records[n].bytes.data()+0x48,&item.material,sizeof(item.material));
        }
        g_lastRed={};
        if (warmed) RememberRedHit(injected,unlimited);
        ++injected.sample.tick;
        surfaceQueries=0;
        g_surfaceHistory={};
        g_nextSurfaceProbe=UINT64_MAX; // Force this fixture to use the original native ray.
        ProbeNearbySurface(injected,records.data());
        const auto expected=blocker?frontBody:farBody;
        uint64_t observed=0;
        std::memcpy(&observed,records[0].bytes.data()+0x40,sizeof(observed));
        return records[0].Position()==expected && records[0].HasBody() &&
            !surfaceQueries && (!blocker || !injected.sample.probeOffered) &&
            (blocker || (records[1].Position()==red && !records[1].HasBody() && observed==handle));
    };
    check(nativeRay(true,50000.f,false,false),
          "Unlimited Reach recovers the first body at 152 units from a grid-terminated ray");
    check(nativeRay(false,500.f,false,false),
          "SAF-only reach recovers the first body at 152 units from a grid-terminated ray");
    check(!nativeRay(false,149.f,false,false) && !injected.sample.probeOffered,
          "SAF configured range still bounds the recovered body");
    check(nativeRay(true,50000.f,true,false),
          "a nearer native body retains priority");
    check(!nativeRay(true,50000.f,false,true) && !injected.sample.probeOffered,
          "an intervening unresolved blocker refuses promotion");
    check(!nativeRay(false,500.f,false,false,true) && !injected.sample.probeOffered,
          "an ordinary off-grid geometry hit is never bypassed");
    check(nativeRay(true,50000.f,false,false,false,-6200.f) &&
          injected.sample.offeredRayDistance==3800.f,
          "direct aim ray accepts the first real body far beyond the grid boundary");
    check(!nativeRay(false,500.f,false,false,false,-6200.f) && !injected.sample.probeOffered,
          "finite SAF range still rejects a distant crosshair hit");
    check(!nativeRay(true,50000.f,false,false,false,-6200.f,20.5f) && !injected.sample.probeOffered,
          "distant promotion still requires alignment with the crosshair ray");
    check(nativeRay(false,50000.f,false,false,false,-6200.f,20.f,false) &&
          injected.sample.offeredRayDistance==3800.f,
          "an unseeded aim ray recovers its own distant collision on first observation");
    const auto orderedRay=[&](const Point& direction,uint32_t material,bool more,bool blocker) {
        constexpr Point origin{0.f,20.f,0.f};
        const Point end{50000.f*direction[0],20.f,50000.f*direction[2]};
        const auto onRay=[&](float distance) {
            return Point{distance*direction[0],20.f,distance*direction[2]};
        };
        std::array<NativeRayCandidate,3> entries{};
        SetProbeCandidate(entries[0],onRay(96.f),false);
        if (more) SetProbeCandidate(entries[1],onRay(blocker?115.f:128.f),false);
        SetProbeCandidate(entries[more?2:1],onRay(530.f),true);
        injected={};
        injected.component=controlled;
        injected.world=controlled;
        injected.effectiveRange=50000.f;
        injected.sample.tick=GetTickCount64();
        injected.sample.count=more?3:2;
        injected.sample.converted=injected.sample.count;
        injected.sample.rayLength=50000.f;
        std::memcpy(injected.sample.from,origin.data(),sizeof(origin));
        std::memcpy(injected.sample.to,end.data(),sizeof(end));
        for (size_t i=0;i<injected.sample.count;++i) {
            auto& observation=injected.sample.candidates[i];
            const auto point=entries[i].Position();
            std::memcpy(observation.point,point.data(),sizeof(point));
            observation.body=entries[i].HasBody();
            observation.kind=1;
            observation.material=observation.body?0u:material;
            std::memcpy(entries[i].bytes.data()+0x48,&observation.material,sizeof(observation.material));
        }
        g_lastRed={};
        g_surfaceHistory={};
        g_nextSurfaceProbe=UINT64_MAX;
        ProbeNearbySurface(injected,entries.data());
        return entries[0].HasBody() && entries[0].Position()==onRay(530.f) &&
               injected.sample.probeOffered && injected.sample.offeredRayDistance==530.f;
    };
    check(orderedRay({-1.f,0.f,0.f},0x1FFFFFFFu,false,false),
          "westward direct aim recovers the first native body without a prior cursor");
    check(orderedRay({0.f,0.f,1.f},0x3FFFFFFFu,false,false),
          "positive-Z direct aim recovers beyond its native grid marker");
    check(orderedRay({0.f,0.f,-1.f},0x7FFFFFFFu,false,false),
          "negative-Z direct aim recovers beyond its native grid marker");
    check(orderedRay({1.f,0.f,0.f},0x5FFFFFFFu,true,false),
          "two ordered verified grid markers expose the first native body on the same ray");
    check(!orderedRay({1.f,0.f,0.f},0x5FFFFFFFu,true,true),
          "an off-grid unresolved record still blocks the farther native body");
    check(!orderedRay({1.f,0.f,0.f},0x1FFFFFFFu,false,false),
          "a marker tagged for the opposite direction cannot authorize promotion");
    const auto extendedRay=[&](bool unlimited,float reach,bool blocker,bool grow=false,bool hit=true,float aimOffset=0.05f) {
        // The primary ray only hit the grid. The offset query starts at
        // the very same camera and offers its first native collider.
        fixtureExtended={};
        fixtureExtended.origin={-10000.f,20.f,30.f};
        fixtureExtended.reach=reach;
        fixtureExtended.hasBlocker=blocker;
        fixtureExtended.grow=grow;
        fixtureExtended.nativeHit=hit;
        g_testFreeProbeVector=FixtureFreeExtended;
        SetProbeCandidate(fixtureExtended.first,
            blocker?Point{-9865.f,22.f,30.f}:Point{-9851.f,20.f+aimOffset,30.f},true);
        SetProbeCandidate(fixtureExtended.second,Point{-9851.f,20.f+aimOffset,30.f},true);
        g_testSurfaceQuery=FixtureExtendedProbe;
        NativeRayCandidate entry{};
        SetProbeCandidate(entry,grid,false);
        injected={};
        injected.component=controlled;
        injected.world=controlled;
        injected.effectiveRange=reach;
        injected.unlimited=unlimited;
        injected.sample.tick=GetTickCount64();
        injected.sample.count=1;
        injected.sample.converted=1;
        injected.sample.path=Path::Primary;
        injected.sample.queried=true;
        injected.sample.mode=2;
        injected.sample.raw=true;
        injected.sample.rayLength=50000.f;
        injected.sample.candidates[0].kind=1;
        std::memcpy(injected.sample.from,endpoints,12);
        std::memcpy(injected.sample.to,endpoints+3,12);
        std::memcpy(injected.sample.hit,grid.data(),sizeof(grid));
        std::memcpy(injected.sample.candidates[0].point,grid.data(),sizeof(grid));
        std::memcpy(injected.searchQuery.data()+8,&mask,sizeof(mask));
        std::memcpy(injected.searchQuery.data()+0x28,endpoints,sizeof(endpoints));
        RememberRedHit(injected,unlimited);
        ++injected.sample.tick;
        surfaceQueries=0;
        g_surfaceHistory={controlled,controlled,Point{-10000.f,20.f,30.f},Point{40000.f,20.f,30.f},grid};
        g_surfaceHistory.tick=injected.sample.tick;
        g_surfaceHistory.preferred=axiom::surface::kLocalProbeCount;
        SetLastError(216);
        ProbeNearbySurface(injected,&entry);
        return fixtureExtended.forwarded && surfaceQueries==1 &&
            injected.sample.recovery==Recovery::ExtendedRay &&
            injected.sample.probeOffered==entry.HasBody();
    };
    check(extendedRay(true,50000.f,false) && injected.sample.probeOffered &&
          injected.sample.offeredRayDistance>=149.f && injected.sample.redRayDistance==144.f,
          "full-origin extended ray supplies a genuine farther collision with Unlimited Reach");
    check(fixtureExtended.nativeInlineLayout,
          "nine-hit growth recognizes the inline buffer beside the native vector header");
    check(extendedRay(true,50000.f,false,false,true,3.04f) && !injected.sample.probeOffered,
          "an off-center far body cannot replace the crosshair hit");
    check(extendedRay(false,150.f,false) && injected.sample.probeOffered,
          "extended offset query clips its endpoint to the shorter SAF setting");
    check(extendedRay(false,148.f,false) && !injected.sample.probeOffered,
          "extended offset query declines a body outside the active reach");
    check(extendedRay(true,50000.f,true) && !injected.sample.probeOffered,
          "offset query cannot skip its first physical blocker");
    check(fixtureExtended.releases==0,"inline probe storage is never sent to an allocator");
    check(extendedRay(false,50000.f,false,true) && injected.sample.probeOffered &&
          fixtureExtended.releases==1 && !fixtureExtended.grown,
          "nine-hit probe keeps its collision and releases only grown storage");
    check(extendedRay(true,50000.f,true,true) && !injected.sample.probeOffered &&
          fixtureExtended.releases==1 && !fixtureExtended.grown,
          "grown storage is released when the first collision blocks recovery");
    check(extendedRay(true,50000.f,false,true,false) && !injected.sample.probeOffered &&
          fixtureExtended.releases==1 && !fixtureExtended.grown,
          "grown storage is released when the native query reports no hit");
    g_testFreeProbeVector=nullptr;

    g_allHitsCollectorReady.store(true);
    const auto continuedRay=[&](bool blocker,uint32_t rawKind=0,unsigned window=0) {
        NativeRayCandidate entry{};
        SetProbeCandidate(entry,grid,false);
        g_testSurfaceQuery=FixtureContinuedProbe;
        fixtureContinuedBlocker=blocker;
        fixtureRetryKind=rawKind;
        fixtureWindow=window;
        fixtureContinuedQuery=false;
        g_testCandidateConvert=FixtureRetryConvert;
        injected={};
        injected.component=controlled;
        injected.world=controlled;
        injected.effectiveRange=50000.f;
        injected.sample.tick=GetTickCount64()+1000;
        injected.sample.count=1;
        injected.sample.converted=1;
        injected.sample.rayLength=50000.f;
        injected.sample.candidates[0].kind=1;
        injected.sample.candidates[0].material=0x5FFFFFFFu;
        const uint32_t tag=injected.sample.candidates[0].material;
        std::memcpy(entry.bytes.data()+0x48,&tag,sizeof(tag));
        std::memcpy(injected.sample.from,endpoints,12);
        std::memcpy(injected.sample.to,endpoints+3,12);
        std::memcpy(injected.searchQuery.data()+0x28,endpoints,sizeof(endpoints));
        g_lastRed={};
        g_surfaceHistory={};
        g_nextSurfaceProbe=0;
        surfaceQueries=0;
        TlsSetValue(g_traceTls,&injected);
        auto before=injected;
        if (window<13) {
            ProbeNearbySurface(injected,&entry);
            if (injected.sample.recovery!=Recovery::NearCameraRay ||
                injected.sample.probeOffered || surfaceQueries!=1) {
                TlsSetValue(g_traceTls,nullptr);
                return false;
            }
            RecordSurfaceResult(injected,false);
            before.sample.tick+=45;
            injected=before;
            surfaceQueries=0;
        }
        ProbeNearbySurface(injected,&entry);
        if (window>=13) {
            TlsSetValue(g_traceTls,nullptr);
            const bool found=window==13;
            return surfaceQueries==1 && injected.sample.probeOffered==found &&
                (found ? entry.HasBody() && entry.Position()==Point{-9940.f,20.f,30.f} :
                         !entry.HasBody());
        }
        if (window) {
            const bool fullMiss=injected.sample.probeTried && !injected.sample.retryHits &&
                !injected.sample.probeOffered;
            RecordSurfaceResult(injected,false);
            injected=before;
            injected.sample.tick+=45;
            if (window==4) injected.sample.to[2]+=4.f;
            ProbeNearbySurface(injected,&entry);
            if (window>=5) {
                const bool markerOnly=injected.sample.retryHits==1 &&
                    injected.sample.retryGridPrefix==1 && !injected.sample.probeOffered;
                RecordSurfaceResult(injected,false);
                injected=before;
                injected.sample.tick+=90;
                ProbeNearbySurface(injected,&entry);
                if (window>=9) {
                    const bool noOffAxisFallback=injected.sample.retryHits==1 &&
                        injected.sample.retryGridPrefix==1 && !injected.sample.probeOffered;
                    RecordSurfaceResult(injected,false);
                    injected=before;
                    injected.sample.tick+=135;
                    ProbeNearbySurface(injected,&entry);
                    TlsSetValue(g_traceTls,nullptr);
                    return fullMiss && markerOnly && noOffAxisFallback && surfaceQueries==4 &&
                        injected.sample.recovery==Recovery::ContinuedRay &&
                        !injected.sample.probeOffered && !entry.HasBody();
                }
                TlsSetValue(g_traceTls,nullptr);
                const bool recovered=injected.sample.probeOffered &&
                    injected.sample.offeredRayDistance==425.f && entry.Position()==Point{-9575.f,20.f,30.f};
                const bool rejected=!injected.sample.probeOffered && !entry.HasBody();
                return fullMiss && markerOnly && surfaceQueries==3 &&
                    injected.sample.retryHits==(window==6?3u:2u) && (window==5?recovered:rejected);
            }
            TlsSetValue(g_traceTls,nullptr);
            const bool recovered=injected.sample.probeOffered && injected.sample.offeredRayDistance==510.f &&
                entry.Position()==Point{-9490.f,20.f,30.f};
            const bool rejected=!injected.sample.probeOffered && !entry.HasBody() && injected.sample.retryHits==2;
            return fullMiss && surfaceQueries==2 && (window==3?rejected:recovered) &&
                injected.sample.retryGridPrefix==(window==2?1u:0u);
        }
        TlsSetValue(g_traceTls,nullptr);
        float fraction=0.f;
        std::memcpy(&fraction,entry.bytes.data(),sizeof(fraction));
        const bool canOffer=!blocker || rawKind==1;
        const auto expected=blocker?Point{-6170.f,20.f,30.f}:Point{-6200.f,20.f,30.f};
        return fixtureContinuedQuery && surfaceQueries==1 && injected.sample.retryHits==2 &&
            injected.sample.retryGridPrefix==size_t(blocker && rawKind==1) &&
            injected.sample.retryHeadBody==!blocker &&
            injected.sample.probeOffered==canOffer && entry.HasBody()==canOffer &&
            (!canOffer || (entry.Position()==expected &&
                           fraction==axiom::surface::Distance(Point{-10000.f,20.f,30.f},expected)/50000.f));
    };
    check(continuedRay(false),"fresh aim continues on the exact ray and normalizes a distant native collision");
    check(continuedRay(true),"continuation stops at a directional record without verified raw kind");
    check(continuedRay(true,1),
          "continuation recovers its first body after the native converter proves the next grid contact");
    check(continuedRay(true,2),
          "a non-grid raw kind stops recovery even with the same packed directional tag");
    check(continuedRay(false,0,1),
          "an empty post-grid continuation falls back to the original camera origin");
    check(continuedRay(false,0,2),
          "the camera retry can traverse a repeated, verified grid marker");
    check(continuedRay(false,0,3),
          "the camera retry respects the nearer physical blocker");
    check(continuedRay(false,0,4),
          "slight camera drift does not suppress a freshly requested camera retry");
    check(continuedRay(false,0,5),"ground-level all-hits retry recovers a real 425-unit body on the crosshair");
    check(continuedRay(false,0,6),"all-hits retry preserves the first unresolved foreground blocker");
    check(continuedRay(false,0,7),"all-hits retry rejects a body away from the original crosshair");
    check(continuedRay(false,0,8),"all-hits retry never promotes a bodyless point");
    check(continuedRay(false,0,9),"grid-only centered rays do not switch to an off-center tower probe");
    check(continuedRay(false,0,13),"short centered query chooses the nearest real body before a farther one");
    check(continuedRay(false,0,14),"short centered query never skips an unresolved foreground hit");
    fixtureWindow=0;
    const Point aimOrigin{-10000.f,20.f,30.f}, aimEnd{40000.f,20.f,30.f};
    SurfaceProbeHistory previousAim{controlled,controlled,aimOrigin,aimEnd,grid};
    previousAim.tick=injected.sample.tick;
    check(SameAim(previousAim,injected,aimOrigin,aimEnd,grid) &&
          !SameAim(previousAim,injected,aimOrigin,Point{40000.f,20.f,35.f},grid) &&
          !SameAim(previousAim,injected,aimOrigin,aimEnd,Point{-9856.f,20.f,30.2f}),
          "preferred probes and native rejections are scoped to the current crosshair ray");
    DrainTargetTrace();
    check(!g_attachmentCount && !g_castCount,"worker drains successful tether and cast records");
    g_testSurfaceQuery=nullptr;
    g_testSurfaceCommit=nullptr;
    g_testCommitAnchor=nullptr;
    g_testCastDispatch=nullptr;
    TlsFree(g_commitTls);
    g_commitTls=TLS_OUT_OF_INDEXES;
    g_searchReady=false;
    g_traceUnlimited=false;
    g_initialized=false;
    g_testTraceTarget=nullptr;
    g_testTraceUpdate=nullptr;
    g_testTraceSweep=nullptr;
    g_testTraceReset=nullptr;
    g_testTraceDispatch=nullptr;
    g_testCandidateConvert=nullptr;
    TlsFree(g_traceTls);
    g_traceTls=TLS_OUT_OF_INDEXES;
    VirtualFree(reinterpret_cast<void*>(traceContext),0,MEM_RELEASE);
    VirtualFree(reinterpret_cast<void*>(traceRoot),0,MEM_RELEASE);
    const auto distance=g.game+kDistance, status=g.menu+g.profile->status;
    const auto angular=g.game+0x716C738, originalRange=g.game+0x716B7E8;
    const auto native=axiom::kNativeDistance;
    Set(distance,native);
    Set(angular,axiom::FloatBits(180.f));
    Set(originalRange,axiom::FloatBits(256.f));
    fixtureSupport=g.game+0x7100000;
    const auto checkOperand=[&](uintptr_t instruction,uintptr_t scalar) {
        const auto end=g.game+instruction+8;
        return end+At<int32_t>(g.game+instruction+4)==scalar;
    };
    const auto putBytes=[&](uintptr_t rva,const auto& bytes) {
        return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(g.game+rva),
                                  bytes.data(),bytes.size(),nullptr)!=0;
    };
    const std::array<uint8_t,8> verification{0xC5,0x7A,0x10,0x05,0x31,0xFA,0xAE,0x06};
    const std::array<uint8_t,8> query{0xC5,0xFA,0x10,0x35,0xB6,0x51,0xDD,0x05};
    const std::array<uint8_t,16> verificationUse{
        0xC5,0xBA,0x58,0x05,0x14,0x30,0xB4,0x05,
        0xC5,0xFA,0x11,0x85,0xB0,0x00,0x00,0x00};
    const std::array<uint8_t,40> queryUse{
        0xC5,0xCA,0x59,0x40,0x08,0xC4,0xC1,0x7A,0x58,0x7D,0x08,
        0xC5,0xCA,0x59,0x40,0x04,0xC4,0x41,0x7A,0x58,0x45,0x04,
        0xC5,0xCA,0x59,0x00,0xC4,0xC1,0x7A,0x58,0x75,0x00,
        0xC5,0xFA,0x11,0xB5,0xF4,0x00,0x00,0x00};
    if (!putBytes(0x67BD5F,verification) || !putBytes(0x139662A,query) ||
        !putBytes(0x67BF48,verificationUse) || !putBytes(0x1396783,queryUse)) return 2;
    const std::array<uint8_t,24> pullClampA{
        0xC5,0xFA,0x5D,0x0D,0xAD,0xDA,0xDD,0x05,0xC5,0xFA,0x10,0x05,0xA9,0x10,0xE3,0x04,
        0xC5,0xFA,0x5F,0xF1,0xC5,0xFA,0x10,0x4F};
    const std::array<uint8_t,24> pullClampB{
        0xC5,0xFA,0x5D,0x0D,0xBD,0x97,0xDD,0x05,0xC5,0xFA,0x10,0x05,0xB9,0xCD,0xE2,0x04,
        0xC5,0xFA,0x5F,0xF1,0xC5,0xFA,0x10,0x4B};
    if (!putBytes(0x138DCE3,pullClampA) || !putBytes(0x1391FD3,pullClampB)) return 2;
    const auto nativeOperands=[&]{
        return checkOperand(0x138DCE3,g.game+0x716B798) && checkOperand(0x1391FD3,g.game+0x716B798) &&
               checkOperand(0x67BD5F,g.game+0x716B798) &&
               checkOperand(0x139662A,originalRange);
    };
    const auto menuOperands=[&]{
        return checkOperand(0x138DCE3,fixtureSupport+0x1000) && checkOperand(0x1391FD3,fixtureSupport+0x1000) &&
               checkOperand(0x67BD5F,fixtureSupport+0x1000) &&
               checkOperand(0x139662A,fixtureSupport+0x1000);
    };
    check(nativeOperands(),"initial references resolve to distinct native scalars");
    {
        axiom::CatchRange candidate;
        Set(g.game+0x139662E,int32_t{1});
        check(!candidate.Update(g.game,fixtureSupport,Read) && candidate.Faulted() &&
              checkOperand(0x67BD5F,g.game+0x716B798) && At<int32_t>(g.game+0x139662E)==1,
              "foreign initial owner refuses both writes");
        putBytes(0x139662A,query);
    }
    {
        axiom::CatchRange candidate;
        Set(g.game+0x1396783,uint8_t{0x90});
        check(!candidate.Update(g.game,fixtureSupport,Read) && candidate.Faulted() &&
              nativeOperands(),"changed endpoint witness refuses both writes");
        putBytes(0x1396783,queryUse);
    }
    {
        axiom::CatchRange candidate;
        check(candidate.Update(g.game,fixtureSupport,Read) && menuOperands(),
              "qualified install moves the complete group");
        Set(g.game+0x139662E,int32_t{1});
        check(!candidate.Update(g.game,fixtureSupport,Read) && candidate.Faulted() &&
              candidate.Restore(g.game,Read) && checkOperand(0x67BD5F,g.game+0x716B798) &&
              At<int32_t>(g.game+0x139662E)==1,"owned restore leaves foreign replacement intact");
        putBytes(0x139662A,query);
    }
    {
        axiom::CatchRange candidate;
        check(!candidate.Update(g.game,g.game+0x80000000ULL+0x1397000,Read) &&
              candidate.Faulted() && nativeOperands(),"unreachable support leaves both sites native");
    }
    Set(object,g.menu+g.profile->vtable);
    g_testSafReady=FixtureSaf;
#ifndef AXIOM_BASELINE_TEST
    g.safUnlimited=&unlimitedRequest;
#endif
    g_testGameWitnesses=FixtureGame;
    g_testLoadMenuSettings=FixtureReadSettings;
    g_testPublishMenuSettings=FixturePublishSettings;
    g_testMenuEnabled=FixtureEnabled;
    check(BeforeAxiomInit(g.menu),"cold module can acquire the startup lease");
    Set(g.menu+g.profile->menuStage,uint32_t{5});
    check(!BeforeAxiomInit(g.menu),"registry startup transition refuses late patch");
    Set(g.menu+g.profile->menuStage,uint32_t{0});
    Set(g.menu+g.profile->oldProbe,uint8_t{1});
    check(!BeforeAxiomInit(g.menu),"past Menu probe refuses late patch");
    Set(g.menu+g.profile->oldProbe,uint8_t{0});
    Set(g.menu+g.profile->registryCount,uint32_t{1});
    Set(g.menu+g.profile->registry,object);
    Set(object+8,uint8_t{1});
    check(!BeforeAxiomInit(g.menu),"already initialized exact object refuses late patch");
    Set(object+8,uint8_t{0});
    check(BeforeAxiomInit(g.menu),"registered untouched exact object still qualifies");

    g.stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!g.stop) return 2;
    std::array<quiesced_patch::Patch,3> patches{
        Detour(shim,HookInit,g.profile->init),Detour(shim+0x40,HookStart,g.profile->start),
        Detour(shim+0x80,HookShutdown,g.profile->shutdown)
    };
    for (const auto& patch:patches) memcpy(patch.address,patch.before,patch.size);
    const uint8_t zero=0;
    const quiesced_patch::Check requireCold{
        reinterpret_cast<const uint8_t*>(g.menu+g.profile->oldProbe),&zero,1};
    Set(g.menu+g.profile->oldProbe,uint8_t{1});
    quiesced_patch::Report report{};
    check(!quiesced_patch::Write(patches.data(),patches.size(),{},&report,&requireCold,1) &&
          report.unchanged && report.allResumed &&
          !memcmp(patches[0].address,patches[0].before,patches[0].size),
          "late race preserves all original fixture bytes");
    Set(g.menu+g.profile->oldProbe,uint8_t{0});
    check(quiesced_patch::Write(patches.data(),patches.size(),{},&report,&requireCold,1) &&
          report.success && report.allResumed,"all three exact fixture detours publish together");
    if (!report.success) return 2;

    auto init=Shim<bool(*)(void*)>(patches[0]);
    auto start=Shim<bool(*)(void*)>(patches[1]);
    auto shutdown=Shim<void(*)(void*)>(patches[2]);
    check(!init(reinterpret_cast<void*>(object)) && !start(nullptr) &&
          !settingReads && !g.worker,"unpublished hooks refuse before any settings or writes");
    g_installed.store(true);
    check(!init(nullptr) && !g.worker && !settingReads,"null Menu object refuses");
    check(!init(reinterpret_cast<void*>(object+8)) && !g.worker,"wrong Menu object refuses");
    check(init(reinterpret_cast<void*>(object)) && start(nullptr),"exact Menu object initializes a single worker");
    check(settingReads==1 && settingPublishes==1 && At<float>(object+0x1C)==seedRange &&
          At<float>(object+0x20)==seedPull && At<uint8_t>(object+0x24)==1 &&
          At<uint32_t>(g.menu+g.profile->enabled)==1 && At<uint32_t>(object+0xC)==1 &&
          At<uint32_t>(object+0x18)==2 && At<uint32_t>(status)==1 && At<uint8_t>(g.menu+g.profile->backendStarted)==1,
          "stock settings and requested enabled state load with explicit pending status");
    check(init(reinterpret_cast<void*>(object)) && settingReads==1 && settingPublishes==1,
          "duplicate initializer avoids reloading or spawning another worker");
    Sleep(310);
    check(At<uint32_t>(distance)==native,"pending SAF leaves game scalar unchanged");
    Set(distance,axiom::FloatBits(9.f));
    testState.store(SafState::Ready);
    Sleep(310);
    check(At<float>(distance)==9.f && At<uint32_t>(status)==1,
          "unknown initial game scalar cannot receive a write or advertise active");
    Set(distance,native);
    check(Eventually([&]{return At<float>(distance)==seedRange && At<uint32_t>(status)==2;}),
          "verified SAF allows Menu range on fallback scalar only");
    check(At<float>(angular)==180.f && At<float>(originalRange)==256.f &&
          At<float>(object+0x20)==seedPull && At<uint8_t>(object+0x24)==1,
          "angular, SAF native settings and Menu persisted fields remain untouched");
    check(Eventually([&]{return menuOperands() && InterlockedCompareExchange(&unlimitedRequest,0,0)==1;}),
          "the dynamic ray also requests SAF ownership when legacy Menu Unlimited is off");
    testState.store(SafState::Busy);
    Sleep(340);
    check(At<float>(distance)==seedRange,"busy SAF does not create restore churn");
    testState.store(SafState::Missing);
    check(Eventually([&]{return At<uint32_t>(distance)==native && At<uint32_t>(status)==1;}),
          "lost SAF lease restores current owned value and reports pending");
    testState.store(SafState::Ready);
    check(Eventually([&]{return At<float>(distance)==seedRange;}),"restored exact lease resumes requested settings");
    Set(g.menu+g.profile->range,axiom::FloatBits(NAN));
    check(Eventually([&]{return At<uint32_t>(distance)==native && At<uint32_t>(status)==1;}),
          "malformed settings restore the scope and cannot advertise active");
    Set(g.menu+g.profile->range,seedRange);
    Set(g.menu+g.profile->unlimited,uint32_t{1});
    check(Eventually([&]{return At<float>(distance)==50000.f &&
                                 InterlockedCompareExchange(&unlimitedRequest,0,0)==1 &&
                                 At<uint32_t>(status)==1;}),
          "unlimited publishes to SAF and reports pending before SAF applies");
    effectiveSafRange.store(50000.f);
    check(Eventually(menuOperands),"unlimited redirects the verified Remote Catch inputs");
    check(Eventually([&]{return At<uint32_t>(status)==2;}),
          "unlimited reports active after SAF publishes effective reach");
    Set(g.menu+g.profile->unlimited,uint32_t{0});
    check(Eventually([&]{return menuOperands() && At<float>(distance)==seedRange &&
                                 InterlockedCompareExchange(&unlimitedRequest,0,0)==1 && !g_traceUnlimited.load();}),
          "unlimited off keeps dynamic SAF targeting while restoring Menu's finite fallback");
    Set(g.menu+g.profile->unlimited,uint32_t{1});
    check(Eventually([&]{return menuOperands() && InterlockedCompareExchange(&unlimitedRequest,0,0)==1;}),
          "unlimited can take priority again with confirmation inputs still bound");
    testState.store(SafState::Busy);
    Sleep(310);
    check(menuOperands() && InterlockedCompareExchange(&unlimitedRequest,0,0)==1,
          "busy provider does not remove active supplemental range");
    testState.store(SafState::Missing);
    check(Eventually([&]{return nativeOperands() && At<uint32_t>(distance)==native &&
                                 InterlockedCompareExchange(&unlimitedRequest,0,0)==0;}),
          "provider loss restores both inputs and the native fallback scalar");
    testState.store(SafState::Ready);
    check(Eventually(menuOperands),"resumed provider reacquires both inputs");
    Set(g.menu+g.profile->enabled,uint32_t{0});
    check(Eventually([&]{return menuOperands() && At<uint32_t>(distance)==native && At<uint32_t>(status)==0 &&
                                 At<uint8_t>(g.menu+g.profile->backendStarted)==0 &&
                                 InterlockedCompareExchange(&unlimitedRequest,0,0)==1 && !g_traceUnlimited.load();}),
          "Menu off restores its fallback radius and dynamic SAF targeting remains active");
    Set(g.menu+g.profile->unlimited,uint32_t{0});
    Set(g.menu+g.profile->enabled,uint32_t{1});
    check(Eventually([&]{return At<float>(distance)==seedRange && At<uint32_t>(status)==2 &&
                                 At<uint8_t>(g.menu+g.profile->backendStarted)==1;}),
          "explicit enable reacquires the same one scalar and reports active");
    Set(g.menu+g.profile->unlimited,uint32_t{1});
    check(Eventually([&]{return menuOperands() && InterlockedCompareExchange(&unlimitedRequest,0,0)==1;}),
          "unlimited can be requested again before shutdown");
    shutdown(reinterpret_cast<void*>(object));
    check(nativeOperands() && At<uint32_t>(distance)==native && !g.worker && !g.stop &&
          At<uint32_t>(status)==0 && !start(nullptr) &&
          InterlockedCompareExchange(&unlimitedRequest,0,0)==0,
          "actual shutdown joins, restores and refuses restart");
    shutdown(nullptr);
    check(!g.worker && !g.stop && At<float>(angular)==180.f &&
          At<float>(originalRange)==256.f,"repeated shutdown remains inert and preserves independent scalars");

    const auto* oldProfile=g.profile;
    const auto menuFixture=g.menu;
    g.profile=nullptr;
    g.safUnlimited=nullptr;
    Set(distance,axiom::FloatBits(9.f));
    const auto menuEnabled=menuFixture+oldProfile->enabled;
    const auto menuUnlimited=menuFixture+oldProfile->unlimited;
    Set(menuEnabled,uint32_t{1});
    Set(menuUnlimited,uint32_t{1});
    Set(status,uint32_t{77});
    Set(menuFixture+oldProfile->backendStarted,uint8_t{17});
    std::atomic<unsigned> traceInstalls{0};
    g_testTraceInstallCount=&traceInstalls;
    g_testTraceInstall=[](unsigned attempt) {
        return attempt==1 ? TraceInstall::Retry : TraceInstall::Installed;
    };
    const auto startStandaloneFixture=[](HMODULE optionalMenu) {
#ifdef AXIOM_BASELINE_OPTIONAL_TEST
        if (!optionalMenu || !StandaloneMenuImage(reinterpret_cast<uintptr_t>(optionalMenu))) return false;
        g.stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        return g.stop && StartStandalone();
#else
        bool retainMenu=false;
        return StartSafBackend(optionalMenu,retainMenu) && !retainMenu;
#endif
    };
    IMAGE_DOS_HEADER forgedDos{};
    forgedDos.e_magic=IMAGE_DOS_SIGNATURE;
    forgedDos.e_lfanew=sizeof(forgedDos);
    IMAGE_NT_HEADERS64 forgedNt{};
    forgedNt.Signature=IMAGE_NT_SIGNATURE;
    forgedNt.FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
    forgedNt.FileHeader.TimeDateStamp=oldProfile->stamp;
    forgedNt.OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    forgedNt.OptionalHeader.SizeOfImage=oldProfile->imageSize;
    if (!Set(menuFixture,forgedDos) || !Set(menuFixture+sizeof(forgedDos),forgedNt)) return 2;
    check(ExactMenu(menuFixture)==oldProfile,"a forged provider can match only the cheap legacy PE identity");
    const struct {
        HMODULE menu;
        const char* started;
    } scenarios[]={
        {nullptr,"SAF-only backend starts without DLMM"},
        {static_cast<HMODULE>(menu604Image),"SAF-only backend starts with an unrecognized Menu and ignores its private worker latch"},
        {reinterpret_cast<HMODULE>(menuFixture),"SAF-only backend starts when a legacy-shaped Menu cannot authenticate"},
    };
    for (const auto& scenario:scenarios) {
        g.safUnlimited=&unlimitedRequest;
        g_stopping=false;
        g_initialized=false;
        traceInstalls.store(0);
        testState.store(SafState::Missing);
        effectiveSafRange.store(150.f);
        const bool started=startStandaloneFixture(scenario.menu);
        check(started && !g.menu && !g.profile,scenario.started);
        if (!started) continue;
        Sleep(310);
        check(!g_searchReady.load() && nativeOperands() && !traceInstalls.load() && !g_initialized.load(),
              "SAF-only worker waits for the producer before attempting trace hooks");
        testState.store(SafState::Ready);
        check(Eventually([&]{return InterlockedCompareExchange(&unlimitedRequest,0,0)==1 &&
                             !g_searchReady.load() && menuOperands();}),
              "SAF-only requests native expansion while the producer still reports finite INI reach");
        effectiveSafRange.store(50000.f);
        check(Eventually([&]{return g_searchReady.load() && CurrentEffectiveRange()==50000.f && menuOperands();}),
              "expanded surface search starts after the producer acknowledges the long ray");
        check(traceInstalls.load()==2 && g_initialized.load(),
              "recoverable installer refusal retries once after SAF owns its patches");
        check(!g_traceUnlimited.load() && At<float>(distance)==9.f &&
              At<uint32_t>(menuEnabled)==1 && At<uint32_t>(menuUnlimited)==1 &&
              At<uint8_t>(menuFixture+oldProfile->backendStarted)==17 && At<uint32_t>(status)==77 &&
              At<uint32_t>(menu604Base+menu604WorkerFlag)==1 &&
              InterlockedCompareExchange(&unlimitedRequest,0,0)==1,
              "SAF-only expansion preserves both Menu fixtures and fallback data");
        effectiveSafRange.store(65000.f);
        check(Eventually([&]{return g_searchReady.load() && CurrentEffectiveRange()==65000.f && menuOperands();}),
              "a longer verified SAF ray remains available");
        testState.store(SafState::Missing);
        check(Eventually([&]{return !g_searchReady.load() && !CurrentEffectiveRange() && nativeOperands();}),
              "SAF-only ownership loss disables search and restores the catch operands");
        testState.store(SafState::Ready);
        check(Eventually([&]{return g_searchReady.load() && menuOperands();}),
              "SAF-only worker resumes once SAF owns its range sites again");
        check(traceInstalls.load()==2,"ownership changes do not reinstall an already published trace");
        HookShutdown(nullptr);
        check(!g.worker && !g.stop && !g_searchReady.load() && !CurrentEffectiveRange() &&
              nativeOperands() && At<float>(distance)==9.f && At<uint32_t>(status)==77 &&
              InterlockedCompareExchange(&unlimitedRequest,0,0)==0,
              "SAF-only shutdown restores game code and releases its native reach request");
    }
    UnmapViewOfFile(menu604Image);
    CloseHandle(menu604Section);
    CloseHandle(menu604File);
    g.profile=oldProfile;
    std::printf("%s Axiom lifecycle %u checks, %u failures, authored code only\n",g.profile->name,checks,failed);
    return failed?1:0;
}

