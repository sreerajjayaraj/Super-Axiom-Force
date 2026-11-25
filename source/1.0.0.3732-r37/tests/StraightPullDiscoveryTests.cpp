#include "../src/SuperAxiomForce/StraightPullDiscovery.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr uintptr_t kImageBase = 0x140000000ULL;
constexpr size_t kImageSize = 0x400;
constexpr size_t kSharedTargetOffset = 0x300;

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void WriteInt32(std::vector<uint8_t>& image, size_t offset, int32_t value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

void AddCandidate(std::vector<uint8_t>& image,
                  size_t contextOffset,
                  straight_pull::LoopKind loopKind,
                  size_t targetOffset,
                  bool nopped = false) {
    static const uint8_t prefix[] = {
        0x4C, 0x8D, 0x4F, 0x48,
        0x4C, 0x8D, 0x87, 0x98, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0xD7,
        0xC5, 0xF8, 0x28, 0xC6,
    };
    std::memcpy(image.data() + contextOffset, prefix, sizeof(prefix));

    const size_t callOffset = contextOffset + sizeof(prefix);
    if (nopped) {
        std::memset(image.data() + callOffset, 0x90, 5);
    } else {
        image[callOffset] = 0xE8;
        const int64_t relative = static_cast<int64_t>(targetOffset) -
                                 static_cast<int64_t>(callOffset + 5);
        Check(relative >= std::numeric_limits<int32_t>::min() &&
                  relative <= std::numeric_limits<int32_t>::max(),
              "test CALL displacement fits in int32");
        WriteInt32(image, callOffset + 1, static_cast<int32_t>(relative));
    }

    const size_t tailOffset = callOffset + 5;
    if (loopKind == straight_pull::LoopKind::RegisterStride) {
        static const uint8_t tail[] = { 0x49, 0x2B, 0xDD, 0x75, 0xC8 };
        std::memcpy(image.data() + tailOffset, tail, sizeof(tail));
    } else {
        static const uint8_t tail[] = { 0x48, 0x83, 0xEB, 0x01, 0x75, 0xC7 };
        std::memcpy(image.data() + tailOffset, tail, sizeof(tail));
    }
}

std::vector<uint8_t> NewImage() {
    return std::vector<uint8_t>(kImageSize, 0xCC);
}

void TestBothCurrentLoopVariantsResolveAsOneFamily() {
    auto image = NewImage();
    AddCandidate(image, 0x40, straight_pull::LoopKind::RegisterStride, kSharedTargetOffset);
    AddCandidate(image, 0xC0, straight_pull::LoopKind::UnitStride, kSharedTargetOffset);

    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);

    Check(result.valid, "two current loop variants must form a valid family");
    Check(result.contextMatches == 2, "two contexts must be counted");
    Check(result.sites.size() == 2, "two call sites must be returned");
    Check(result.callCount == 2, "both current sites must be live CALLs");
    Check(result.noppedCount == 0, "no current site should be classified as NOPed");
    Check(result.sharedTarget == kImageBase + kSharedTargetOffset,
          "both current calls must resolve to the shared helper");
    if (result.sites.size() == 2) {
        Check(result.sites[0].callOffset == 0x40 + 18, "author branch call offset must be returned");
        Check(result.sites[0].loopKind == straight_pull::LoopKind::RegisterStride,
              "author branch loop kind must be identified");
        Check(result.sites[1].callOffset == 0xC0 + 18, "SAF branch call offset must be returned");
        Check(result.sites[1].loopKind == straight_pull::LoopKind::UnitStride,
              "SAF branch loop kind must be identified");
    }
}

void TestDistantLookalikeIsReportedButNotPatched() {
    auto image = std::vector<uint8_t>(0x1000, 0xCC);
    AddCandidate(image, 0x40, straight_pull::LoopKind::RegisterStride, 0xE00);
    AddCandidate(image, 0xC0, straight_pull::LoopKind::UnitStride, 0xE00);
    AddCandidate(image, 0x700, straight_pull::LoopKind::UnitStride, 0xE00);

    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);

    Check(result.valid, "the unique adjacent author/SAF pair must remain valid");
    Check(result.contextMatches == 3, "the distant lookalike must be counted for diagnostics");
    Check(result.ignoredContextCount == 1, "the distant lookalike must be explicitly ignored");
    Check(result.sites.size() == 2, "only the adjacent author/SAF pair may be patchable");
    if (result.sites.size() == 2) {
        Check(result.sites[0].callOffset == 0x40 + 18,
              "the register-stride author site must be retained");
        Check(result.sites[1].callOffset == 0xC0 + 18,
              "the adjacent unit-stride SAF site must be retained");
    }
}

void Test2760UnitUnitPairExcludesDistantLookalike() {
    auto image = std::vector<uint8_t>(0x2000, 0xCC);
    AddCandidate(image, 0x40, straight_pull::LoopKind::UnitStride, 0x1E00);
    AddCandidate(image, 0x11E, straight_pull::LoopKind::UnitStride, 0x1E00);
    AddCandidate(image, 0x180E, straight_pull::LoopKind::UnitStride, 0x1E00);
    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);
    Check(result.valid, "2760 unit/unit loops must form a valid family");
    Check(result.sites.size() == 2, "2760 must select exactly two adjacent sites");
    Check(result.ignoredContextCount == 1, "2760 distant unit loop must remain ignored");
}

