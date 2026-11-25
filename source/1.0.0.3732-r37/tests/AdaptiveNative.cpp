#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <vector>
#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"

int main(int argc, char** argv) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    if (argc != 2 && argc != 3) return 2;
    const bool lowBase=argc==3 && !std::strcmp(argv[2],"--low-base");
    if (!std::strcmp(argv[1], "--host-refusal")) {
        const bool refused = !IsCrimsonDesertProcess();
        std::printf("%s small or differently named host refuses before worker startup\n", refused ? "PASS" : "FAIL");
        return refused ? 0 : 1;
    }
    std::ifstream stream(argv[1], std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length < 4096 || length > 0x24000000) return 2;
    std::vector<uint8_t> raw(static_cast<size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(raw.data()), length)) return 2;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) > raw.size() - sizeof(IMAGE_NT_HEADERS64)) return 2;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    if (nt->Signature != IMAGE_NT_SIGNATURE || imageSize > 0x24000000 ||
        nt->OptionalHeader.SizeOfHeaders > raw.size() || nt->OptionalHeader.SizeOfHeaders > imageSize) return 2;
    auto* image = static_cast<uint8_t*>(VirtualAlloc(lowBase?reinterpret_cast<void*>(0x140000000ULL):nullptr, imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image) return 2;
    native::Image game{image, imageSize, {}, {}, {}, {}};
    std::memcpy(image, raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const auto count = (std::min)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (section.PointerToRawData > raw.size() || count > raw.size() - section.PointerToRawData ||
            section.VirtualAddress > imageSize || count > imageSize - section.VirtualAddress) return 2;
        std::memcpy(image + section.VirtualAddress, raw.data() + section.PointerToRawData, count);
        const native::Range span{section.VirtualAddress, section.Misc.VirtualSize};
        if (span.offset >= game.size || span.size > game.size - span.offset) return 2;
        if (section.Characteristics & IMAGE_SCN_MEM_READ) game.readable.push_back(span);
        if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) game.code.push_back(span);
        else if (section.Characteristics & IMAGE_SCN_MEM_WRITE) game.writable.push_back(span);
    }
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    game.exceptions = {directory.VirtualAddress, directory.Size};
    const auto host = [&](uintptr_t base) {
        const auto read = [&](uintptr_t at, void* out, size_t count) {
            if (at < base || at - base > imageSize || count > imageSize - (at - base)) return false;
            std::memcpy(out, image + (at - base), count);
            return true;
        };
        return saf_host::CompatibleImage(base, read);
    };
    unsigned failures = 0;
    const auto check = [&](bool ok, const char* description) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
        if (!ok) ++failures;
    };
    check(host(nt->OptionalHeader.ImageBase), "real archived game family admitted at preferred base");
    check(host(nt->OptionalHeader.ImageBase + 0x10000), "same game admitted with loader rebasing");
    const auto timestamp = static_cast<size_t>(dos->e_lfanew) + 8;
    image[timestamp] ^= 1;
    check(host(nt->OptionalHeader.ImageBase), "build timestamp drift does not decide compatibility");
    image[timestamp] ^= 1;
    const auto checksum = static_cast<size_t>(dos->e_lfanew) + 88;
    image[checksum] ^= 1;
    check(host(nt->OptionalHeader.ImageBase), "build checksum drift does not decide compatibility");
    image[checksum] ^= 1;
    const uintptr_t base = reinterpret_cast<uintptr_t>(image);
    auto plan = saf_adaptive::Resolve(game, base);
    check(!plan.refusal, "approved native SAF layout resolves without pinned absolute RVAs");
    if (plan.refusal) {
        std::printf("Reason: %s\n", plan.refusal);
        return 1;
    }
    std::printf("Angle=%zX returns=%zX/%zX witnesses=%zu\n", plan.angularSite,
        plan.selectedReturns[0], plan.selectedReturns[1], plan.witnesses.size());
    if (argc == 3 && !lowBase && std::strcmp(argv[2],"--optional-clock-refusal") &&
        std::strcmp(argv[2],"--temporarily-unavailable-clock")) {
        const auto refuseMutation = [&](size_t rva, const char* label) {
            image[rva] ^= 1;
            const auto changed = saf_adaptive::Resolve(game, base);
            check(changed.refusal != nullptr, label);
            image[rva] ^= 1;
        };
        refuseMutation(plan.bindings.activation.offset + 2, "wrong in-owner activation branch refuses");
        const size_t helper = plan.callers.sharedTarget - base;
        refuseMutation(helper + 8, "changed StraightPull ABI inside helper owner refuses");
        const auto badSize = game.exceptions.size;
        game.exceptions.size -= 1;
        check(saf_adaptive::Resolve(game, base).refusal != nullptr, "malformed exception directory refuses");
        game.exceptions.size = badSize;
        saf_adaptive::Owners table(game);
        if (!table.Read()) return 2;
        const auto* function = table.Find(helper, 1);
        if (!function) return 2;
        refuseMutation(function->unwind + 6, "changed native unwind code refuses");
        const auto* rangeOwner = table.Find(plan.bindings.range.instruction, 1);
        if (!rangeOwner) return 2;
        const size_t relativeCall = rangeOwner->begin + 0x24;
        int32_t oldCall = 0;
        std::memcpy(&oldCall, image + relativeCall, sizeof(oldCall));
        const auto oldTarget = saf_adaptive::DecodeRelative(relativeCall, image + relativeCall);
        const auto* targetOwner = table.Find(oldTarget, 1);
        if (!targetOwner || targetOwner->begin != oldTarget) return 2;
        const int32_t middle = oldCall + 1;
        std::memcpy(image + relativeCall, &middle, sizeof(middle));
        check(saf_adaptive::Resolve(game, base).refusal != nullptr, "masked rel32 redirected to a function interior refuses");
        std::memcpy(image + relativeCall, &oldCall, sizeof(oldCall));
        const size_t genericCall = rangeOwner->begin + 0xA8;
        int32_t original = 0;
        std::memcpy(&original, image + genericCall, sizeof(original));
        const auto target = saf_adaptive::DecodeRelative(genericCall, image + genericCall);
        const auto* targetFunction = table.Find(target, 1);
        if (!targetFunction || targetFunction->begin != target) return 2;
        // Both fixtures choose a different native function with the same
        // prologue, extent and unwind as the real status-slot getter.
        const size_t unrelated = nt->FileHeader.TimeDateStamp == 0x6AF5D198 ? 0xB830D0 : 0xB4BF40;
        const auto* unrelatedFunction = table.Find(unrelated, 1);
        if (!unrelatedFunction || unrelatedFunction->begin != unrelated ||
            unrelatedFunction->end - unrelated != targetFunction->end - target ||
            std::memcmp(image + unrelated, image + target, sizeof(uint64_t)) ||
            std::memcmp(image + unrelatedFunction->unwind, image + targetFunction->unwind, 16)) return 2;
        const int32_t redirected = static_cast<int32_t>(static_cast<int64_t>(unrelated) - static_cast<int64_t>(genericCall + 4));
        std::memcpy(image + genericCall, &redirected, sizeof(redirected));
        check(saf_adaptive::Resolve(game, base).refusal != nullptr,
            "wrong range status-slot function with identical prologue, length and unwind refuses");
        std::memcpy(image + genericCall, &original, sizeof(original));
        const auto constant = std::find_if(plan.witnesses.begin(), plan.witnesses.end(), [](const auto& witness) {
            constexpr uint8_t nativeRange[] = {0, 0, 0x80, 0x43};
            return witness.bytes.size() == sizeof(nativeRange) &&
                !std::memcmp(witness.bytes.data(), nativeRange, sizeof(nativeRange));
        });
        if (constant == plan.witnesses.end()) return 2;
        refuseMutation(constant->rva, "incorrect native range constructor scalar refuses");
        const size_t duplicate = 0x700;
        std::memcpy(image + duplicate, image + plan.angularSite, 25);
        game.code.push_back({duplicate, 25});
        check(saf_adaptive::Resolve(game, base).refusal != nullptr, "duplicate angular context refuses");
        game.code.pop_back();
    }
    g_gameBase = base;
    g_gameSize = imageSize;
    g_image = game;
    g_logPath = "AdaptiveNative.fixture.log";
    *reinterpret_cast<float*>(image + plan.bindings.range.target) = 256;
    *reinterpret_cast<float*>(image + plan.bindings.limit.target) = 20;
    *reinterpret_cast<float*>(image + plan.bindings.pull.target) = 40;
    const bool stockMenuToggle = nt->FileHeader.TimeDateStamp == 0x6AF5D198;
    const auto gameFloat = [&](size_t rva) -> float& {
        return *reinterpret_cast<float*>(image + rva);
    };
    if (stockMenuToggle) {
        check(plan.bindings.range.target == 0x716B7E8 && plan.bindings.limit.target == 0x716B798 &&
              plan.bindings.pull.target == 0x716C328,
              "Menu stock Axiom writes and SAF resolve the same game 3663 native inputs");
        // Model the verified Menu 6.0.1 ON state before SAF discovers.
        gameFloat(plan.bindings.range.target) = 50000.f;
        gameFloat(plan.bindings.limit.target) = 50000.f;
        gameFloat(plan.bindings.pull.target) = 10000.f;
        gameFloat(0x716B838) = 50000.f;
        check(!saf_adaptive::Resolve(game, base).refusal,
              "Menu's stock Axiom scalars do not invalidate SAF code discovery");
    }
