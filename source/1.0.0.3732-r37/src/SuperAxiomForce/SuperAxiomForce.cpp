// SuperAxiomForce - SuperAxiomForce.cpp
// Enhanced Axiom Force mod for Crimson Desert
//
// INSTALL: use Definitive Mod Manager; do not manually install into bin64.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#ifdef SAF_GAME_RELEASE
#include "../GameMidHook.h"
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "NativeDiscovery.h"
#include "ClockAcceleration.h"
#include "QuiescedPatch.h"
#include "StraightPullAngular.h"
#include "NearAllocation.h"
#include "ExactHostPolicy.h"
#include "AdaptiveNative.h"
#include <algorithm>
#include <exception>
#include "OwnedPatchProtocol.h"
#include "PatchEncoding.h"
#ifdef SAF_EMBEDDED
static safcompat::Descriptor SAFOwnedPatchStateV1{};
static volatile LONG SAFMenuUnlimitedReachV1=0;
#else
extern "C" {
__declspec(dllexport) safcompat::Descriptor SAFOwnedPatchStateV1{};
__declspec(dllexport) volatile LONG SAFMenuUnlimitedReachV1=0;
}
#endif
static bool g_compatPoisoned=false;
static std::array<safcompat::PatchSpec, 11> g_patchContract{};
static bool g_adaptiveWitnessReady = false;

#pragma comment(lib, "psapi.lib")

// ============================================================
//  CONSTANTS
// ============================================================
static const char* kModName = "SuperAxiomForce";
static const char* kLogFile = "SuperAxiomForce.log";
static const char* kIniSection = "AxiomForce";
static const char* kBuildId = "native-adaptive-r37-region-allocator";
static HMODULE g_module = nullptr;
static std::string g_logPath;

static const float kFallbackRange = 500.0f;
static const float kFallbackPullSpeed = 1000.0f;

// ============================================================
//  STATE
// ============================================================
static std::string g_iniPath;
static ULONGLONG g_lastIniModTime = 0;
static std::atomic<bool> g_isInitialized{ false };
static uintptr_t g_gameBase = 0;
static size_t g_gameSize = 0;

struct StraightPullSite {
    uint8_t* address = nullptr;
    uint8_t original[5] = {};
    bool canRestore = false;
    bool owned = false;
};

static std::vector<StraightPullSite> g_straightPullSites;

struct BytePatchSite {
    const char* name = nullptr;
    uint8_t* address = nullptr;
    uint8_t original[16] = {};
    uint8_t patch[16] = {};
    size_t size = 2;
    bool canRestore = false;
    bool owned = false;
};

static std::vector<BytePatchSite> g_instantAxiomSites;
static std::vector<BytePatchSite> g_scopedRangeSites;
static std::vector<BytePatchSite> g_scopedPullSites, g_angularSites;
struct FloatPatchSite {
    float* address = nullptr;
    float original = 0;
    float lastWritten = 0;
    bool owned = false;
};
static FloatPatchSite g_range, g_limit, g_pull;
static native::Image g_image;
// Retain the near allocation and function table until process termination. A
// reload may restore a CALL while a game thread is still inside its old stub.
static uint8_t* g_supportAllocation = nullptr;
// Owned RW storage only: no adapters, unwind records or game writes exist until
// delayed discovery validates all ten exact native references.
static uint8_t* g_earlySupportAllocation = nullptr;
static uintptr_t g_earlyImageBase = 0;
static size_t g_earlyImageSize = 0;
static ULONGLONG g_earlyReservedAt = 0;
#define SAF_EARLY_RESERVATION_AVAILABLE 1
static float* g_configuredRange = nullptr;
static float* g_configuredPull = nullptr;
static volatile LONG* g_keepTravelClock = nullptr;
static bool g_angularReady = false;
static bool g_clockSupportReady = false;
#ifdef SAF_GAME_RELEASE
static void Logf(const char* format, ...);
#include "NativePolicyHooks.h"
#endif
#ifdef SAF_NEAR_ALLOCATION_TESTING
static const near_allocation::Api* g_testNearApiOverride = nullptr;
#endif
#ifdef SAF_OWNERSHIP_TESTING
static void (*g_testBeforeClockSnapshot)(size_t) = nullptr;
static void (*g_testBeforeAngularSnapshot)() = nullptr;
static void (*g_testBeforeExpectedSnapshot)(BytePatchSite*) = nullptr;
#endif


static void Log(const char* msg);

