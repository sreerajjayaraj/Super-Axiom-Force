#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace straight_pull {

enum class LoopKind {
    RegisterStride,
    UnitStride,
};

enum class SiteState {
    Call,
    Nopped,
};

struct Candidate {
    size_t callOffset = 0;
    SiteState state = SiteState::Call;
    LoopKind loopKind = LoopKind::RegisterStride;
    uintptr_t target = 0;
};

struct ScanRange {
    size_t offset = 0;
    size_t size = 0;
};

inline bool Contains(const std::vector<ScanRange>& ranges, size_t offset, size_t size) {
    for (const auto& range : ranges) {
        if (offset >= range.offset && offset - range.offset < range.size &&
            size <= range.size - (offset - range.offset)) return true;
    }
    return false;
}

struct DiscoveryResult {
    bool valid = false;
    size_t contextMatches = 0;
    size_t ignoredContextCount = 0;
    size_t callCount = 0;
    size_t noppedCount = 0;
    uintptr_t sharedTarget = 0;
    std::string reason;
    std::vector<Candidate> sites;
    std::vector<Candidate> ignored;
};

enum class RuntimeState {
    OriginalCall,
    Nopped,
    Other,
};

enum class PatchOperation {
    None,
    ApplyNops,
    RestoreOriginal,
    Conflict,
};

inline PatchOperation DecidePatchOperation(bool enabled,
                                           bool owned,
                                           bool canRestore,
                                           RuntimeState state) {
    if (enabled) {
        if (state == RuntimeState::OriginalCall) {
            return PatchOperation::ApplyNops;
        }
        if (state == RuntimeState::Nopped) {
            return PatchOperation::None;
        }
        return PatchOperation::Conflict;
    }

    if (!owned) {
        return PatchOperation::None;
    }
    if (state != RuntimeState::Nopped) {
        return PatchOperation::Conflict;
    }
    return canRestore ? PatchOperation::RestoreOriginal : PatchOperation::None;
}

inline bool OwnershipAfterOperation(bool currentOwned,
                                    PatchOperation operation,
                                    bool operationSucceeded) {
    switch (operation) {
    case PatchOperation::None:
        return currentOwned;
    case PatchOperation::ApplyNops:
        return operationSucceeded ? true : currentOwned;
    case PatchOperation::RestoreOriginal:
        return operationSucceeded ? false : currentOwned;
    case PatchOperation::Conflict:
        return false;
    }
    return false;
}

inline bool IsNopped(const uint8_t* bytes) {
    return bytes[0] == 0x90 &&
           bytes[1] == 0x90 &&
           bytes[2] == 0x90 &&
           bytes[3] == 0x90 &&
           bytes[4] == 0x90;
}

inline size_t MatchCommonPrefix(const uint8_t* bytes) {
    // Two disassembled member layouts; reject cross-layout or arbitrary displacements.
    const bool memberLayout = ((bytes[3] == 0x48 && bytes[7] == 0x98) ||
                               (bytes[3] == 0x50 && bytes[7] == 0xA0)) &&
                              bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0;
    const bool arguments = bytes[0] == 0x4C && bytes[1] == 0x8D && bytes[2] == 0x4F && memberLayout &&
           bytes[4] == 0x4C && bytes[5] == 0x8D && bytes[6] == 0x87 &&
           bytes[11] == 0x48 && bytes[12] == 0x8B && bytes[13] == 0xD7;
    if (!arguments) return 0;
    // Exact validated compiler forms: vmovaps xmm0,xmm6 (2760) or xmm0,xmm8 (2944/3460).
    if (bytes[14] == 0xC5 && bytes[15] == 0xF8 && bytes[16] == 0x28 && bytes[17] == 0xC6) return 18;
    if (bytes[14] == 0xC4 && bytes[15] == 0xC1 && bytes[16] == 0x78 &&
        bytes[17] == 0x28 && bytes[18] == 0xC0) return 19;
    return 0;
}

inline bool MatchRegisterStrideTail(const uint8_t* bytes) {
    return bytes[0] == 0x49 && bytes[1] == 0x2B && bytes[2] == 0xDD && bytes[3] == 0x75;
}

inline bool MatchUnitStrideTail(const uint8_t* bytes) {
    return bytes[0] == 0x48 && bytes[1] == 0x83 && bytes[2] == 0xEB &&
           bytes[3] == 0x01 && bytes[4] == 0x75;
}

