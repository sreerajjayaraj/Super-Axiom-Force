// This file is included after the existing ray observer helpers.
#include <cstddef>
#include "NearbySurfaceNative.h"

using axiom::surface::Point;
using axiom::surface::Recovery;

using NativeSurfaceQuery=bool(*)(void*,const void*,uint8_t,void*);
using NativeSurfaceCommit=void(*)(void*,uint8_t,const void*);
using NativeCommitAnchor=bool(*)(void*,uint8_t,void*);
using NativeCastDispatch=void*(*)(void*,void*,void*,void*,const void*);
using NativeAllocTls=void*(*)();
using NativeArrayFree=void(*)(void*);
constexpr uintptr_t kSurfaceQuery=0x3D792E0;
constexpr uintptr_t kSurfaceCommit=0x689B00;
constexpr uintptr_t kCommitAnchor=0x68CC50;
constexpr uintptr_t kCastDispatch=0x5CBF40;
DWORD g_commitTls=TLS_OUT_OF_INDEXES;

std::atomic<bool> g_searchReady{false};
std::atomic<bool> g_allHitsCollectorReady{false};
std::atomic<uint32_t> g_effectiveRangeBits{0};
std::atomic<unsigned> g_probeIndex{0};
std::atomic<uint64_t> g_nextSurfaceProbe{0};

float CurrentEffectiveRange() {
    const auto bits=g_effectiveRangeBits.load(std::memory_order_relaxed);
    float range=0;
    std::memcpy(&range,&bits,sizeof(range));
    return range;
}

using NativeTargetKey=std::array<uint8_t,32>;

struct AimSurface {
    void* component=nullptr;
    Point origin{},point{};
    uint64_t tick=0;
    float distance=0,range=0;
    bool unlimited=false;
    NativeTargetKey key{};
    Point normal{};
    uint64_t castTick=0;
    Recovery recovery=Recovery::None;
    float redRayDistance=0.f;
};

AimSurface g_lastRed{};
AimSurface g_publishedRecovery{};
std::array<AimSurface,16> g_castEvents{};
size_t g_castCount=0;
std::array<AimSurface,16> g_dispatchedTethers{};
size_t g_dispatchedIndex=0;

void RememberRedHit(const TargetFrame& frame,bool unlimited) {
    if (!frame.world || frame.sample.probeOffered) return;
    const auto& hit=frame.sample;
    AimSurface red{};
    if (hit.mode==2 && axiom::trace::PublishedHit(hit).source==axiom::trace::RaySource::WithoutBody) {
        red.component=frame.component;
        std::memcpy(red.origin.data(),hit.from,sizeof(red.origin));
        std::memcpy(red.point.data(),hit.hit,sizeof(red.point));
        red.tick=hit.tick;
        red.range=frame.effectiveRange;
        red.unlimited=unlimited;
    }
    AcquireSRWLockExclusive(&g_traceLock);
    g_lastRed=red;
    ReleaseSRWLockExclusive(&g_traceLock);
}

bool KnownRedHit(const TargetFrame& frame,const Point& origin,const Point& point) {
    if (!TryAcquireSRWLockShared(&g_traceLock)) return false;
    const auto red=g_lastRed;
    ReleaseSRWLockShared(&g_traceLock);
    return red.component==frame.component && frame.sample.tick>=red.tick &&
           frame.sample.tick-red.tick<=120 && red.unlimited==frame.unlimited &&
           red.range==frame.effectiveRange &&
           axiom::surface::Distance(red.origin,origin)<=0.5f &&
           axiom::surface::Distance(red.point,point)<=0.20f;
}

void ClearPublishedRecovery(void* component) {
    AcquireSRWLockExclusive(&g_traceLock);
    if (g_publishedRecovery.component==component) g_publishedRecovery={};
    ReleaseSRWLockExclusive(&g_traceLock);
}