#ifdef SAF_HOOK_FIXTURE
    for (const auto& span:game.code) {
        DWORD previous=0;
        if (!VirtualProtect(image+span.offset,span.size,PAGE_EXECUTE_READ,&previous)) return 2;
    }
#endif
    BindFeatureSites(plan.bindings, plan.angularSite, plan.selectedReturns);
    g_retryContractReady = kAllFeatureBits;
    for (auto& witness : plan.witnesses)
        g_retryContracts.push_back({g_image.bytes + witness.rva, std::move(witness.bytes), witness.mask});
    g_adaptiveWitnessReady = true;
#ifdef SAF_HOOK_FIXTURE
    const bool ready = g_clockSupportReady && g_angularReady && g_scopedRangeSites.size() == 6 &&
        g_scopedPullSites.size() == 1 && g_instantAxiomSites.size() == 1;
    constexpr uint32_t expectedMask=0x17F;
    check(ready, "SafetyHook prepares three verified native results with eight owned byte edits");
    if (!ready) return 1;
#else
    constexpr uint32_t expectedMask=0x7FF;
    const bool ready = g_clockSupportReady && g_angularReady && g_scopedRangeSites.size() == 6 &&
        g_scopedPullSites.size() == 1 && g_angularSites.size() == 1 && g_instantAxiomSites.size() == 3;
    check(ready, "production binding emits all 11 patches and three owned support adapters");
    if (!ready) return 1;
    for (const auto& patch : g_patchContract)
        std::printf("Patch 0x%X bytes=%u kind=%u\n", patch.rva, patch.size, patch.kind);
    std::array<uintptr_t, 2> actualReturns{};
    for (size_t i = 0; i < actualReturns.size(); ++i)
        std::memcpy(&actualReturns[i], g_supportAllocation + 0x400 + straight_pull_angular::kDataOffset + i * 16, sizeof(uintptr_t));
    check(actualReturns[0] == base + plan.selectedReturns[0] && actualReturns[1] == UINT64_MAX,
        "emitted StraightPull adapter leaves the obstacle-correction return native");
    if (nt->FileHeader.TimeDateStamp == 0x6AF5D198) {
        // Export the actual generated support as inert fixture data.
        // Normalize the ordinary caller; the correction sentinel is invariant.
        std::array<uint8_t, 4096> support{};
        std::memcpy(support.data(), g_supportAllocation, support.size());
        const uintptr_t preferred = nt->OptionalHeader.ImageBase + plan.selectedReturns[0];
        std::memcpy(support.data() + 0x400 + straight_pull_angular::kDataOffset,
                    &preferred, sizeof(preferred));
        std::ofstream data("SAF3663-Support.pe-data", std::ios::binary | std::ios::trunc);
        if (!data.write(reinterpret_cast<const char*>(support.data()), support.size())) return 2;
    }
