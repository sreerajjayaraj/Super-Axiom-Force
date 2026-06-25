// SuperAxiomForce - SuperAxiomForce.cpp
// Enhanced Axiom Force mod for Crimson Desert
//
// INSTALL: use Definitive Mod Manager; do not manually install into bin64.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

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

#pragma comment(lib, "psapi.lib")

// ============================================================
//  CONSTANTS
// ============================================================
static const char* kModName = "SuperAxiomForce";
static const char* kLogFile = "SuperAxiomForce.log";
static const char* kIniSection = "AxiomForce";
static HMODULE g_module = nullptr;
static std::string g_logPath;

// Fallback offsets for Crimson Desert 1.12/1.12.01 local builds.
// Range/PullSpeed still self-scan at runtime if these data RVAs move.
static const uintptr_t kAxiomRangeOffset_Fallback = 0x605FAD8;
static const uintptr_t kAxiomLimitRangeOffset_Fallback = kAxiomRangeOffset_Fallback - 0x50;
static const uintptr_t kAxiomPullSpeedOffset_Fallback = 0x6060618;
static const uintptr_t kAerialManeuverOffset_Fallback = 0xEEC8B0;
static const uintptr_t kActivationDelayOffset_Fallback = 0x4609ED;
static const uintptr_t kReelDurationOffset_Fallback = 0x461662;
static const uintptr_t kPropellDurationOffset_Fallback = 0x461D24;

static const float kDefaultRange = 20.0f;
static const float kFallbackRange = 500.0f;
static const float kDefaultPullSpeed = 40.0f;
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
    uint8_t original[2] = {};
    uint8_t patch[2] = {};
    bool owned = false;
};

static std::vector<BytePatchSite> g_instantAxiomSites;
static float* g_pAxiomRange = nullptr;
static float* g_pAxiomLimitRange = nullptr;
static float* g_pAxiomPullSpeed = nullptr;

// ============================================================
//  LOGGING
// ============================================================
static void Log(const char* msg) {
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
}

static void Logf(const char* format, ...) {
    char buffer[768];
    va_list args;
    va_start(args, format);
    vsprintf_s(buffer, format, args);
    va_end(args);
    Log(buffer);
}

static std::string GetModuleDirectory() {
    char modulePath[MAX_PATH] = {};
    if (g_module != nullptr && GetModuleFileNameA(g_module, modulePath, MAX_PATH) != 0) {
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

static bool IsCrimsonDesertProcess() {
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return false;
    }

    std::string path(exePath);
    const auto slash = path.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return _stricmp(name.c_str(), "CrimsonDesert.exe") == 0;
}

// ============================================================
//  INI READING
// ============================================================
static void FlushIniCache() {
    if (!g_iniPath.empty()) {
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, g_iniPath.c_str());
    }
}

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
//  IMAGE + PATTERN HELPERS
// ============================================================
static bool QueryMainImage() {
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi))) {
        return false;
    }

    g_gameBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    g_gameSize = static_cast<size_t>(mi.SizeOfImage);
    return g_gameBase != 0 && g_gameSize != 0;
}

static bool AddressInMainImage(const uintptr_t address, const size_t bytes) {
    if (g_gameBase == 0 || g_gameSize == 0 || bytes == 0) return false;
    if (address < g_gameBase) return false;

    const size_t offset = static_cast<size_t>(address - g_gameBase);
    return offset < g_gameSize && bytes <= (g_gameSize - offset);
}

static bool IsWritableMemory(const void* address, const size_t bytes) {
    if (address == nullptr || bytes == 0) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region_begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t region_end = region_begin + mbi.RegionSize;
    if (begin < region_begin || begin + bytes > region_end) {
        return false;
    }

    const DWORD writable =
        PAGE_READWRITE |
        PAGE_WRITECOPY |
        PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & writable) != 0;
}

static const char* ProtectName(const DWORD protect) {
    switch (protect & 0xFF) {
    case PAGE_NOACCESS: return "NOACCESS";
    case PAGE_READONLY: return "READONLY";
    case PAGE_READWRITE: return "READWRITE";
    case PAGE_WRITECOPY: return "WRITECOPY";
    case PAGE_EXECUTE: return "EXECUTE";
    case PAGE_EXECUTE_READ: return "EXECUTE_READ";
    case PAGE_EXECUTE_READWRITE: return "EXECUTE_READWRITE";
    case PAGE_EXECUTE_WRITECOPY: return "EXECUTE_WRITECOPY";
    default: return "UNKNOWN";
    }
}