inline DiscoveryResult Discover(const uint8_t* image, size_t imageSize, uintptr_t imageBase,
                                const std::vector<ScanRange>& codeRanges = {}) {
    DiscoveryResult result{};
    if (image == nullptr || imageSize < 29) {
        result.reason = "invalid image";
        return result;
    }

    constexpr size_t kPatchSize = 5;
    constexpr size_t kMinContextSize = 18 + kPatchSize + 6;
    constexpr size_t kMaxCandidates = 4;
    constexpr size_t kMaxPairDistance = 0x400;
    std::vector<Candidate> contexts;

    const auto ranges = codeRanges.empty() ? std::vector<ScanRange>{{0, imageSize}} : codeRanges;
    for (const auto& range : ranges) {
    if (range.offset > imageSize || range.size > imageSize - range.offset) continue;
    for (size_t contextOffset = range.offset;
         contextOffset + kMinContextSize <= range.offset + range.size; ++contextOffset) {
        const uint8_t* context = image + contextOffset;
        const size_t prefixSize = MatchCommonPrefix(context);
        if (!prefixSize || prefixSize + kPatchSize + 6 > range.offset + range.size - contextOffset) {
            continue;
        }

        const size_t callOffset = contextOffset + prefixSize;
        const uint8_t* call = image + callOffset;
        const uint8_t* tail = call + kPatchSize;

        LoopKind loopKind{};
        if (MatchRegisterStrideTail(tail)) {
            loopKind = LoopKind::RegisterStride;
        } else if (MatchUnitStrideTail(tail)) {
            loopKind = LoopKind::UnitStride;
        } else {
            continue;
        }

        ++result.contextMatches;
        Candidate candidate{};
        candidate.callOffset = callOffset;
        candidate.loopKind = loopKind;

        if (call[0] == 0xE8) {
            int32_t displacement = 0;
            std::memcpy(&displacement, call + 1, sizeof(displacement));

            const int64_t nextInstruction = static_cast<int64_t>(imageBase) +
                                            static_cast<int64_t>(callOffset + kPatchSize);
            const int64_t target = nextInstruction + static_cast<int64_t>(displacement);
            const int64_t imageBegin = static_cast<int64_t>(imageBase);
            const int64_t imageEnd = imageBegin + static_cast<int64_t>(imageSize);
            if (target < imageBegin || target >= imageEnd) {
                result.reason = "call target outside image";
                return result;
            }
            if (!Contains(ranges, static_cast<size_t>(target - imageBegin), 1)) {
                result.reason = "call target outside executable spans";
                return result;
            }

            candidate.state = SiteState::Call;
            candidate.target = static_cast<uintptr_t>(target);
        } else if (IsNopped(call)) {
            candidate.state = SiteState::Nopped;
        } else {
            result.reason = "unexpected call bytes";
            return result;
        }

        contexts.push_back(candidate);
    }
    }

    if (result.contextMatches == 0) {
        result.reason = "no candidates";
        return result;
    }
    if (contexts.size() > kMaxCandidates) {
        result.reason = "too many candidates";
        return result;
    }

    size_t pairCount = 0;
    Candidate registerSite{};
    Candidate unitSite{};
    for (const Candidate& first : contexts) {
        for (const Candidate& second : contexts) {
            if (second.loopKind != LoopKind::UnitStride ||
                second.callOffset <= first.callOffset ||
                second.callOffset - first.callOffset > kMaxPairDistance) {
                continue;
            }
            ++pairCount;
            registerSite = first;
            unitSite = second;
        }
    }

    if (pairCount == 0) {
        result.reason = "no adjacent maneuver pair";
        return result;
    }
    if (pairCount != 1) {
        result.reason = "ambiguous adjacent pair";
        return result;
    }

    result.sites.push_back(registerSite);
    result.sites.push_back(unitSite);
    result.ignoredContextCount = contexts.size() - result.sites.size();
    for (const auto& context : contexts) {
        if (context.callOffset != registerSite.callOffset && context.callOffset != unitSite.callOffset)
            result.ignored.push_back(context);
    }

    uintptr_t sharedTarget = 0;
    for (const Candidate& site : result.sites) {
        if (site.state == SiteState::Nopped) {
            ++result.noppedCount;
            continue;
        }
        ++result.callCount;
        if (sharedTarget == 0) {
            sharedTarget = site.target;
        } else if (sharedTarget != site.target) {
            result.reason = "call targets differ";
            return result;
        }
    }

    result.sharedTarget = sharedTarget;
    result.valid = true;
    result.reason = "ok";
    return result;
}

inline const char* LoopKindName(LoopKind kind) {
    return kind == LoopKind::RegisterStride ? "register-stride" : "unit-stride";
}

inline const char* SiteStateName(SiteState state) {
    return state == SiteState::Call ? "call" : "nopped";
}

} // namespace straight_pull