#endif
#ifdef SAF_HOOK_FIXTURE
    if (argc==3 && !std::strcmp(argv[2],"--temporarily-unavailable-clock")) {
        g_coreHooks[1].hook.reset();
        g_coreHooks[2].hook.reset();
        g_clockSupportReady=false;
        auto* site=image+g_coreSites[1].rva;
        DWORD previous=0,temporary=0;
        if (!VirtualProtect(site,1,PAGE_READONLY,&previous)) return 2;
        ApplyConfig config{};
        check(!ApplyBatch(config,kInstantBit,0,false) &&
              (g_recovery.pending&kInstantBit) && !g_instantPolicy.load(),
              "temporarily unavailable clock schedules a retry without activation");
        if (!VirtualProtect(site,1,previous,&temporary)) return 2;
        check(ApplyBatch(config,kInstantBit,1000,true) &&
              !(g_recovery.pending&kInstantBit) && g_instantPolicy.load() &&
              g_coreHooks[1].hook->enabled() && g_coreHooks[2].hook->enabled(),
              "bounded retry attaches both clock hooks when verified executable ownership returns");
        return failures?1:0;
    }
    if (argc==3 && !std::strcmp(argv[2],"--optional-clock-refusal")) {
        g_instantAxiomSites.clear();
        g_clockSupportReady=false;
        ApplyConfig rangeOnly{};
        rangeOnly.straight=false;
        rangeOnly.instant=false;
        check(ApplyBatch(rangeOnly,kRangeBit|kPullBit,0,false),
              "range and pull remain independently installable after an optional clock refusal");
        {
            safcompat::Lease lease;
            check(lease.Acquire(&SAFOwnedPatchStateV1)==safcompat::Result::Valid &&
                  (lease.state.ownedMask&0x3F)==0x3F && !(lease.state.ownedMask&0x100),
                  "optional clock refusal leaves a valid integrated targeting lease");
        }
        saf_targeting::PrepareMappedImage([](const char* message){ std::puts(message); },
                                           base,imageSize,nt->OptionalHeader.ImageBase);
        ServiceUnifiedTargeting();
        check(saf_targeting::MappedImageFeaturesReady(),
              "production service activates native targeting with the optional clock unavailable");
        return failures?1:0;
    }
