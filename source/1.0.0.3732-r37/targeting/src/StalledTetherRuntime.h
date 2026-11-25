#pragma once


using axiom::surface::Point;

constexpr size_t kStallPullRelay=kObstacleGateRelay+kObstaclePathGates.size()*kObstacleGateStride;
constexpr uintptr_t kStallRadialCall=0x13BD11F;
constexpr uintptr_t kNativeRadialStep=0x13BC380;
constexpr uintptr_t kNativeTetherRelease=0x68F130;

// Exact call, native ABI and native retract-rate/radial-result witnesses.
constexpr std::array<uint8_t,20> kStallRadialContext{
    0xC5,0xF8,0x28,0xCE,0x48,0x8B,0xCF,0xE8,0x5C,0xF2,0xFF,0xFF,
    0xC5,0xB2,0x5E,0x05,0xAC,0x59,0xDB,0x05,
};
constexpr std::array<uint8_t,30> kStallRadialEntry{
    0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x57,0x48,0x81,0xEC,0xB0,0x00,0x00,0x00,
    0xC5,0xF8,0x29,0x70,0xE8,0xC5,0xF8,0x29,0x78,0xD8,0xC5,0x78,0x29,0x40,0xC8,
};
constexpr std::array<uint8_t,31> kStallReelInput{
    0x0F,0xB7,0x49,0x3C,0xC5,0x78,0x29,0x58,0x98,0xC5,0x78,0x28,0xD1,
    0xE8,0xA3,0x0B,0x19,0xFF,0x0F,0xB7,0x4B,0x28,0xC5,0xF8,0x28,0xF0,
    0xE8,0x96,0x0B,0x19,0xFF,
};
constexpr std::array<uint8_t,22> kTetherReleaseCall{
    0xC6,0x44,0x24,0x20,0x01,0x45,0x33,0xC9,0x41,0xB0,0x02,
    0x0F,0xB6,0xD3,0x48,0x8B,0xCF,0xE8,0xB9,0xAF,0x01,0x00,
};
constexpr std::array<uint8_t,38> kTetherReleaseEntry{
    0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,
    0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x50,0x45,0x0F,0xB6,0xF1,0x45,0x0F,0xB6,0xF8,
    0x0F,0xBE,0xEA,0x48,0x8B,0xF9,
};

using NativeRadialStep=void(*)(void*,float);
using NativeTetherRelease=void(*)(void*,uint8_t,uint8_t,uint8_t,bool);
#ifdef AXIOM_TESTING
NativeRadialStep g_testRadialStep=nullptr;
NativeTetherRelease g_testTetherRelease=nullptr;
uint64_t g_testStallTick=0;
#endif

struct TetherStall {
    uintptr_t actor=0,component=0;
    uint8_t kind=0;
    Point anchor{};
    axiom::stall::MotionWindow motion{};
    bool pending=false,issued=false;
};

struct TetherReleaseEvent {
    uint64_t tick=0;
    float stalledSeconds=0;
    uint8_t before=0,after=255;
};

std::atomic<bool> g_stallReady{false};
SRWLOCK g_stallLock=SRWLOCK_INIT;
TetherStall g_stall{};
TetherReleaseEvent g_tetherReleaseEvent{};

void ClearStalledTether() {
    AcquireSRWLockExclusive(&g_stallLock);
    g_stall={};
    ReleaseSRWLockExclusive(&g_stallLock);
}

void RememberStallAttachment(uintptr_t component,uint8_t lane,uint8_t kind,
                             bool anchorKnown,const Point& anchor) {
    // The stock movement solver reads its rope origin from lane zero.
    if (lane!=0 || !g_stallReady.load(std::memory_order_acquire)) return;
    uintptr_t actor=0;
    const bool valid=anchorKnown && Get(component+8,actor) && actor &&
        std::isfinite(axiom::surface::Length(anchor));
    AcquireSRWLockExclusive(&g_stallLock);
    g_stall={};
    if (valid) {
        g_stall.actor=actor;
        g_stall.component=component;
        g_stall.kind=kind;
        g_stall.anchor=anchor;
    }
    ReleaseSRWLockExclusive(&g_stallLock);
}

bool ActiveTraversalTether(uintptr_t component,uint8_t& phase,uint8_t& kind,uint32_t& waypoints) {
    return Get(component+0xA4C,phase) && (phase==3 || phase==4) &&
        Get(component+0x168,kind) && (kind==1 || kind==2) &&
        Get(component+0xD78,waypoints);
}

uint64_t StalledTetherTick() {
#ifdef AXIOM_TESTING
    if (g_testStallTick) return g_testStallTick;
#endif
    return GetTickCount64();
}

void ObserveStallRadialStep(uintptr_t state,float delta) {
    const auto controlled=g_obstacleOwner.load(std::memory_order_acquire);
    uintptr_t actor=0,parts=0,component=0;
    if (!controlled || !Get(state+0x20,actor) || actor!=controlled ||
        !Get(actor+0x68,parts) || !parts || !Get(parts+0x78,component) ||
        !ControlledCatch(component)) return;

    uint8_t phase=0,kind=0;
    uint32_t waypoints=0;
    uint16_t lengthRate=0,reelRate=0;
    std::array<float,6> radial{};
    const bool active=ActiveTraversalTether(component,phase,kind,waypoints) &&
        Get(state+0x28,lengthRate) && Get(state+0x3C,reelRate) &&
        axiom::stall::RetractionRequested(lengthRate,reelRate) &&
        Get(state+0x50,radial);
    const Point position{radial[0]+radial[3],radial[1]+radial[4],radial[2]+radial[5]};
    const auto tick=StalledTetherTick();
    if (!TryAcquireSRWLockExclusive(&g_stallLock)) return;
    if (g_stall.component==component && g_stall.actor==actor && !g_stall.issued)
        g_stall.pending=g_stall.motion.Observe(tick,delta,position,g_stall.anchor,
            active&&kind==g_stall.kind,waypoints);
    ReleaseSRWLockExclusive(&g_stallLock);
}