struct NearSupportSample {
    uintptr_t at = 0;
    SIZE_T returned = 0;
    MEMORY_BASIC_INFORMATION info{};
};
struct NearSupportErrors {
    DWORD query = 0, allocation = 0, release = 0;
    unsigned queries = 0, free = 0, reserved = 0, committed = 0, other = 0, eligibleFree = 0;
    size_t largestFreeTail = 0;
    std::array<NearSupportSample, 8> first{};
    std::array<NearSupportSample, 4> freeSamples{}, last{};
};
static void RecordNearSupportQuery(NearSupportErrors& errors, uintptr_t address,
                                   SIZE_T returned, const MEMORY_BASIC_INFORMATION& info) {
    const NearSupportSample sample{address, returned, info};
    if (errors.queries < errors.first.size()) errors.first[errors.queries] = sample;
    errors.last[errors.queries % errors.last.size()] = sample;
    ++errors.queries;
    if (info.State == MEM_FREE) {
        if (errors.free < errors.freeSamples.size()) errors.freeSamples[errors.free] = sample;
        ++errors.free;
        if (info.RegionSize >= 0x2000) ++errors.eligibleFree;
        errors.largestFreeTail = (std::max)(errors.largestFreeTail, info.RegionSize);
    } else if (info.State == MEM_RESERVE) ++errors.reserved;
    else if (info.State == MEM_COMMIT) ++errors.committed;
    else ++errors.other;
}
static void LogNearSupportMap(const NearSupportErrors& errors, const SYSTEM_INFO& system, const char* phase) {
    Logf("NearSupport map query_counts=%u free=%u reserve=%u commit=%u other=%u free_tail_at_least_8KB=%u largest_free_tail=%llu mbi_bytes=%u state_offset=%u granularity=%lu page=%lu phase=%s",
        errors.queries, errors.free, errors.reserved, errors.committed, errors.other, errors.eligibleFree,
        static_cast<unsigned long long>(errors.largestFreeTail), static_cast<unsigned>(sizeof(MEMORY_BASIC_INFORMATION)),
        static_cast<unsigned>(offsetof(MEMORY_BASIC_INFORMATION, State)), system.dwAllocationGranularity, system.dwPageSize, phase);
    auto sample = [&](const NearSupportSample& s, const char* group) {
        Logf("NearSupport map sample=%s at=%p returned=%llu base=%p size=%llu allocation_base=%p state=0x%lX protect=0x%lX type=0x%lX allocation_protect=0x%lX phase=%s",
            group, reinterpret_cast<void*>(s.at), static_cast<unsigned long long>(s.returned), s.info.BaseAddress,
            static_cast<unsigned long long>(s.info.RegionSize), s.info.AllocationBase, s.info.State, s.info.Protect,
            s.info.Type, s.info.AllocationProtect, phase);
    };
    for (unsigned i = 0; i < (std::min)(errors.queries, static_cast<unsigned>(errors.first.size())); ++i) sample(errors.first[i], "first");
    for (unsigned i = 0; i < (std::min)(errors.free, static_cast<unsigned>(errors.freeSamples.size())); ++i) sample(errors.freeSamples[i], "free");
    const unsigned lastCount = (std::min)(errors.queries, static_cast<unsigned>(errors.last.size()));
    const unsigned start = errors.queries > errors.last.size() ? errors.queries % static_cast<unsigned>(errors.last.size()) : 0;
    for (unsigned i = 0; i < lastCount; ++i) sample(errors.last[(start + i) % errors.last.size()], "last");
}
static bool QueryNearSupport(void* context, uintptr_t address, near_allocation::Region& region) {
    MEMORY_BASIC_INFORMATION info{};
    const SIZE_T returned = VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info));
    auto& errors = *static_cast<NearSupportErrors*>(context);
    const DWORD error = returned == sizeof(info) ? ERROR_SUCCESS : GetLastError();
    RecordNearSupportQuery(errors, address, returned, info);
    if (returned != sizeof(info)) { errors.query = error; return false; }
    region = {reinterpret_cast<uintptr_t>(info.BaseAddress), info.RegionSize, info.State == MEM_FREE};
    return true;
}
static uintptr_t AllocateNearSupport(void* context, uintptr_t address, size_t bytes) {
    auto* memory = VirtualAlloc(reinterpret_cast<void*>(address), bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) static_cast<NearSupportErrors*>(context)->allocation = GetLastError();
    return reinterpret_cast<uintptr_t>(memory);
}
static void ReleaseNearSupport(void* context, uintptr_t address, size_t) {
    if (!VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE)) static_cast<NearSupportErrors*>(context)->release = GetLastError();
}
static void ReleaseEarlySupport() {
    if (!g_earlySupportAllocation) return;
    const auto* allocation = g_earlySupportAllocation;
    const bool released = VirtualFree(g_earlySupportAllocation, 0, MEM_RELEASE) != 0;
    Logf("NearSupport unused early reservation release=%s address=%p error=%lu", released ? "ok" : "failed", allocation,
        released ? ERROR_SUCCESS : GetLastError());
    g_earlySupportAllocation = nullptr; g_earlyImageBase = 0; g_earlyImageSize = 0; g_earlyReservedAt = 0;
}
static bool PreallocateSupportForImage(uintptr_t imageBase, size_t imageSize) {
    if (!imageBase || !imageSize || imageSize > UINTPTR_MAX - imageBase || g_supportAllocation) return false;
    if (g_earlySupportAllocation) return g_earlyImageBase == imageBase && g_earlyImageSize == imageSize;
    // These endpoints conservatively cover every instruction end in the entire
    // image and every byte of the owned block. No version-specific RVA is used.
    const near_allocation::Reference envelope[] = {{imageBase + imageSize, 0}, {imageBase, 0x1FFF}};
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    NearSupportErrors errors{};
    const near_allocation::Api api{&errors, QueryNearSupport, AllocateNearSupport, ReleaseNearSupport};
    const auto allocation = near_allocation::Allocate(api, envelope, 2, 0x2000, system.dwAllocationGranularity,
        reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress), reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress), imageBase, imageSize);
    LogNearSupportMap(errors, system, "early-reservation");
    Logf("NearSupport early reservation image_base=%p image_size=%llu window=[%p,%p] result=%p queries=%u attempts=%u alloc_failures=%u query_failures=%u invalid_regions=%u tick=%llu; owned RW only, native binding pending",
        reinterpret_cast<void*>(imageBase), static_cast<unsigned long long>(imageSize), reinterpret_cast<void*>(allocation.window.low),
        reinterpret_cast<void*>(allocation.window.high), reinterpret_cast<void*>(allocation.address), allocation.queries,
        allocation.attempts, allocation.allocationFailures, allocation.queryFailures, allocation.invalidRegions, GetTickCount64());
    if (!allocation.address) return false;
    g_earlySupportAllocation = reinterpret_cast<uint8_t*>(allocation.address);
    g_earlyImageBase = imageBase; g_earlyImageSize = imageSize; g_earlyReservedAt = GetTickCount64();
    return true;
}
static bool InitializeSupport(const native::Result& result, size_t angularSite,
                              const std::array<size_t, 2>& selectedReturns) {
    if (g_supportAllocation) return g_clockSupportReady;
    if (!g_range.address || !g_limit.address || !g_pull.address) { ReleaseEarlySupport(); return false; }
    const auto resume = angularSite + sizeof(straight_pull_angular::kOriginal);
    if (g_gameSize > UINTPTR_MAX - g_gameBase || !angularSite || resume >= g_gameSize ||
        !result.scopedPull.binding.valid || !result.clock[0].valid || !result.clock[1].valid ||
        !std::all_of(result.scopedRange.begin(), result.scopedRange.end(),
            [](const native::CodePatch& p) { return p.binding.valid && p.size == 4; })) {
        Log("ERROR: complete near-support reference plan unavailable"); ReleaseEarlySupport(); return false;
    }
#ifdef SAF_GAME_RELEASE
    std::array<near_allocation::Reference, 7> references{};
#else
    const size_t widths[] = {10, 9};
    std::array<near_allocation::Reference, 10> references{};
#endif
    for (size_t i = 0; i < result.scopedRange.size(); ++i)
        references[i] = {g_gameBase + result.scopedRange[i].offset + 4, 0x1000};
    references[6] = {g_gameBase + result.scopedPull.offset + 4, 0x1008};
#ifndef SAF_GAME_RELEASE
    references[7] = {g_gameBase + result.clock[0].instruction + widths[0], 0};
    references[8] = {g_gameBase + result.clock[1].instruction + widths[1], 0x200};
    references[9] = {g_gameBase + resume, 0x400};
#endif
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    NearSupportErrors errors{};
    const near_allocation::Api api{&errors, QueryNearSupport, AllocateNearSupport, ReleaseNearSupport};
#ifdef SAF_NEAR_ALLOCATION_TESTING
    const auto& selectedApi = g_testNearApiOverride ? *g_testNearApiOverride : api;
#else
    const auto& selectedApi = api;
#endif
    near_allocation::Result allocation{};
    allocation.window = near_allocation::Intersect(references.data(), references.size(), 0x2000, system.dwAllocationGranularity,
        reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress), reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress));
    if (g_earlySupportAllocation) {
        const uintptr_t early = reinterpret_cast<uintptr_t>(g_earlySupportAllocation);
        if (g_earlyImageBase == g_gameBase && g_earlyImageSize == g_gameSize && allocation.window.valid &&
            early >= allocation.window.low && early <= allocation.window.high && near_allocation::Fits(early, references.data(), references.size())) {
            allocation.address = early;
            Logf("NearSupport early reservation adopted address=%p exact_references=%zu reserved_tick=%llu bind_tick=%llu elapsed_ms=%llu",
                g_earlySupportAllocation, references.size(), g_earlyReservedAt, GetTickCount64(), GetTickCount64() - g_earlyReservedAt);
            g_earlySupportAllocation = nullptr; g_earlyImageBase = 0; g_earlyImageSize = 0; g_earlyReservedAt = 0;
        } else ReleaseEarlySupport();
    }
    if (!allocation.address) {
        allocation = near_allocation::Allocate(selectedApi, references.data(), references.size(), 0x2000,
            system.dwAllocationGranularity, reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress),
            reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress), g_gameBase, g_gameSize);
        LogNearSupportMap(errors, system, "delayed-fallback");
    }
    g_supportAllocation = reinterpret_cast<uint8_t*>(allocation.address);
    Logf("NearSupport allocation image_base=%p window=[%p,%p] valid=%d result=%p above=%d below=%d queries=%u attempts=%u alloc_failures=%u query_failures=%u invalid_regions=%u unexpected_returns=%u budget_exceeded=%d query_error=%lu allocation_error=%lu release_error=%lu",
        reinterpret_cast<void*>(g_gameBase), reinterpret_cast<void*>(allocation.window.low), reinterpret_cast<void*>(allocation.window.high),
        allocation.window.valid ? 1 : 0, g_supportAllocation, allocation.searchedAbove ? 1 : 0, allocation.searchedBelow ? 1 : 0,
        allocation.queries, allocation.attempts, allocation.allocationFailures, allocation.queryFailures, allocation.invalidRegions,
        allocation.releasedUnexpected, allocation.budgetExceeded ? 1 : 0, errors.query, errors.allocation, errors.release);
    if (!g_supportAllocation) { Log("ERROR: near support allocation failed; range/instant unavailable"); return false; }
    g_configuredRange = reinterpret_cast<float*>(g_supportAllocation + 0x1000);
    *g_configuredRange = g_range.original;
    g_configuredPull = reinterpret_cast<float*>(g_supportAllocation + 0x1008);
    *g_configuredPull = g_pull.original;
    g_keepTravelClock = reinterpret_cast<volatile LONG*>(g_supportAllocation + 0x100C);
    InterlockedExchange(g_keepTravelClock,1);
#ifdef SAF_GAME_RELEASE
    const bool bound=BindCoreHooks(result,angularSite,selectedReturns);
    g_angularReady=bound && PrepareCoreHooks(0,1);
    g_clockSupportReady=bound && PrepareCoreHooks(1,3);
    return true;
#else
    if (!result.clock[0].valid || !result.clock[1].valid) return false;
    auto* table = reinterpret_cast<RUNTIME_FUNCTION*>(g_supportAllocation + 0x700);
    for (size_t i = 0; i < 2; ++i) {
        const auto instruction = g_gameBase + result.clock[i].instruction;
#ifdef SAF_OWNERSHIP_TESTING
        if (g_testBeforeClockSnapshot) g_testBeforeClockSnapshot(i);
#endif
        const auto code = clock_acceleration::Emit(i == 0 ? clock_acceleration::Kind::Reel : clock_acceleration::Kind::Propell,
            i == 0 ? clock_acceleration::kReelAdd : clock_acceleration::kPropellAdd,
            widths[i], instruction + widths[i], reinterpret_cast<uintptr_t>(g_keepTravelClock));
        if (code.size() >= 0x200) return false;
        memcpy(g_supportAllocation + i*0x200, code.data(), code.size());
        memcpy(g_supportAllocation + 0x600 + i*0x10, clock_acceleration::kUnwind, sizeof(clock_acceleration::kUnwind));
        table[i] = {static_cast<DWORD>(i*0x200), static_cast<DWORD>(i*0x200+code.size()), static_cast<DWORD>(0x600+i*0x10)};
    }
    size_t angularSize = 0;
    g_angularReady = selectedReturns[0] < g_gameSize && selectedReturns[1] < g_gameSize &&
        straight_pull_angular::EmitSelected(g_supportAllocation + 0x400, 0x200,
            {g_gameBase + selectedReturns[0], g_gameBase + selectedReturns[1]}, &angularSize);
    if (g_angularReady) {
        memcpy(g_supportAllocation + 0x620, straight_pull_angular::kUnwind, sizeof(straight_pull_angular::kUnwind));
        table[2] = {0x400, static_cast<DWORD>(0x400 + straight_pull_angular::kCodeSize), 0x620};
    }
    DWORD previous = 0;
    if (!VirtualProtect(g_supportAllocation, 0x1000, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), g_supportAllocation, 0x1000) ||
        !RtlAddFunctionTable(table, g_angularReady ? 3 : 2, reinterpret_cast<DWORD64>(g_supportAllocation))) {
        Log("ERROR: clock support protection/cache/unwind registration failed; clock CALLs will not install");
        return false;
    }
    g_clockSupportReady = true;
    return true;