#endif
    ApplyConfig config{};
    check(ApplyBatch(config, kAllFeatureBits, 0, false) && SAFOwnedPatchStateV1.ownedMask == expectedMask &&
        SAFOwnedPatchStateV1.status == static_cast<uint32_t>(safcompat::Status::Ready),
        "applies and publishes all scoped owned spans in a private image");
#ifdef SAF_HOOK_FIXTURE
    check(g_straightPolicy.load() && g_instantPolicy.load() &&
        g_coreHooks[0].hook->enabled() && g_coreHooks[1].hook->enabled() && g_coreHooks[2].hook->enabled(),
        "Straight Pull and Instant Axiom publish only after their SafetyHook detours are validated");
    std::array<uint8_t,0x200> angularFrame{};
    safetyhook::Context angular{};
    angular.rsp=reinterpret_cast<uintptr_t>(angularFrame.data());
    uintptr_t caller=base+plan.selectedReturns[0];
    std::memcpy(angularFrame.data()+straight_pull_angular::kCallerReturnOffset-8,&caller,8);
    angular.xmm1.f32[0]=angular.xmm2.f32[0]=angular.xmm3.f32[0]=1.f;
    angular.xmm1.f32[1]=7.f;
    ApplyAngularPolicy(angular);
    check(!angular.xmm1.u32[0] && !angular.xmm2.u32[0] && !angular.xmm3.u32[0] && angular.xmm1.f32[1]==7.f,
        "ordinary pull zeros three scalar increments and preserves the other lanes");
    caller=base+plan.selectedReturns[1];
    std::memcpy(angularFrame.data()+straight_pull_angular::kCallerReturnOffset-8,&caller,8);
    angular.xmm1.f32[0]=1.f;
    ApplyAngularPolicy(angular);
    check(angular.xmm1.f32[0]==1.f,"native obstacle correction keeps its rotation");
    std::array<uint8_t,0xB30> lane{};
    std::array<uint8_t,0x20> object{};
    uintptr_t live[2]={0,reinterpret_cast<uintptr_t>(object.data())};
    const auto liveAddress=reinterpret_cast<uintptr_t>(live);
    std::memcpy(lane.data()+0x9C0,&liveAddress,8);
    std::memcpy(lane.data()+0x9B8,&liveAddress,8);
    const uint32_t duration=0x3F800000;
    std::memcpy(lane.data()+0xB24,&duration,4);
    safetyhook::Context clock{};
    clock.rcx=reinterpret_cast<uintptr_t>(lane.data());
    clock.xmm0.f32[0]=clock.xmm1.f32[0]=0.03f;
    ApplyClockPolicy<false>(clock);
    check(clock.xmm0.u32[0]==duration-1,"valid Reel clock gets the native duration predecessor");
    clock.xmm0.f32[0]=0.03f;
    ApplyClockPolicy<true>(clock);
    check(clock.xmm0.f32[0]==0.03f,"Straight Pull keeps the Propell clock native");
    object[0x15]=1;
    ApplyClockPolicy<false>(clock);
    check(clock.xmm0.f32[0]==0.03f,"invalid native acquisition remains native");