void TestMultipleNearbyPairsFailClosed() {
    auto image = std::vector<uint8_t>(0x1000, 0xCC);
    AddCandidate(image, 0x40, straight_pull::LoopKind::RegisterStride, 0xE00);
    AddCandidate(image, 0xC0, straight_pull::LoopKind::UnitStride, 0xE00);
    AddCandidate(image, 0x180, straight_pull::LoopKind::UnitStride, 0xE00);

    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);

    Check(!result.valid, "multiple nearby register/unit pairings must fail closed");
    Check(result.reason == "ambiguous adjacent pair",
          "ambiguous pair rejection must have a precise reason");
}

void TestMismatchedTargetsFailClosed() {
    auto image = NewImage();
    AddCandidate(image, 0x40, straight_pull::LoopKind::RegisterStride, kSharedTargetOffset);
    AddCandidate(image, 0xC0, straight_pull::LoopKind::UnitStride, kSharedTargetOffset - 0x20);

    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);

    Check(!result.valid, "different CALL targets must fail closed");
    Check(result.contextMatches == 2, "mismatched contexts must still be reported");
    Check(result.reason == "call targets differ", "target mismatch must have a precise reason");
}

void TestOutOfImageTargetFailsClosed() {
    auto image = NewImage();
    AddCandidate(image, 0x40, straight_pull::LoopKind::RegisterStride, kImageSize + 0x40);

    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);

    Check(!result.valid, "out-of-image CALL target must fail closed");
    Check(result.reason == "call target outside image", "out-of-image target reason must be precise");
}

void TestExternalNopAndLiveSiblingAreSafe() {
    auto image = NewImage();
    AddCandidate(image, 0x40, straight_pull::LoopKind::RegisterStride, kSharedTargetOffset, true);
    AddCandidate(image, 0xC0, straight_pull::LoopKind::UnitStride, kSharedTargetOffset);

    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);

    Check(result.valid, "external NOP plus live sibling must remain discoverable");
    Check(result.callCount == 1, "one sibling must remain a live CALL");
    Check(result.noppedCount == 1, "one sibling must be classified as externally NOPed");
    if (result.sites.size() == 2) {
        Check(result.sites[0].state == straight_pull::SiteState::Nopped,
              "first site must retain external NOP state");
        Check(result.sites[1].state == straight_pull::SiteState::Call,
              "second site must retain live CALL state");
    }

    Check(straight_pull::DecidePatchOperation(true, false, false, straight_pull::RuntimeState::Nopped) ==
              straight_pull::PatchOperation::None,
          "enabling must not claim an external NOP");
    Check(straight_pull::DecidePatchOperation(false, false, false, straight_pull::RuntimeState::Nopped) ==
              straight_pull::PatchOperation::None,
          "disabling must not restore an external NOP");
}

void TestOwnedSiteRestoresOnlyItsOriginalCall() {
    Check(straight_pull::DecidePatchOperation(true, false, true, straight_pull::RuntimeState::OriginalCall) ==
              straight_pull::PatchOperation::ApplyNops,
          "enabling a validated CALL must request a NOP patch");
    Check(straight_pull::DecidePatchOperation(false, true, true, straight_pull::RuntimeState::Nopped) ==
              straight_pull::PatchOperation::RestoreOriginal,
          "disabling an owned NOP must request restoration");
    Check(straight_pull::DecidePatchOperation(false, true, true, straight_pull::RuntimeState::Other) ==
              straight_pull::PatchOperation::Conflict,
          "changed bytes at an owned site must fail as a conflict");
}

void TestOwnershipSurvivesLiveReloadWithoutClaimingExternalPatches() {
    Check(straight_pull::OwnershipAfterOperation(
              true, straight_pull::PatchOperation::None, true),
          "an owned NOP must stay owned when a live reload needs no write");
    Check(!straight_pull::OwnershipAfterOperation(
               false, straight_pull::PatchOperation::None, true),
          "an external NOP must remain externally owned");
    Check(straight_pull::OwnershipAfterOperation(
              false, straight_pull::PatchOperation::ApplyNops, true),
          "a successful NOP patch must become owned");
    Check(!straight_pull::OwnershipAfterOperation(
               true, straight_pull::PatchOperation::RestoreOriginal, true),
          "a successful restore must release ownership");
    Check(straight_pull::OwnershipAfterOperation(
              true, straight_pull::PatchOperation::RestoreOriginal, false),
          "a failed restore must retain ownership for a later retry");
    Check(!straight_pull::OwnershipAfterOperation(
               true, straight_pull::PatchOperation::Conflict, true),
          "conflicting bytes must release ownership rather than overwrite another patch");
}