void RememberRecovery(const TargetFrame& frame,bool unlimited) {
    AimSurface target{};
    target.component=frame.component;
    std::memcpy(target.origin.data(),frame.sample.from,sizeof(target.origin));
    target.point=frame.offeredPoint;
    target.tick=frame.sample.tick;
    target.distance=axiom::surface::Distance(target.origin,target.point);
    target.range=frame.effectiveRange;
    target.unlimited=unlimited;
    target.recovery=frame.sample.recovery;
    target.redRayDistance=frame.sample.redRayDistance;
    const auto component=reinterpret_cast<uintptr_t>(frame.component);
    if (!Get(component+0xF0,target.key) || (target.key[0]!=1 && target.key[0]!=2) ||
        !Get(component+0x114,target.normal)) {
        ClearPublishedRecovery(frame.component);
        return;
    }
    AcquireSRWLockExclusive(&g_traceLock);
    g_publishedRecovery=target;
    // Successful native acceptance keeps the retry eligible while this same
    // red point remains under the cursor. Movement or a range change expires it.
    g_lastRed=target;
    g_lastRed.point=frame.redPoint;
    ReleaseSRWLockExclusive(&g_traceLock);
}

AimSurface MatchCast(const NativeTargetKey& descriptor,const Point& normal,uint64_t tick) {
    // The cast can normalize type 1 to type 2 and resolve a native
    // handle. The packed position and surface vector stay unchanged.
    const bool unlimited=g_traceUnlimited.load(std::memory_order_relaxed);
    const float range=CurrentEffectiveRange();
    AcquireSRWLockShared(&g_traceLock);
    const auto target=g_publishedRecovery;
    ReleaseSRWLockShared(&g_traceLock);
    const auto component=reinterpret_cast<uintptr_t>(target.component);
    NativeTargetKey current{};
    uint8_t mode=2;
    if (!target.tick || tick<target.tick || tick-target.tick>500 ||
        target.unlimited!=unlimited || target.range!=range || !ControlledCatch(component) ||
        !Get(component+0x140,mode) || mode!=0 ||
        !Get(component+0xF0,current) || current[0]!=target.key[0] ||
        std::memcmp(current.data()+4,target.key.data()+4,28)!=0 ||
        descriptor[0]!=2 || std::memcmp(descriptor.data()+7,target.key.data()+8,16)!=0 ||
        std::memcmp(normal.data(),target.normal.data(),sizeof(normal))!=0) return {};
    uint64_t nativeHandle=0;
    std::memcpy(&nativeHandle,descriptor.data()+23,sizeof(nativeHandle));
    if (target.key[0]==2 &&
        (std::memcmp(descriptor.data()+3,target.key.data()+4,4)!=0 ||
         std::memcmp(descriptor.data()+23,target.key.data()+24,8)!=0)) return {};
    if (target.key[0]==1 && nativeHandle==UINT64_MAX) return {};
    return target;
}

void RecordCast(AimSurface cast,uint64_t tick) {
    cast.castTick=tick;
    AcquireSRWLockExclusive(&g_traceLock);
    g_dispatchedTethers[g_dispatchedIndex++%g_dispatchedTethers.size()]=cast;
    if (g_castCount<g_castEvents.size()) g_castEvents[g_castCount++]=cast;
    ReleaseSRWLockExclusive(&g_traceLock);
}

AimSurface TakeRecovery(void* component,const Point& anchor,uint64_t tick,bool unlimited,float range) {
    AimSurface* match=nullptr;
    AcquireSRWLockExclusive(&g_traceLock);
    for (auto& entry:g_dispatchedTethers) {
        if (entry.component==component && entry.unlimited==unlimited && entry.range==range &&
            entry.castTick && tick>=entry.castTick && tick-entry.castTick<=5000 &&
            (!match || entry.castTick>match->castTick) &&
            axiom::surface::SameAnchor(entry.point,anchor)) match=&entry;
    }
    AimSurface result{};
    if (match) {
        result=*match;
        *match={};
    }
    ReleaseSRWLockExclusive(&g_traceLock);
    return result;
}