__declspec(noinline) __declspec(guard(nocf))
void TraceStallRadialStep(void* state,float delta) {
    auto native=reinterpret_cast<NativeRadialStep>(TargetAddress(kNativeRadialStep));
#ifdef AXIOM_TESTING
    if (g_testRadialStep) native=g_testRadialStep;
#endif
    const auto error=GetLastError();
    if (g_stallReady.load(std::memory_order_acquire))
        ObserveStallRadialStep(reinterpret_cast<uintptr_t>(state),delta);
    SetLastError(error);
    native(state,delta);
}

__declspec(noinline) __declspec(guard(nocf))
void ReleaseStalledTether(uintptr_t component,float delta) {
    // Called from the verified catch update after the native body returns.
    // Native release manages attachment, rope and events on its own.
    if (!(delta>0 && delta<=0.1f) || !g_stallReady.load(std::memory_order_acquire) ||
        !ControlledCatch(component)) return;
    uintptr_t actor=0;
    uint8_t phase=0,kind=0;
    uint32_t waypoints=0;
    if (!Get(component+8,actor) || !actor || actor!=g_obstacleOwner.load(std::memory_order_acquire) ||
        !ActiveTraversalTether(component,phase,kind,waypoints) || !waypoints ||
        !TryAcquireSRWLockExclusive(&g_stallLock)) return;
    const auto tick=StalledTetherTick();
    float elapsed=0;
    if (g_stall.pending && !g_stall.issued &&
        g_stall.component==component && g_stall.actor==actor && g_stall.kind==kind &&
        tick>=g_stall.motion.latestTick &&
        tick-g_stall.motion.latestTick<=axiom::stall::kMaxSampleGapMs) {
        elapsed=g_stall.motion.elapsed;
        g_stall.issued=true;
        g_stall.pending=false;
    }
    ReleaseSRWLockExclusive(&g_stallLock);
    if (elapsed<=0) return;
    auto native=reinterpret_cast<NativeTetherRelease>(TargetAddress(kNativeTetherRelease));
#ifdef AXIOM_TESTING
    if (g_testTetherRelease) native=g_testTetherRelease;
#endif
    native(reinterpret_cast<void*>(component),0,2,0,true);
    uint8_t after=255;
    Get(component+0xA4C,after);
    AcquireSRWLockExclusive(&g_stallLock);
    g_tetherReleaseEvent={tick,elapsed,phase,after};
    ReleaseSRWLockExclusive(&g_stallLock);
}

void DrainStalledTether() {
    TetherReleaseEvent event{};
    AcquireSRWLockExclusive(&g_stallLock);
    event=g_tetherReleaseEvent;
    g_tetherReleaseEvent={};
    ReleaseSRWLockExclusive(&g_stallLock);
    if (!event.tick) return;
    char message[200]{};
    snprintf(message,sizeof(message),
        "AxiomObstacle tick=%llu event=stalled-tether-release-requested seconds=%.3f phaseBefore=%u phaseAfter=%u",
        static_cast<unsigned long long>(event.tick),event.stalledSeconds,unsigned(event.before),unsigned(event.after));
    Log(message);
}

#ifndef SAF_UNIFIED_RUNTIME
bool InstallStalledTether(uintptr_t relay,DWORD imageType) {
    const auto game=g.game;
    const auto witness=[&](uintptr_t rva,const auto& bytes) {
        return quiesced_patch::Check{reinterpret_cast<const uint8_t*>(game+rva),bytes.data(),bytes.size()};
    };
    const std::array checks{
        witness(kStallRadialCall-7,kStallRadialContext),
        witness(kNativeRadialStep,kStallRadialEntry),
        witness(0x13BC3AB,kStallReelInput),
        witness(0x674161,kTetherReleaseCall),
        witness(kNativeTetherRelease,kTetherReleaseEntry),
    };
    for (const auto& check:checks) {
        const auto address=reinterpret_cast<uintptr_t>(check.address);
        if (!safcompat::Region(address,check.size,game,imageType,PAGE_EXECUTE_READ) &&
            !safcompat::Region(address,check.size,game,imageType,PAGE_EXECUTE_WRITECOPY)) return false;
    }
    std::array<uint8_t,5> call{};
    if (!axiom::trace::RelativeCall(game+kStallRadialCall+5,relay,call)) return false;
    quiesced_patch::Patch patch{};
    patch.address=reinterpret_cast<uint8_t*>(game+kStallRadialCall);
    patch.size=call.size();
    std::memcpy(patch.before,kStallRadialContext.data()+7,patch.size);
    std::memcpy(patch.after,call.data(),patch.size);
    return quiesced_patch::Write(&patch,1,{},nullptr,checks.data(),checks.size());
}
#endif