#endif
}

#if defined(SAF_NEAR_ALLOCATION_TESTING)
static bool InitializeSupport(const native::Result& result) {
    return InitializeSupport(result, straight_pull_angular::kSiteRva,
        {straight_pull_angular::kSelectedReturnRvas[0], straight_pull_angular::kSelectedReturnRvas[1]});
}
#endif


// ============================================================
//  LOGGING
// ============================================================
#ifdef SAF_EMBEDDED
static saf::embedded::LogSink g_embeddedLog = nullptr;
#endif

static void Log(const char* msg) {
#ifdef SAF_EMBEDDED
    if (g_embeddedLog) g_embeddedLog(msg);
#else
    const char* path = g_logPath.empty() ? kLogFile : g_logPath.c_str();
    std::ofstream f(path, std::ios::app);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char prefix[128];
    sprintf_s(prefix,
              "[%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu tid=%lu] [%s] ",
              st.wYear,
              st.wMonth,
              st.wDay,
              st.wHour,
              st.wMinute,
              st.wSecond,
              st.wMilliseconds,
              GetCurrentProcessId(),
              GetCurrentThreadId(),
              kModName);
    f << prefix << msg << "\n";
#endif
}

static void Logf(const char* format, ...) {
    char buffer[768];
    va_list args;
    va_start(args, format);
    vsprintf_s(buffer, format, args);
    va_end(args);
    Log(buffer);
}

[[maybe_unused]] static std::string GetModuleDirectory(HMODULE module) {
    char modulePath[MAX_PATH] = {};
    if (module != nullptr && GetModuleFileNameA(module, modulePath, MAX_PATH) != 0) {
        std::string path(modulePath);
        const auto slash = path.find_last_of("\\/");
        if (slash != std::string::npos) {
            return path.substr(0, slash);
        }
    }

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0) {
        std::string path(exePath);
        const auto slash = path.find_last_of("\\/");
        if (slash != std::string::npos) {
            return path.substr(0, slash);
        }
    }

    return ".";
}

[[maybe_unused]] static std::string ResolveIniPath(const std::string& hostDir, const std::string& moduleDir) {
    const std::string hostIni = hostDir + "\\" + kModName + ".ini";
    if (GetFileAttributesA(hostIni.c_str()) != INVALID_FILE_ATTRIBUTES) return hostIni;
    const DWORD error = GetLastError();
    // An unreadable host path is not proof of absence; do not silently switch configs.
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return hostIni;
    return moduleDir + "\\" + kModName + ".ini";
}

static bool IsCrimsonDesertProcess() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    if (_wcsicmp(name ? name + 1 : path, L"CrimsonDesert.exe") != 0) return false;
    const HMODULE main = GetModuleHandleW(nullptr);
    if (!main || GetModuleHandleW(L"CrimsonDesert.exe") != main) return false;
    const auto read = [main](uintptr_t at, void* out, size_t bytes) noexcept {
        MEMORY_BASIC_INFORMATION region{};
        if (!out || !bytes || !VirtualQuery(reinterpret_cast<const void*>(at), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || region.Type != MEM_IMAGE || region.AllocationBase != main ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(region.BaseAddress);
        constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(region.Protect & readable) || at < begin || at - begin > region.RegionSize ||
            bytes > region.RegionSize - (at - begin)) return false;
        __try { std::memcpy(out, reinterpret_cast<const void*>(at), bytes); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return true;
    };
    return saf_host::CompatibleImage(reinterpret_cast<uintptr_t>(main), read);
}

// ============================================================
//  INI READING
// ============================================================
static std::string Trim(std::string value) {
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }

    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

static bool EqualsIgnoreCase(const std::string& a, const char* b) {
    if (b == nullptr) {
        return false;
    }
    const size_t len = strlen(b);
    if (a.size() != len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static std::string ReadIniString(const char* section, const char* key, const char* def) {
    std::ifstream file(g_iniPath);
    if (!file) {
        return def ? std::string(def) : std::string();
    }

    const std::string wantedSection = section ? section : "";
    const std::string wantedKey = key ? key : "";
    bool inWantedSection = wantedSection.empty();
    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            const std::string currentSection = Trim(line.substr(1, line.size() - 2));
            inWantedSection = EqualsIgnoreCase(currentSection, wantedSection.c_str());
            continue;
        }

        if (!inWantedSection) {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string currentKey = Trim(line.substr(0, equals));
        if (!EqualsIgnoreCase(currentKey, wantedKey.c_str())) {
            continue;
        }

        return Trim(line.substr(equals + 1));
    }

    return def ? std::string(def) : std::string();
}

static float ReadIniFloat(const char* section, const char* key, float def) {
    const std::string value = ReadIniString(section, key, "");
    return !value.empty() ? static_cast<float>(atof(value.c_str())) : def;
}

static bool ReadIniBool(const char* section, const char* key, bool def) {
    const std::string value = ReadIniString(section, key, "");
    if (value.empty()) return def;
    return (EqualsIgnoreCase(value, "true") || EqualsIgnoreCase(value, "1") ||
            EqualsIgnoreCase(value, "yes") || EqualsIgnoreCase(value, "on"));
}

static bool GetFileModTime(ULONGLONG* outTime) {
    WIN32_FILE_ATTRIBUTE_DATA fi;
    if (!GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &fi)) return false;
    ULARGE_INTEGER uli;
    uli.LowPart = fi.ftLastWriteTime.dwLowDateTime;
    uli.HighPart = fi.ftLastWriteTime.dwHighDateTime;
    *outTime = uli.QuadPart;
    return true;
}

// ============================================================
//  IMAGE VALIDATION + NATIVE DISCOVERY
// ============================================================
static HMODULE GetCrimsonDesertModule() {
    return GetModuleHandleW(L"CrimsonDesert.exe");
}

static bool AddressInMainImage(uintptr_t address, size_t bytes) {
    return address >= g_gameBase && address - g_gameBase < g_gameSize &&
           bytes <= g_gameSize - (address - g_gameBase);
}

static bool IsReadableMemory(const void* address, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !bytes || !VirtualQuery(address, &mbi, sizeof(mbi)) ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t at = reinterpret_cast<uintptr_t>(address);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & readable) && at >= begin && at - begin < mbi.RegionSize &&
           bytes <= mbi.RegionSize - (at - begin);
}

static unsigned long long RvaOf(const void* address) {
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - g_gameBase);
}

[[maybe_unused]] static bool IsAllNops(const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; ++i) if (bytes[i] != 0x90) return false;
    return true;
}

static bool QueryMainImageHeaders() {
    const HMODULE module = GetCrimsonDesertModule();
    MODULEINFO info{};
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return false;
    g_gameBase = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    g_gameSize = info.SizeOfImage;
    if (!g_gameBase || !g_gameSize || g_gameSize > UINTPTR_MAX - g_gameBase) return false;
    g_image = {};
    g_image.bytes = reinterpret_cast<const uint8_t*>(g_gameBase);
    g_image.size = g_gameSize;
    if (!IsReadableMemory(g_image.bytes, sizeof(IMAGE_DOS_HEADER))) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_image.bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !AddressInMainImage(g_gameBase + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS64))) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_image.bytes + dos->e_lfanew);
    if (!IsReadableMemory(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || nt->OptionalHeader.SizeOfImage != g_gameSize) return false;
    return true;
}
static bool QueryMainImage() {
    if (!QueryMainImageHeaders()) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_image.bytes);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_image.bytes + dos->e_lfanew);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    const size_t sectionBytes = sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION ||
        !AddressInMainImage(reinterpret_cast<uintptr_t>(sections), sectionBytes) ||
        !IsReadableMemory(sections, sectionBytes)) return false;
    const auto& exceptions = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    g_image.exceptions = {exceptions.VirtualAddress, exceptions.Size};
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const size_t begin = section.VirtualAddress;
        const size_t size = section.Misc.VirtualSize;
        if (begin >= g_gameSize || size > g_gameSize - begin) return false;
        const size_t end = begin + size;
        for (size_t cursor = begin; cursor < end;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(g_image.bytes + cursor, &mbi, sizeof(mbi))) return false;
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            const size_t next = (std::min)(end, static_cast<size_t>(regionEnd - g_gameBase));
            if (next <= cursor) return false;
            if (mbi.Type == MEM_IMAGE && mbi.AllocationBase == reinterpret_cast<void*>(g_gameBase) &&
                IsReadableMemory(g_image.bytes + cursor, next - cursor)) {
                g_image.readable.push_back({cursor, next - cursor});
                constexpr DWORD execute = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && (mbi.Protect & execute)) {
                    g_image.code.push_back({cursor, next - cursor});
                } else if ((section.Characteristics & IMAGE_SCN_MEM_WRITE) &&
                           (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) {
                    g_image.writable.push_back({cursor, next - cursor});
                }
            }
            cursor = next;
        }
    }
    return !g_image.code.empty();
}