struct SurfaceAttachment {
    uint64_t tick=0;
    float distance=0;
    uint8_t lane=0,kind=0;
    bool unlimited=false,anchorKnown=false;
    uint64_t recoveredTick=0,castTick=0;
    float recoveredDistance=0,effectiveRange=0;
    Point anchor{};
    Recovery recovery=Recovery::None;
    float redRayDistance=0.f;
};

struct CommitFrame {
    void* component=nullptr;
    uint8_t lane=0;
    Point anchor{};
    bool known=false;
};

std::array<SurfaceAttachment,16> g_attachmentEvents{};
size_t g_attachmentCount=0;
unsigned g_attachmentDropped=0;

#ifdef AXIOM_TESTING
NativeSurfaceQuery g_testSurfaceQuery=nullptr;
NativeSurfaceCommit g_testSurfaceCommit=nullptr;
NativeCommitAnchor g_testCommitAnchor=nullptr;
NativeArrayFree g_testFreeProbeVector=nullptr;
NativeCastDispatch g_testCastDispatch=nullptr;
#endif

struct NativeRayCandidate {
    std::array<uint8_t,0x58> bytes{};

    Point Position() const {
        Point point{};
        std::memcpy(point.data(),bytes.data()+8,sizeof(point));
        return point;
    }
    uint32_t Material() const {
        uint32_t value=0;
        std::memcpy(&value,bytes.data()+0x48,sizeof(value));
        return value;
    }
    bool HasBody() const {
        uintptr_t value=0;
        std::memcpy(&value,bytes.data()+0x50,sizeof(value));
        return value!=0;
    }
};

struct NativeRayVector {
    NativeRayCandidate* entries=nullptr;
    uint32_t count=0,capacity=0;
};
// Native growth identifies inline storage by comparing entries with
// &vector + 0x10. Separate stack objects take its heap-free path.
struct alignas(16) NativeRayBuffer {
    NativeRayVector vector{};
    std::array<NativeRayCandidate,8> inlineHits{};

    NativeRayBuffer() noexcept {
        vector={inlineHits.data(),0,static_cast<uint32_t>(inlineHits.size())};
    }
};
static_assert(sizeof(NativeRayCandidate)==0x58 && sizeof(NativeRayVector)==0x10);
static_assert(offsetof(NativeRayBuffer,vector)==0 && offsetof(NativeRayBuffer,inlineHits)==0x10);

struct NativeRayOutput {
    NativeRayVector* vector=nullptr;
    void* alternate=nullptr;
};

// Match the native destructor's allocator choice. The vector's entries are
// plain collision records. Their body references are owned by the game.
__declspec(noinline) __declspec(guard(nocf))
void FreeProbeVector(NativeRayCandidate* entries) {
#ifdef AXIOM_TESTING
    if (g_testFreeProbeVector) {
        g_testFreeProbeVector(entries);
        return;
    }
#endif
    reinterpret_cast<NativeAllocTls>(TargetAddress(0x1595740))();
    const auto* slots=reinterpret_cast<const uintptr_t*>(__readgsqword(0x58));
    const auto* tls=reinterpret_cast<const uint8_t*>(slots[0]);
    const uintptr_t freeRva=tls[0x1FE]?0x4B7EACC:0x4B7E9CC;
    reinterpret_cast<NativeArrayFree>(TargetAddress(freeRva))(entries);
}

struct SurfaceProbeHistory {
    void* component=nullptr;
    void* world=nullptr;
    Point origin{},end{},red{},rejectedPoint{};
    uint64_t tick=0;
    unsigned preferred=axiom::surface::kNoProbe;
    uint64_t nearTick=0;
    bool nearBlocked=false, rejectedOriginal=false, tryWindow=false, tryAllHits=false;
};

SurfaceProbeHistory g_surfaceHistory{};
SRWLOCK g_surfaceHistoryLock=SRWLOCK_INIT;

