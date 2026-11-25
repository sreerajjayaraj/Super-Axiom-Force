#pragma once

#include <algorithm>
#include <array>
#include "NativeDiscovery.h"
#include "StraightPullAngular.h"
#include "ClockAcceleration.h"

namespace saf_adaptive {

enum class Ref : uint8_t { ReadOnly, Writable, ExecutableData, ImageBase, FunctionStart, FunctionLabel, Leaf };

struct Field {
    uint16_t offset;
    uint8_t trailingBytes;
    Ref kind;
    uint8_t width;
    uint16_t targetOffset = 0;
    uint64_t prefix = 0;
    uint64_t prefixMask = 0;
    uint32_t targetSize = 0;
    uint16_t targetUnwindSize = 0;
    uint64_t targetUnwindHash = 0;
};

struct Profile {
    uint16_t anchorOffset, size;
    uint64_t bodyHash;
    uint16_t unwindSize;
    uint64_t unwindHash;
    const Field* fields;
    size_t fieldCount;
};

} // namespace saf_adaptive

#include "OwnerContracts.generated.h"

namespace saf_adaptive {

struct RuntimeOwner { uint32_t begin, end, unwind; };
static_assert(sizeof(RuntimeOwner) == 12);

struct Witness {
    size_t rva;
    std::vector<uint8_t> bytes;
    unsigned mask;
};

struct Plan {
    native::Result bindings;
    straight_pull::DiscoveryResult callers;
    size_t angularSite = 0;
    std::array<size_t, 2> selectedReturns{};
    std::vector<Witness> witnesses;
    const char* refusal = "unresolved";
};

inline uint64_t Fingerprint(const uint8_t* bytes, size_t count) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

inline void AddWitness(Plan& plan, size_t at, const uint8_t* bytes, size_t count, unsigned mask) {
    const auto existing = std::find_if(plan.witnesses.begin(), plan.witnesses.end(), [&](const Witness& witness) {
        return witness.rva == at && witness.bytes.size() == count && !std::memcmp(witness.bytes.data(), bytes, count);
    });
    if (existing != plan.witnesses.end()) existing->mask |= mask;
    else plan.witnesses.push_back({at, {bytes, bytes + count}, mask});
}

inline const uint8_t* ObservedBytes(const Plan& plan, size_t rva, size_t count) {
    for (const auto& witness : plan.witnesses) {
        if (rva >= witness.rva && rva - witness.rva <= witness.bytes.size() &&
            count <= witness.bytes.size() - (rva - witness.rva)) return witness.bytes.data() + (rva - witness.rva);
    }
    return nullptr;
}

inline size_t DecodeRelative(size_t field, const uint8_t* bytes) {
    if (!bytes) return SIZE_MAX;
    int32_t displacement = 0;
    std::memcpy(&displacement, bytes, sizeof(displacement));
    const auto target = static_cast<int64_t>(field) + 4 + displacement;
    return target >= 0 ? static_cast<size_t>(target) : SIZE_MAX;
}

inline size_t ObservedTarget(const Plan& plan, size_t field) {
    return DecodeRelative(field, ObservedBytes(plan, field, sizeof(int32_t)));
}

class Owners {
    const native::Image& image_;
    std::vector<RuntimeOwner> rows_;

    struct Destination {
        size_t rva = 0;
        uint64_t prefix = 0;
        const RuntimeOwner* owner = nullptr;
        std::vector<uint8_t> unwind;
    };

    const RuntimeOwner* Containing(size_t anchor) const {
        const auto upper = std::upper_bound(rows_.begin(), rows_.end(), anchor,
            [](size_t rva, const RuntimeOwner& row) { return rva < row.begin; });
        return upper != rows_.begin() && anchor < std::prev(upper)->end ? &*std::prev(upper) : nullptr;
    }