static bool QueryMemoryForLog(const void* address, MEMORY_BASIC_INFORMATION& mbi) {
    memset(&mbi, 0, sizeof(mbi));
    return address != nullptr && VirtualQuery(address, &mbi, sizeof(mbi)) != 0;
}

static unsigned long long RvaOf(const void* address) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    return static_cast<unsigned long long>(g_gameBase != 0 && value >= g_gameBase ? value - g_gameBase : value);
}

static uint8_t* PatternScan(const uint8_t* pattern, const char* mask, size_t len) {
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi))) {
        return nullptr;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    const size_t size = static_cast<size_t>(mi.SizeOfImage);
    if (base == nullptr || len == 0 || size < len) {
        return nullptr;
    }

    for (size_t i = 0; i <= size - len; i++) {
        bool found = true;
        for (size_t j = 0; j < len; j++) {
            if (mask[j] == 'x' && base[i + j] != pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) return base + i;
    }
    return nullptr;
}

static bool IsReadableMemory(const void* address, const size_t bytes) {
    if (address == nullptr || bytes == 0) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t region_begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t region_end = region_begin + mbi.RegionSize;
    if (begin < region_begin || begin + bytes > region_end) {
        return false;
    }

    const DWORD readable =
        PAGE_READONLY |
        PAGE_READWRITE |
        PAGE_WRITECOPY |
        PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & readable) != 0;
}

static std::vector<uint8_t*> PatternScanAll(const uint8_t* pattern, const char* mask, size_t len);

static bool FindRangePullByInstructionPattern(float** outRange, float** outPull) {
    if (outRange == nullptr || outPull == nullptr || g_gameBase == 0 || g_gameSize == 0) {
        return false;
    }

    // Current Crimson Desert builds load the vanilla range through a RIP-relative
    // vmovss instruction. Decode that operand instead of searching for mutable
    // 20.0f/40.0f data bytes, which may be initialized later or moved by updates.
    static const uint8_t pattern[] = {
        0xC5, 0x7A, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00,
        0xC4, 0xC1, 0x78, 0x2F, 0xF8, 0x76, 0x00,
        0xC4, 0x41, 0x10, 0x57, 0xED,
    };
    static const char mask[] = "xxxx????xxxxxx?xxxxx";
    constexpr uintptr_t kKnownGap = 0xB40;

    const auto matches = PatternScanAll(pattern, mask, sizeof(pattern));
    if (matches.empty()) {
        Log("Range/PullSpeed instruction scan found 0 matches");
        return false;
    }
    if (matches.size() > 1) {
        Logf("Range/PullSpeed instruction scan ambiguous matches=%u", static_cast<unsigned>(matches.size()));
        return false;
    }

    uint8_t* instr = matches.front();
    int32_t rel = 0;
    memcpy(&rel, instr + 4, sizeof(rel));
    auto* range = reinterpret_cast<float*>(instr + 8 + rel);
    auto* pull = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(range) + kKnownGap);

    const uintptr_t rangeAddr = reinterpret_cast<uintptr_t>(range);
    const uintptr_t pullAddr = reinterpret_cast<uintptr_t>(pull);
    if (!AddressInMainImage(rangeAddr, sizeof(float)) ||
        !AddressInMainImage(pullAddr, sizeof(float)) ||
        !IsReadableMemory(range, sizeof(float)) ||
        !IsReadableMemory(pull, sizeof(float))) {
        Logf("Range/PullSpeed instruction scan resolved invalid target instr_rva=0x%llX range_rva=0x%llX pull_rva=0x%llX",
             static_cast<unsigned long long>(RvaOf(instr)),
             static_cast<unsigned long long>(rangeAddr >= g_gameBase ? rangeAddr - g_gameBase : 0),
             static_cast<unsigned long long>(pullAddr >= g_gameBase ? pullAddr - g_gameBase : 0));
        return false;
    }

    *outRange = range;
    *outPull = pull;
    Logf("Range/PullSpeed instruction binding enabled instr_rva=0x%llX range_rva=0x%llX pull_rva=0x%llX range_current=%g pull_current=%g",
         static_cast<unsigned long long>(RvaOf(instr)),
         static_cast<unsigned long long>(RvaOf(range)),
         static_cast<unsigned long long>(RvaOf(pull)),
         *range,
         *pull);
    return true;
}
static bool FindRangePullPair(float** outRange, float** outPull) {
    if (outRange == nullptr || outPull == nullptr || g_gameBase == 0 || g_gameSize == 0) {
        return false;
    }

    constexpr uintptr_t kKnownGap = 0xB40;
    const uintptr_t imageEnd = g_gameBase + g_gameSize;
    uintptr_t cursor = g_gameBase;
    unsigned candidates = 0;
    unsigned logged = 0;

    while (cursor < imageEnd) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0) {
            cursor += 0x1000;
            continue;
        }

        const uintptr_t regionBegin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBegin + mbi.RegionSize;
        const uintptr_t scanBegin = regionBegin < g_gameBase ? g_gameBase : regionBegin;
        const uintptr_t scanEnd = regionEnd > imageEnd ? imageEnd : regionEnd;

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & PAGE_GUARD) == 0 &&
            (mbi.Protect & PAGE_NOACCESS) == 0 &&
            IsWritableMemory(reinterpret_cast<void*>(scanBegin), sizeof(float)) &&
            scanEnd > scanBegin + kKnownGap + sizeof(float)) {
            for (uintptr_t address = scanBegin; address + kKnownGap + sizeof(float) <= scanEnd; address += sizeof(float)) {
                auto* range = reinterpret_cast<float*>(address);
                auto* pull = reinterpret_cast<float*>(address + kKnownGap);

                if (*range == kDefaultRange && *pull == kDefaultPullSpeed) {
                    ++candidates;
                    if (*outRange == nullptr && IsWritableMemory(pull, sizeof(float))) {
                        *outRange = range;
                        *outPull = pull;
                    }

                    if (logged < 8) {
                        ++logged;
                        char msg[224];
                        sprintf_s(msg,
                                  "Range/PullSpeed scan candidate #%u range_rva=0x%llX pull_rva=0x%llX writable=yes protect=0x%lX",
                                  candidates,
                                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(range) - g_gameBase),
                                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pull) - g_gameBase),
                                  mbi.Protect);
                        Log(msg);
                    }
                }
            }
        }

        cursor = regionEnd > cursor ? regionEnd : cursor + 0x1000;
    }

    char msg[96];
    sprintf_s(msg, "Range/PullSpeed scan complete candidates=%u", candidates);
    Log(msg);
    return *outRange != nullptr && *outPull != nullptr;
}

