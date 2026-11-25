// Included inside the bridge namespace after its trace event lock.
std::atomic<bool> g_straightPullOwned{false};
bool g_obstaclePathReady=false;
std::atomic<uintptr_t> g_obstacleOwner{0};
std::atomic<uint32_t> g_obstacleGateEntries{0};
std::atomic<uint32_t> g_obstacleWaypointReads{0};
static_assert(std::atomic<uintptr_t>::is_always_lock_free &&
              sizeof(g_obstacleOwner)==sizeof(uintptr_t));
static_assert(std::atomic<uint32_t>::is_always_lock_free &&
              sizeof(g_obstacleGateEntries)==sizeof(uint32_t));
struct ObstacleSample {
    uint64_t tick=0;
    axiom::surface::Point direction{};
    float solverDistance=0.f;
    bool playerOwner=false;
};
ObstacleSample g_obstacleEvent{};
std::atomic<uint64_t> g_nextObstacle{0};

constexpr size_t kObstacleRelay=416;
constexpr size_t kObstacleGateRelay=448;
constexpr size_t kObstacleGateStride=96;
constexpr uintptr_t kObstacleGateSite=0x13BD1B4;
constexpr uintptr_t kNativeObstacleFlag=0x716D858;
enum class ObstaclePathStage { Correction, Basis, Length, Display };

struct ObstaclePathGate {
    uintptr_t rva;
    std::array<uint8_t,13> context;
    std::array<uint8_t,4> compareOwner;
    ObstaclePathStage stage;
};
constexpr std::array<ObstaclePathGate,4> kObstaclePathGates{{
    {kObstacleGateSite,{0x80,0x3D,0x9D,0x06,0xDB,0x05,0x00,0x0F,0x84,0x58,0x01,0x00,0x00},
        {0x48,0x39,0x47,0x20},ObstaclePathStage::Correction}, // [RDI+20] pull-state actor
    {0x13B8CB7,{0x40,0x38,0x2D,0x9A,0x4B,0xDB,0x05,0x0F,0x84,0x52,0x01,0x00,0x00},
        {0x48,0x39,0xC6,0x90},ObstaclePathStage::Basis},      // RSI actor, RBX catch component
    {0x3C89BE,{0x80,0x3D,0x93,0x4E,0xDA,0x06,0x00,0x0F,0x84,0xBC,0x01,0x00,0x00},
        {0x48,0x39,0xC3,0x90},ObstaclePathStage::Length},     // RBX actor
    {0x69683F,{0x80,0x3D,0x12,0x70,0xAD,0x06,0x00,0x0F,0x84,0x6D,0x03,0x00,0x00},
        {0x49,0x39,0x45,0x08},ObstaclePathStage::Display},    // [R13+8] catch component's actor
}};

constexpr std::array<uint8_t,36> kWaypointBasisOwner{
    0x48,0x8B,0xF9,0x48,0x8B,0x71,0x20,0xC5,0xF8,0x57,0xC0,0xC5,0xFA,0x11,0x40,0xA8,
    0xC5,0xFA,0x11,0x40,0xAC,0xC5,0xFA,0x11,0x40,0xB0,0x48,0x8B,0x46,0x68,0x48,0x8B,0x58,0x78,
    0x33,0xED,
};
constexpr std::array<uint8_t,38> kWaypointBasisLookup{
    0x8B,0x83,0x78,0x0D,0,0,0x85,0xC0,0x74,0x2E,0xFF,0xC8,0x48,0x8D,0x14,0x40,
    0x48,0x8B,0x83,0x70,0x0D,0,0,0xC5,0xFB,0x10,0x04,0xD0,0xC5,0xFB,0x11,0x44,0x24,0x20,
    0x8B,0x5C,0xD0,0x08,
};
constexpr std::array<uint8_t,4> kWaypointLengthOwner{0x48,0x8B,0x59,0x08};
constexpr std::array<uint8_t,17> kWaypointLengthLookup{
    0x48,0x8B,0x9E,0x70,0x0D,0,0,0x8B,0x86,0x78,0x0D,0,0,0x48,0x8D,0x0C,0x40,
};
constexpr std::array<uint8_t,32> kWaypointDisplayOwner{
    0xC5,0x78,0x28,0xF1,0x4C,0x8B,0xE9,0x44,0x8B,0xE3,0x4C,0x89,0x64,0x24,0x40,0x49,
    0x69,0xC4,0xA0,0x01,0,0,0x48,0x8D,0xB1,0xB8,0x09,0,0,0x48,0x03,0xF0,
};
constexpr std::array<uint8_t,12> kWaypointDisplayLookup{
    0x45,0x8B,0xB5,0x78,0x0D,0,0,0xBB,0xFE,0x01,0,0,
};