static void BindFloat(const char* name, const native::Binding& binding, FloatPatchSite& site) {
    Logf("%s discovery matches=%u valid=%s instruction_rva=0x%llX target_rva=0x%llX",
         name, static_cast<unsigned>(binding.matches), binding.valid ? "true" : "false",
         static_cast<unsigned long long>(binding.instruction),
         static_cast<unsigned long long>(binding.target));
    if (!binding.valid) return;
    auto* target = reinterpret_cast<float*>(g_gameBase + binding.target);
    if (!IsReadableMemory(target, sizeof(float)) || !std::isfinite(*target) || *target <= 0) {
        Logf("WARNING: %s rejected nonpositive/nonfinite/unreadable native value", name);
        return;
    }
    site.address = target;
    site.original = *target;
    Logf("%s bound native_value=%g", name, site.original);
}

#define SAF_APPLY_RECOVERY_AVAILABLE 1
struct ApplyConfig {
 bool enabled=true,straight=true,instant=true;float range=kFallbackRange,pull=kFallbackPullSpeed;
 bool operator==(const ApplyConfig&b)const{return enabled==b.enabled&&straight==b.straight&&instant==b.instant&&range==b.range&&pull==b.pull;}
};
struct ConfigSnapshot{ApplyConfig config;ULONGLONG stamp=0;bool exists=false;};
static constexpr unsigned kRangeBit=1,kPullBit=2,kStraightBit=4,kInstantBit=8,kAllFeatureBits=15;
struct RetryContract{const uint8_t*address=nullptr;std::vector<uint8_t>expected;unsigned mask=0;};
static std::vector<RetryContract>g_retryContracts;
static unsigned g_retryContractReady=0;
struct RecoveryState{
 ApplyConfig desired{};ULONGLONG stamp=0;bool exists=false,initialized=false;unsigned generation=0,pending=0,readFailures=0;
 unsigned attempts[4]{};ULONGLONG due[4]{};
};
static RecoveryState g_recovery;
struct CodeApplyReport{unsigned retryMask=0;bool retryable=false;quiesced_patch::Report transaction{};};
#ifdef SAF_APPLY_RECOVERY_TESTING
static const quiesced_patch::Ops*g_testPatchOps=nullptr;
#endif
[[maybe_unused]] static void AddRetryContract(unsigned mask,const uint8_t*address,size_t size){
 if(!size||!AddressInMainImage(reinterpret_cast<uintptr_t>(address),size)||!IsReadableMemory(address,size)){g_retryContractReady&=~mask;return;}
 g_retryContracts.push_back({address,std::vector<uint8_t>(address,address+size),mask});
}
static bool BuildRetryChecks(unsigned mask,std::vector<std::vector<uint8_t>>&storage,std::vector<quiesced_patch::Check>&checks){
 if((g_retryContractReady&mask)!=mask)return false;
 const std::vector<BytePatchSite>*groups[]={&g_scopedRangeSites,&g_scopedPullSites,&g_angularSites,&g_instantAxiomSites};
 storage.reserve(g_retryContracts.size());checks.reserve(g_retryContracts.size());
 for(const auto&c:g_retryContracts)if(c.mask&mask){
  if(!AddressInMainImage(reinterpret_cast<uintptr_t>(c.address),c.expected.size())||!IsReadableMemory(c.address,c.expected.size()))return false;
  storage.push_back(c.expected);auto&expected=storage.back();
  for(size_t at=0;at<expected.size();++at){
   const auto address=reinterpret_cast<uintptr_t>(c.address+at);
#ifdef SAF_GAME_RELEASE
   AccountForCoreHook(address,expected[at]);
#endif
   for(const auto*group:groups)for(const auto&site:*group){
    const auto begin=reinterpret_cast<uintptr_t>(site.address);
    if(site.owned&&site.canRestore&&site.address&&site.size<=16&&address>=begin&&address-begin<site.size)expected[at]=site.patch[address-begin];
   }
  }
  if(!quiesced_patch::SameBytes(c.address,expected.data(),expected.size()))return false;
  checks.push_back({c.address,expected.data(),expected.size()});
 }
 return !checks.empty();
}
static bool RetryContractsValid(unsigned mask){
 std::vector<std::vector<uint8_t>>storage;std::vector<quiesced_patch::Check>checks;return BuildRetryChecks(mask,storage,checks);
}
#if defined(SAF_NEAR_ALLOCATION_TESTING)
static void CaptureRetryContracts(const native::Result&n){
 g_retryContracts.clear();g_retryContractReady=kAllFeatureBits;
 const auto pattern=[&](unsigned mask,const native::Binding&binding,const char*text){
  const auto bytes=native::ParsePattern(text);
  if(!binding.valid||!g_image.Code(binding.instruction,bytes.size())){g_retryContractReady&=~mask;return;}
  const auto*address=g_image.bytes+binding.instruction;
  for(size_t i=0;i<bytes.size();++i)if(bytes[i]>=0&&address[i]!=bytes[i]){g_retryContractReady&=~mask;return;}
  AddRetryContract(mask,address,bytes.size());
 };
 pattern(kRangeBit,n.range,native::kRange);pattern(kRangeBit,n.limit,native::kLimit);
 pattern(kRangeBit,n.scopedRange[1].binding,native::kCandidateRange);pattern(kRangeBit,n.scopedRange[2].binding,native::kLeashRange);
 pattern(kPullBit,n.pull,native::kPull);
 pattern(kInstantBit,n.activation.binding,native::kActivation);pattern(kInstantBit,n.clock[0],native::kReelClock);pattern(kInstantBit,n.clock[1],native::kPropellClock);
 pattern(kInstantBit,n.reel.binding,native::kReel);pattern(kInstantBit,n.propell.binding,native::kPropell);
 if(!g_angularReady){g_retryContractReady&=~kStraightBit;return;}
 const auto angular=[&](uint32_t rva,size_t count){AddRetryContract(kStraightBit,g_image.bytes+rva,count);};
 angular(straight_pull_angular::kHelperRva,sizeof(straight_pull_angular::kHelperPrologue));
 angular(straight_pull_angular::kSiteRva,sizeof(straight_pull_angular::kOriginal));
 angular(straight_pull_angular::kResumeRva,sizeof(straight_pull_angular::kQuaternionCall));
 angular(0x137E32C,sizeof(straight_pull_angular::kFirstArguments));angular(0x137E40C,sizeof(straight_pull_angular::kSecondArguments));
 for(uint32_t ret:{straight_pull_angular::kSelectedReturnRvas[0],straight_pull_angular::kSelectedReturnRvas[1],uint32_t(0x137FA43)})angular(ret-5,5);
}
#endif
[[maybe_unused]] static bool ReadConfigSnapshot(ConfigSnapshot&out){
 ULONGLONG before=0,after=0;const bool existed=GetFileModTime(&before);
 ApplyConfig c{};c.enabled=ReadIniBool(kIniSection,"Enabled",true);
 c.pull=ReadIniFloat(kIniSection,"PullSpeed",ReadIniFloat(kIniSection,"PullNorm",kFallbackPullSpeed));
 c.straight=ReadIniBool(kIniSection,"StraightPull",true);c.instant=ReadIniBool(kIniSection,"InstantAxiom",true);
 const bool exists=GetFileModTime(&after);
 if(existed!=exists||before!=after)return false;out={c,after,exists};return true;
}