#endif
    check(*g_configuredPull==40.f && *g_keepTravelClock==1,
        "Straight Pull enables native travel time at the stock maximum speed");
    if (stockMenuToggle) {
        check(*g_configuredRange == 50000.f && *g_configuredPull == 40.f &&
              gameFloat(plan.bindings.range.target) == 50000.f &&
              gameFloat(plan.bindings.limit.target) == 50000.f &&
              gameFloat(plan.bindings.pull.target) == 10000.f && gameFloat(0x716B838) == 50000.f,
              "SAF sets its private config while Menu owns the stock globals");
        // Model the four stores performed by the Menu worker on a live OFF transition.
        gameFloat(plan.bindings.range.target) = 256.f;
        gameFloat(plan.bindings.limit.target) = 20.f;
        gameFloat(plan.bindings.pull.target) = 40.f;
        gameFloat(0x716B838) = 4.f;
        ServiceMenuRange();
        const auto stillOwned = std::all_of(g_patchContract.begin(), g_patchContract.end(), [&](const auto& patch) {
#ifdef SAF_HOOK_FIXTURE
            if (patch.kind==2 || patch.kind==4 || patch.kind==5)
                return std::memcmp(image+patch.rva,patch.original.data(),patch.size)==0;
#endif
            uint8_t expected[16]{};
            return safcompat::ExpectedPatch(patch, base, reinterpret_cast<uintptr_t>(g_supportAllocation),
                                             expected) && !std::memcmp(image + patch.rva, expected, patch.size);
        });
        check(stillOwned && *g_configuredRange == 50000.f && *g_configuredPull == 40.f &&
              *g_limit.address == 20.f && gameFloat(0x716B838) == 4.f,
              "stock Menu OFF keeps all SAF spans and configured range/pull live with no reload");
    } else {
        check(*g_configuredRange == 50000.f && *g_configuredPull == 40.f && *g_limit.address == 20.f,
              "range and pull use owned storage and the native physics limit stays at 20");
    }
    g_recovery.initialized=true;
    g_recovery.desired=config;
    InterlockedExchange(&SAFMenuUnlimitedReachV1,1);
    ServiceMenuRange();
    {
        safcompat::Lease reader;
        const auto readyState=static_cast<uint32_t>(safcompat::Status::Ready);
        check(reader.Acquire(&SAFOwnedPatchStateV1)==safcompat::Result::Valid &&
              reader.state.status==readyState && reader.state.ownedMask==expectedMask &&
              std::memcmp(&reader.state.rangeBits,g_configuredRange,4)==0 &&
              *g_configuredRange==50000.f && *g_configuredPull==40.f && *g_limit.address==20.f,
              "Menu switches preserve the dynamic range and complete SAF lease");
        InterlockedExchange(&SAFMenuUnlimitedReachV1,0);
        ServiceMenuRange();
        check(*g_configuredRange==50000.f,"clearing the menu toggle leaves a borrowed range unchanged");
    }
    ServiceMenuRange();
    check(*g_configuredRange==50000.f && *g_configuredPull==40.f && g_recovery.desired.range==500.f,
          "clearing Unlimited Reach keeps crosshair targeting active");
    g_iniPath = "AdaptiveNative.reload-fixture.ini";
    const auto writeRange = [](float range) {
        std::ofstream ini(g_iniPath, std::ios::binary | std::ios::trunc);
        ini << "[AxiomForce]\nEnabled=true\nRange=" << range <<
               "\nPullSpeed=1000\nStraightPull=true\nInstantAxiom=true\n";
        return static_cast<bool>(ini);
    };
    check(writeRange(5000), "create private host INI fixture");
    LoadAndApply();
    check(*g_configuredRange==50000.f && g_recovery.pending==0, "SAF starts with the native aim budget and the Menu request clear");
    const auto firstStamp = g_lastIniModTime;
    check(writeRange(1500), "edit only fixture Range");
    const auto file = std::filesystem::path(g_iniPath);
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(2));
    ULONGLONG editedStamp=0;
    check(GetFileModTime(&editedStamp) && editedStamp!=firstStamp, "poll gate detects the INI edit");
    LoadAndApply();
    check(*g_configuredRange==50000.f && SAFOwnedPatchStateV1.ownedMask==expectedMask &&
          *g_configuredPull==40.f && g_recovery.pending==0, "editing obsolete Range preserves the aim budget and all feature ownership");
    check(writeRange(5000), "restore the fixture Range");
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(4));
    LoadAndApply();
    check(*g_configuredRange==50000.f && g_recovery.pending==0 && *g_limit.address==20.f,
          "restoring the fixture INI has no effect on the dynamic aiming budget");
    config.enabled = false;
    check(ApplyBatch(config, kAllFeatureBits, 0, false) && SAFOwnedPatchStateV1.ownedMask == 0,
        "real production disable restores ownership and data");
    const auto nativeRestored = std::all_of(g_patchContract.begin(), g_patchContract.end(), [&](const auto& patch) {
        return !std::memcmp(image + patch.rva, patch.original.data(), patch.size);
    });
    check(nativeRestored && *g_configuredRange == (stockMenuToggle ? 50000.f : 256.f) &&
          *g_configuredPull == (stockMenuToggle ? 10000.f : 40.f),
        "all tracked native byte spans and configured scalars are restored exactly");