bool SameAim(const SurfaceProbeHistory& aim,const TargetFrame& frame,
             const Point& origin,const Point& end,const Point& red) {
    return aim.component==frame.component && aim.world==frame.world &&
        frame.sample.tick>=aim.tick && frame.sample.tick-aim.tick<=300 &&
        axiom::surface::Distance(aim.origin,origin)<=0.25f &&
        axiom::surface::Distance(aim.end,end)<=2.f &&
        axiom::surface::Distance(aim.red,red)<=0.12f;
}

void RecordSurfaceResult(const TargetFrame& frame,bool accepted) {
    const bool direct=frame.sample.recovery==Recovery::OriginalRay;
    if (!frame.sample.probeTried && !direct) return;
    Point origin{},end{};
    std::memcpy(origin.data(),frame.sample.from,sizeof(origin));
    std::memcpy(end.data(),frame.sample.to,sizeof(end));
    AcquireSRWLockExclusive(&g_surfaceHistoryLock);
    auto& aim=g_surfaceHistory;
    if (frame.sample.tick>=aim.tick) {
        if (!SameAim(aim,frame,origin,end,frame.redPoint))
            aim={frame.component,frame.world,origin,end,frame.redPoint};
        aim.tick=frame.sample.tick;
        if (direct) {
            aim.rejectedOriginal=!accepted;
            aim.rejectedPoint=frame.offeredPoint;
        } else if (frame.sample.recovery==Recovery::NearCameraRay) {
            aim.nearTick=frame.sample.tick;
            aim.nearBlocked=!accepted && frame.sample.retryHits>frame.sample.retryGridPrefix;
            aim.preferred=accepted?axiom::surface::kNearCameraProbe:axiom::surface::kNoProbe;
        } else {
            aim.preferred=accepted?frame.surfaceProbe:axiom::surface::kNoProbe;
            // Only an empty full-length continuation can select the
            // shorter query. A recorded blocker keeps its precedence.
            aim.tryWindow=frame.sample.recovery==Recovery::ContinuedRay &&
                !frame.sample.retryHits && !frame.sample.probeOffered;
            // Kind 4 can merge raw contacts. Retry the same camera segment
            // with kind 2 only when every returned contact was a proven grid marker.
            const bool onlyGrid=frame.sample.retryHits!=0 &&
                frame.sample.retryGridPrefix==frame.sample.retryHits && !frame.sample.probeOffered;
            aim.tryAllHits=frame.sample.recovery==Recovery::WindowRay && onlyGrid;

        }
    }
    ReleaseSRWLockExclusive(&g_surfaceHistoryLock);
}

bool ReserveSurfaceProbe(uint64_t tick) {
    auto next=g_nextSurfaceProbe.load(std::memory_order_relaxed);
    return tick>=next &&
           g_nextSurfaceProbe.compare_exchange_strong(next,tick+40,std::memory_order_relaxed);
}

// The native collector sorts by ray fraction. Walk only the leading
// verified grid contacts. Unknown records and physical hits end the walk.
template<class Kind>
size_t GridPrefix(const NativeRayCandidate* entries,size_t count,const Point& origin,const Point& end,
                  float limit,const Kind& kindAt) {
    size_t index=0;
    for (;index<count;++index) {
        const auto& entry=entries[index];
        const auto point=entry.Position();
        if (entry.HasBody() || kindAt(index)!=1 ||
            !axiom::surface::DirectionalGridMarker(origin,end,point,entry.Material(),limit)) break;
        if (index) {
            const auto previous=entries[index-1].Position();
            if (!axiom::surface::SameAnchor(previous,point) &&
                !axiom::surface::EligibleBeyondGrid(origin,end,previous,point,limit,true)) break;
        }
    }
    return index;
}

void RecordOfferedSurface(TargetFrame& frame,const Point& origin,const Point& red,const Point& offered) {
    frame.redPoint=red;
    frame.offeredPoint=offered;
    frame.sample.redRayDistance=axiom::surface::Distance(origin,red);
    frame.sample.offeredRayDistance=axiom::surface::Distance(origin,offered);
    frame.sample.probeOffered=true;
}

