#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "NearbySurface.h"

namespace axiom::trace {
enum class Path : uint8_t { None, Primary, Alternate };

enum class ResetReason : uint8_t { Unknown, ActorMode, ContextOverride };
inline constexpr uint8_t kActorResetMarker=2;

// Only the relay on the taken actor-mode edge can replace the native 0/1 flag.
inline ResetReason DecodeResetReason(uint8_t flag) {
    if (flag==kActorResetMarker) return ResetReason::ActorMode;
    if (flag<=1) return ResetReason::ContextOverride;
    return ResetReason::Unknown;
}

struct RayCandidate {
    float point[3]{};
    uint32_t kind=0;
    uint32_t material=0;
    bool body=false;
};

enum class RaySource : uint8_t { Unknown, WithoutBody, WithBody };

inline constexpr size_t kRayCandidateLimit=16;

struct Sample {
    uint64_t tick=0;
    float from[3]{}, to[3]{}, hit[3]{};
    float rayLength=0.f;
    uint32_t count=0;
    std::array<RayCandidate,kRayCandidateLimit> candidates{};
    unsigned converted=0;
    bool probeTried=false, probeOffered=false, retryHeadBody=false;
    unsigned gridPrefix=0, retryHits=0, retryGridPrefix=0;
    uint32_t retryHeadMaterial=0;
    surface::Recovery recovery=surface::Recovery::None;
    float redRayDistance=0.f, offeredRayDistance=0.f;
    Path path=Path::None;
    uint16_t queryKind=0;
    ResetReason resetReason=ResetReason::Unknown;
    uint8_t mode=255, beforeMode=255, sampledActorMode=255;
    bool queried=false, raw=false, beforeRaw=false, reset=false;
};

struct RayProvenance {
    RaySource source=RaySource::Unknown;
    uint32_t kind=0;
    uint32_t material=0;
};

inline RayProvenance PublishedHit(const Sample& sample) {
    // A repeated position without unique provenance is intentionally ambiguous.
    if (!sample.raw || !sample.queried || sample.path!=Path::Primary ||
        sample.count!=sample.converted || sample.converted>sample.candidates.size()) return {};
    const RayCandidate* match=nullptr;
    for (unsigned i=0;i<sample.converted;++i) {
        const auto& hit=sample.candidates[i];
        if (std::memcmp(hit.point,sample.hit,sizeof(hit.point))!=0) continue;
        if (match) return {};
        match=&hit;
    }
    if (!match) return {};
    return {match->body?RaySource::WithBody:RaySource::WithoutBody,match->kind,match->material};
}

enum class Result { NoQuery, NoPhysicsHit, Rejected, Raw, Accepted, StateUnavailable };

inline Result Outcome(const Sample& sample) {
    if (!sample.queried) return Result::NoQuery;
    if (sample.mode>2) return Result::StateUnavailable;
    if ((sample.path==Path::Primary && sample.mode==0) ||
        (sample.path==Path::Alternate && sample.mode==1)) return Result::Accepted;
    if (sample.raw) return Result::Raw;
    return sample.count ? Result::Rejected : Result::NoPhysicsHit;
}

inline bool RecordQuery(Sample& sample,uint32_t mask,const float (&from)[3],
                        const float (&to)[3],uint32_t count) {
    if (sample.queried || mask!=0x70) return false;
    const float distance=std::hypot(to[0]-from[0],to[1]-from[1],to[2]-from[2]);
    if (!std::isfinite(distance)) return false;
    std::memcpy(sample.from,from,sizeof(from));
    std::memcpy(sample.to,to,sizeof(to));
    sample.rayLength=distance;
    sample.count=count;
    sample.queried=true;
    return true;
}

inline void RecordSweep(Sample& sample,uint32_t count,uint16_t kind,
                        const float (&from)[3],const float (&to)[3]) {
    if (sample.queried) return;
    std::memcpy(sample.from,from,sizeof(from));
    std::memcpy(sample.to,to,sizeof(to));
    sample.count=count;
    sample.queryKind=kind;
    sample.queried=true;
}

inline bool CursorCleared(const Sample& sample) {
    return sample.beforeMode<=2 && (sample.beforeMode<2 || sample.beforeRaw) &&
           sample.mode==2 && !sample.raw;
}

inline bool RelativeBranch(uintptr_t next,uintptr_t target,uint8_t opcode,std::array<uint8_t,5>& code) {
    const auto magnitude=target>=next?target-next:next-target;
    if (magnitude>(target>=next?0x7fffffffULL:0x80000000ULL)) return false;
    const auto displacement=static_cast<int32_t>(target>=next
        ?static_cast<int64_t>(magnitude):-static_cast<int64_t>(magnitude));
    code[0]=opcode;
    std::memcpy(code.data()+1,&displacement,sizeof(displacement));
    return true;
}

inline bool RelativeCall(uintptr_t next,uintptr_t target,std::array<uint8_t,5>& code) {
    return RelativeBranch(next,target,0xE8,code);
}

inline std::array<uint8_t,14> Relay(uint64_t target) {
    std::array<uint8_t,14> code{0xFF,0x25};
    std::memcpy(code.data()+6,&target,sizeof(target));
    return code;
}

// Replay the native compare and split. The flag slot is dead on the reset path.
inline std::array<uint8_t,38> ResetDecisionRelay(uint64_t reset,uint64_t proceed) {
    std::array<uint8_t,38> code{
        0x80,0x7D,0x67,0x00,               // cmp byte ptr [rbp+0x67], 0
        0x74,0x12,                         // je to the original no-reset continuation
        0xC6,0x45,0x67,kActorResetMarker};  // mov byte ptr [rbp+0x67], marker
    const auto resetJump=Relay(reset);
    const auto proceedJump=Relay(proceed);
    std::memcpy(code.data()+10,resetJump.data(),resetJump.size());
    std::memcpy(code.data()+24,proceedJump.data(),proceedJump.size());
    return code;
}
}