#ifndef SAF_UNIFIED_RUNTIME
// The original conditional transfer stays in place at every site. RAX
// is dead on the outgoing edges; the leaf preserves all other registers.
std::vector<uint8_t> ObstacleGateRelay(const ObstaclePathGate& gate,
                                      uintptr_t flag,uintptr_t owner) {
    std::vector<uint8_t> code;
    auto append=[&](std::initializer_list<uint8_t> bytes) {
        code.insert(code.end(),bytes.begin(),bytes.end());
    };
    auto loadAddress=[&](uintptr_t value) {
        append({0x48,0xB8}); // mov rax, address
        const auto* address=reinterpret_cast<const uint8_t*>(&value);
        code.insert(code.end(),address,address+sizeof(value));
    };
    auto jump=[&](uint8_t opcode) {
        append({opcode,0});
        return code.size()-1;
    };
    auto resolve=[&](size_t position,size_t destination) {
        code[position]=static_cast<uint8_t>(destination-position-1);
    };

    loadAddress(flag);
    append({0x80,0x38,0x00}); // native global flag
    const auto stock=jump(0x75);
    loadAddress(owner);
    append({0x48,0x8B,0x00,0x48,0x85,0xC0}); // pinned control slot
    const auto disabled=jump(0x74);
    code.insert(code.end(),gate.compareOwner.begin(),gate.compareOwner.end());
    const auto foreign=jump(0x75);
    if (gate.stage==ObstaclePathStage::Correction) {
        loadAddress(reinterpret_cast<uintptr_t>(&g_obstacleGateEntries));
        append({0xF0,0xFF,0x00}); // lock inc dword ptr [rax]
    } else if (gate.stage==ObstaclePathStage::Basis) {
        append({0x83,0xBB,0x78,0x0D,0x00,0x00,0x00}); // actual waypoint count, not gate entries
        const auto empty=jump(0x74);
        loadAddress(reinterpret_cast<uintptr_t>(&g_obstacleWaypointReads));
        append({0xF0,0xFF,0x00});
        resolve(empty,code.size());
    }
    resolve(stock,code.size());
    append({0xB0,0x01,0xC3}); // allow: mov al, 1; ret
    resolve(disabled,code.size());
    resolve(foreign,code.size());
    append({0x31,0xC0,0xC3}); // deny: xor eax, eax; ret
    return code;
}
#endif
constexpr uintptr_t kObstacleCall=0x13BA03E;
constexpr uintptr_t kObstacleQuaternion=0x4D6990;
constexpr std::array<uint8_t,23> kObstacleContext{
    0x4C,0x8B,0xC0,0x49,0x8D,0x8D,0x80,0x00,0x00,0x00,0x48,0x8D,0x15,0x42,0x6A,0x5B,0x04,
    0xE8,0x4D,0xC9,0x11,0xFF,0x90,
};
constexpr std::array<uint8_t,20> kObstacleQuaternionEntry{
    0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x57,0x48,0x81,0xEC,0xC0,0x00,0x00,0x00,0xC5,0xF8,0x29,0x70,0xE8,
};

using NativeObstacleQuaternion=void(*)(void*,const void*,const void*);
#ifdef AXIOM_TESTING
NativeObstacleQuaternion g_testObstacleQuaternion=nullptr;
#endif