    bool ReferenceMatches(size_t rva, const Field& field, Destination& captured) const {
        switch (field.kind) {
        case Ref::ImageBase: return rva == 0;
        case Ref::ReadOnly: return image_.Readable(rva, field.width) &&
            !image_.Data(rva, field.width) && !image_.Code(rva, field.width);
        case Ref::Writable: return image_.Data(rva, field.width);
        case Ref::ExecutableData: return image_.Code(rva, field.width);
        default: break;
        }
        if (!image_.Code(rva, field.width)) return false;
        const auto* destination = Containing(rva);
        if (field.kind == Ref::Leaf ? destination != nullptr :
            !destination || rva - destination->begin != field.targetOffset ||
            destination->end - destination->begin != field.targetSize) return false;
        captured.rva = rva;
        std::memcpy(&captured.prefix, image_.bytes + rva, sizeof(captured.prefix));
        if ((captured.prefix & field.prefixMask) != field.prefix) return false;
        if (destination) {
            const size_t unwind = destination->unwind;
            if (!field.targetUnwindSize || !image_.Readable(unwind, field.targetUnwindSize) ||
                image_.Code(unwind, field.targetUnwindSize) || image_.Data(unwind, field.targetUnwindSize)) return false;
            captured.owner = destination;
            captured.unwind.assign(image_.bytes + unwind, image_.bytes + unwind + field.targetUnwindSize);
            if (Fingerprint(captured.unwind.data(), captured.unwind.size()) != field.targetUnwindHash) return false;
        }
        return true;
    }
public:
    explicit Owners(const native::Image& image) : image_(image) {}

    bool Read() {
        const auto span = image_.exceptions;
        if (!span.size || span.size % sizeof(RuntimeOwner) || !image_.Readable(span.offset, span.size) ||
            image_.Code(span.offset, span.size) || image_.Data(span.offset, span.size)) return false;
        rows_.resize(span.size / sizeof(RuntimeOwner));
        std::memcpy(rows_.data(), image_.bytes + span.offset, span.size);
        uint32_t previousEnd = 0;
        for (const auto& row : rows_) {
            if (row.begin < previousEnd || row.begin >= row.end || row.end > image_.size ||
                !row.unwind || row.unwind >= image_.size) return false;
            previousEnd = row.end;
        }
        return true;
    }

    const RuntimeOwner* Find(size_t anchor, size_t count) const {
        const auto* row = Containing(anchor);
        if (!row || count > row->end - anchor || !image_.Code(row->begin, row->end - row->begin)) return nullptr;
        return row;
    }

    void Record(Plan& plan, const RuntimeOwner& row, unsigned mask) const {
        const size_t rva = image_.exceptions.offset + sizeof(RuntimeOwner) * static_cast<size_t>(&row - rows_.data());
        const auto* copy = reinterpret_cast<const uint8_t*>(&row);
        AddWitness(plan, rva, copy, sizeof(row), mask);
    }