void TestExcessiveCandidateFamilyFailsClosed() {
    auto image = std::vector<uint8_t>(0x800, 0xCC);
    for (size_t i = 0; i < 5; ++i) {
        AddCandidate(image,
                     0x20 + (i * 0x80),
                     (i % 2 == 0) ? straight_pull::LoopKind::RegisterStride
                                  : straight_pull::LoopKind::UnitStride,
                     0x700);
    }

    const auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);

    Check(!result.valid, "more than four matching contexts must fail closed");
    Check(result.contextMatches == 5, "all excessive contexts must be counted for diagnostics");
    Check(result.reason == "too many candidates", "excessive family reason must be precise");
}


void Test2944Xmm8Pair() {
    auto image = std::vector<uint8_t>(0x1000, 0xCC);
    for (size_t context : {size_t{0x40}, size_t{0x120}}) {
        AddCandidate(image, context, straight_pull::LoopKind::UnitStride, 0xE00);
        // Hand-inspected 2944: vmovaps xmm0,xmm8 has a three-byte VEX prefix.
        std::memmove(image.data()+context+19, image.data()+context+18, 11);
        const uint8_t move[] = {0xC4,0xC1,0x78,0x28,0xC0};
        std::memcpy(image.data()+context+14, move, sizeof(move));
        WriteInt32(image, context+20, static_cast<int32_t>(0xE00-(context+24)));
    }
    AddCandidate(image, 0x800, straight_pull::LoopKind::UnitStride, 0xE00);
    auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);
    Check(result.valid && result.sites.size()==2 && result.contextMatches==3,
          "2944 XMM8 pair resolves while distant XMM6 lookalike remains unselected");
    if(result.sites.size()==2) {
        Check(result.sites[0].callOffset==0x53 && result.sites[1].callOffset==0x133,
              "2944 patch starts at CALL, not the final VEX operand");
    }
    image[0x40+18]=0xC1;
    Check(!straight_pull::Discover(image.data(),image.size(),kImageBase).valid,
          "wrong 2944 time-step source register fails closed");
}

void Test3446LayoutPair() {
    auto image = std::vector<uint8_t>(0x2000, 0xCC);
    for (size_t context : {size_t{0x40}, size_t{0x120}}) {
        AddCandidate(image, context, straight_pull::LoopKind::UnitStride, 0x1E00);
        std::memmove(image.data()+context+19, image.data()+context+18, 11);
        const uint8_t move[] = {0xC4,0xC1,0x78,0x28,0xC0};
        std::memcpy(image.data()+context+14, move, sizeof(move));
        image[context+3]=0x50;
        image[context+7]=0xA0;
        WriteInt32(image, context+20, static_cast<int32_t>(0x1E00-(context+24)));
    }
    AddCandidate(image, 0x1800, straight_pull::LoopKind::UnitStride, 0x1E00);
    image[0x1803]=0x50;
    image[0x1807]=0xA0;
    const auto original = image;
    auto result = straight_pull::Discover(image.data(), image.size(), kImageBase);
    Check(result.valid && result.sites.size()==2 && result.contextMatches==3 &&
          result.ignoredContextCount==1 && result.sharedTarget==kImageBase+0x1E00,
          "3446 exact object layout resolves adjacent XMM8 pair and excludes distant XMM6");
    if(result.sites.size()==2) {
        Check(result.sites[0].callOffset==0x53 && result.sites[1].callOffset==0x133,
              "3446 patch starts at both five-byte CALL instructions");
    }
    image[0x40+7]=0x98;
    Check(!straight_pull::Discover(image.data(), image.size(), kImageBase).valid,
          "mixed old and new maneuver member offsets fail closed");
    image=original;
    image[0x40+8]=1;
    Check(!straight_pull::Discover(image.data(), image.size(), kImageBase).valid,
          "unproven maneuver member displacement fails closed");
    image=original;
    image[0x40+18]=0xC1;
    Check(!straight_pull::Discover(image.data(), image.size(), kImageBase).valid,
          "wrong 3446 time-step source register fails closed");
    image=original;
    std::memset(image.data()+0x53,0x90,5);
    result=straight_pull::Discover(image.data(), image.size(), kImageBase);
    Check(result.valid && result.noppedCount==1 && result.callCount==1,
          "3446 external NOP ownership remains recognizable");
}
} // namespace

int main() {
    Test3446LayoutPair();
    Test2944Xmm8Pair();
    Test2760UnitUnitPairExcludesDistantLookalike();
    TestBothCurrentLoopVariantsResolveAsOneFamily();
    TestDistantLookalikeIsReportedButNotPatched();
    TestMultipleNearbyPairsFailClosed();
    TestMismatchedTargetsFailClosed();
    TestOutOfImageTargetFailsClosed();
    TestExternalNopAndLiveSiblingAreSafe();
    TestOwnedSiteRestoresOnlyItsOriginalCall();
    TestOwnershipSurvivesLiveReloadWithoutClaimingExternalPatches();
    TestExcessiveCandidateFamilyFailsClosed();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d StraightPull discovery test(s) failed.\n", g_failures);
        return 1;
    }

    std::puts("All StraightPull discovery tests passed.");
    return 0;
}