static std::vector<uint8_t*> PatternScanAll(const uint8_t* pattern, const char* mask, size_t len) {
    std::vector<uint8_t*> out;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi))) {
        return out;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    const size_t size = static_cast<size_t>(mi.SizeOfImage);
    if (base == nullptr || len == 0 || size < len) {
        return out;
    }

    for (size_t i = 0; i <= size - len; i++) {
        bool found = true;
        for (size_t j = 0; j < len; j++) {
            if (mask[j] == 'x' && base[i + j] != pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            out.push_back(base + i);
        }
    }

    return out;
}

static bool IsAllNops(const uint8_t* p, const size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (p[i] != 0x90) return false;
    }
    return true;
}

static void AddStraightPullSite(uint8_t* address) {
    if (address == nullptr) return;

    for (const auto& site : g_straightPullSites) {
        if (site.address == address) return;
    }

    StraightPullSite site{};
    site.address = address;
    memcpy(site.original, address, sizeof(site.original));
    site.canRestore = site.original[0] == 0xE8;
    site.owned = false;
    g_straightPullSites.push_back(site);

    char msg[192];
    sprintf_s(msg,
              "StraightPull site added rva=0x%llX bytes=%02X %02X %02X %02X %02X can_restore=%s",
              RvaOf(address),
              site.original[0],
              site.original[1],
              site.original[2],
              site.original[3],
              site.original[4],
              site.canRestore ? "true" : "false");
    Log(msg);
}

static void AddInstantAxiomSite(const char* name,
                                uint8_t* address,
                                const uint8_t expected0,
                                const uint8_t expected1,
                                const uint8_t patch0,
                                const uint8_t patch1) {
    if (address == nullptr) return;

    for (const auto& site : g_instantAxiomSites) {
        if (site.address == address) return;
    }

    if (!AddressInMainImage(reinterpret_cast<uintptr_t>(address), 2)) {
        return;
    }

    const bool original = address[0] == expected0 && address[1] == expected1;
    const bool already = address[0] == patch0 && address[1] == patch1;
    if (!original && !already) {
        return;
    }

    BytePatchSite site{};
    site.name = name;
    site.address = address;
    site.original[0] = expected0;
    site.original[1] = expected1;
    site.patch[0] = patch0;
    site.patch[1] = patch1;
    g_instantAxiomSites.push_back(site);

    char msg[192];
    sprintf_s(msg,
              "InstantAxiom site added name=%s rva=0x%llX original=%02X %02X patch=%02X %02X",
              name != nullptr ? name : "unknown",
              RvaOf(address),
              expected0,
              expected1,
              patch0,
              patch1);
    Log(msg);
}

