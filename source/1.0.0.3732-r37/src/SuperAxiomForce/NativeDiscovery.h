#pragma once

#include "StraightPullDiscovery.h"
#include <array>
#include <cstdlib>
#include <sstream>

namespace native {

using Range = straight_pull::ScanRange;

struct Image {
    const uint8_t* bytes = nullptr;
    size_t size = 0;
    std::vector<Range> code;
    std::vector<Range> writable;
    std::vector<Range> readable;
    Range exceptions{};

    bool Code(size_t offset, size_t count) const {
        return offset < size && count <= size - offset &&
               straight_pull::Contains(code, offset, count);
    }
    bool Data(size_t offset, size_t count) const {
        return offset < size && count <= size - offset &&
               straight_pull::Contains(writable, offset, count) && !Code(offset, count);
    }
    bool Readable(size_t offset, size_t count) const {
        return offset < size && count <= size - offset && straight_pull::Contains(readable, offset, count);
    }
};

struct Binding {
    size_t matches = 0;
    size_t instruction = 0;
    size_t target = 0;
    bool valid = false;
};

struct CodePatch {
    Binding binding;
    size_t offset = 0;
    size_t size = 0;
    std::array<uint8_t, 16> original{};
    std::array<uint8_t, 16> patch{};
    bool canRestore = false;
};

struct Result {
    Binding range;
    Binding limit;
    Binding pull;
    std::array<CodePatch, 6> scopedRange{};
    std::array<Binding, 2> clock{};
    CodePatch scopedPull{};
    CodePatch activation;
    CodePatch reel;
    CodePatch propell;
};

// Exact disassembled 3460 instruction contexts, not RVA or float-pair guesses.
inline constexpr const char* kRange =
    "C5 FA 10 3D ?? ?? ?? ?? C5 F8 2F F7 76 07 C4 41 78 28 DE EB 59";
inline constexpr const char* kLimit =
    "48 8D 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 80 BD 18 15 00 00 00 48 0F 44 C1 C5 FA 10 38";
inline constexpr const char* kPull =
    "C5 FA 10 35 ?? ?? ?? ?? C5 F8 2F F7 76 3C C4 C1 7A 5C E2 C4 C1 22 5C C9 C4 C1 52 5C C0";
inline constexpr const char* kActivation =
    "4C 3B F3 ?? ?? ?? ?? ?? ?? C6 44 24 20 01 45 33 C9 41 B0 02 40 0F B6 D6 48 8B CF E8 ?? ?? ?? ?? E9";
inline constexpr const char* kReel =
    "C4 C1 78 2F CA ?? 0F C5 FA 5E C1 C4 C1 7A 5D CB C5 AA 5F F1 EB 05 C4 C1 78 28 F3";
inline constexpr const char* kPropell =
    "C5 FA 10 9C 1F 24 0B 00 00 C4 C1 78 2F DB C4 C1 7A 59 D4 C5 6A 58 6C 24 74 ?? 1F "
    "C5 FA 10 84 1F 20 0B 00 00 C5 FA 5E CB C4 C1 72 5D D1 C5 22 5F D2 C4 41 78 2F D1 72 13 EB 05 C4 41 78 28 D1";

inline std::vector<int> ParsePattern(const char* text) {
    std::istringstream input(text);
    std::vector<int> pattern;
    std::string token;
    while (input >> token) pattern.push_back(token == "??" ? -1 :
        static_cast<int>(std::strtoul(token.c_str(), nullptr, 16)));
    return pattern;
}

inline Binding Find(const Image& image, const char* text) {
    Binding result;
    const auto pattern = ParsePattern(text);
    if (!image.bytes || pattern.empty()) return result;
    for (const auto& span : image.code) {
        if (!image.Code(span.offset, span.size) || span.size < pattern.size()) continue;
        const size_t last = span.offset + span.size - pattern.size();
        for (size_t at = span.offset; at <= last; ++at) {
            if (pattern[0] >= 0) {
                const auto* found = static_cast<const uint8_t*>(std::memchr(image.bytes + at, pattern[0], last - at + 1));
                if (!found) break;
                at = static_cast<size_t>(found - image.bytes);
            }
            size_t i = 1;
            for (; i < pattern.size(); ++i) {
                if (pattern[i] >= 0 && image.bytes[at + i] != pattern[i]) break;
            }
            if (i == pattern.size()) {
                ++result.matches;
                result.instruction = at;
            }
        }
    }
    result.valid = result.matches == 1;
    return result;
}

inline size_t RelativeTarget(const Image& image, size_t field) {
    if (!image.Code(field, 4)) return image.size;
    int32_t displacement = 0;
    std::memcpy(&displacement, image.bytes + field, 4);
    const int64_t target = static_cast<int64_t>(field) + 4 + displacement;
    return target >= 0 && static_cast<uint64_t>(target) < image.size ?
        static_cast<size_t>(target) : image.size;
}

inline Binding FloatBinding(const Image& image, const char* pattern, size_t fieldOffset) {
    auto result = Find(image, pattern);
    if (result.valid) {
        result.target = RelativeTarget(image, result.instruction + fieldOffset);
        result.valid = result.target % alignof(float) == 0 && image.Data(result.target, 4);
    }
    return result;
}

inline CodePatch Duration(const Image& image, const char* pattern, size_t branch) {
    CodePatch result;
    result.binding = Find(image, pattern);
    result.size = 2;
    if (!result.binding.valid) return result;
    result.offset = result.binding.instruction + branch;
    const auto* bytes = image.bytes + result.offset;
    result.canRestore = bytes[0] == 0x76;
    result.binding.valid = result.canRestore || bytes[0] == 0xEB;
    result.original = {0x76, bytes[1]};
    result.patch = {0xEB, bytes[1]};
    return result;
}

// Only these geometric readers may use the configured reach. The common
// Limit float also controls native velocity/damping; it must never be written.
inline constexpr const char* kCandidateRange =
    "C5 F8 2F 35 ?? ?? ?? ?? 76 14 48 89 7C 24 28 48 8D 4C 24 28 E8 ?? ?? ?? ?? E9";
inline constexpr const char* kLeashRange =
    "48 8D 0D ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 41 80 7D 00 06 48 0F 45 C1 C5 7A 10 00 C5 3A 58 0D";

#ifdef SAF_OWNERSHIP_TESTING
inline void (*g_testBeforeRedirectSnapshot)(const Image&, size_t) = nullptr;
inline void (*g_testAfterActivationSnapshot)(const Image&, size_t) = nullptr;
#endif

inline CodePatch RedirectRange(const Image& image, Binding binding, size_t fieldOffset,
                               const Binding& common, const Binding& range, bool same = false) {
    CodePatch patch{};
    patch.binding = binding;
    patch.size = 4;
    if (!binding.valid || !common.valid || !range.valid || (!same && common.target == range.target)) {
        patch.binding.valid = false;
        return patch;
    }
    patch.offset = binding.instruction + fieldOffset;
    const auto current = RelativeTarget(image, patch.offset);
    patch.binding.valid = current == common.target || current == range.target;
    if (!patch.binding.valid) return patch;
    const auto displacement = static_cast<int64_t>(range.target) - static_cast<int64_t>(patch.offset + 4);
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        patch.binding.valid = false;
        return patch;
    }
#ifdef SAF_OWNERSHIP_TESTING
    if (g_testBeforeRedirectSnapshot) g_testBeforeRedirectSnapshot(image, patch.offset);
#endif
    const int32_t originalDisplacement = static_cast<int32_t>(static_cast<int64_t>(current) - static_cast<int64_t>(patch.offset + 4));
    std::memcpy(patch.original.data(), &originalDisplacement, 4);
    const int32_t encoded = static_cast<int32_t>(displacement);
    std::memcpy(patch.patch.data(), &encoded, 4);
    patch.canRestore = current == common.target;
    return patch;
}

