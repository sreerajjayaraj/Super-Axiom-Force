
#define AXIOM_TESTING 1
#include "../src/AxiomCoexist.cpp"
#include <filesystem>
#include <fstream>
#include <vector>

void FixtureObstacleQuaternion(void* output,const void*,const void*) {
    constexpr float value=0.25f;
    std::memcpy(output,&value,sizeof(value));
    SetLastError(227);
}

unsigned radialSteps=0,releaseCalls=0;
bool releaseAbi=true;
void FixtureRadialStep(void*,float) { ++radialSteps; }
void FixtureTetherRelease(void* component,uint8_t lane,uint8_t reason,uint8_t immediate,bool notify) {
    ++releaseCalls;
    releaseAbi=releaseAbi && lane==0 && reason==2 && immediate==0 && notify;
    static_cast<uint8_t*>(component)[0xA4C]=6;
    std::memset(static_cast<uint8_t*>(component)+0xD78,0,4);
}

bool TestStalledTether(uint8_t* image,void* rootContext) {
    int32_t relative=0;
    std::memcpy(&relative,image+kStallRadialCall+1,sizeof(relative));
    const auto invoke=reinterpret_cast<NativeRadialStep>(g.game+kStallRadialCall+5+relative);
    std::array<uint8_t,0xA8> actors[3]{};
    std::array<uint8_t,0x80> parts{};
    std::array<uint8_t,0xE00> component{};
    std::array<uint8_t,0xC0> state{};
    const auto put=[](void* data,size_t offset,const auto value) {
        std::memcpy(static_cast<uint8_t*>(data)+offset,&value,sizeof(value));
    };
    const auto part=reinterpret_cast<uintptr_t>(parts.data());
    const auto catchAddress=reinterpret_cast<uintptr_t>(component.data());
    put(component.data(),0,g.game+0x5994CC8);
    put(parts.data(),0x78,catchAddress);
    g_testRadialStep=FixtureRadialStep;
    g_testTetherRelease=FixtureTetherRelease;
    g_testStallTick=1000;
    const auto selectActor=[&](size_t index) {
        const auto player=reinterpret_cast<uintptr_t>(actors[index].data());
        put(rootContext,0x58,player);
        g_obstacleOwner.store(player);
        put(actors[index].data(),0x68,part);
        put(actors[index].data(),0x94,uint8_t{1});
        put(actors[index].data(),0xA0,player);
        put(component.data(),8,player);
        put(state.data(),0x20,player);
    };
    const auto arm=[&](bool known=true,uint8_t lane=0) {
        put(component.data(),0x168,uint8_t{1});
        put(component.data(),0xA4C,uint8_t{3});
        put(component.data(),0xD78,uint32_t{1});
        put(state.data(),0x28,uint16_t{0x4000});
        put(state.data(),0x3C,uint16_t{0});
        put(state.data(),0x50,axiom::surface::Point{3.f,0.f,0.f});
        put(state.data(),0x5C,axiom::surface::Point{});
        RememberStallAttachment(catchAddress,lane,1,known,{33.f,0.f,0.f});
        g_testStallTick+=1000;
    };
    const auto step=[&](unsigned ticks,float movement=0.f) {
        for (unsigned i=0;i<ticks;++i) {
            put(state.data(),0x50,axiom::surface::Point{3.f,static_cast<float>(i)*movement,0.f});
            invoke(state.data(),0.02f);
            g_testStallTick+=20;
        }
    };
    const auto request=[&]{ReleaseStalledTether(catchAddress,0.02f);};
    selectActor(0);
    put(rootContext,0x50,reinterpret_cast<uintptr_t>(actors[0].data()));
    g_stopping.store(false);
    bool switched=ControlledCatch(catchAddress);
    bool clean=UpdateTargetControl(catchAddress).actor!=0;
    for (size_t i=1;i<3;++i) {
        g_publishedRecovery.component=component.data();
        g_lastRed.component=component.data();
        g_dispatchedTethers[0].component=component.data();
        arm();
        g_stall.pending=true;
        put(rootContext,0x58,reinterpret_cast<uintptr_t>(actors[i].data()));
        clean=clean && !UpdateTargetControl(catchAddress).actor;
        selectActor(i);
        switched=switched && ControlledCatch(catchAddress);
        clean=clean && UpdateTargetControl(catchAddress).actor &&
            !g_publishedRecovery.component && !g_lastRed.component &&
            !g_dispatchedTethers[0].component && !g_stall.pending;
    }
    put(actors[2].data(),0x94,uint8_t{0});
    switched=switched && !ControlledCatch(catchAddress);
    put(actors[2].data(),0x94,uint8_t{1});
    put(rootContext,0x58,uintptr_t{0});
    switched=switched && !ControlledCatch(catchAddress);
    RefreshTargetControl(true);
    clean=clean && !g_targetControl.actor && !g_obstacleOwner.load();
    selectActor(0);
    switched=switched && ControlledCatch(catchAddress);
    printf("%s active catch follows Kliff, Damiane, Oongka and Kliff while the legacy actor pointer stays fixed\n",
        switched?"PASS":"FAIL");
    printf("%s switch clears recovered aim and stalled pull and refuses the previous control token\n",
        clean?"PASS":"FAIL");
    bool guards=true;
    arm();
    step(31);
    request();
    guards=guards && releaseCalls==0;
    arm();
    put(state.data(),0x28,uint16_t{0});
    step(40);
    request();
    guards=guards && releaseCalls==0;
    arm();
    put(component.data(),0xD78,uint32_t{0});
    step(40);
    request();
    guards=guards && releaseCalls==0;
    arm();
    step(75,0.4f);
    request();
    guards=guards && releaseCalls==0;
    arm(false);
    step(40);
    request();
    guards=guards && releaseCalls==0;
    arm();
    step(40);
    selectActor(1);
    request();
    guards=guards && releaseCalls==0;
    arm();
    step(40);
    put(component.data(),0xA4C,uint8_t{6});
    request();
    guards=guards && releaseCalls==0;
    arm();
    step(40);
    g_testStallTick+=500;
    request();
    guards=guards && releaseCalls==0;
    printf("%s idle, clear, advancing, switched and stale tethers are retained\n",guards?"PASS":"FAIL");
    bool releases=releaseAbi;
    for (size_t i=0;i<3;++i) {
        selectActor(i);
        arm();
        step(40);
        request();
        request();
        releases=releases && releaseCalls==i+1 && component[0xA4C]==6;
    }
    releases=releases && releaseAbi && radialSteps>=300 &&
        g_tetherReleaseEvent.before==3 && g_tetherReleaseEvent.after==6 &&
        g_tetherReleaseEvent.stalledSeconds>=axiom::stall::kReleaseDelaySeconds;
    DrainStalledTether();
    ClearStalledTether();
    g_obstacleOwner.store(0);
    g_testRadialStep=nullptr;
    g_testTetherRelease=nullptr;
    g_testStallTick=0;
    printf("%s stalled obstruction releases once through native detach for three controlled owners\n",
        releases?"PASS":"FAIL");
    return switched && clean && guards && releases;
}