#ifdef SAF_HOOK_FIXTURE
    check(!g_straightPolicy.load() && !g_instantPolicy.load() && ApplyBatch(ApplyConfig{},kAllFeatureBits,0,false) &&
        g_straightPolicy.load() && g_instantPolicy.load(),"disable pauses callbacks and re-enable reuses owned detours");
#endif
    if (argc == 3) {
        auto* readPage = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!readPage) return 2;
        uint8_t expected = 0;
        quiesced_patch::Check checkSpan{readPage, &expected, 1};
        quiesced_patch::Patch write{};
        write.address = image + g_patchContract[0].rva;
        write.size = 1;
        write.before[0] = *write.address;
        write.after[0] = static_cast<uint8_t>(write.before[0] ^ 1);
        for (const DWORD protection : {PAGE_NOACCESS, PAGE_READWRITE | PAGE_GUARD}) {
            DWORD previous = 0;
            if (!VirtualProtect(readPage, 0x1000, protection, &previous)) return 2;
            quiesced_patch::Report report;
            MEMORY_BASIC_INFORMATION state{};
            const bool applied = quiesced_patch::Write(&write, 1, {}, &report, &checkSpan, 1);
            check(!applied && report.stage == quiesced_patch::Stage::ContractBytes && report.unchanged &&
                report.allResumed && *write.address == write.before[0] &&
                VirtualQuery(readPage, &state, sizeof(state)) && state.Protect == protection,
                "frozen read refuses unavailable witness without changing bytes or protections");
        }
        VirtualFree(readPage, 0, MEM_RELEASE);
    }
    std::printf("Adaptive verification failures=%u foreignGameCodeExecuted=0\n", failures);
    return failures ? 1 : 0;
}