// Runs on the last conversion of an exact primary query. All candidate
// slots are fully written and still owned by the native caller.
__declspec(noinline) __declspec(guard(nocf))
void ProbeNearbySurface(TargetFrame& frame,NativeRayCandidate* entries) {
    if (!g_searchReady.load(std::memory_order_acquire) || !entries || !frame.world ||
        !frame.sample.count || frame.sample.count>frame.sample.candidates.size()) return;
    Point origin{},end{},probeStart{},probeEnd{};
    std::memcpy(origin.data(),frame.sample.from,sizeof(origin));
    std::memcpy(end.data(),frame.sample.to,sizeof(end));
    const auto count=frame.sample.count;
    const float nativeLimit=frame.sample.rayLength;
    const float reach=frame.effectiveRange;
    if (!(std::isfinite(reach) && std::isfinite(nativeLimit))) return;
    const float limit=(std::min)(reach,nativeLimit);
    const auto prefix=GridPrefix(entries,count,origin,end,limit,
        [&](size_t index){return frame.sample.candidates[index].kind;});
    frame.sample.gridPrefix=static_cast<unsigned>(prefix);

    size_t redIndex=prefix?prefix-1:0;
    if (!prefix && std::any_of(entries,entries+count,[](const auto& hit){return hit.HasBody();})) {
        redIndex=count;
        // Other bodyless contacts can only use the existing local search
        // after the native filter has identified the same red point.
        for (size_t i=0;i<count;++i) {
            if (entries[i].HasBody() || !KnownRedHit(frame,origin,entries[i].Position())) continue;
            if (redIndex!=count) return;
            redIndex=i;
        }
        if (redIndex==count) return;
    }
    const auto red=entries[redIndex].Position();
    const float depth=axiom::surface::Distance(origin,red);
    if (!(depth<=limit)) return;
    frame.redPoint=red;
    frame.sample.redRayDistance=depth;
    SurfaceProbeHistory history{};
    if (TryAcquireSRWLockShared(&g_surfaceHistoryLock)) {
        const auto& previous=g_surfaceHistory;
        if (SameAim(previous,frame,origin,end,red)) {
            history=previous;
        } else if ((previous.tryWindow || previous.tryAllHits) && previous.component==frame.component &&
                   previous.world==frame.world && frame.sample.tick>=previous.tick &&
                   frame.sample.tick-previous.tick<=300) {
            // Only the retry choice can survive small aim changes. The
            // shortened ray is rebuilt and validated against the current aim.
            history.tryWindow=previous.tryWindow;
            history.tryAllHits=previous.tryAllHits;
            if (!previous.nearBlocked &&
                axiom::surface::Distance(previous.origin,origin)<=0.10f &&
                axiom::surface::Distance(previous.red,red)<=0.12f &&
                axiom::surface::Distance(previous.end,end)<=8.f)
                history.nearTick=previous.nearTick;
        }
        ReleaseSRWLockShared(&g_surfaceHistoryLock);
    }
    const bool preferNear=history.preferred==axiom::surface::kNearCameraProbe;
    const bool nearFresh=history.nearTick && frame.sample.tick-history.nearTick<=500;
    if (nearFresh && history.nearBlocked) return;
    const bool inspectNear=prefix && (!nearFresh || preferNear) &&
        g_allHitsCollectorReady.load(std::memory_order_acquire);
    if (prefix && prefix<count && !inspectNear) {
        if (entries[prefix].HasBody() &&
            (!history.rejectedOriginal || !axiom::surface::SameAnchor(history.rejectedPoint,entries[prefix].Position())) &&
            axiom::surface::EligibleBeyondGrid(origin,end,red,entries[prefix].Position(),limit,true)) {
            std::rotate(entries,entries+prefix,entries+prefix+1);
            auto* samples=frame.sample.candidates.data();
            std::rotate(samples,samples+prefix,samples+prefix+1);
            frame.sample.recovery=Recovery::OriginalRay;
            RecordOfferedSurface(frame,origin,red,entries[0].Position());
        }
        return;
    }
    const bool legacyGrid=redIndex==0 && depth>=axiom::surface::kWideSearchDistance &&
        frame.sample.candidates[0].kind==1 && axiom::surface::GridBoundary(red) &&
        KnownRedHit(frame,origin,red);
    const unsigned probes=legacyGrid?axiom::surface::kProbeCount:
        depth>=axiom::surface::kWideSearchDistance?axiom::surface::kLocalProbeCount:8;
    auto native=reinterpret_cast<NativeSurfaceQuery>(TargetAddress(kSurfaceQuery));
#ifdef AXIOM_TESTING
    if (g_testSurfaceQuery) native=g_testSurfaceQuery;
#endif
    const bool continuation=prefix!=0;
    const float clippedLimit=(std::min)(limit,depth+axiom::surface::kContinuationWindow);
    const bool shortQuery=inspectNear && (preferNear || ReserveSurfaceProbe(frame.sample.tick));
    if (inspectNear && !shortQuery) return;
    const bool allHits=continuation && !shortQuery && clippedLimit<limit &&
        g_allHitsCollectorReady.load(std::memory_order_acquire) &&
        (history.tryAllHits || history.preferred==axiom::surface::kAllHitsProbe);
    const bool window=continuation && !shortQuery && clippedLimit<limit &&
        (allHits || history.tryWindow || history.preferred==axiom::surface::kWindowProbe);
    const auto continuationProbe=allHits?axiom::surface::kAllHitsProbe:
        window?axiom::surface::kWindowProbe:axiom::surface::kContinuationProbe;
    const bool preferred=continuation?history.preferred==continuationProbe:history.preferred<probes;
    if (!shortQuery && !preferred && !ReserveSurfaceProbe(frame.sample.tick)) return;
    const auto probeIndex=shortQuery?axiom::surface::kNearCameraProbe:continuation?continuationProbe:
        preferred?history.preferred:g_probeIndex.fetch_add(1,std::memory_order_relaxed)%probes;
    const float probeLimit=shortQuery?(std::min)({limit,256.f,depth+64.f}):window?clippedLimit:limit;

    const bool extended=!continuation && probeIndex>=axiom::surface::kLocalProbeCount;
    probeStart=origin;
    if (shortQuery) {
        const auto ray=axiom::surface::Difference(end,origin);
        const float length=axiom::surface::Length(ray);
        if (!(std::isfinite(length) && length>0.f)) return;
        for (size_t i=0;i<probeEnd.size();++i) probeEnd[i]=origin[i]+ray[i]*(probeLimit/length);
    } else if (continuation) {
        if (!axiom::surface::ContinueRay(origin,end,red,probeLimit,probeStart,probeEnd)) return;
        // A segment starting past the grid entry can miss the entire native
        // collision traversal. Keep the camera origin for the shorter retry.
        if (window) probeStart=origin;
    } else {
        if (!axiom::surface::ProbeEnd(origin,end,red,probeIndex,probeEnd)) return;
        const auto probeLength=axiom::surface::Distance(origin,probeEnd);
        if (probeLength>limit) {
            const float scale=limit/probeLength;
            for (size_t i=0;i<probeEnd.size();++i) probeEnd[i]=origin[i]+(probeEnd[i]-origin[i])*scale;
        }
    }
    frame.sample.recovery=shortQuery?Recovery::NearCameraRay:allHits?Recovery::AllHitsRay:
        continuation?(window?Recovery::WindowRay:Recovery::ContinuedRay):
        (extended?Recovery::ExtendedRay:Recovery::Nearby);

    alignas(32) auto probeQuery=frame.searchQuery;
    std::memcpy(probeQuery.data()+0x28,probeStart.data(),sizeof(probeStart));
    std::memcpy(probeQuery.data()+0x34,probeEnd.data(),sizeof(probeEnd));
    NativeRayBuffer buffer;
    auto& probe=buffer.vector;
    NativeRayOutput probeOutput{&probe,nullptr};
    frame.sample.probeTried=true;
    frame.surfaceProbe=probeIndex;
    frame.retryKinds.fill(0);
    frame.retryVector=&probe;
    constexpr uint8_t allHitsCollector=2, groupedCollector=4;
    const bool hit=native(frame.world,probeQuery.data(),(shortQuery || allHits)?allHitsCollector:groupedCollector,&probeOutput);
    frame.retryVector=nullptr;

    size_t retryIndex=0;
    const bool valid=hit && probe.entries && probe.count && probe.count<=probe.capacity;
    frame.sample.retryHits=hit?probe.count:0;
    if (valid) {
        frame.sample.retryHeadBody=probe.entries[0].HasBody();
        frame.sample.retryHeadMaterial=probe.entries[0].Material();
        // The nested converter proves the raw kind for each leading
        // contact. Stop if its provenance is unavailable.
        if (continuation) {
            const auto known=(std::min)(size_t{probe.count},frame.retryKinds.size());
            retryIndex=GridPrefix(probe.entries,known,origin,end,probeLimit,
                [&](size_t index){return frame.retryKinds[index];});
            if (!shortQuery && !window && retryIndex && !axiom::surface::EligibleBeyondGrid(
                    origin,end,red,probe.entries[0].Position(),probeLimit,true))
                retryIndex=0;
            frame.sample.retryGridPrefix=static_cast<unsigned>(retryIndex);
        }
    }
    // Let the original classifier decide whether the complete native
    // collision can produce an Axiom tether.
    const auto ref=retryIndex ? probe.entries[retryIndex-1].Position() : red;
    const bool hasBody=valid && retryIndex<probe.count && probe.entries[retryIndex].HasBody();
    const auto position=hasBody?probe.entries[retryIndex].Position():Point{};
    const bool eligible=hasBody && axiom::surface::OnAimRay(origin,end,position,probeLimit) &&
        (shortQuery || ((continuation || extended)?axiom::surface::EligibleBeyondGrid(
            origin,end,ref,position,probeLimit,continuation) &&
            (!window || axiom::surface::EligibleBeyondGrid(origin,end,red,position,probeLimit,true)):
        axiom::surface::Eligible(origin,red,position,true,limit)));
    if (eligible) {
        auto candidate=probe.entries[retryIndex];
        const float fraction=axiom::surface::Distance(origin,candidate.Position())/nativeLimit;
        std::memcpy(candidate.bytes.data(),&fraction,sizeof(fraction));
        // Replace only the red slot with the complete native result,
        // including the body. Let the original classifier evaluate it.
        const auto slot=continuation?size_t{0}:redIndex;
        entries[slot]=candidate;
        RecordOfferedSurface(frame,origin,red,candidate.Position());
        auto& observed=frame.sample.candidates[slot];
        std::memcpy(observed.point,frame.offeredPoint.data(),sizeof(observed.point));
        std::memcpy(&observed.material,candidate.bytes.data()+0x48,sizeof(observed.material));
        observed.body=true;
        observed.kind=0; // The probe result does not expose the raw collector kind here.
    }
    if (probe.entries && probe.entries!=buffer.inlineHits.data()) FreeProbeVector(probe.entries);
}