static void BindFeatureSites(const native::Result& result, size_t angularSite,
                             const std::array<size_t, 2>& selectedReturns) {
    BindFloat("Range", result.range, g_range);
    BindFloat("AxiomLimitRange", result.limit, g_limit);
    BindFloat("PullSpeed", result.pull, g_pull);
    const bool supportReady = InitializeSupport(result, angularSite, selectedReturns);
    g_patchContract = {};
    if (supportReady) {
        for (size_t i = 0; i < result.scopedRange.size(); ++i)
            g_patchContract[i] = {static_cast<uint32_t>(result.scopedRange[i].offset), 4, 0, result.scopedRange[i].original};
        g_patchContract[6] = {static_cast<uint32_t>(result.scopedPull.offset), 4, 1, result.scopedPull.original};
        g_patchContract[7] = {static_cast<uint32_t>(angularSite), 15, 2, {}};
        g_patchContract[8] = {static_cast<uint32_t>(result.activation.offset), 6, 3, result.activation.original};
        g_patchContract[9] = {static_cast<uint32_t>(result.clock[0].instruction), 10, 4, {}};
        g_patchContract[10] = {static_cast<uint32_t>(result.clock[1].instruction), 9, 5, {}};
        std::memcpy(g_patchContract[7].original.data(), straight_pull_angular::kOriginal, 15);
        std::memcpy(g_patchContract[9].original.data(), clock_acceleration::kReelAdd, 10);
        std::memcpy(g_patchContract[10].original.data(), clock_acceleration::kPropellAdd, 9);
    }
    if (supportReady && g_configuredRange) g_range.address = g_configuredRange;
    if (supportReady && g_configuredPull) g_pull.address = g_configuredPull;
    const bool scopedRangeReady = supportReady && std::all_of(result.scopedRange.begin(), result.scopedRange.end(),
        [](const native::CodePatch& p) { return p.binding.valid; });
    const char* rangeNames[] = {"acquisition-limit", "candidate-distance", "attached-leash-limit", "query-range", "acquisition-range", "attached-leash-range"};
    for (size_t i = 0; i < result.scopedRange.size(); ++i) {
        const auto& patch = result.scopedRange[i];
        Logf("ScopedRange %s matches=%u valid=%s field_rva=0x%llX",
            rangeNames[i], static_cast<unsigned>(patch.binding.matches), patch.binding.valid ? "true" : "false",
            static_cast<unsigned long long>(patch.offset));
        if (!scopedRangeReady) continue;
        BytePatchSite site{}; site.name = rangeNames[i]; site.address = reinterpret_cast<uint8_t*>(g_gameBase + patch.offset);
        site.size = patch.size; site.canRestore = patch.canRestore;
        memcpy(site.original, patch.original.data(), patch.size);
        const auto displacement = reinterpret_cast<intptr_t>(g_configuredRange) - reinterpret_cast<intptr_t>(site.address + 4);
        if (displacement < INT32_MIN || displacement > INT32_MAX) { g_scopedRangeSites.clear(); break; }
        const int32_t encoded = static_cast<int32_t>(displacement); memcpy(site.patch, &encoded, 4);
        // Both original native256 operands and common20 operands are restorable.
        site.canRestore = true;
        g_scopedRangeSites.push_back(site);
    }

    if (supportReady && result.scopedPull.binding.valid) {
        BytePatchSite site{}; site.name = "axiom-pull-duration";
        site.address = reinterpret_cast<uint8_t*>(g_gameBase + result.scopedPull.offset);
        site.size = 4; site.canRestore = true; memcpy(site.original, result.scopedPull.original.data(), 4);
        const auto displacement = reinterpret_cast<intptr_t>(g_configuredPull) - reinterpret_cast<intptr_t>(site.address + 4);
        if (displacement >= INT32_MIN && displacement <= INT32_MAX) {
            const int32_t encoded = static_cast<int32_t>(displacement); memcpy(site.patch, &encoded, 4);
            g_scopedPullSites.push_back(site);
        }
    }
#ifndef SAF_GAME_RELEASE
    if (supportReady && g_angularReady) {
#ifdef SAF_OWNERSHIP_TESTING
        if (g_testBeforeAngularSnapshot) g_testBeforeAngularSnapshot();
#endif
        BytePatchSite site{}; site.name = "scoped-angular-step";
        site.address = reinterpret_cast<uint8_t*>(g_gameBase + angularSite);
        site.size = 15; site.canRestore = true; memcpy(site.original, straight_pull_angular::kOriginal, 15); memset(site.patch, 0x90, 15);
        const auto displacement = reinterpret_cast<intptr_t>(g_supportAllocation + 0x400) - reinterpret_cast<intptr_t>(site.address + 15);
        if (displacement >= INT32_MIN && displacement <= INT32_MAX) {
            const int32_t encoded = static_cast<int32_t>(displacement); site.patch[10] = 0xE8; memcpy(site.patch + 11, &encoded, 4);
            g_angularSites.push_back(site);
            Log("StraightPull adapter ready; obstacle correction and radial/attachment work retain native behavior");
        }
    }
#endif
    const bool completeInstant = result.activation.binding.valid &&
                                 result.reel.binding.valid && result.propell.binding.valid &&
                                 result.reel.canRestore && result.propell.canRestore && supportReady
#ifndef SAF_GAME_RELEASE
                                 && g_clockSupportReady
#endif
                                 ;
    if (completeInstant) {
        BytePatchSite activation{}; activation.name = "activation-delay";
        activation.address = reinterpret_cast<uint8_t*>(g_gameBase + result.activation.offset);
        activation.size = result.activation.size; activation.canRestore = result.activation.canRestore;
        memcpy(activation.original, result.activation.original.data(), activation.size);
        memcpy(activation.patch, result.activation.patch.data(), activation.size);
        g_instantAxiomSites.push_back(activation);
#ifndef SAF_GAME_RELEASE
        for (size_t i = 0; i < 2; ++i) {
            BytePatchSite site{}; site.name = i == 0 ? "reel-native-clock" : "propell-native-clock";
            site.address = reinterpret_cast<uint8_t*>(g_gameBase + result.clock[i].instruction);
            site.size = i == 0 ? 10 : 9; site.canRestore = true;
            memcpy(site.original, i == 0 ? clock_acceleration::kReelAdd : clock_acceleration::kPropellAdd, site.size); memset(site.patch, 0x90, site.size);
            site.patch[site.size - 5] = 0xE8;
            const auto displacement = reinterpret_cast<intptr_t>(g_supportAllocation + i*0x200) - reinterpret_cast<intptr_t>(site.address + site.size);
            if (displacement < INT32_MIN || displacement > INT32_MAX) { g_instantAxiomSites.clear(); break; }
            const int32_t encoded = static_cast<int32_t>(displacement); memcpy(site.patch + site.size - 4, &encoded, 4);
            Logf("InstantAxiom native-clock bound rva=0x%llX size=%u; native duration/completion branches preserved",
                RvaOf(site.address), static_cast<unsigned>(site.size));
            g_instantAxiomSites.push_back(site);
        }
#endif
    }
#ifdef SAF_GAME_RELEASE
    const bool angularReady=g_angularReady, instantReady=completeInstant && g_clockSupportReady;
#else
    const bool angularReady=g_angularSites.size()==1, instantReady=g_instantAxiomSites.size()==3;
#endif
    Logf("Discovery summary: Range=%s Limit=%s PullSpeed=%s StraightPull=%s InstantAxiom=%s",
         g_scopedRangeSites.size() == 6 ? "scoped-ready" : "unavailable", g_scopedRangeSites.size() == 6 ? "scoped-ready" : "unavailable",
         g_scopedPullSites.size() == 1 ? "scoped-ready" : "unavailable", angularReady ? "scoped-ready" : "unavailable",
         instantReady ? "ready" : "unavailable");
}

#if defined(SAF_NEAR_ALLOCATION_TESTING) || defined(SAF_OWNERSHIP_TESTING)
static void BindFeatureSites(const native::Result& result, const straight_pull::DiscoveryResult& straight) {
    if (straight.valid && straight.sites.size() == 2 &&
        straight_pull_angular::ValidateNative(g_image.bytes, g_image.size))
        BindFeatureSites(result, straight_pull_angular::kSiteRva,
            {straight_pull_angular::kSelectedReturnRvas[0], straight_pull_angular::kSelectedReturnRvas[1]});
}
#endif

static void DiscoverAddresses() {
    Log("Resolving native feature sites; discovery is not gameplay verification.");
    g_straightPullSites.clear();
    g_instantAxiomSites.clear();
    g_scopedRangeSites.clear();
    g_scopedPullSites.clear();
    g_angularSites.clear();
    g_range = {};
    g_limit = {};
    g_pull = {};
    if (!QueryMainImage()) {
        Log("ERROR: Unable to query readable executable game sections.");
        ReleaseEarlySupport(); return;
    }
    auto plan = saf_adaptive::Resolve(g_image, g_gameBase);
    if (plan.refusal) {
        Logf("Adaptive discovery refused: %s; no feature enabled", plan.refusal);
        ReleaseEarlySupport();
        return;
    }
    const auto& straight = plan.callers;
    Logf("StraightPull discovery valid=%s contexts=%u selected=%u ignored=%u reason=%s",
         straight.valid ? "true" : "false", static_cast<unsigned>(straight.contextMatches),
         static_cast<unsigned>(straight.sites.size()), static_cast<unsigned>(straight.ignoredContextCount),
         straight.reason.c_str());
    if (straight.valid) {
        for (const auto& candidate : straight.sites) {
            StraightPullSite site{};
            site.address = reinterpret_cast<uint8_t*>(g_gameBase + candidate.callOffset);
            memcpy(site.original, site.address, sizeof(site.original));
            site.canRestore = candidate.state == straight_pull::SiteState::Call;
            g_straightPullSites.push_back(site);
            Logf("StraightPull bound rva=0x%llX loop=%s can_restore=%s",
                 RvaOf(site.address), straight_pull::LoopKindName(candidate.loopKind),
                 site.canRestore ? "true" : "false");
        }
    }
    BindFeatureSites(plan.bindings, plan.angularSite, plan.selectedReturns);
    g_retryContracts.clear();
    g_retryContractReady = kAllFeatureBits;
    for (auto& witness : plan.witnesses)
        g_retryContracts.push_back({g_image.bytes + witness.rva, std::move(witness.bytes), witness.mask});
    g_adaptiveWitnessReady = true;
    Logf("Adaptive native owners proved: angular_rva=0x%llX selected_returns=[0x%llX,0x%llX] witnesses=%u",
        static_cast<unsigned long long>(plan.angularSite), static_cast<unsigned long long>(plan.selectedReturns[0]),
        static_cast<unsigned long long>(plan.selectedReturns[1]), static_cast<unsigned>(g_retryContracts.size()));
    ReleaseEarlySupport(); // Only unused, unregistered early storage can remain here.
}

// ============================================================
//  CORE: APPLY PATCHES
// ============================================================
static bool ApplyCodeSites(std::vector<BytePatchSite>& sites, const char* feature, bool enabled,CodeApplyReport* report=nullptr);
static bool ApplyStraightPull(bool enabled,CodeApplyReport* report=nullptr) {
#ifdef SAF_GAME_RELEASE
    if (enabled && !g_angularReady) {
        g_angularReady=PrepareCoreHooks(0,1);
        if (!g_angularReady && report) report->retryable=g_coreSites[0].callback!=nullptr;
    }
    if (enabled && (!g_angularReady || (report && report->retryMask &&
        !RetryContractsValid(report->retryMask)) || !EnableCoreHooks(0,1))) {
        g_straightPolicy.store(false,std::memory_order_release);
        Log("StraightPull SafetyHook refused; native angular path retained");
        return false;
    }
    g_straightPolicy.store(enabled,std::memory_order_release);
    return true;
#else
    return ApplyCodeSites(g_angularSites, "StraightPull angular-only", enabled,report);
#endif
}