static void AddInstantAxiomJumpSite(const char* name, uint8_t* address) {
    if (address == nullptr) return;

    for (const auto& site : g_instantAxiomSites) {
        if (site.address == address) return;
    }

    if (!AddressInMainImage(reinterpret_cast<uintptr_t>(address), 2)) {
        return;
    }

    const bool original = address[0] == 0x76;
    const bool already = address[0] == 0xEB;
    if (!original && !already) {
        return;
    }

    BytePatchSite site{};
    site.name = name;
    site.address = address;
    site.original[0] = 0x76;
    site.original[1] = address[1];
    site.patch[0] = 0xEB;
    site.patch[1] = address[1];
    g_instantAxiomSites.push_back(site);

    char msg[192];
    sprintf_s(msg,
              "InstantAxiom jump site added name=%s rva=0x%llX original=%02X %02X patch=%02X %02X",
              name != nullptr ? name : "unknown",
              RvaOf(address),
              site.original[0],
              site.original[1],
              site.patch[0],
              site.patch[1]);
    Log(msg);
}

// ============================================================
//  ADDRESS DISCOVERY
// ============================================================
static void DiscoverAddresses() {
    Log("Initializing mod features...");
    if (!QueryMainImage()) {
        Log("ERROR: Unable to query game image.");
        return;
    }

    g_straightPullSites.clear();
    g_instantAxiomSites.clear();
    g_pAxiomRange = nullptr;
    g_pAxiomLimitRange = nullptr;
    g_pAxiomPullSpeed = nullptr;

    // StraightPull call-sites. The CALL displacement and loop branch byte move
    // between game versions, so use the surrounding maneuver loop shape from the
    // current Tweak source first, then keep older signatures as fallbacks.
    {
        static const uint8_t maneuverCurrent[] = {
            0xE8, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x83, 0xEB, 0x01,
            0x75, 0x00,
            0x48, 0x8B, 0xB5, 0x00, 0x00, 0x00, 0x00,
            0x44, 0x0F, 0xB6, 0xB5,
        };
        static const char maneuverCurrentMask[] = "x????xxxxx?xxx????xxxx";
        for (uint8_t* site : PatternScanAll(maneuverCurrent, maneuverCurrentMask, sizeof(maneuverCurrent))) {
            AddStraightPullSite(site);
        }

        static const uint8_t pattern106[] = { 0xE8, 0x00, 0xD1, 0xFF, 0xFF, 0x48, 0x83, 0xEB, 0x01, 0x75, 0xC8 };
        static const char mask106[] = "x?xxxxxxxxx";
        for (uint8_t* site : PatternScanAll(pattern106, mask106, sizeof(pattern106))) {
            AddStraightPullSite(site);
        }

        static const uint8_t alreadyPatched106[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x83, 0xEB, 0x01, 0x75, 0xC8 };
        static const char alreadyPatchedMask106[] = "xxxxxxxxxxx";
        for (uint8_t* site : PatternScanAll(alreadyPatched106, alreadyPatchedMask106, sizeof(alreadyPatched106))) {
            AddStraightPullSite(site);
        }
    }

    // Legacy straight-pull fallback (1.05.x)
    if (g_straightPullSites.empty()) {
        static const uint8_t legacyPattern[] = { 0xE8, 0x4C, 0x31, 0x49, 0xFF };
        static const char legacyMask[] = "xxxxx";
        if (uint8_t* legacy = PatternScan(legacyPattern, legacyMask, sizeof(legacyPattern))) {
            AddStraightPullSite(legacy);
        }

        const uintptr_t fallback = g_gameBase + kAerialManeuverOffset_Fallback;
        if (AddressInMainImage(fallback, 5)) {
            AddStraightPullSite(reinterpret_cast<uint8_t*>(fallback));
        }
    }

    if (!g_straightPullSites.empty()) {
        char msg[128];
        sprintf_s(msg, "StraightPull: Ready (%u patch site%s)",
                  static_cast<unsigned>(g_straightPullSites.size()),
                  g_straightPullSites.size() == 1 ? "" : "s");
        Log(msg);
    } else {
        Log("WARNING: StraightPull feature unavailable for this version.");
    }

    // InstantAxiom current build: remove the "elapsed time <= required time" skip
    // so the bracelet activation path runs immediately. The patch is 76 2B -> 90 90.
    {
        static const uint8_t instant106[] = {
            0x4C, 0x3B, 0xF8,
            0x76, 0x2B,
            0xC6, 0x44, 0x24, 0x20, 0x01,
            0x45, 0x33, 0xC9,
            0x41, 0xB0, 0x02,
            0x40, 0x0F, 0xB6, 0xD6,
            0x48, 0x8B, 0xCF,
            0xE8, 0x00, 0x00, 0x00, 0x00,
            0xE9
        };
        static const char instant106Mask[] = "xxxxxxxxxxxxxxxxxxxxxxxx????x";
        for (uint8_t* site : PatternScanAll(instant106, instant106Mask, sizeof(instant106))) {
            AddInstantAxiomSite("activation-delay-v106", site + 3, 0x76, 0x2B, 0x90, 0x90);
        }
    }

    // Legacy offsets: bind only if the expected jump byte pairs still match.
    {
        const uintptr_t activationDelayAddr = g_gameBase + kActivationDelayOffset_Fallback;
        if (AddressInMainImage(activationDelayAddr, 2)) {
            AddInstantAxiomSite("activation-delay-fallback", reinterpret_cast<uint8_t*>(activationDelayAddr), 0x76, 0x2B, 0x90, 0x90);
        }

        // Current 1.0.0.1492 duration gates moved after the first 1.06-era
        // InstantAxiom update. Convert their "not done yet" JBE to an
        // unconditional short jump while preserving the branch distance.
        {
            static const uint8_t reelDuration1492[] = {
                0xC4, 0xC1, 0x78, 0x2F, 0xD4,
                0x76, 0x00,
                0xC5, 0xFA, 0x5E, 0xC2,
                0xC4, 0xC1, 0x7A, 0x5D, 0xCD,
                0xC5, 0x9A, 0x5F, 0xF1,
                0xEB, 0x00,
                0xC4, 0xC1, 0x78, 0x28, 0xF5
            };
            static const char reelDuration1492Mask[] = "xxxxxx?xxxxxxxxxxxxxx?xxxxx";
            for (uint8_t* site : PatternScanAll(reelDuration1492, reelDuration1492Mask, sizeof(reelDuration1492))) {
                AddInstantAxiomJumpSite("reel-duration-v1492", site + 5);
            }
        }

        {
            static const uint8_t propellDuration1492[] = {
                0xC4, 0xC1, 0x78, 0x28, 0xE4,
                0xC4, 0xC1, 0x78, 0x28, 0xEC,
                0xC4, 0xC1, 0x78, 0x2F, 0xDC,
                0x76, 0x00,
                0xC5, 0xF8, 0x57, 0xC0,
                0xC5, 0xF8, 0x2E, 0xC3,
                0x77
            };
            static const char propellDuration1492Mask[] = "xxxxxxxxxxxxxxxx?xxxxxxxxx";
            for (uint8_t* site : PatternScanAll(propellDuration1492, propellDuration1492Mask, sizeof(propellDuration1492))) {
                AddInstantAxiomJumpSite("propell-duration-v1492", site + 15);
            }
        }

        const uintptr_t reelAddr = g_gameBase + kReelDurationOffset_Fallback;
        const uintptr_t propAddr = g_gameBase + kPropellDurationOffset_Fallback;
        if (AddressInMainImage(reelAddr, 2) && AddressInMainImage(propAddr, 2)) {
            AddInstantAxiomJumpSite("reel-duration-fallback", reinterpret_cast<uint8_t*>(reelAddr));
            AddInstantAxiomJumpSite("propell-duration-fallback", reinterpret_cast<uint8_t*>(propAddr));
        }
    }

    if (!g_instantAxiomSites.empty()) {
        char msg[128];
        sprintf_s(msg, "InstantAxiom: Ready (%u patch site%s)",
                  static_cast<unsigned>(g_instantAxiomSites.size()),
                  g_instantAxiomSites.size() == 1 ? "" : "s");
        Log(msg);
    } else {
        Log("WARNING: InstantAxiom feature unavailable for this version.");
    }

    // Range/PullSpeed are the core Super Axiom Force feature. Crimson Desert 1.12
    // also keeps a nearby limit range used by the actual latch/confirm path.
    // Updating only AxiomRange can extend the marker without extending the hook.
    {
        auto* range = reinterpret_cast<float*>(g_gameBase + kAxiomRangeOffset_Fallback);
        auto* pull = reinterpret_cast<float*>(g_gameBase + kAxiomPullSpeedOffset_Fallback);
        if (AddressInMainImage(reinterpret_cast<uintptr_t>(range), sizeof(float)) &&
            AddressInMainImage(reinterpret_cast<uintptr_t>(pull), sizeof(float)) &&
            IsWritableMemory(range, sizeof(float)) &&
            IsWritableMemory(pull, sizeof(float)) &&
            *range == kDefaultRange &&
            *pull == kDefaultPullSpeed) {
            g_pAxiomRange = range;
            g_pAxiomPullSpeed = pull;

            Logf("Range/PullSpeed: fallback binding enabled range_rva=0x%llX pull_rva=0x%llX range_current=%g pull_current=%g image_size=0x%llX",
                 static_cast<unsigned long long>(kAxiomRangeOffset_Fallback),
                 static_cast<unsigned long long>(kAxiomPullSpeedOffset_Fallback),
                 *g_pAxiomRange,
                 *g_pAxiomPullSpeed,
                 static_cast<unsigned long long>(g_gameSize));
        } else {
            Logf("Range/PullSpeed: fallback offsets unavailable or stale; scanning image. fallback_range=%g fallback_pull=%g image_base=0x%llX image_size=0x%llX",
                 (AddressInMainImage(reinterpret_cast<uintptr_t>(range), sizeof(float)) && IsReadableMemory(range, sizeof(float))) ? *range : NAN,
                 (AddressInMainImage(reinterpret_cast<uintptr_t>(pull), sizeof(float)) && IsReadableMemory(pull, sizeof(float))) ? *pull : NAN,
                 static_cast<unsigned long long>(g_gameBase),
                 static_cast<unsigned long long>(g_gameSize));

            if (FindRangePullByInstructionPattern(&g_pAxiomRange, &g_pAxiomPullSpeed) ||
                FindRangePullPair(&g_pAxiomRange, &g_pAxiomPullSpeed)) {
                Logf("Range/PullSpeed: scan binding enabled range_rva=0x%llX pull_rva=0x%llX range_current=%g pull_current=%g",
                     RvaOf(g_pAxiomRange),
                     RvaOf(g_pAxiomPullSpeed),
                     *g_pAxiomRange,
                     *g_pAxiomPullSpeed);
            } else {
                Log("WARNING: Range/PullSpeed feature unavailable for this version after scan.");
            }
        }

        if (g_pAxiomRange != nullptr) {
            auto* limit = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(g_pAxiomRange) - 0x50);
            if (AddressInMainImage(reinterpret_cast<uintptr_t>(limit), sizeof(float)) &&
                IsWritableMemory(limit, sizeof(float))) {
                g_pAxiomLimitRange = limit;
                Logf("AxiomLimitRange: binding enabled limit_range_rva=0x%llX limit_current=%g source_range_rva=0x%llX",
                     RvaOf(g_pAxiomLimitRange),
                     *g_pAxiomLimitRange,
                     RvaOf(g_pAxiomRange));
            } else {
                g_pAxiomLimitRange = nullptr;
                Log("WARNING: AxiomLimitRange unavailable; marker range may exceed actual hook range.");
            }
        }
    }
    Log("All features initialized successfully");
}