void QueueSurfaceAttachment(const SurfaceAttachment& attachment) {
    AcquireSRWLockExclusive(&g_traceLock);
    if (g_attachmentCount<g_attachmentEvents.size())
        g_attachmentEvents[g_attachmentCount++]=attachment;
    else ++g_attachmentDropped;
    ReleaseSRWLockExclusive(&g_traceLock);
}

// The event carries the complete native target descriptor. Observe the
// matching cursor after native validation and before the game's dispatch.
__declspec(noinline) __declspec(guard(nocf))
void* TraceCastSurface(void* owner,void* result,void* request,void* context,const void* event) {
    auto native=reinterpret_cast<NativeCastDispatch>(TargetAddress(kCastDispatch));
#ifdef AXIOM_TESTING
    if (g_testCastDispatch) native=g_testCastDispatch;
#endif
    const DWORD entryError=GetLastError();
    if (g_searchReady.load(std::memory_order_acquire) && !g_stopping.load(std::memory_order_relaxed)) {
        const auto tick=GetTickCount64();
        NativeTargetKey descriptor{};
        Point normal{};
        const auto source=reinterpret_cast<uintptr_t>(event);
        uint16_t tag=0;
        if (Get(source,tag) && tag==6 && Get(source+9,descriptor) && Get(source+0x68,normal)) {
            const auto cast=MatchCast(descriptor,normal,tick);
            if (cast.tick) RecordCast(cast,tick);
        }
    }
    SetLastError(entryError);
    return native(owner,result,request,context,event);
}