static bool ApplyCodeSites(std::vector<BytePatchSite>& sites, const char* feature, bool enabled,CodeApplyReport* report) {
    if (sites.empty()) { Logf("WARNING: %s feature unavailable", feature); return false; }
    std::vector<quiesced_patch::Patch> writes; writes.reserve(sites.size());
    std::vector<BytePatchSite*> changing; changing.reserve(sites.size());
    std::vector<straight_pull::PatchOperation> operations; operations.reserve(sites.size());
    unsigned conflicts = 0;
    for (auto& site : sites) {
        if (!site.address || !site.size || site.size > sizeof(site.original) ||
            !AddressInMainImage(reinterpret_cast<uintptr_t>(site.address), site.size) || !IsReadableMemory(site.address, site.size)) {
            ++conflicts; continue;
        }
        auto state = straight_pull::RuntimeState::Other;
        if (site.canRestore && quiesced_patch::SameBytes(site.address, site.original, site.size)) state = straight_pull::RuntimeState::OriginalCall;
        else if (quiesced_patch::SameBytes(site.address, site.patch, site.size)) state = straight_pull::RuntimeState::Nopped;
        const auto operation = straight_pull::DecidePatchOperation(enabled, site.owned, site.canRestore, state);
        if (operation == straight_pull::PatchOperation::Conflict) { ++conflicts; continue; }
        if (operation == straight_pull::PatchOperation::None) continue;
#ifdef SAF_OWNERSHIP_TESTING
        if (g_testBeforeExpectedSnapshot) g_testBeforeExpectedSnapshot(&site);
#endif
        quiesced_patch::Patch write{}; write.address = site.address; write.size = site.size;
        memcpy(write.before, operation == straight_pull::PatchOperation::ApplyNops ? site.original : site.patch, site.size);
        memcpy(write.after, operation == straight_pull::PatchOperation::ApplyNops ? site.patch : site.original, site.size);
        writes.push_back(write); changing.push_back(&site); operations.push_back(operation);
    }
    // Enabling is a single complete group; disabling restores owned,
    // unconflicted sites while leaving an external conflict untouched.
    if (enabled && conflicts) { Logf("WARNING: %s group conflict; no code changed", feature); return false; }
    quiesced_patch::Report transaction{};
    const quiesced_patch::Ops* ops=nullptr;
#ifdef SAF_APPLY_RECOVERY_TESTING
    ops=g_testPatchOps;
#endif
    std::vector<std::vector<uint8_t>>contractStorage;std::vector<quiesced_patch::Check>contractChecks;
    if(report&&report->retryMask&&!BuildRetryChecks(report->retryMask,contractStorage,contractChecks)){
        Logf("WARNING: %s retry native contract changed; no code changed",feature);return false;
    }
    const bool success=writes.empty()||quiesced_patch::Write(writes.data(),writes.size(),ops?*ops:quiesced_patch::Ops{},&transaction,contractChecks.data(),contractChecks.size());
    bool exactBefore=true;
    if(!success)for(const auto&write:writes)if(!IsReadableMemory(write.address,write.size)||memcmp(write.address,write.before,write.size))exactBefore=false;
    if(report){report->transaction=transaction;report->retryable=conflicts==0&&exactBefore&&quiesced_patch::Recoverable(transaction);}
    if(!success||transaction.resumeFailures)Logf("Patch transaction feature=%s stage=%s error=%lu ntstatus=0x%08lX attempt=%u patch=%u thread=%lu unchanged=%d protection_restored=%d rollback_ok=%d all_resumed=%d resume_failures=%u resume_error=%lu rollback_error=%lu",
        feature,quiesced_patch::StageName(transaction.stage),transaction.error,transaction.status,transaction.attempts,transaction.patch,transaction.thread,
        exactBefore&&transaction.unchanged,transaction.protectionsRestored,transaction.rollbackOk,transaction.allResumed,transaction.resumeFailures,transaction.resumeError,transaction.rollbackError);
    if (success) for (size_t i = 0; i < changing.size(); ++i)
        changing[i]->owned = operations[i] == straight_pull::PatchOperation::ApplyNops;
    Logf("%s %s changed=%u conflicts=%u group_write=%s", feature, enabled ? "ENABLED" : "DISABLED",
        success ? static_cast<unsigned>(writes.size()) : 0, conflicts, success ? "ok" : exactBefore&&transaction.unchanged&&transaction.protectionsRestored&&transaction.rollbackOk ? "failed-unchanged" : "failed-indeterminate");
    return success && conflicts == 0;
}

static bool ApplyInstantAxiom(bool enabled,CodeApplyReport* report=nullptr) {
#ifdef SAF_GAME_RELEASE
    if (enabled && !g_clockSupportReady) {
        g_clockSupportReady=PrepareCoreHooks(1,3);
        if (!g_clockSupportReady && report) report->retryable=g_coreSites[1].callback!=nullptr;
    }
    if (enabled && (!g_clockSupportReady || (report && report->retryMask &&
        !RetryContractsValid(report->retryMask)) || !EnableCoreHooks(1,3))) {
        g_instantPolicy.store(false,std::memory_order_release);
        Log("InstantAxiom SafetyHook refused; native clock path retained");
        return false;
    }
    const bool applied=ApplyCodeSites(g_instantAxiomSites,"InstantAxiom",enabled,report);
    if (applied) g_instantPolicy.store(enabled,std::memory_order_release);
    return applied;
#else
    return ApplyCodeSites(g_instantAxiomSites, "InstantAxiom", enabled,report);
#endif
}

// ============================================================
//  OWNED FLOAT PATCHES
// ============================================================
static bool WriteAxiomFloat(const char* name, FloatPatchSite& site, bool enabled, float value) {
    if (!site.address || !(AddressInMainImage(reinterpret_cast<uintptr_t>(site.address), sizeof(float)) ||
            (g_configuredRange && site.address == g_configuredRange) ||
            (g_configuredPull && site.address == g_configuredPull)) ||
        !IsReadableMemory(site.address, sizeof(float))) {
        Logf("WARNING: %s feature unavailable", name);
        return false;
    }
    const float current = *site.address;
    if (!std::isfinite(current) || (enabled && (!std::isfinite(value) || value <= 0))) {
        Logf("WARNING: %s invalid native/config value", name);
        return false;
    }
    if (!enabled && !site.owned) return true;
    if (site.owned && current != site.lastWritten && current != site.original) {
        site.owned = false;
        Logf("WARNING: %s ownership conflict current=%g last_written=%g; leaving external value",
             name, current, site.lastWritten);
        return false;
    }
    const float requested = enabled ? value : site.original;
    if (current == requested) {
        if (!enabled) site.owned = false;
        return true;
    }
    // Never reacquire a changed external value after ownership was lost.
    if (!site.owned && current != site.original) {
        Logf("WARNING: %s changed since discovery; leaving external value", name);
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(site.address, sizeof(float), PAGE_READWRITE, &oldProtect)) {
        Logf("ERROR: %s write protection failed error=%lu", name, GetLastError());
        return false;
    }
    *site.address = requested;
    site.lastWritten = requested;
    site.owned = enabled;
    DWORD ignored = 0;
    const bool restoredProtection = VirtualProtect(site.address, sizeof(float), oldProtect, &ignored) != 0;
    Logf("%s %s rva=0x%llX previous=%g value=%g protection_restored=%s",
         name, enabled ? "applied" : "restored", RvaOf(site.address), current, requested,
         restoredProtection ? "true" : "false");
    return restoredProtection;
}

static bool ApplyAxiomRange(float value, bool enabled,CodeApplyReport* report=nullptr) {
    // The common Limit float has native physics consumers outside reach checks.
    // Redirect only the six proven geometric operands; never mutate that float.
    if (enabled && g_scopedRangeSites.size() != 6) {
        Log("WARNING: ScopedRange requires all six geometric consumers; leaving native reach intact");
        return false;
    }
    if (enabled) {
        if (!std::isfinite(value) || value <= 0 || value > 1.0e18f || !g_range.address ||
            !(AddressInMainImage(reinterpret_cast<uintptr_t>(g_range.address), sizeof(float)) ||
              (g_configuredRange && g_range.address == g_configuredRange)) ||
            !IsReadableMemory(g_range.address, sizeof(float))) return false;
        const float current = *g_range.address;
        if (!std::isfinite(current) ||
            (g_range.owned ? current != g_range.original && current != g_range.lastWritten
                           : current != g_range.original)) return false;
        // Validate the complete group before the first instruction mutation.
        for (const auto& site : g_scopedRangeSites) {
            if (!site.address || site.size != 4 ||
                !AddressInMainImage(reinterpret_cast<uintptr_t>(site.address), site.size) ||
                !IsReadableMemory(site.address, site.size) ||
                !((site.canRestore && memcmp(site.address, site.original, site.size) == 0) ||
                  memcmp(site.address, site.patch, site.size) == 0)) return false;
        }
    }
    const bool redirectsOk = ApplyCodeSites(g_scopedRangeSites, "ScopedRange", enabled,report);
    const bool rangeOk = !enabled || redirectsOk
        ? WriteAxiomFloat("Range", g_range, enabled, value) : false;
    return rangeOk && redirectsOk;
}

