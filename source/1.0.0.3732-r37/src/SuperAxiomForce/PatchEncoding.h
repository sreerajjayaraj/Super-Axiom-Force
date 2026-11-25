#pragma once

#include <array>
#include <cstdint>
#include <cstring>

namespace safcompat {

struct PatchSpec {
    uint32_t rva, size, kind;
    std::array<uint8_t, 16> original;
};

// The r22 producer uses these same support offsets and encodings. The caller
// supplies the RVA and native original bytes from the current witness.
inline bool ExpectedPatch(const PatchSpec& spec, uintptr_t base, uintptr_t support, uint8_t* out) {
    constexpr uint32_t sizes[] = {4, 4, 15, 6, 10, 9};
    constexpr uint32_t offsets[] = {0x1000, 0x1008, 0x400, 0, 0, 0x200};
    if (!out || !support || spec.kind >= std::size(sizes) || spec.size != sizes[spec.kind]) return false;
    std::memset(out, 0x90, 16);
    if (spec.kind == 3) return true;
    const auto target = support + offsets[spec.kind];
    const auto delta = static_cast<int64_t>(target) - static_cast<int64_t>(base + spec.rva + spec.size);
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    if (spec.kind >= 2) out[spec.size - 5] = 0xE8;
    const auto encoded = static_cast<int32_t>(delta);
    std::memcpy(out + spec.size - 4, &encoded, sizeof(encoded));
    return true;
}

} // namespace safcompat