__declspec(noinline) __declspec(guard(nocf))
void TraceObstacleCorrection(void* quaternion,const void* from,const void* to) {
    auto call=reinterpret_cast<NativeObstacleQuaternion>(TargetAddress(kObstacleQuaternion));
#ifdef AXIOM_TESTING
    if (g_testObstacleQuaternion) call=g_testObstacleQuaternion;
#endif
    call(quaternion,from,to);
    if (!g_straightPullOwned.load(std::memory_order_relaxed) ||
        g_stopping.load(std::memory_order_relaxed)) return;
    const auto savedError=GetLastError();
    const auto tick=GetTickCount64();
    if (tick>=g_nextObstacle.load(std::memory_order_relaxed)) {
        const auto state=reinterpret_cast<uintptr_t>(quaternion)-0x80;
        uintptr_t owner=0;
        ObstacleSample event{};
        event.tick=tick;
        event.playerOwner=Get(state+0x20,owner) &&
            owner==g_obstacleOwner.load(std::memory_order_acquire) && ReadCatchControl(owner).actor;
        if (Get(reinterpret_cast<uintptr_t>(to),event.direction) &&
            std::isfinite(axiom::surface::Length(event.direction)) &&
            Get(state+0xB4,event.solverDistance) && std::isfinite(event.solverDistance) &&
            TryAcquireSRWLockExclusive(&g_traceLock)) {
            if (!g_obstacleEvent.tick || (event.playerOwner && !g_obstacleEvent.playerOwner))
                g_obstacleEvent=event;
            g_nextObstacle.store(tick+125,std::memory_order_relaxed);
            ReleaseSRWLockExclusive(&g_traceLock);
        }
    }
    SetLastError(savedError);
}

void LogObstacleCorrection(const ObstacleSample& obstacle) {
    if (!obstacle.tick) return;
    char text[256]{};
    snprintf(text,sizeof(text),
             "AxiomObstacle tick=%llu event=native-correction owner=%s solverDistance=%.3f direction=[%.4f,%.4f,%.4f]",
             static_cast<unsigned long long>(obstacle.tick),obstacle.playerOwner?"player":"unverified",
             obstacle.solverDistance,obstacle.direction[0],obstacle.direction[1],obstacle.direction[2]);
    Log(text);
}

#ifndef SAF_UNIFIED_RUNTIME
bool InstallObstacleObserver(uintptr_t relay,uintptr_t gates,DWORD imageType) {
    const auto game=g.game;
    const auto witness=[&](uintptr_t rva,const auto& bytes) {
        return quiesced_patch::Check{reinterpret_cast<const uint8_t*>(game+rva),bytes.data(),bytes.size()};
    };
    const std::array checks{
        witness(kObstacleCall-17,kObstacleContext),
        witness(kObstacleQuaternion,kObstacleQuaternionEntry),
        witness(0x13B8C93,kWaypointBasisOwner),
        witness(0x13B8CE4,kWaypointBasisLookup),
        witness(0x3C89BA,kWaypointLengthOwner),
        witness(0x3C8A3D,kWaypointLengthLookup),
        witness(0x69656E,kWaypointDisplayOwner),
        witness(0x6968C5,kWaypointDisplayLookup),
        witness(kObstaclePathGates[0].rva,kObstaclePathGates[0].context),
        witness(kObstaclePathGates[1].rva,kObstaclePathGates[1].context),
        witness(kObstaclePathGates[2].rva,kObstaclePathGates[2].context),
        witness(kObstaclePathGates[3].rva,kObstaclePathGates[3].context),
    };
    for (const auto& check:checks) {
        const auto address=reinterpret_cast<uintptr_t>(check.address);
        if (!safcompat::Region(address,check.size,game,imageType,PAGE_EXECUTE_READ) &&
            !safcompat::Region(address,check.size,game,imageType,PAGE_EXECUTE_WRITECOPY)) return false;
    }
    std::array<uint8_t,5> call{};
    if (!axiom::trace::RelativeCall(game+kObstacleCall+5,relay,call)) return false;
    std::array<quiesced_patch::Patch,kObstaclePathGates.size()+1> patches{};
    auto& observer=patches[0];
    observer.address=reinterpret_cast<uint8_t*>(game+kObstacleCall);
    observer.size=call.size();
    std::memcpy(observer.before,kObstacleContext.data()+17,call.size());
    std::memcpy(observer.after,call.data(),call.size());
    for (size_t i=0;i<kObstaclePathGates.size();++i) {
        const auto& gate=kObstaclePathGates[i];
        auto& patch=patches[i+1];
        patch.address=reinterpret_cast<uint8_t*>(game+gate.rva);
        patch.size=7;
        std::memcpy(patch.before,gate.context.data(),patch.size);
        if (!axiom::trace::RelativeCall(game+gate.rva+5,gates+i*kObstacleGateStride,call)) return false;
        std::memcpy(patch.after,call.data(),call.size());
        patch.after[5]=0x84;
        patch.after[6]=0xC0;
    }
    uint8_t nativeFlag=0;
    if (!Get(game+kNativeObstacleFlag,nativeFlag)) return false;
    return quiesced_patch::Write(patches.data(),patches.size(),{},nullptr,checks.data(),checks.size());
}
#endif