static bool ApplyAxiomPullSpeed(float value, bool enabled,CodeApplyReport* report=nullptr) {
    if (enabled && (!std::isfinite(value) || value <= 0 || g_scopedPullSites.size() != 1)) return false;
    if (enabled) {
        if (!g_pull.address || !IsReadableMemory(g_pull.address, sizeof(float))) return false;
        const float current = *g_pull.address;
        if (!std::isfinite(current) || (g_pull.owned ? current != g_pull.original && current != g_pull.lastWritten
                                                   : current != g_pull.original)) return false;
    }
    const bool codeOk = ApplyCodeSites(g_scopedPullSites, "ScopedPullSpeed", enabled,report);
    return (!enabled || codeOk) && WriteAxiomFloat("PullSpeed", g_pull, enabled, value) && codeOk;
}

// ============================================================
//  CONFIG LOAD + APPLY
// ============================================================
#ifndef SAF_EMBEDDED
static void LoadAndApply();
#endif
static const char*const kFeatureNames[]={"ScopedRange","ScopedPullSpeed","StraightPull","InstantAxiom"};
// This bounds the native collision query. The camera ray and the engine's
// validated surface determine the actual tether point.
static constexpr float kNativeAimBudget=50000.f;
// Verified stock divisor at the game 3663 pull-duration load.
static constexpr float kNativeTravelSpeed=40.f;

static float EffectiveAxiomRange(const ApplyConfig&) {
    return kNativeAimBudget;
}

static bool ApplyConfiguredFeature(unsigned index,const ApplyConfig& config,CodeApplyReport& report) {
    const bool straight=config.enabled && config.straight;
    switch (index) {
    case 0:
        return ApplyAxiomRange(EffectiveAxiomRange(config),config.enabled,&report);
    case 1: {
        // The operand is the divisor of anchor distance when the game builds
        // the pull duration. Keep the stock maximum while native steering
        // needs time to sweep and correct. Lower configured values still apply.
        const float safeSpeed=straight && std::isfinite(config.pull) ?
            (std::min)(config.pull,kNativeTravelSpeed) : config.pull;
        if (straight && safeSpeed<config.pull)
            Logf("StraightPull travel speed bounded: requested=%.3f effective=%.3f (native travel duration)",
                 config.pull,safeSpeed);
        return ApplyAxiomPullSpeed(safeSpeed,config.enabled,&report);
    }
    case 2: {
        if (straight && g_clockSupportReady) InterlockedExchange(g_keepTravelClock,1);
        const bool applied=ApplyStraightPull(straight,&report);
        if (applied && g_clockSupportReady) {
            InterlockedExchange(g_keepTravelClock,straight?1:0);
            if (straight) Log("StraightPull uses the native travel clock and preserves instant tether launch.");
        }
        return applied;
    }
    default:
#ifdef SAF_GAME_RELEASE
        if (g_keepTravelClock) InterlockedExchange(g_keepTravelClock,straight?1:0);
#endif
        return ApplyInstantAxiom(config.enabled&&config.instant,&report);
    }
}
static void PublishOwnedState(){
 auto&d=SAFOwnedPatchStateV1;d.gameBase=g_gameBase;d.support=reinterpret_cast<uintptr_t>(g_supportAllocation);d.ownedMask=0;
#ifdef SAF_GAME_RELEASE
 // The V1 external contract describes game byte ownership. The SafetyHook
 // angular and clock callbacks sit after those legacy instruction spans.
 // A missing optional timing hook owns no activation bytes. It cannot
 // invalidate a separately verified range group.
 if(g_scopedRangeSites.size()!=6 || g_scopedPullSites.size()!=1 || g_instantAxiomSites.size()>1)
  g_compatPoisoned=true;
 for(size_t i=0;i<g_scopedRangeSites.size();++i)
  if(g_scopedRangeSites[i].owned)d.ownedMask|=1u<<i;
 if(g_scopedPullSites.size()==1 && g_scopedPullSites[0].owned)d.ownedMask|=1u<<6;
 if(g_instantAxiomSites.size()==1 && g_instantAxiomSites[0].owned)d.ownedMask|=1u<<8;
#else
 const std::vector<BytePatchSite>*groups[]={&g_scopedRangeSites,&g_scopedPullSites,&g_angularSites,&g_instantAxiomSites};
 const bool complete=g_clockSupportReady&&g_angularReady&&g_scopedRangeSites.size()==6&&g_scopedPullSites.size()==1&&g_angularSites.size()==1&&g_instantAxiomSites.size()==3;
 size_t i=0;for(const auto*group:groups)for(const auto&site:*group){
  if(i>=g_patchContract.size()){g_compatPoisoned=true;continue;}
  const auto&spec=g_patchContract[i];uint8_t expected[16]{};
  if(RvaOf(site.address)!=spec.rva||site.size!=spec.size||!site.canRestore||memcmp(site.original,spec.original.data(),spec.size)||!safcompat::ExpectedPatch(spec,g_gameBase,d.support,expected)||memcmp(expected,site.patch,spec.size))g_compatPoisoned=true;
  if(site.owned)d.ownedMask|=1u<<i;++i;
 }
 if(!complete||i!=g_patchContract.size())g_compatPoisoned=true;
#endif
 d.rangeBits=d.pullBits=0;if(g_configuredRange)memcpy(&d.rangeBits,g_configuredRange,4);if(g_configuredPull)memcpy(&d.pullBits,g_configuredPull,4);
 if((g_range.owned&&(!g_configuredRange||g_range.address!=g_configuredRange||memcmp(g_configuredRange,&g_range.lastWritten,4)))||(g_pull.owned&&(!g_configuredPull||g_pull.address!=g_configuredPull||memcmp(g_configuredPull,&g_pull.lastWritten,4))))g_compatPoisoned=true;
 d.status=static_cast<uint32_t>(g_compatPoisoned?safcompat::Status::Poison:safcompat::Status::Ready);
}
struct OwnedWriteEpoch {
 bool acquired=false;
 OwnedWriteEpoch(){acquired=safcompat::BeginWrite(SAFOwnedPatchStateV1);}
 ~OwnedWriteEpoch(){if(acquired){if(std::uncaught_exceptions())g_compatPoisoned=true;PublishOwnedState();safcompat::EndWrite(SAFOwnedPatchStateV1);}}
};
static bool ApplyBatch(const ApplyConfig&c,unsigned mask,ULONGLONG now,bool retry){
 OwnedWriteEpoch ownedEpoch;
 if(!ownedEpoch.acquired){
  // No wait and no suspended threads. The existing bounded recovery service
  // retries from an unchanged state; readers never wait on this writer.
  for(unsigned i=0;i<4;++i)if(mask&(1u<<i)){
   if(retry)++g_recovery.attempts[i];
   if(g_recovery.attempts[i]<4){g_recovery.pending|=1u<<i;g_recovery.due[i]=now+(1000ULL<<g_recovery.attempts[i]);}
   else g_recovery.pending&=~(1u<<i);
  }
  Log("Apply deferred: owned-contract reader active; no patch attempted");return false;
 }
 bool complete=true;
 for(unsigned i=0;i<4;++i){const unsigned bit=1u<<i;if(!(mask&bit))continue;
  if(retry){
   if(!RetryContractsValid(bit)){g_recovery.pending&=~bit;Logf("Apply recovery stopped feature=%s generation=%u reason=native-contract-changed; no write",kFeatureNames[i],g_recovery.generation);complete=false;continue;}
   ++g_recovery.attempts[i];
   Logf("Apply recovery attempt feature=%s generation=%u retry=%u/4",kFeatureNames[i],g_recovery.generation,g_recovery.attempts[i]);
  }
  const bool enabling = c.enabled && (i < 2 || (i == 2 ? c.straight : c.instant));
  CodeApplyReport report{};report.retryMask=retry||(g_adaptiveWitnessReady&&enabling)?bit:0;const bool ok=ApplyConfiguredFeature(i,c,report);complete=complete&&ok;g_recovery.pending&=~bit;
  const auto&tx=report.transaction;if(tx.resumeFailures||!tx.protectionsRestored||!tx.rollbackOk||(tx.stage!=quiesced_patch::Stage::None&&!tx.allResumed)||(!ok&&!tx.unchanged))g_compatPoisoned=true;
  if(!ok&&report.retryable){
   if(g_recovery.attempts[i]<4){const ULONGLONG delay=1000ULL<<g_recovery.attempts[i];
    g_recovery.pending|=bit;g_recovery.due[i]=now+delay;
    Logf("Apply recovery pending feature=%s generation=%u used=%u/4 delay_ms=%llu",kFeatureNames[i],g_recovery.generation,g_recovery.attempts[i],delay);
   }else Logf("Apply recovery exhausted feature=%s generation=%u retries=4; awaiting configuration change",kFeatureNames[i],g_recovery.generation);
  }else if(retry)Logf("Apply recovery %s feature=%s generation=%u retry=%u/4",ok?"complete":"stopped",kFeatureNames[i],g_recovery.generation,g_recovery.attempts[i]);
 }
 return complete;
}
[[maybe_unused]] static void ServiceMenuRange() {
    if (!g_recovery.initialized || !g_recovery.desired.enabled || g_compatPoisoned ||
        (g_recovery.pending&kRangeBit) || g_scopedRangeSites.size()!=6 ||
        !std::all_of(g_scopedRangeSites.begin(),g_scopedRangeSites.end(),
                     [](const BytePatchSite& site){return site.owned;})) return;
    const auto& config=g_recovery.desired;
    const auto desired=EffectiveAxiomRange(config);
    if (desired==g_range.lastWritten) return;

    OwnedWriteEpoch epoch;
    if (!epoch.acquired) return;
    // Only the SAF worker writes its scalar and publishes the descriptor.
    const auto currentRequest=EffectiveAxiomRange(config);
    if (currentRequest!=g_range.lastWritten && ApplyAxiomRange(currentRequest,true))
        Logf("Menu Reach effective=%g saved=%g",currentRequest,config.range);
}