inline constexpr const char* kReelClock =
    "C4 C1 72 58 84 0E 20 0B 00 00 C4 C1 7A 11 84 0E 20 0B 00 00 C4 C1 7A 10 8C 0E 24 0B 00 00";
inline constexpr const char* kPropellClock =
    "C5 F2 58 84 0F 20 0B 00 00 C5 FA 11 84 0F 20 0B 00 00 48 8B 8C 0F C0 09 00 00 48 85 C9 0F 84";

inline Result Discover(const Image& image) {
    Result result;
    result.range = FloatBinding(image, kRange, 4);
    result.pull = FloatBinding(image, kPull, 4);
    result.limit = FloatBinding(image, kLimit, 3);
    // Independently prove the selector's other operand is the same Range global.
    if (result.limit.valid) {
        const size_t selectedRange = RelativeTarget(image, result.limit.instruction + 10);
        result.limit.valid = result.range.valid && selectedRange == result.range.target &&
                             result.limit.target != selectedRange;
    }
    if (result.pull.valid && ((result.range.valid && result.pull.target == result.range.target) ||
                            (result.limit.valid && result.pull.target == result.limit.target))) {
        result.pull.valid = false;
    }
    auto& activation = result.activation;
    activation.binding = Find(image, kActivation);
    activation.size = 6;
    if (activation.binding.valid) {
        activation.offset = activation.binding.instruction + 3;
        std::memcpy(activation.original.data(), image.bytes + activation.offset, 6);
#ifdef SAF_OWNERSHIP_TESTING
        if (g_testAfterActivationSnapshot) g_testAfterActivationSnapshot(image, activation.offset);
#endif
        const auto* bytes = activation.original.data();
        activation.patch.fill(0x90);
        activation.canRestore = bytes[0] == 0x0F && bytes[1] == 0x86;
        int32_t capturedDelta = 0;
        std::memcpy(&capturedDelta, bytes + 2, 4);
        const int64_t capturedDestination = static_cast<int64_t>(activation.offset + 6) + capturedDelta;
        const size_t destination = capturedDestination >= 0 && static_cast<uint64_t>(capturedDestination) < image.size
            ? static_cast<size_t>(capturedDestination) : image.size;
        const bool original = activation.canRestore && image.Code(destination, 1) &&
            destination < activation.offset && activation.offset - destination <= 0x200;
        const bool external = std::memcmp(bytes, activation.patch.data(), 6) == 0;
        const size_t callee = RelativeTarget(image, activation.binding.instruction + 28);
        activation.binding.valid = (original || external) && image.Code(callee, 1);
    }
    result.scopedRange[0] = RedirectRange(image, Find(image, kLimit), 3, result.limit, result.range);
    result.scopedRange[1] = RedirectRange(image, Find(image, kCandidateRange), 4, result.limit, result.range);
    auto leash = Find(image, kLeashRange);
    if (leash.valid && RelativeTarget(image, leash.instruction + 10) != result.range.target) leash.valid = false;
    result.scopedRange[2] = RedirectRange(image, leash, 3, result.limit, result.range);
    result.scopedRange[3] = RedirectRange(image, result.range, 4, result.range, result.range, true);
    result.scopedRange[4] = RedirectRange(image, Find(image, kLimit), 10, result.range, result.range, true);
    result.scopedRange[5] = RedirectRange(image, leash, 10, result.range, result.range, true);
    result.scopedPull = RedirectRange(image, result.pull, 4, result.pull, result.pull, true);
    result.clock[0] = Find(image, kReelClock);
    result.clock[1] = Find(image, kPropellClock);

    result.reel = Duration(image, kReel, 5);
    result.propell = Duration(image, kPropell, 25);
    return result;
}

} // namespace native
