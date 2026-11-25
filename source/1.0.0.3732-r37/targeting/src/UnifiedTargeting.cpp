#include "../../src/UnifiedTargeting.h"
#include <Windows.h>
#include <Psapi.h>
#include "../../src/GameMidHook.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <type_traits>
#include <utility>

#include "AxiomNativeRoles.h"
#include "CatchRange.h"
#include "TargetTrace.h"
#include "StalledTether.h"
#include "contracts/NearAllocation.h"

namespace {

struct TargetHost { uintptr_t game=0; size_t size=0; } g;
std::atomic<bool> g_stopping{true};
saf_targeting::LogSink g_logSink=nullptr;
axiom::CatchRange g_catchRange;
bool g_hostValid=false;
std::atomic<bool> g_initialized{false};
unsigned g_installAttempts=0;

void Log(const char* text) {
    if (g_logSink) g_logSink(text);
}

bool Read(void*,uintptr_t at,void* out,size_t n) {
    SIZE_T copied=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),out,n,&copied) && copied==n;
}

template<class T>
bool Get(uintptr_t at,T& value) { return Read(nullptr,at,&value,sizeof(value)); }

axiom::native::Roles g_nativeRoles;
#if defined(SAF_HOOK_FIXTURE) || defined(SAF_MAPPED_IMAGE_TESTING)
uintptr_t g_fixturePreferredBase=0;
#endif
uintptr_t TargetAddress(uintptr_t role) {
#ifdef SAF_HOOK_FIXTURE
    if (!g_nativeRoles.Ready() || role<4096) return g.game+role; // authored call ABI fixture
#endif
    return g_nativeRoles.Address(role);
}

#define SAF_UNIFIED_RUNTIME 1
#include "TargetTraceRuntime.h"

void PauseSearch() {
    g_stopping.store(true,std::memory_order_release);
    g_searchReady.store(false,std::memory_order_release);
    g_straightPullOwned.store(false,std::memory_order_relaxed);
    RefreshTargetControl(false);
    g_effectiveRangeBits.store(0,std::memory_order_relaxed);
}

}

namespace saf_targeting {

#ifdef SAF_MAPPED_IMAGE_TESTING
void PrepareMappedImage(LogSink sink,uintptr_t base,size_t size,uintptr_t pointerBase) {
    g_fixturePreferredBase=pointerBase;
    g_logSink=sink;
    g={base,size};
    g_hostValid=true;
}

bool MappedImageFeaturesReady() {
    return g_searchReady.load() && g_traceHooksReady.load() &&
           g_allHitsCollectorReady.load() && g_obstaclePathReady && g_stallReady.load();
}
#endif

bool Prepare(LogSink sink) {
    g_logSink=sink;
    HMODULE game=GetModuleHandleW(L"CrimsonDesert.exe");
    MODULEINFO info{};
    g_hostValid=game && game==GetModuleHandleW(nullptr) &&
        GetModuleInformation(GetCurrentProcess(),game,&info,sizeof(info)) &&
        info.lpBaseOfDll==game && info.SizeOfImage;
    if (g_hostValid) g={reinterpret_cast<uintptr_t>(game),info.SizeOfImage};
    return g_hostValid;
}

Result Sync(const State& state) {
    DrainTargetTrace();
    if (!state.support) {
        PauseSearch();
        return g_catchRange.Restore(g.game,Read)?Result::Inactive:Result::Retry;
    }
    if (!g_hostValid || !std::isfinite(state.range) || state.range<50000.f) {
        PauseSearch();
        return g_catchRange.Restore(g.game,Read)?Result::Unavailable:Result::Retry;
    }
    if (!g_initialized.load(std::memory_order_acquire)) {
        if (g_installAttempts>=8) return Result::Unavailable;
        ++g_installAttempts;
        const auto result=InstallTargetTrace();
        if (result==TraceInstall::Retry) return Result::Retry;
        if (result!=TraceInstall::Installed) {
            g_installAttempts=8;
            return Result::Conflict;
        }
        g_initialized.store(true,std::memory_order_release);
    }
    if (!g_catchRange.Update(g.game,state.support,Read)) {
        PauseSearch();
        return g_catchRange.Faulted()?Result::Conflict:Result::Retry;
    }
    uint32_t rangeBits=0;
    std::memcpy(&rangeBits,&state.range,sizeof(rangeBits));
    g_effectiveRangeBits.store(rangeBits,std::memory_order_relaxed);
    const bool obstaclePath=state.straightPull && g_obstaclePathReady;
    g_straightPullOwned.store(obstaclePath,std::memory_order_relaxed);
    RefreshTargetControl(obstaclePath);
    g_stopping.store(false,std::memory_order_release);
    g_searchReady.store(true,std::memory_order_release);
    return Result::Active;
}

}