#ifndef SAF_EMBEDDED
#include "../UnifiedTargeting.h"

static bool g_targetingDisablePending=false;

static void ServiceUnifiedTargeting() {
    safcompat::Lease lease;
    if (lease.Acquire(&SAFOwnedPatchStateV1)!=safcompat::Result::Valid) return;
    const auto& owned=lease.state;
    constexpr uint32_t rangeSites=0x3F;
#ifndef SAF_GAME_RELEASE
    constexpr uint32_t straightPullSite=0x80;
#endif
    saf_targeting::State state{};
    if (!g_targetingDisablePending && owned.status==static_cast<uint32_t>(safcompat::Status::Ready) &&
        (owned.ownedMask&rangeSites)==rangeSites) {
        state.support=owned.support;
#ifdef SAF_GAME_RELEASE
        state.straightPull=g_straightPolicy.load(std::memory_order_acquire);
#else
        state.straightPull=(owned.ownedMask&straightPullSite)!=0;
#endif
        std::memcpy(&state.range,&owned.rangeBits,sizeof(state.range));
    }
    static auto previous=saf_targeting::Result::Inactive;
    const auto result=saf_targeting::Sync(state);
    if (previous==result) return;
    previous=result;
    switch (result) {
    case saf_targeting::Result::Active:
        Log("Integrated targeting active: native confirmation and tether distance inputs verified.");
        break;
    case saf_targeting::Result::Inactive:
        Log("Integrated targeting inactive: extra native distance references restored.");
        break;
    case saf_targeting::Result::Retry:
        Log("Integrated targeting transaction pending.");
        break;
    case saf_targeting::Result::Unavailable:
        Log("Integrated targeting unavailable for this game or ownership state.");
        break;
    case saf_targeting::Result::Conflict:
        Log("Integrated targeting refused a changed native span. Existing SAF features remain subject to their own checks.");
        break;
    }
}

static void ProcessPendingApply(ULONGLONG now){
 if(!g_recovery.initialized||!g_recovery.pending)return;
 for(unsigned i=0;i<4;++i){
  const unsigned bit=1u<<i;if(!(g_recovery.pending&bit)||now<g_recovery.due[i])continue;
  ConfigSnapshot fresh{};
  if(!ReadConfigSnapshot(fresh)){if(++g_recovery.readFailures>=4){g_recovery.pending=0;Log("Apply recovery stopped: configuration changed during four bounded snapshot reads; no write");}return;}
  g_recovery.readFailures=0;
  if(!(fresh.config==g_recovery.desired)||fresh.stamp!=g_recovery.stamp||fresh.exists!=g_recovery.exists){
   Log("Apply recovery observed configuration change; discarding old pending requests");LoadAndApply();return;
  }
  ApplyBatch(fresh.config,bit,now,true);
 }
}

static void LoadAndApply() {
    ConfigSnapshot snapshot{};
    if(!ReadConfigSnapshot(snapshot)){Log("WARNING: configuration changed during read; no patch attempted");return;}
    const auto& config=snapshot.config;
    const bool enabled=config.enabled,straightPull=config.straight,instantAxiom=config.instant;
    // Stop publishing anchors and restore the extra game inputs while the
    // producer range and its support page are still live.
    g_targetingDisablePending=!enabled;
    if (g_targetingDisablePending && saf_targeting::Sync({})==saf_targeting::Result::Retry) return;
    if(!g_recovery.initialized||!(config==g_recovery.desired)||snapshot.stamp!=g_recovery.stamp||snapshot.exists!=g_recovery.exists){
        const unsigned generation=g_recovery.generation+1;g_recovery={};g_recovery.initialized=true;g_recovery.generation=generation;
        g_recovery.desired=config;g_recovery.stamp=snapshot.stamp;g_recovery.exists=snapshot.exists;
    }
    g_lastIniModTime=snapshot.stamp;
    const std::string enabledRaw = ReadIniString(kIniSection, "Enabled", "<missing>");
    const std::string rangeRaw = ReadIniString(kIniSection, "Range", "<missing>");
    const std::string pullSpeedRaw = ReadIniString(kIniSection, "PullSpeed", "<missing>");
    const std::string straightPullRaw = ReadIniString(kIniSection, "StraightPull", "<missing>");
    const std::string instantAxiomRaw = ReadIniString(kIniSection, "InstantAxiom", "<missing>");

    char buf[512];
    WIN32_FILE_ATTRIBUTE_DATA iniInfo{};
    const bool iniExists = GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &iniInfo) != 0;

    sprintf_s(buf,
              "Config loaded: path=%s exists=%s Enabled=%s StraightPull=%s InstantAxiom=%s raw={Enabled:%s Range:%s PullSpeed:%s StraightPull:%s InstantAxiom:%s}",
              g_iniPath.c_str(),
              iniExists ? "true" : "false",
              enabled ? "true" : "false",
              straightPull ? "true" : "false",
              instantAxiom ? "true" : "false",
              enabledRaw.c_str(),
              rangeRaw.c_str(),
              pullSpeedRaw.c_str(),
              straightPullRaw.c_str(),
              instantAxiomRaw.c_str());
    Log(buf);

    const bool complete=ApplyBatch(config,kAllFeatureBits,GetTickCount64(),false);
    Logf("Apply summary: Enabled=%s result=%s pending_mask=0x%X generation=%u",
         enabled?"true":"false",complete?"complete":"partial",g_recovery.pending,g_recovery.generation);

}

// ============================================================
//  MAIN MOD THREAD
// ============================================================
static void ModMain() {
    const std::string moduleDir = GetModuleDirectory(g_module);
    const std::string hostDir = GetModuleDirectory(GetCrimsonDesertModule());
    g_iniPath = ResolveIniPath(hostDir, moduleDir);
    g_logPath = moduleDir + "\\" + kLogFile;

    {
        std::ofstream reset(g_logPath, std::ios::trunc);
    }

    Logf("Starting up build=%s module_dir=%s ini_path=%s", kBuildId, moduleDir.c_str(), g_iniPath.c_str());

    // Acquire only our own RW storage while nearby slots are still available.
    // Header reads require the same named game host; game code remains untouched.
    if (IsCrimsonDesertProcess() && QueryMainImageHeaders()) {
        PreallocateSupportForImage(g_gameBase,g_gameSize);
#ifdef SAF_GAME_RELEASE
        const auto pool=game_hooks::AllocatorFor(g_gameBase,g_gameSize);
        Log(pool?"SafetyHook game-near pool reserved before native discovery":
                 "SafetyHook early pool unavailable; discovery will attempt a fresh region search");
#endif
    }
    else Log("NearSupport early reservation skipped: game host/PE header validation failed");

    // Preserve the native discovery delay.
    const auto discoverAt=GetTickCount64()+15000;
    if (saf_targeting::Prepare(Log)) {
        if (GetModuleHandleW(L"DesertLinkAxiomCoexist.asi"))
            Log("Legacy Axiom bridge present. Unified SAF uses its local ownership contract.");
    } else Log("Integrated targeting skipped: current game image is unrecognized.");
    for (auto now=GetTickCount64(); now<discoverAt; now=GetTickCount64()) {
        Sleep(static_cast<DWORD>((std::min)(discoverAt-now,250ULL)));
    }

    DiscoverAddresses();

    if (!g_adaptiveWitnessReady) return;

    LoadAndApply();
    ServiceUnifiedTargeting();

    auto nextConfig=GetTickCount64()+1000;
    while (true) {
        Sleep(250);
        const auto now=GetTickCount64();
        if (now>=nextConfig) {
            nextConfig=now+1000;
            ULONGLONG currentModTime=0;
            if (GetFileModTime(&currentModTime) && currentModTime!=g_lastIniModTime) {
                Sleep(100);
                Log("INI changed - reloading.");
                LoadAndApply();
            } else ProcessPendingApply(now);
            ServiceMenuRange();
        }
        ServiceUnifiedTargeting();
    }
}

// ============================================================
//  DLL ENTRY POINT
// ============================================================
static DWORD WINAPI SafWorker(void* retained) {
    const auto module=static_cast<HMODULE>(retained);
    HMODULE pinned=nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(module),&pinned))
        FreeLibrary(module);
    // A failed pin keeps the strong reference for the lifetime of the hooks.
    ModMain();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        if (!IsCrimsonDesertProcess() || !GetCrimsonDesertModule()) {
            return TRUE;
        }

        g_module = hModule;

        bool expected = false;
        if (!g_isInitialized.compare_exchange_strong(expected, true)) {
            return TRUE;
        }

        HMODULE retained=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(hModule),&retained)) return TRUE;
        if (const auto thread=CreateThread(nullptr,0,SafWorker,retained,0,nullptr))
            CloseHandle(thread);
        else
            FreeLibrary(retained);
    }
    return TRUE;
}
#endif