int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    std::ifstream file(std::filesystem::path(argv[1]),std::ios::binary|std::ios::ate);
    const auto length=file.tellg();
    if (length<4096 || length>0x24000000) return 2;
    std::vector<uint8_t> raw(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(raw.data()),length)) return 2;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<64 || dos->e_lfanew>0x1000) return 2;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data()+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage!=0x144E9000) return 2;
    // No executable loading, TLS, imports or process entry point is run.
    // A private pagefile mapping exposes real executable copy-on-write page
    // protection without loading or running the game executable.
    const HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_EXECUTE_READWRITE,
                                            0,nt->OptionalHeader.SizeOfImage,nullptr);
    if (!mapping) return 2;
    auto* image=static_cast<uint8_t*>(MapViewOfFile(mapping,FILE_MAP_WRITE|FILE_MAP_EXECUTE,0,0,0));
    CloseHandle(mapping);
    if (!image) return 2;
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for (size_t i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& section=sections[i];
        if (!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t count=(std::min)(section.Misc.VirtualSize,section.SizeOfRawData);
        if (section.PointerToRawData>raw.size() || count>raw.size()-section.PointerToRawData ||
            section.VirtualAddress>nt->OptionalHeader.SizeOfImage ||
            count>nt->OptionalHeader.SizeOfImage-section.VirtualAddress) return 2;
        auto* const base=image+section.VirtualAddress;
        std::memcpy(base,raw.data()+section.PointerToRawData,count);
        DWORD previous=0;
        if (!VirtualProtect(base,count,PAGE_EXECUTE_READ,&previous)) return 2;
    }
    g.game=reinterpret_cast<uintptr_t>(image);
    g_testTraceMemoryType=MEM_MAPPED;
    g_log=GetStdHandle(STD_OUTPUT_HANDLE);
    auto* const first=image+kUpdate;
    DWORD original=0,ignored=0;
    if (!VirtualProtect(first,1,PAGE_EXECUTE_READWRITE,&original)) return 2;
    const auto initial=InstallTargetTrace();
    const auto untouchedAfterBusy=image[kResetDecision]==kResetDecisionBytes[0];
    printf("%s writer-open page is retryable and untouched\n",
           initial==TraceInstall::Retry && untouchedAfterBusy?"PASS":"FAIL");
    const uint8_t expected=*first;
    *first^=1;
    if (!VirtualProtect(first,1,original,&ignored)) return 2;
    const auto foreign=InstallTargetTrace();
    printf("%s differing native bytes refuse without overwriting\n",
           foreign==TraceInstall::Refused && *first!=expected &&
           image[kResetDecision]==kResetDecisionBytes[0]?"PASS":"FAIL");
    if (!VirtualProtect(first,1,PAGE_EXECUTE_READWRITE,&ignored)) return 2;
    *first=expected;
    if (!VirtualProtect(first,1,original,&ignored)) return 2;
    constexpr uintptr_t allocatorWitness=0x8AB5509;
    auto* const cow=image+allocatorWitness;
    if (!VirtualProtect(cow,1,PAGE_EXECUTE_READWRITE,&ignored)) return 2;
    const uint8_t originalCow=*cow;
    *cow^=1;
    if (!VirtualProtect(cow,1,PAGE_EXECUTE_WRITECOPY,&ignored)) return 2;
    MEMORY_BASIC_INFORMATION page{};
    if (!VirtualQuery(cow,&page,sizeof(page)) || page.Protect!=PAGE_EXECUTE_WRITECOPY) return 2;
    const auto changedCow=InstallTargetTrace();
    const bool refusedCow=changedCow==TraceInstall::Refused && *cow!=originalCow &&
                          image[kResetDecision]==kResetDecisionBytes[0];
    printf("%s changed allocator witness on executable copy-on-write page refuses\n",
           refusedCow?"PASS":"FAIL");
    if (!VirtualProtect(cow,1,PAGE_EXECUTE_READWRITE,&ignored)) return 2;
    *cow=originalCow;
    if (!VirtualProtect(cow,1,PAGE_EXECUTE_WRITECOPY,&ignored) ||
        !VirtualProtect(image+kResetDecision,1,PAGE_EXECUTE_WRITECOPY,&ignored)) return 2;
    // Simulate an independent owner of Flash Mode's exact operand.
    auto* const flash=image+0x684621;
    DWORD flashProtect=0;
    if (!VirtualProtect(flash,4,PAGE_EXECUTE_READWRITE,&flashProtect)) return 2;
    constexpr std::array<uint8_t,4> menuOperand{0x01,0x02,0x03,0x04};
    std::memcpy(flash,menuOperand.data(),menuOperand.size());
    if (!VirtualProtect(flash,4,flashProtect,&ignored)) return 2;
    const auto& display=kObstaclePathGates[3];
    auto* foreignGate=image+display.rva;
    DWORD displayProtection=0;
    if (!VirtualProtect(foreignGate,1,PAGE_EXECUTE_READWRITE,&displayProtection)) return 2;
    *foreignGate^=1;
    if (!VirtualProtect(foreignGate,1,displayProtection,&ignored)) return 2;
    const bool foreignGateRefused=!InstallObstacleObserver(g.game+kObstacleCall,
        g.game+kObstacleCall+64,MEM_MAPPED);
    bool leftNative=foreignGateRefused &&
        std::memcmp(image+kObstacleCall,kObstacleContext.data()+17,5)==0 &&
        *foreignGate!=display.context[0];
    for (size_t i=0;i<kObstaclePathGates.size()-1;++i) {
        const auto& untouched=kObstaclePathGates[i];
        leftNative=leftNative && std::memcmp(image+untouched.rva,
            untouched.context.data(),untouched.context.size())==0;
    }
    printf("%s an occupied waypoint gate refuses the entire obstacle path transaction\n",
           leftNative?"PASS":"FAIL");
    if (!leftNative) return 1;
    if (!VirtualProtect(foreignGate,1,PAGE_EXECUTE_READWRITE,&ignored)) return 2;
    *foreignGate=display.context[0];
    if (!VirtualProtect(foreignGate,1,displayProtection,&ignored)) return 2;
    const auto clean=InstallTargetTrace();
    const bool collectorVerified=g_allHitsCollectorReady.load() &&
        VerifiedAllHitsCollector(g.game,MEM_MAPPED);
    auto* const collectorSite=image+0x3D36A14;
    if (!VirtualProtect(collectorSite,1,PAGE_EXECUTE_READWRITE,&ignored)) return 2;
    const uint8_t saved=*collectorSite;
    *collectorSite^=1;
    if (!VirtualProtect(collectorSite,1,PAGE_EXECUTE_READ,&ignored)) return 2;
    const bool collectorRefused=!VerifiedAllHitsCollector(g.game,MEM_MAPPED) &&
        *collectorSite!=saved && image[kResetDecision]==0xE9;
    if (!VirtualProtect(collectorSite,1,PAGE_EXECUTE_READWRITE,&ignored)) return 2;
    *collectorSite=saved;
    if (!VirtualProtect(collectorSite,1,PAGE_EXECUTE_READ,&ignored)) return 2;
    printf("%s all-hits collector verifies on the exact game and refuses a changed constructor\n",
           collectorVerified && collectorRefused?"PASS":"FAIL");
    if (!collectorVerified || !collectorRefused) return 1;
    const bool flashUnchanged=clean==TraceInstall::Installed &&
        std::memcmp(flash,menuOperand.data(),menuOperand.size())==0;
    printf("%s bridge installs while leaving independent Flash Mode operand untouched\n",
           flashUnchanged?"PASS":"FAIL");
    const bool installed=image[kResetDecision]==0xE9 && image[kUpdateACall]==0xE8;
    const bool retainedCow=VirtualQuery(cow,&page,sizeof(page)) &&
        page.Protect==PAGE_EXECUTE_WRITECOPY && *cow==originalCow;
    const bool retainedPatchCow=VirtualQuery(image+kResetDecision,&page,sizeof(page)) &&
        page.Protect==PAGE_EXECUTE_WRITECOPY;
    printf("%s production trace transaction on exact executable copy-on-write pages\n",
           clean==TraceInstall::Installed && installed && retainedCow && retainedPatchCow?"PASS":"FAIL");
    int32_t obstacleDisplacement=0;
    std::memcpy(&obstacleDisplacement,image+kObstacleCall+1,sizeof(obstacleDisplacement));
    const auto actualRelay=g.game+kObstacleCall+5+static_cast<int64_t>(obstacleDisplacement);
    const auto observer=axiom::trace::Relay(reinterpret_cast<uintptr_t>(TraceObstacleCorrection));
    const bool obstacleInstalled=clean==TraceInstall::Installed && image[kObstacleCall]==0xE8 &&
        VirtualQuery(reinterpret_cast<const void*>(actualRelay),&page,sizeof(page)) &&
        page.State==MEM_COMMIT && page.Type==MEM_PRIVATE && page.Protect==PAGE_EXECUTE_READ &&
        quiesced_patch::SameBytes(reinterpret_cast<const uint8_t*>(actualRelay),observer.data(),observer.size());
    printf("%s native obstacle correction call has its own verified observer relay\n",
           obstacleInstalled?"PASS":"FAIL");
    std::array<uint8_t,0xC0> state{};
    std::array<uint8_t,0x38> root{};
    std::array<uint8_t,0x60> context{};
    const auto rootValue=reinterpret_cast<uintptr_t>(root.data());
    const auto contextValue=reinterpret_cast<uintptr_t>(context.data());
    const auto actorValue=reinterpret_cast<uintptr_t>(state.data());
    const float distance=12.5f;
    if (!VirtualProtect(image+0x72451C0,sizeof(rootValue),PAGE_READWRITE,&ignored)) return 2;
    std::memcpy(image+0x72451C0,&rootValue,sizeof(rootValue));
    std::memcpy(root.data()+0x30,&contextValue,sizeof(contextValue));
    std::memcpy(context.data()+0x50,&actorValue,sizeof(actorValue));
    std::memcpy(context.data()+0x58,&actorValue,sizeof(actorValue));
    std::memcpy(state.data()+0x20,&actorValue,sizeof(actorValue));
    std::memcpy(state.data()+0xA0,&actorValue,sizeof(actorValue));
    state[0x94]=1;
    std::memcpy(state.data()+0xB4,&distance,sizeof(distance));
    const axiom::surface::Point direction{1.f,0.f,0.f};
    float result=0.f;
    g_testObstacleQuaternion=FixtureObstacleQuaternion;
    g_straightPullOwned.store(false);
    TraceObstacleCorrection(state.data()+0x80,direction.data(),direction.data());
    std::memcpy(&result,state.data()+0x80,sizeof(result));
    const bool inactive=!g_obstacleEvent.tick && result==0.25f;
    g_straightPullOwned.store(true);
    g_obstacleOwner.store(actorValue);
    TraceObstacleCorrection(state.data()+0x80,direction.data(),direction.data());
    const bool event=GetLastError()==227 && g_obstacleEvent.tick && g_obstacleEvent.playerOwner &&
        g_obstacleEvent.direction==direction && g_obstacleEvent.solverDistance==distance;
    DrainTargetTrace();
    const bool obstacleLogged=inactive && event && !g_obstacleEvent.tick;
    printf("%s native obstacle correction records player steering and drains outside the callback\n",
           obstacleLogged?"PASS":"FAIL");
    if (!obstacleLogged) return 1;
    constexpr uintptr_t solverGate=0x13BD1B4;
    const bool scopedSolver=image[solverGate]==0xE8 && image[solverGate+5]==0x84 &&
        image[solverGate+6]==0xC0 && image[solverGate+7]==0x0F && image[solverGate+8]==0x84;
    printf("%s controlled-player gate preserves the native obstacle branch and continuation\n",
           scopedSolver?"PASS":"FAIL");
    if (!scopedSolver) return 1;

    const std::array<uintptr_t,3> waypointGates{0x13b8cb7,0x3c89be,0x69683f};
    bool waypointContinuations=true;
    for (const auto site:waypointGates)
        waypointContinuations=waypointContinuations && image[site]==0xE8 &&
            image[site+5]==0x84 && image[site+6]==0xC0 &&
            image[site+7]==0x0F && image[site+8]==0x84;
    printf("%s complete rope path installed for waypoint basis, length and display\n",
           waypointContinuations?"PASS":"FAIL");
    if (!waypointContinuations) return 1;
    const std::array<uint8_t,5> stockPullCall{0xE8,0x5C,0xF2,0xFF,0xFF};
    const auto* pullCall=image+0x13BD11F;
    const bool stallObservation=pullCall[0]==0xE8 &&
        std::memcmp(pullCall,stockPullCall.data(),stockPullCall.size())!=0;
    printf("%s stalled-pull observer attaches to the exact radial step\n",
           stallObservation?"PASS":"FAIL");
    if (!stallObservation) return 1;

    int32_t gateDisplacement=0;
    std::memcpy(&gateDisplacement,image+solverGate+1,sizeof(gateDisplacement));
    const auto gate=g.game+solverGate+5+static_cast<int64_t>(gateDisplacement);
    // This fixture only executes the SAF leaf and the test harness below.
    // RDI is the nonvolatile game register holding the live pull state.
    std::array<uint8_t,18> invoke{
        0x57,0x48,0x8B,0xF9,0x48,0xB8,0,0,0,0,0,0,0,0,0xFF,0xD0,0x5F,0xC3,
    };
    std::memcpy(invoke.data()+6,&gate,sizeof(gate));
    auto* driver=VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!driver) return 2;
    std::memcpy(driver,invoke.data(),invoke.size());
    if (!VirtualProtect(driver,0x1000,PAGE_EXECUTE_READ,&ignored)) return 2;
    FlushInstructionCache(GetCurrentProcess(),driver,invoke.size());
    auto* nativeFlag=image+kNativeObstacleFlag;
    if (!VirtualProtect(nativeFlag,1,PAGE_READWRITE,&ignored)) return 2;
    *nativeFlag=0;
    using Gate=bool(*)(const void*);
    const auto allowed=reinterpret_cast<Gate>(driver);
    std::array<uint8_t,0xC0> foreignState{};
    g_obstacleOwner.store(0);
    g_obstacleGateEntries.store(0);
    const bool dormant=!allowed(state.data()) && !allowed(foreignState.data());
    g_obstacleOwner.store(actorValue);
    const bool local=allowed(state.data()) && !allowed(foreignState.data()) &&
        g_obstacleGateEntries.load()==1 && *nativeFlag==0;
    *nativeFlag=1;
    const bool stock=allowed(state.data()) && allowed(foreignState.data()) &&
        g_obstacleGateEntries.load()==1 && *nativeFlag==1;
    *nativeFlag=0;
    g_obstacleOwner.store(0);
    const bool disabled=!allowed(state.data());
    // Exercise the real emitted leaves with the registers populated exactly
    // as the three native callers populate them.
    const auto executeGate=[&](size_t i,const void* local,const void* other,
                                void* catchComponent) {
        const auto& descriptor=kObstaclePathGates[i];
        int32_t delta=0;
        std::memcpy(&delta,image+descriptor.rva+1,sizeof(delta));
        const auto destination=g.game+descriptor.rva+5+static_cast<int64_t>(delta);
        const std::array<std::vector<uint8_t>,3> prologs{{
            {0x56,0x53,0x48,0x8B,0xF1,0x48,0x8B,0xDA,0x48,0x83,0xEC,0x28}, // RSI, RBX
            {0x53,0x48,0x8B,0xD9,0x48,0x83,0xEC,0x20},                          // RBX
            {0x41,0x55,0x4C,0x8B,0xE9,0x48,0x83,0xEC,0x20},                     // R13
        }};
        const std::array<std::vector<uint8_t>,3> epilogs{{
            {0x48,0x83,0xC4,0x28,0x5B,0x5E,0xC3},
            {0x48,0x83,0xC4,0x20,0x5B,0xC3},
            {0x48,0x83,0xC4,0x20,0x41,0x5D,0xC3},
        }};
        auto code=prologs[i-1];
        code.insert(code.end(),{0x48,0xB8});
        const auto* address=reinterpret_cast<const uint8_t*>(&destination);
        code.insert(code.end(),address,address+sizeof(destination));
        code.insert(code.end(),{0xFF,0xD0});
        const auto& tail=epilogs[i-1];
        code.insert(code.end(),tail.begin(),tail.end());
        if (!VirtualProtect(driver,0x1000,PAGE_READWRITE,&ignored)) return false;
        std::memcpy(driver,code.data(),code.size());
        if (!VirtualProtect(driver,0x1000,PAGE_EXECUTE_READ,&ignored) ||
            !FlushInstructionCache(GetCurrentProcess(),driver,code.size())) return false;
        using PathGate=bool(*)(const void*,const void*);
        const auto invokePath=reinterpret_cast<PathGate>(driver);
        *nativeFlag=0;
        g_obstacleOwner.store(0);
        const bool disabledBefore=!invokePath(local,catchComponent);
        g_obstacleOwner.store(actorValue);
        const bool controlled=invokePath(local,catchComponent) && !invokePath(other,catchComponent);
        *nativeFlag=1;
        const bool nativeDefault=invokePath(local,catchComponent) && invokePath(other,catchComponent);
        *nativeFlag=0;
        g_obstacleOwner.store(0);
        return disabledBefore && controlled && nativeDefault;
    };
    std::array<uint8_t,0xD80> catchComponent{};
    std::array<uint8_t,16> foreignComponent{};
    std::memcpy(catchComponent.data()+8,&actorValue,sizeof(actorValue));
    g_obstacleWaypointReads.store(0);
    const bool basisWithoutWrap=executeGate(1,state.data(),foreignState.data(),catchComponent.data()) &&
        g_obstacleWaypointReads.load()==0;
    const uint32_t waypoints=2;
    std::memcpy(catchComponent.data()+0xD78,&waypoints,sizeof(waypoints));
    const bool basisWithWrap=executeGate(1,state.data(),foreignState.data(),catchComponent.data()) &&
        g_obstacleWaypointReads.load()==1;
    const bool lengthGate=executeGate(2,state.data(),foreignState.data(),catchComponent.data());
    const bool displayGate=executeGate(3,catchComponent.data(),foreignComponent.data(),catchComponent.data());
    const bool allPathGates=basisWithoutWrap && basisWithWrap && lengthGate && displayGate;
    printf("%s waypoint, length and display leaves preserve stock behavior for other actors\n",
           allPathGates?"PASS":"FAIL");
    VirtualFree(driver,0,MEM_RELEASE);
    printf("%s emitted solver gate runs only for the enabled owner and preserves the native flag\n",
           dormant&&local&&stock&&disabled?"PASS":"FAIL");
    if (!dormant || !local || !stock || !disabled || !allPathGates) return 1;
    if (!g_stallReady.load() || !TestStalledTether(image,context.data())) return 1;
    return obstacleInstalled && initial==TraceInstall::Retry && untouchedAfterBusy && foreign==TraceInstall::Refused &&
           refusedCow && flashUnchanged && clean==TraceInstall::Installed && installed && retainedCow && retainedPatchCow?0:1;
}
