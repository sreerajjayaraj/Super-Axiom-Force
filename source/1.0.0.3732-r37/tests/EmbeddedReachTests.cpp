#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include "../src/AxiomReach.cpp"

int main(int argc, char** argv) {
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    if (argc != 2 && argc != 3) return 2;
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
    auto* image = static_cast<uint8_t*>(VirtualAlloc(nullptr, imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
    if (argc == 3) {
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
    BindFeatureSites(plan.bindings, plan.angularSite, plan.selectedReturns);
    g_retryContractReady = kAllFeatureBits;
    for (auto& witness : plan.witnesses)
        g_retryContracts.push_back({g_image.bytes + witness.rva, std::move(witness.bytes), witness.mask});
    g_adaptiveWitnessReady = true;
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
    using saf::embedded::Apply;
    using saf::embedded::Result;
    using saf::embedded::Service;
    using saf::embedded::Settings;
    check(!saf::embedded::ReserveEarly(nullptr) && !saf::embedded::Initialize(nullptr),
          "actual embedded API refuses the helper process");
    check(Apply(Settings{}) == Result::Unavailable, "uninitialized API refuses apply");
    saf::embedded::ready = ready; // Native binding above ran on a private image.
    check(Service() == Result::Inactive, "service has no configuration before first apply");

    Settings settings{true,false,true,true,5000.f,1000.f};
    check(Apply(settings) == Result::Applied && *g_configuredRange == 50000.f &&
          *g_configuredPull == 1000.f && SAFOwnedPatchStateV1.ownedMask == 0x7FF,
          "menu settings apply range, pull, straight pull and instant");
    settings.range = 1250.f;
    check(Apply(settings) == Result::Applied && *g_configuredRange == 50000.f,
          "changing obsolete Menu range does not affect dynamic aiming");
    settings.unlimitedReach = true;
    check(Apply(settings) == Result::Applied && *g_configuredRange == 50000.f &&
          *g_limit.address == 20.f, "Unlimited Reach uses the private scalar");
    settings.range = 7500.f;
    check(Apply(settings) == Result::Applied && *g_configuredRange == 50000.f,
          "editing finite reach retains the unlimited override");
    settings.unlimitedReach = false;
    check(Apply(settings) == Result::Applied && *g_configuredRange == 50000.f,
          "turning Unlimited Reach off retains crosshair-driven targeting");
    settings.range = 0;
    check(Apply(settings) == Result::Applied && *g_configuredRange == 50000.f,
          "an obsolete zero range leaves the active aim budget intact");
    settings.pullSpeed = 0.f;
    check(Apply(settings) == Result::InvalidSettings && *g_configuredPull == 1000.f,
          "invalid pull speed leaves the active pull configuration intact");
    settings.pullSpeed = 1500.f;
    {
        safcompat::Lease lease;
        check(lease.Acquire(&SAFOwnedPatchStateV1)==safcompat::Result::Valid &&
              Apply(settings) == Result::Pending && *g_configuredRange == 50000.f,
              "busy lease defers writes");
    }
    for (auto& due : g_recovery.due) due = 0;
    check(Service() == Result::Applied && *g_configuredRange == 50000.f && *g_configuredPull == 1500.f,
          "service applies the pending menu configuration");
    settings.enabled = false;
    check(Apply(settings) == Result::Inactive && SAFOwnedPatchStateV1.ownedMask == 0 &&
          *g_configuredRange == 256.f && *g_configuredPull == 40.f &&
          std::all_of(g_patchContract.begin(),g_patchContract.end(),[&](const auto& site){
              return std::memcmp(image+site.rva,site.original.data(),site.size)==0;
          }), "disable restores all owned bytes and both private values");
    std::printf("Embedded SAF failures=%u\n",failures);
    return failures ? 1 : 0;
}
