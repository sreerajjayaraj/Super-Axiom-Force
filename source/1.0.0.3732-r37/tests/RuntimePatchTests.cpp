#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"
#include <filesystem>

int main(int argc, char** argv) {
    int failures = 0;
    auto check = [&](bool condition, const char* name) {
        std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
        if (!condition) ++failures;
    };
    g_logPath = "RuntimePatchTests.log";
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 2;
    g_gameBase = reinterpret_cast<uintptr_t>(memory);
    g_gameSize = 0x1000;
    memory[0] = 0x76;
    memory[1] = 0x0F;
    BytePatchSite site{};
    site.canRestore = true;
    site.address = memory;
    site.original[0] = 0x76;
    site.original[1] = 0x0F;
    site.patch[0] = 0xEB;
    site.patch[1] = 0x0F;
    g_instantAxiomSites = {site};
    check(sizeof(site.original) >= 6, "activation storage supports six-byte near JBE");
    check(ApplyInstantAxiom(true), "enable native duration gate");
    check(ApplyInstantAxiom(true), "repeat enabled reload");
    check(g_instantAxiomSites[0].owned, "enabled reload retains our ownership");
    ApplyInstantAxiom(false);
    check(memory[0] == 0x76 && memory[1] == 0x0F, "disable restores original after repeated reload");

    const uint8_t activation[] = {0x0F, 0x86, 0x2A, 0xFF, 0xFF, 0xFF};
    BytePatchSite activationSite{};
    activationSite.address = memory + 0x20;
    activationSite.size = 6;
    activationSite.canRestore = true;
    memcpy(activationSite.original, activation, 6);
    memset(activationSite.patch, 0x90, 6);
    memcpy(activationSite.address, activation, 6);
    memory[0x1F] = memory[0x26] = 0xCC;
    g_instantAxiomSites = {activationSite};
    check(ApplyInstantAxiom(true) && IsAllNops(activationSite.address, 6), "six-byte activation patch fills whole JBE");
    ApplyInstantAxiom(true);
    check(ApplyInstantAxiom(false) && memcmp(activationSite.address, activation, 6) == 0,
          "six-byte activation restores exactly after reload");
    check(memory[0x1F] == 0xCC && memory[0x26] == 0xCC, "activation patch leaves neighboring bytes untouched");
    memset(activationSite.address, 0x90, 6);
    activationSite.canRestore = false;
    g_instantAxiomSites = {activationSite};
    ApplyInstantAxiom(true);
    ApplyInstantAxiom(false);
    check(IsAllNops(activationSite.address, 6) && !g_instantAxiomSites[0].owned,
          "external six-byte NOP is never claimed or restored");
    activationSite.canRestore = true;
    memcpy(activationSite.address, activation, 6);
    g_instantAxiomSites = {activationSite};
    ApplyInstantAxiom(true);
    activationSite.address[2] = 0xCC;
    check(!ApplyInstantAxiom(false) && activationSite.address[2] == 0xCC,
          "conflicting external activation edit is not overwritten");

    // All writes and INI fixtures below are private test memory/files on E:.
    memcpy(activationSite.address, activation, 6);
    g_instantAxiomSites = {activationSite, site};
    site.address = memory + 0x40;
    site.original[1] = site.patch[1] = 0x1F;
    memcpy(site.address, site.original, 2);
    g_instantAxiomSites.push_back(site);
    for (size_t offset : {0x60U, 0x80U}) {
        StraightPullSite straight{};
        straight.address = memory + offset;
        straight.original[0] = 0xE8;
        straight.original[1] = 0x12;
        straight.canRestore = true;
        memcpy(straight.address, straight.original, 5);
        g_straightPullSites.push_back(straight);
    }
    BytePatchSite angular{}; angular.address = memory + 0x300; angular.size = 15; angular.canRestore = true;
    memcpy(angular.original, straight_pull_angular::kOriginal, 15); memset(angular.patch, 0x90, 15); angular.patch[10] = 0xE8;
    memcpy(angular.address, angular.original, 15); g_angularSites.push_back(angular);
    BytePatchSite pullField{}; pullField.address = memory + 0x320; pullField.size = 4; pullField.canRestore = true;
    memset(pullField.original, 0x11, 4); memset(pullField.patch, 0x22, 4); memcpy(pullField.address,pullField.original,4);
    g_scopedPullSites.push_back(pullField);
    auto* values = reinterpret_cast<float*>(memory + 0x100);
    values[0] = 32;
    values[1] = 20;
    values[2] = 40;
    g_range = {values, 32, 0, false};
    g_limit = {values + 1, 20, 0, false};
    g_pull = {values + 2, 40, 0, false};
    if (g_scopedRangeSites.empty()) {
        for (size_t i = 0; i < 6; ++i) {
            BytePatchSite p{}; p.address = memory + 0x180 + i*0x10; p.size = 4; p.canRestore = true;
            int32_t original = static_cast<int32_t>(reinterpret_cast<uintptr_t>(values+1) - reinterpret_cast<uintptr_t>(p.address+4));
            int32_t target = static_cast<int32_t>(reinterpret_cast<uintptr_t>(values) - reinterpret_cast<uintptr_t>(p.address+4));
            memcpy(p.original, &original, 4); memcpy(p.patch, &target, 4); memcpy(p.address,p.original,4);
            g_scopedRangeSites.push_back(p);
        }
    }

    if (argc == 2) {
        g_iniPath = argv[1];
        check(ReadIniBool(kIniSection, "Enabled", false) &&
              ReadIniFloat(kIniSection, "Range", 0) == 500 &&
              ReadIniFloat(kIniSection, "PullSpeed", 0) == 1000 &&
              ReadIniBool(kIniSection, "StraightPull", false) &&
              ReadIniBool(kIniSection, "InstantAxiom", false), "unchanged active INI semantics read exactly");
        LoadAndApply();
        LoadAndApply();
        check(values[0] == 50000 && values[1] == 20 && values[2] == 40 &&
              IsAllNops(memory + 0x20, 6) && memcmp(memory + 0x60, g_straightPullSites[0].original, 5) == 0 && memcmp(angular.address,angular.patch,15)==0,
              "active INI applies all features to private buffers");
    }
    g_iniPath = "RuntimePatchTests.generated.ini";
    for (int flags = 7; flags >= 0; --flags) {
        const bool enabled = (flags & 4) != 0;
        const bool straight = (flags & 2) != 0;
        const bool instant = (flags & 1) != 0;
        {
            std::ofstream ini(g_iniPath);
            ini << "[AxiomForce]\nEnabled=" << enabled << "\nRange=500.0\nPullSpeed=1000.0\nStraightPull="
                << straight << "\nInstantAxiom=" << instant << "\n";
        }
        LoadAndApply();
        LoadAndApply();
        check(values[0] == (enabled ? 50000 : 32) && values[1] == 20 &&
              values[2] == (enabled && !straight ? 1000 : 40),
              "Straight Pull limits travel speed independently and disable restores native defaults");
        for (const auto& s : g_straightPullSites) {
            check(memcmp(s.address, s.original, 5) == 0,
                  "StraightPull never removes either native integration call");
        }
        check(memcmp(angular.address,enabled&&straight?angular.patch:angular.original,15)==0,
              "angular-only adapter follows independent StraightPull toggle");
        for (const auto& s : g_instantAxiomSites) {
            check(memcmp(s.address, enabled && instant ? s.patch : s.original, s.size) == 0,
                  "InstantAxiom toggle reverses every native gate");
        }
    }
    check(WriteAxiomFloat("Range", g_range, true, 500), "owned Range update");
    values[0] = 999;
    check(!WriteAxiomFloat("Range", g_range, false, 0) && values[0] == 999,
          "master disable does not overwrite an external float change");
    check(!WriteAxiomFloat("Range", g_range, true, 500) && values[0] == 999,
          "reload cannot reacquire a conflicting external float");
    check(!WriteAxiomFloat("PullSpeed", g_pull, true, NAN) && values[2] == 40,
          "nonfinite INI value rejected");
    check(!WriteAxiomFloat("PullSpeed", g_pull, true, -1) && values[2] == 40,
          "nonpositive INI value rejected");
    {
        std::ofstream ini(g_iniPath);
        ini << "[axiomforce]\nEnabled=off\nPullNorm=123.5\nStraightPull=YES\nInstantAxiom=0\n";
    }
    check(!ReadIniBool(kIniSection, "Enabled", true) && ReadIniBool(kIniSection, "StraightPull", false) &&
          !ReadIniBool(kIniSection, "InstantAxiom", true), "case-insensitive sections and boolean aliases preserved");
    check(ReadIniFloat(kIniSection, "PullSpeed", ReadIniFloat(kIniSection, "PullNorm", 1000)) == 123.5f,
          "legacy PullNorm alias preserved");

    const auto pathRoot = std::filesystem::current_path() /
        ("IniPathFixture-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    const auto hostDir = pathRoot / "host";
    const auto moduleDir = pathRoot / "module";
    std::filesystem::create_directories(hostDir);
    std::filesystem::create_directories(moduleDir);
    const auto hostIni = hostDir / "SuperAxiomForce.ini";
    const auto moduleIni = moduleDir / "SuperAxiomForce.ini";
    const auto writeIni = [](const std::filesystem::path& path, int range) {
        std::ofstream file(path, std::ios::binary);
        file << "[AxiomForce]\nEnabled=true\nRange=" << range << "\nPullSpeed=" << range * 2
             << "\nStraightPull=false\nInstantAxiom=false\n";
    };
    const auto readBytes = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    writeIni(hostIni, 321);
    writeIni(moduleIni, 123);
    const auto hostBefore = readBytes(hostIni);
    const auto moduleBefore = readBytes(moduleIni);
    g_iniPath = ResolveIniPath(hostDir.string(), moduleDir.string());
    check(g_iniPath == hostIni.string(), "existing host INI wins over conflicting module INI");
    check(readBytes(hostIni) == hostBefore && readBytes(moduleIni) == moduleBefore,
          "path selection leaves both existing INIs byte-for-byte unchanged");
    values[0] = 32; values[1] = 20; values[2] = 40;
    g_range = {values, 32, 0, false};
    g_limit = {values + 1, 20, 0, false};
    g_pull = {values + 2, 40, 0, false};
    LoadAndApply();
    check(values[0] == 50000 && values[1] == 20 && values[2] == 642,
          "conflicting INIs apply authoritative host values");
    check(GetFileModTime(&g_lastIniModTime), "selected INI timestamp captured for live reload");
    writeIni(hostIni, 333);
    std::filesystem::last_write_time(hostIni,
        std::filesystem::last_write_time(hostIni) + std::chrono::seconds(2));
    ULONGLONG currentModTime = 0;
    const bool changed = GetFileModTime(&currentModTime) && currentModTime != g_lastIniModTime;
    check(changed, "host INI edits trigger the existing reload predicate");
    if (changed) {
        g_lastIniModTime = currentModTime;
        LoadAndApply();
    }
    check(values[0] == 50000 && values[1] == 20 && values[2] == 666,
          "host INI reload applies changed host settings");
    writeIni(moduleIni, 124);
    std::filesystem::last_write_time(moduleIni,
        std::filesystem::last_write_time(moduleIni) + std::chrono::seconds(4));
    check(GetFileModTime(&currentModTime) && currentModTime == g_lastIniModTime,
          "module INI edits do not trigger reload when host INI is authoritative");
    g_iniPath = ResolveIniPath((pathRoot / "absent-host").string(), moduleDir.string());
    check(g_iniPath == moduleIni.string(), "absent host INI falls back to module-adjacent INI");
    LoadAndApply();
    check(values[0] == 50000 && values[1] == 20 && values[2] == 248,
          "module fallback applies its own settings");
    const auto absentModule = pathRoot / "absent-module";
    g_iniPath = ResolveIniPath((pathRoot / "absent-host").string(), absentModule.string());
    check(!std::filesystem::exists(pathRoot / "absent-host") && !std::filesystem::exists(absentModule),
          "missing path probes create no INI or directory");
    check(g_iniPath == (absentModule / "SuperAxiomForce.ini").string(),
          "both absent retains module path for existing default behavior");
    check(!IsCrimsonDesertProcess(), "test executable rejected by game-only guard");
    DllMain(nullptr, DLL_PROCESS_ATTACH, nullptr);
    check(!g_isInitialized && g_module == nullptr, "helper attach starts no mod worker");
    VirtualFree(memory, 0, MEM_RELEASE);
    std::printf("Runtime patch failures=%d\n", failures);
    return failures ? 1 : 0;
}