    template<size_t N>
    const RuntimeOwner* Prove(Plan& plan, size_t anchor, const Profile (&profiles)[N], unsigned mask) const {
        const auto* row = Find(anchor, 1);
        if (!row) return nullptr;
        for (const auto& profile : profiles) {
            if (anchor - row->begin != profile.anchorOffset || row->end - row->begin != profile.size ||
                !image_.Readable(row->unwind, profile.unwindSize) || image_.Code(row->unwind, profile.unwindSize) ||
                image_.Data(row->unwind, profile.unwindSize)) continue;
            const uint8_t* raw = image_.bytes + row->begin;
            const uint8_t* unwind = image_.bytes + row->unwind;
            std::vector<uint8_t> body(raw, raw + profile.size);
            std::vector<uint8_t> info(unwind, unwind + profile.unwindSize);
            if (Fingerprint(info.data(), info.size()) != profile.unwindHash) continue;
            auto normalized = body;
            std::vector<Destination> destinations;
            bool valid = true;
            for (size_t i = 0; i < profile.fieldCount; ++i) {
                const auto& field = profile.fields[i];
                int32_t displacement = 0;
                std::memcpy(&displacement, body.data() + field.offset, sizeof(displacement));
                const auto target = static_cast<int64_t>(row->begin) + field.offset + 4 + field.trailingBytes + displacement;
                if (target < 0 || static_cast<uint64_t>(target) >= image_.size) { valid = false; break; }
                Destination captured;
                if (!ReferenceMatches(static_cast<size_t>(target), field, captured)) { valid = false; break; }
                if (field.prefixMask) destinations.push_back(std::move(captured));
                std::memset(normalized.data() + field.offset, 0, 4);
            }
            if (!valid || Fingerprint(normalized.data(), normalized.size()) != profile.bodyHash) continue;
            Record(plan, *row, mask);
            plan.witnesses.push_back({row->begin, std::move(body), mask});
            plan.witnesses.push_back({row->unwind, std::move(info), mask});
            for (const auto& destination : destinations) {
                if (destination.owner) {
                    Record(plan, *destination.owner, mask);
                    AddWitness(plan, destination.owner->unwind, destination.unwind.data(), destination.unwind.size(), mask);
                }
                AddWitness(plan, destination.rva, reinterpret_cast<const uint8_t*>(&destination.prefix), sizeof(uint64_t), mask);
            }
            return row;
        }
        return nullptr;
    }
};

inline bool ProveInitializers(const native::Image& image, const Owners& owners, Plan& plan) {
    constexpr uint8_t load[] = {0xC5, 0xFA, 0x10, 0x05};
    constexpr uint8_t store[] = {0xC5, 0xFA, 0x11, 0x05};
    constexpr uint32_t expected[] = {0x43800000, 0x41A00000, 0x42200000}; // 256, 20, 40
    const std::array<size_t, 3> globals = {plan.bindings.range.target, plan.bindings.limit.target, plan.bindings.pull.target};
    unsigned hits[3]{};
    for (const auto& code : image.code) {
        if (code.size < 16) continue;
        const auto* last = image.bytes + code.offset + code.size - 16;
        for (auto* at = image.bytes + code.offset; at <= last; ++at) {
            at = static_cast<const uint8_t*>(std::memchr(at, load[0], static_cast<size_t>(last - at + 1)));
            if (!at) break;
            if (std::memcmp(at, load, sizeof(load)) || std::memcmp(at + 8, store, sizeof(store))) continue;
            const size_t rva = static_cast<size_t>(at - image.bytes);
            uint8_t instructions[16]{};
            std::memcpy(instructions, at, sizeof(instructions));
            if (std::memcmp(instructions, load, sizeof(load)) || std::memcmp(instructions + 8, store, sizeof(store))) continue;
            const size_t destination = DecodeRelative(rva + 12, instructions + 12);
            const auto match = std::find(globals.begin(), globals.end(), destination);
            if (match == globals.end()) continue;
            const size_t index = static_cast<size_t>(match - globals.begin());
            const size_t source = DecodeRelative(rva + 4, instructions + 4);
            const auto* owner = owners.Find(rva, 16);
            uint32_t bits = 0;
            if (++hits[index] != 1 || !owner || !image.Readable(owner->unwind, 4) ||
                (rva - owner->begin != 0x5B && rva - owner->begin != 0x63) ||
                !image.Readable(source, 4) || image.Code(source, 4) || image.Data(source, 4)) return false;
            uint8_t unwind[4]{};
            std::memcpy(unwind, image.bytes + owner->unwind, sizeof(unwind));
            if (std::memcmp(unwind, "\x11\x0C\x04\x00", sizeof(unwind))) return false;
            std::memcpy(&bits, image.bytes + source, sizeof(bits));
            if (bits != expected[index]) return false;
            const unsigned mask = index == 2 ? 2 : 1;
            owners.Record(plan, *owner, mask);
            AddWitness(plan, rva, instructions, sizeof(instructions), mask);
            AddWitness(plan, owner->unwind, unwind, sizeof(unwind), mask);
            AddWitness(plan, source, reinterpret_cast<const uint8_t*>(&bits), sizeof(bits), mask);
        }
    }
    return hits[0] == 1 && hits[1] == 1 && hits[2] == 1;
}

inline Plan Resolve(const native::Image& image, uintptr_t base) {
    Plan plan;
    auto& n = plan.bindings;
    auto& s = plan.callers;
    if (!image.bytes || image.code.empty()) return plan;
    Owners owners(image);
    plan.refusal = "malformed or ambiguous native exception table";
    if (!owners.Read()) return plan;
    plan.refusal = "missing, ambiguous or redirected native patch context";
    n = native::Discover(image);
    if (!n.range.valid || !n.limit.valid || !n.pull.valid || !n.scopedPull.binding.valid ||
        !n.activation.binding.valid || !n.activation.canRestore || !n.reel.binding.valid || !n.reel.canRestore ||
        !n.propell.binding.valid || !n.propell.canRestore || !n.clock[0].valid || !n.clock[1].valid ||
        !std::all_of(n.scopedRange.begin(), n.scopedRange.end(), [](const auto& p) { return p.binding.valid && p.canRestore; })) return plan;
    plan.refusal = "native range, limit or pull constructor changed";
    if (!ProveInitializers(image, owners, plan)) return plan;
    plan.refusal = "native range or pull owner changed";
    const auto* range = owners.Prove(plan, n.range.instruction, kRange, 1);
    if (!range || owners.Find(n.limit.instruction, 25) != range ||
        !owners.Prove(plan, ObservedTarget(plan, range->begin + 0xA8), kRangeStatusSlot, 1) ||
        !owners.Prove(plan, n.scopedRange[1].binding.instruction, kCandidate, 1) ||
        !owners.Prove(plan, n.scopedRange[2].binding.instruction, kLeash, 1) ||
        !owners.Prove(plan, n.pull.instruction, kPull, 2)) return plan;
    const auto originalsMatch = [&](const native::CodePatch& patch) {
        const auto* captured = ObservedBytes(plan, patch.offset, patch.size);
        return captured && !std::memcmp(captured, patch.original.data(), patch.size);
    };
    if (!std::all_of(n.scopedRange.begin(), n.scopedRange.end(), originalsMatch) || !originalsMatch(n.scopedPull)) return plan;
    plan.refusal = "activation branch, native callee or clock owner changed";
    const auto* activation = owners.Prove(plan, n.activation.binding.instruction, kActivation, 8);
    const auto* reel = owners.Prove(plan, n.clock[0].instruction, kReel, 8);
    const auto* propell = owners.Prove(plan, n.clock[1].instruction, kPropellClock, 8);
    const auto* duration = owners.Prove(plan, n.propell.binding.instruction, kPropellDuration, 8);
    if (!activation || !originalsMatch(n.activation) || ObservedTarget(plan, n.activation.offset + 2) != activation->begin + 0xBC ||
        !owners.Prove(plan, ObservedTarget(plan, n.activation.binding.instruction + 28), kActivationCall, 8) ||
        !reel || owners.Find(n.reel.binding.instruction, 23) != reel ||
        !propell || !duration || propell->end != duration->begin) return plan;
    plan.refusal = "native StraightPull helper, callers or quaternion contract changed";
    s = straight_pull::Discover(image.bytes, image.size, base, image.code);
    if (!s.valid || s.callCount != 2 || s.sites.size() != 2 || s.ignored.size() != 1 ||
        s.ignored[0].state != straight_pull::SiteState::Call || s.ignored[0].target != s.sharedTarget || s.sharedTarget < base) return plan;
    constexpr const char* angular = "C4 C1 22 59 DE C4 C1 32 59 D6 C4 C1 2A 59 CE 48 8D 4C 24 40 E8 ?? ?? ?? ??";
    const auto math = native::Find(image, angular);
    if (!math.valid || s.sites[0].callOffset < sizeof(straight_pull_angular::kFirstArguments) || s.ignored[0].callOffset < 18) return plan;
    const auto* helper = owners.Prove(plan, math.instruction, kAngular, 4);
    const auto* callers = owners.Prove(plan, s.sites[0].callOffset - sizeof(straight_pull_angular::kFirstArguments), kSelected, 4);
    if (!helper || helper->begin != s.sharedTarget - base || !callers ||
        s.sites[1].callOffset != callers->begin + 0x4BE ||
        !owners.Prove(plan, s.ignored[0].callOffset - 18, kIgnored, 4) ||
        !owners.Prove(plan, ObservedTarget(plan, math.instruction + 21), kQuaternion, 4) ||
        ObservedTarget(plan, s.sites[0].callOffset + 1) != helper->begin ||
        ObservedTarget(plan, s.sites[1].callOffset + 1) != helper->begin ||
        ObservedTarget(plan, s.ignored[0].callOffset + 1) != helper->begin) return plan;
    plan.angularSite = math.instruction;
    plan.selectedReturns = {s.sites[0].callOffset + 5, s.sites[1].callOffset + 5};
    plan.refusal = nullptr;
    return plan;
}

} // namespace saf_adaptive