// The native helper returns a world transform for the pending lane.
// Observe it only when called by the commit path, before native commit
// has a chance to clear or replace the targeting cursor.
__declspec(noinline) __declspec(guard(nocf))
bool TraceCommitAnchor(void* object,uint8_t lane,void* transform) {
    auto native=reinterpret_cast<NativeCommitAnchor>(TargetAddress(kCommitAnchor));
#ifdef AXIOM_TESTING
    if (g_testCommitAnchor) native=g_testCommitAnchor;
#endif
    const bool found=native(object,lane,transform);
    const DWORD nativeError=GetLastError();
    if (found && lane<2 && g_commitTls!=TLS_OUT_OF_INDEXES) {
        auto* frame=static_cast<CommitFrame*>(TlsGetValue(g_commitTls));
        if (frame && frame->component==object && frame->lane==lane) {
            const auto position=reinterpret_cast<uintptr_t>(transform)+0x1C;
            frame->known=Get(position,frame->anchor);
        }
    }
    SetLastError(nativeError);
    return found;
}

__declspec(noinline) __declspec(guard(nocf))
void TraceSurfaceCommit(void* object,uint8_t lane,const void* packet) {
    auto native=reinterpret_cast<NativeSurfaceCommit>(TargetAddress(kSurfaceCommit));
#ifdef AXIOM_TESTING
    if (g_testSurfaceCommit) native=g_testSurfaceCommit;
#endif
    const DWORD entryError=GetLastError();
    const auto component=reinterpret_cast<uintptr_t>(object);
    uint8_t before=255;
    const auto stateAddress=component+0xA4C+0x1A0*lane;
    const bool pending=lane<2 && g_initialized.load(std::memory_order_acquire) &&
        !g_stopping.load(std::memory_order_acquire) && ControlledCatch(component) &&
        Get(stateAddress,before) && before==2;
    CommitFrame frame{object,lane};
    const bool watch=pending && g_commitTls!=TLS_OUT_OF_INDEXES &&
        !TlsGetValue(g_commitTls) && TlsSetValue(g_commitTls,&frame);
    SetLastError(entryError);
    DWORD nativeError=0;
    __try {
        native(object,lane,packet);
    } __finally {
        nativeError=GetLastError();
        if (watch) TlsSetValue(g_commitTls,nullptr);
        SetLastError(nativeError);
    }
    if (!pending) return;
    if (ControlledCatch(component)) {
        const auto attachment=component+0x428*lane;
        uint8_t state=0,kind=0;
        float distance=0;
        if (Get(stateAddress,state) && Get(attachment+0x168,kind) &&
            Get(attachment+0x4C8,distance) && axiom::surface::SurfaceAttached(before,state,kind,distance)) {
            const auto tick=GetTickCount64();
            const bool unlimited=g_traceUnlimited.load(std::memory_order_relaxed);
            const float range=CurrentEffectiveRange();
            const auto recovered=frame.known?TakeRecovery(object,frame.anchor,tick,unlimited,range):AimSurface{};
            RememberStallAttachment(component,lane,kind,frame.known,frame.anchor);
            QueueSurfaceAttachment({tick,distance,lane,kind,unlimited,frame.known,recovered.tick,recovered.castTick,
                                    recovered.distance,range,frame.anchor,recovered.recovery,recovered.redRayDistance});
        }
    }
    SetLastError(nativeError);
}