// ============================================================
//  CORE: APPLY PATCHES
// ============================================================
static bool ApplyStraightPull(bool enabled) {
    if (g_straightPullSites.empty()) {
        Log("ERROR: StraightPull feature not available");
        return false;
    }

    if (enabled) {
        unsigned patched = 0;
        unsigned already = 0;
        unsigned unavailable = 0;

        for (auto& site : g_straightPullSites) {
            uint8_t* pCall = site.address;
            if (pCall == nullptr) {
                unavailable++;
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(pCall, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                unavailable++;
                continue;
            }

            if (site.original[0] == 0xE8 && memcmp(pCall, site.original, 5) == 0) {
                memset(pCall, 0x90, 5);
                site.owned = true;
                patched++;
            } else if (IsAllNops(pCall, 5)) {
                site.owned = false;
                already++;
            } else {
                site.owned = false;
                unavailable++;
            }

            VirtualProtect(pCall, 5, oldProtect, &oldProtect);
        }

        char msg[160];
        sprintf_s(msg, "StraightPull ENABLED (patched=%u already=%u unavailable=%u)",
                  patched, already, unavailable);
        Log(msg);
        return patched > 0 || already > 0;
    }

    unsigned restored = 0;
    for (auto& site : g_straightPullSites) {
        uint8_t* pCall = site.address;
        if (pCall == nullptr || !site.owned || !site.canRestore) {
            continue;
        }
        if (!IsAllNops(pCall, 5)) {
            site.owned = false;
            continue;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(pCall, 5, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            continue;
        }
        memcpy(pCall, site.original, 5);
        VirtualProtect(pCall, 5, oldProtect, &oldProtect);
        site.owned = false;
        restored++;
    }

    char msg[96];
    sprintf_s(msg, "StraightPull DISABLED (restored=%u)", restored);
    Log(msg);
    return true;
}

// Instant Axiom patch sites are discovered per game build. Current builds NOP
// the activation-delay skip; legacy builds convert duration checks to jumps.
static bool ApplyInstantAxiom(bool enabled) {
    if (g_instantAxiomSites.empty()) {
        Log("WARNING: InstantAxiom feature unavailable");
        return false;
    }

    if (enabled) {
        unsigned patched = 0;
        unsigned already = 0;
        unsigned unavailable = 0;

        for (auto& site : g_instantAxiomSites) {
            if (site.address == nullptr) {
                ++unavailable;
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(site.address, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                ++unavailable;
                continue;
            }

            if (memcmp(site.address, site.patch, 2) == 0) {
                site.owned = false;
                ++already;
            } else if (memcmp(site.address, site.original, 2) == 0) {
                memcpy(site.address, site.patch, 2);
                FlushInstructionCache(GetCurrentProcess(), site.address, 2);
                site.owned = true;
                ++patched;
            } else {
                site.owned = false;
                ++unavailable;
            }

            VirtualProtect(site.address, 2, oldProtect, &oldProtect);
        }

        char msg[160];
        sprintf_s(msg, "InstantAxiom ENABLED (patched=%u already=%u unavailable=%u)",
                  patched, already, unavailable);
        Log(msg);
        return patched > 0 || already > 0;
    }

    unsigned restored = 0;
    unsigned skipped = 0;
    for (auto& site : g_instantAxiomSites) {
        if (site.address == nullptr || !site.owned) {
            continue;
        }

        if (memcmp(site.address, site.patch, 2) != 0) {
            site.owned = false;
            ++skipped;
            continue;
        }

        DWORD oldProtect = 0;
        if (!VirtualProtect(site.address, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            ++skipped;
            continue;
        }

        memcpy(site.address, site.original, 2);
        FlushInstructionCache(GetCurrentProcess(), site.address, 2);
        VirtualProtect(site.address, 2, oldProtect, &oldProtect);
        site.owned = false;
        ++restored;
    }

    char msg[128];
    sprintf_s(msg, "InstantAxiom DISABLED (restored=%u skipped=%u)", restored, skipped);
    Log(msg);
    return true;
}

// ============================================================
//  CORE: APPLY RANGE
// ============================================================
static bool WriteAxiomFloat(const char* name, float* target, float value) {
    if (target == nullptr) {
        Logf("WARNING: %s feature unavailable", name);
        return false;
    }

    const float current = *target;
    if (!std::isfinite(current)) {
        Logf("WARNING: %s feature unavailable", name);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        const DWORD error = GetLastError();
        MEMORY_BASIC_INFORMATION mbi{};
        if (QueryMemoryForLog(target, mbi)) {
            Logf("ERROR: %s memory protection failed rva=0x%llX current=%g target=%g state=0x%lX protect=%s(0x%lX) region_size=0x%llX error=%lu",
                 name,
                 RvaOf(target),
                 current,
                 value,
                 mbi.State,
                 ProtectName(mbi.Protect),
                 mbi.Protect,
                 static_cast<unsigned long long>(mbi.RegionSize),
                 error);
        } else {
            Logf("ERROR: %s memory protection failed rva=0x%llX current=%g target=%g query_failed error=%lu",
                 name,
                 RvaOf(target),
                 current,
                 value,
                 error);
        }
        return false;
    }

    *target = value;
    VirtualProtect(target, sizeof(float), oldProtect, &oldProtect);
    Logf("%s applied rva=0x%llX previous=%g target=%g old_protect=%s(0x%lX)",
         name,
         RvaOf(target),
         current,
         value,
         ProtectName(oldProtect),
         oldProtect);
    return true;
}

static bool ApplyAxiomRange(float range) {
    bool ok = WriteAxiomFloat("Range", g_pAxiomRange, range);
    if (g_pAxiomLimitRange != nullptr) {
        ok = WriteAxiomFloat("AxiomLimitRange", g_pAxiomLimitRange, range) && ok;
    } else {
        Log("WARNING: AxiomLimitRange not patched; actual hook range may remain vanilla.");
    }
    return ok;
}
static bool ApplyAxiomPullSpeed(float value) {
    if (!g_pAxiomPullSpeed) {
        Log("WARNING: PullSpeed feature unavailable");
        return false;
    }

    const float current = *g_pAxiomPullSpeed;
    if (!std::isfinite(current)) {
        Log("WARNING: PullSpeed feature unavailable");
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(g_pAxiomPullSpeed, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        const DWORD error = GetLastError();
        MEMORY_BASIC_INFORMATION mbi{};
        if (QueryMemoryForLog(g_pAxiomPullSpeed, mbi)) {
            Logf("ERROR: PullSpeed memory protection failed rva=0x%llX current=%g target=%g state=0x%lX protect=%s(0x%lX) region_size=0x%llX error=%lu",
                 RvaOf(g_pAxiomPullSpeed),
                 current,
                 value,
                 mbi.State,
                 ProtectName(mbi.Protect),
                 mbi.Protect,
                 static_cast<unsigned long long>(mbi.RegionSize),
                 error);
        } else {
            Logf("ERROR: PullSpeed memory protection failed rva=0x%llX current=%g target=%g query_failed error=%lu",
                 RvaOf(g_pAxiomPullSpeed),
                 current,
                 value,
                 error);
        }
        return false;
    }

    *g_pAxiomPullSpeed = value;
    VirtualProtect(g_pAxiomPullSpeed, sizeof(float), oldProtect, &oldProtect);
    Logf("PullSpeed applied rva=0x%llX previous=%g target=%g old_protect=%s(0x%lX)",
         RvaOf(g_pAxiomPullSpeed),
         current,
         value,
         ProtectName(oldProtect),
         oldProtect);
    return true;
}

// ============================================================
//  CONFIG LOAD + APPLY
// ============================================================
static void LoadAndApply() {
    FlushIniCache();

    const bool enabled = ReadIniBool(kIniSection, "Enabled", true);
    const float range = ReadIniFloat(kIniSection, "Range", kFallbackRange);
    const float pullSpeed = ReadIniFloat(
        kIniSection,
        "PullSpeed",
        ReadIniFloat(kIniSection, "PullNorm", kFallbackPullSpeed));
    const bool straightPull = ReadIniBool(kIniSection, "StraightPull", true);
    const bool instantAxiom = ReadIniBool(kIniSection, "InstantAxiom", true);
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

    if (!enabled) {
        ApplyAxiomRange(kDefaultRange);
        ApplyAxiomPullSpeed(kDefaultPullSpeed);
        ApplyStraightPull(false);
        ApplyInstantAxiom(false);
        Log("Mod disabled - defaults restored");
        return;
    }

    ApplyAxiomRange(range);
    ApplyAxiomPullSpeed(pullSpeed);
    ApplyStraightPull(straightPull);
    ApplyInstantAxiom(instantAxiom);
}

// ============================================================
//  MAIN MOD THREAD
// ============================================================
static void ModMain() {
    const std::string moduleDir = GetModuleDirectory();
    g_iniPath = moduleDir + "\\" + kModName + ".ini";
    g_logPath = moduleDir + "\\" + kLogFile;

    {
        std::ofstream reset(g_logPath, std::ios::trunc);
    }

    Logf("Starting up module_dir=%s", moduleDir.c_str());

    // Wait for the game to finish loading before touching memory.
    Sleep(15000);

    DiscoverAddresses();

    LoadAndApply();
    GetFileModTime(&g_lastIniModTime);

    while (true) {
        Sleep(1000);

        ULONGLONG currentModTime = 0;
        if (GetFileModTime(&currentModTime) && currentModTime != g_lastIniModTime) {
            Sleep(100);
            g_lastIniModTime = currentModTime;
            Log("INI changed - reloading.");
            LoadAndApply();
        }
    }
}

// ============================================================
//  DLL ENTRY POINT
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        if (!IsCrimsonDesertProcess()) {
            return TRUE;
        }

        g_module = hModule;

        bool expected = false;
        if (!g_isInitialized.compare_exchange_strong(expected, true)) {
            return TRUE;
        }

        std::thread(ModMain).detach();
    }
    return TRUE;
}



