#pragma once

// Included in the bridge's private namespace after the host and log helpers.
#include "TargetTraceNative.h"

using axiom::trace::Path;
using axiom::trace::Sample;
using axiom::trace::Result;
using axiom::trace::ResetReason;

constexpr uintptr_t kUpdate=0x6747A0;
constexpr uintptr_t kPrimary=0x675EA0;
constexpr uintptr_t kRay=0x3D763B0;
constexpr uintptr_t kCandidateConvert=0x3D77F40;
constexpr uintptr_t kSweep=0x3D79DA0;
constexpr uintptr_t kReset=0x6983A0;
constexpr std::array<const char*,3> kPathNames{"none","ray","sweep"};
constexpr std::array<const char*,6> kOutcomeNames{
    "no-query","no-physics-hit","post-query-rejected","raw-hit","attachable","state-unavailable"};
constexpr size_t kResetSlot=kPathNames.size()*kOutcomeNames.size();
constexpr std::array<const char*,3> kResetReasons{"unknown","actor-mode","context-override"};
constexpr size_t kClearSlot=kResetSlot+kResetReasons.size();
constexpr size_t kExternalResetSlot=kClearSlot+kPathNames.size()*kResetReasons.size();
constexpr size_t kTraceStride=kExternalResetSlot+1;
constexpr unsigned kTraceRowsPerOutcome=120;

size_t TraceIndex(Path path,Result result,bool unlimited) {
    return (unlimited?kTraceStride:0)+static_cast<size_t>(path)*kOutcomeNames.size()+
           static_cast<size_t>(result);
}
size_t TraceResetIndex(bool unlimited,ResetReason reason) {
    return (unlimited?kTraceStride:0)+kResetSlot+static_cast<size_t>(reason);
}

size_t TraceClearIndex(bool unlimited,Path path,ResetReason reason=ResetReason::Unknown) {
    return (unlimited?kTraceStride:0)+kClearSlot+
           static_cast<size_t>(path)*kResetReasons.size()+static_cast<size_t>(reason);
}

size_t TraceExternalResetIndex(bool unlimited) {
    return (unlimited?kTraceStride:0)+kExternalResetSlot;
}

struct TraceSlot {
    Sample sample{};
    bool pending=false;
};
struct NativeRayVector;
struct TargetFrame {
    void* component=nullptr;
    Sample sample{};
    uint32_t expectedConversions=0;
    NativeRayVector* retryVector=nullptr;
    std::array<uint32_t,axiom::trace::kRayCandidateLimit> retryKinds{};
    bool contiguousConversions=true;
    uintptr_t firstCandidate=0;
    void* world=nullptr;
    std::array<uint8_t,0x48> searchQuery{};
    unsigned surfaceProbe=axiom::surface::kNoProbe;
    float effectiveRange=0.f;
    bool unlimited=false;
    axiom::surface::Point offeredPoint{},redPoint{};
};
DWORD g_traceTls=TLS_OUT_OF_INDEXES;
std::array<std::atomic<uint64_t>,2*kTraceStride> g_traceNext{};
std::array<unsigned,2*kTraceStride> g_traceRows{};
std::array<TraceSlot,2*kTraceStride> g_traceSlots{};
SRWLOCK g_traceLock=SRWLOCK_INIT;
std::atomic<bool> g_traceUnlimited{false};
#ifndef SAF_UNIFIED_RUNTIME
uintptr_t TargetAddress(uintptr_t role) { return g.game+role; }
#endif
#include "CatchControl.h"
#include "ObstacleObserver.h"

using NativeUpdate=void(*)(void*,float);
using NativeTarget=void(*)(void*,const void*);
using NativeDispatch=void(*)(void*,const void*,void*,void*);
using NativeCandidateConvert=void(*)(void*,const void*,void*,uint8_t);
using NativeSweep=bool(*)(void*,const void*,uint8_t,void*,float);
using NativeReset=void(*)(void*);
#ifdef AXIOM_TESTING
NativeUpdate g_testTraceUpdate=nullptr;
NativeTarget g_testTraceTarget=nullptr;
NativeDispatch g_testTraceDispatch=nullptr;
NativeCandidateConvert g_testCandidateConvert=nullptr;
NativeSweep g_testTraceSweep=nullptr;
NativeReset g_testTraceReset=nullptr;
#endif

CatchControl ReadControlledCatch(uintptr_t component) {
    std::array<uintptr_t,2> object{};
    if (!Get(component,object) || object[0]!=TargetAddress(0x5994CC8)) return {};
    return ReadCatchControl(object[1]);
}

bool ControlledCatch(uintptr_t component) {
    return ReadControlledCatch(component).actor!=0;
}

uint8_t ReadActorMode(uintptr_t component) {
    uintptr_t actor=0, parts=0, action=0, root=0, state=0;
    uint8_t mode=255;
    const bool found=Get(component+8,actor) && actor &&
        Get(actor+0x68,parts) && parts &&
        Get(parts+0x40,action) && action &&
        Get(action+0x88,root) && root &&
        Get(root+0x10,state) && state && Get(state+0xFA,mode);
    return found?mode:255;
}

#include "StalledTetherRuntime.h"

void QueueTraceRow(size_t index,const Sample& sample,uint64_t interval,bool capped) {
    auto& next=g_traceNext[index];
    if (sample.tick<next.load(std::memory_order_relaxed) ||
        !TryAcquireSRWLockExclusive(&g_traceLock)) return;
    if (!g_traceSlots[index].pending && sample.tick>=next.load(std::memory_order_relaxed)) {
        g_traceSlots[index]={sample,true};
        const bool reachedCap=capped && ++g_traceRows[index]>=kTraceRowsPerOutcome;
        next.store(reachedCap?UINT64_MAX:sample.tick+interval,std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_traceLock);
}

void QueueTargetTrace(const Sample& sample,bool unlimited) {
    const Result outcome=axiom::trace::Outcome(sample);
    const bool heartbeat=sample.path==Path::None && outcome==Result::NoQuery;
    QueueTraceRow(TraceIndex(sample.path,outcome,unlimited),sample,heartbeat?8000:800,!heartbeat);
    if (sample.reset) QueueTraceRow(TraceResetIndex(unlimited,sample.resetReason),sample,800,false);
    if (axiom::trace::CursorCleared(sample))
        QueueTraceRow(TraceClearIndex(unlimited,sample.path,sample.resetReason),sample,250,false);
}

void LogTargetTrace(const Sample& sample,size_t index) {
    const size_t slot=index%kTraceStride;
    const char* event=slot==kExternalResetSlot?"external-reset":
                      slot>=kClearSlot?"cursor-cleared":slot>=kResetSlot?"pre-query-reset":"update";
    const char* reason=sample.reset?kResetReasons[static_cast<size_t>(sample.resetReason)]:"none";
    const char* outcome=kOutcomeNames[static_cast<size_t>(axiom::trace::Outcome(sample))];
    const auto hit=axiom::trace::PublishedHit(sample);
    const char* provenance=hit.source==axiom::trace::RaySource::WithBody?"body":
                           hit.source==axiom::trace::RaySource::WithoutBody?"no-body":"unknown";
    char text[800]{};
    snprintf(text,sizeof(text),
        "TargetTrace6 tick=%llu unlimited=%u event=%s path=%s outcome=%s queried=%u ray=%.3f "
        "hits=%u shape=%u before=%u/%u after=%u/%u preReset=%u resetReason=%s "
        "sampledActorMode=%u "
        "start=[%.4f,%.4f,%.4f] end=[%.4f,%.4f,%.4f] hit=[%.4f,%.4f,%.4f] converted=%u rawSource=%s kind=%u material=%u probe=%u offered=%u recovery=%s redRayDistance=%.3f offeredRayDistance=%.3f gridPrefix=%u retryHits=%u retryGridPrefix=%u retryHeadBody=%u retryHeadMaterial=%u",
        static_cast<unsigned long long>(sample.tick),unsigned(index>=kTraceStride),event,
        kPathNames[static_cast<size_t>(sample.path)],outcome,unsigned(sample.queried),
        sample.rayLength,sample.count,unsigned(sample.queryKind),
        unsigned(sample.beforeMode),unsigned(sample.beforeRaw),unsigned(sample.mode),unsigned(sample.raw),
        unsigned(sample.reset),reason,unsigned(sample.sampledActorMode),sample.from[0],sample.from[1],sample.from[2],sample.to[0],
        sample.to[1],sample.to[2],sample.hit[0],sample.hit[1],sample.hit[2],
        sample.converted,provenance,hit.kind,hit.material,unsigned(sample.probeTried),unsigned(sample.probeOffered),
        axiom::surface::RecoveryName(sample.recovery),sample.redRayDistance,sample.offeredRayDistance,
        sample.gridPrefix,sample.retryHits,sample.retryGridPrefix,unsigned(sample.retryHeadBody),sample.retryHeadMaterial);
    Log(text);
}

#include "NearbySurfaceRuntime.h"

CatchControl g_targetControl{};
bool g_controlChanged=false;
uint32_t g_controlUid=0;

void ReplaceTargetControlLocked(const CatchControl& control) {
    if (SameControl(g_targetControl,control)) return;
    g_obstacleOwner.store(0,std::memory_order_release);
    ClearStalledTether();
    g_lastRed={};
    g_publishedRecovery={};
    g_dispatchedTethers={};
    g_dispatchedIndex=0;
    g_targetControl=control;
    g_controlUid=0;
    if (control.actor) Get(control.actor+0x60,g_controlUid);
    g_controlChanged=true;
}

CatchControl UpdateTargetControl(uintptr_t component) {
    if (g_stopping.load(std::memory_order_acquire)) return {};
    AcquireSRWLockExclusive(&g_traceLock);
    const auto control=g_stopping.load(std::memory_order_acquire)?
        CatchControl{}:ReadControlledCatch(component);
    if (control.actor) {
        ReplaceTargetControlLocked(control);
        if (g_straightPullOwned.load(std::memory_order_relaxed))
            g_obstacleOwner.store(control.actor,std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_traceLock);
    return control;
}

void RefreshTargetControl(bool obstaclePath) {
    AcquireSRWLockExclusive(&g_traceLock);
    if (g_targetControl.actor && (g_stopping.load(std::memory_order_acquire) ||
        !SameControl(g_targetControl,ReadCatchControl(g_targetControl.actor))))
        ReplaceTargetControlLocked({});
    if (!obstaclePath) g_obstacleOwner.store(0,std::memory_order_release);
    ReleaseSRWLockExclusive(&g_traceLock);
}

void DrainTargetTrace() {
    DrainStalledTether();
    std::array<TraceSlot,2*kTraceStride> slots{};
    decltype(g_attachmentEvents) attachments{};
    decltype(g_castEvents) casts{};
    size_t count=0,castCount=0;
    ObstacleSample obstacle{};
    unsigned dropped=0;
    bool controlChanged=false;
    uint32_t controlUid=0;
    AcquireSRWLockExclusive(&g_traceLock);
    controlChanged=g_controlChanged;
    controlUid=g_controlUid;
    g_controlChanged=false;
    slots=g_traceSlots;
    count=g_attachmentCount;
    attachments=g_attachmentEvents;
    casts=g_castEvents;
    castCount=g_castCount;
    obstacle=g_obstacleEvent;
    g_obstacleEvent={};
    g_castCount=0;
    dropped=g_attachmentDropped;
    g_attachmentCount=0;
    g_attachmentDropped=0;
    for (auto& slot:g_traceSlots) slot.pending=false;
    ReleaseSRWLockExclusive(&g_traceLock);
    if (controlChanged) {
        char message[100]{};
        snprintf(message,sizeof(message),"AxiomControl event=active-catch-changed uid=%08X",controlUid);
        Log(message);
    }
    LogObstacleCorrection(obstacle);
    const auto reads=g_obstacleWaypointReads.exchange(0,std::memory_order_relaxed);
    if (reads) {
        char message[128]{};
        snprintf(message,sizeof(message),"AxiomObstacle event=waypoint-basis-used count=%u",reads);
        Log(message);
    }
    const auto entries=g_obstacleGateEntries.exchange(0,std::memory_order_relaxed);
    if (entries) {
        char message[120]{};
        snprintf(message,sizeof(message),"AxiomObstacle event=controlled-solver-entry count=%u",entries);
        Log(message);
    }
    for (size_t i=0;i<slots.size();++i)
        if (slots[i].pending) LogTargetTrace(slots[i].sample,i);
    for (size_t i=0;i<castCount;++i) {
        const auto& cast=casts[i];
        char text[290]{};
        snprintf(text,sizeof(text),"AxiomCast tick=%llu event=recovered-target-dispatched recoveredTick=%llu "
                 "rayDistance=%.3f effectiveRange=%.3f unlimited=%u anchor=[%.4f,%.4f,%.4f] recovery=%s redRayDistance=%.3f",
                 static_cast<unsigned long long>(cast.castTick),static_cast<unsigned long long>(cast.tick),
                 cast.distance,cast.range,unsigned(cast.unlimited),cast.point[0],cast.point[1],cast.point[2],
                 axiom::surface::RecoveryName(cast.recovery),cast.redRayDistance);
        Log(text);
    }
    for (size_t i=0;i<count;++i) {
        const auto& a=attachments[i];
        char text[345]{};
        snprintf(text,sizeof(text),"AxiomAttachment tick=%llu event=surface-attached lane=%u kind=%u distance=%.3f unlimited=%u anchorKnown=%u anchor=[%.4f,%.4f,%.4f] recoveredAnchorTick=%llu recoveredCastTick=%llu recoveredRayDistance=%.3f effectiveRange=%.3f recovery=%s redRayDistance=%.3f",
                 static_cast<unsigned long long>(a.tick),unsigned(a.lane),unsigned(a.kind),
                 a.distance,unsigned(a.unlimited),unsigned(a.anchorKnown),a.anchor[0],a.anchor[1],a.anchor[2],
                 static_cast<unsigned long long>(a.recoveredTick),static_cast<unsigned long long>(a.castTick),
                 a.recoveredDistance,a.effectiveRange,axiom::surface::RecoveryName(a.recovery),a.redRayDistance);
        Log(text);
    }
    if (dropped) {
        char text[100]{};
        snprintf(text,sizeof(text),"AxiomAttachment dropped=%u",dropped);
        Log(text);
    }
}

bool ReadCursor(uintptr_t component,uint8_t& mode,bool& raw,float (*hit)[3]=nullptr) {
    std::array<uint8_t,20> state{};
    if (!Get(component+0x140,state) || state[0]>2 || state[4]>1) return false;
    mode=state[0];
    raw=state[4]==1;
    if (raw && hit) std::memcpy(*hit,state.data()+8,sizeof(*hit));
    return true;
}

// Wrap the native outer update without altering its body or its callers' ABI.
__declspec(noinline) __declspec(guard(nocf))
void TraceUpdate(void* object,float delta) {
    auto native=reinterpret_cast<NativeUpdate>(TargetAddress(kUpdate));
#ifdef AXIOM_TESTING
    if (g_testTraceUpdate) native=g_testTraceUpdate;
#endif
    const auto component=reinterpret_cast<uintptr_t>(object);
    const DWORD beforeError=GetLastError();
    CatchControl control{};
    if (g_initialized.load(std::memory_order_acquire) && !g_stopping.load(std::memory_order_acquire) &&
        !TlsGetValue(g_traceTls)) control=UpdateTargetControl(component);
    if (!control.actor) {
        SetLastError(beforeError);
        native(object,delta);
        return;
    }
    TargetFrame frame{};
    frame.component=object;
    frame.sample.tick=GetTickCount64();
    const bool unlimited=g_traceUnlimited.load(std::memory_order_relaxed);
    ReadCursor(component,frame.sample.beforeMode,frame.sample.beforeRaw);
    if (!TlsSetValue(g_traceTls,&frame)) {
        SetLastError(beforeError);
        native(object,delta);
        return;
    }
    SetLastError(beforeError);
    DWORD nativeError=0;
    __try {
        native(object,delta);
    } __finally {
        nativeError=GetLastError();
        TlsSetValue(g_traceTls,nullptr);
        SetLastError(nativeError);
    }
    if (!SameControl(control,ReadControlledCatch(component))) {
        SetLastError(nativeError);
        return;
    }
    ReadCursor(component,frame.sample.mode,frame.sample.raw,&frame.sample.hit);
    const bool accepted=frame.sample.probeOffered && frame.sample.mode==0 && frame.sample.raw &&
        std::memcmp(frame.sample.hit,frame.offeredPoint.data(),sizeof(frame.sample.hit))==0;
    RecordSurfaceResult(frame,accepted);
    if (accepted) RememberRecovery(frame,unlimited);
    RememberRedHit(frame,unlimited);
    if (!accepted && (frame.sample.queried || frame.sample.mode!=0)) ClearPublishedRecovery(object);
    QueueTargetTrace(frame.sample,unlimited);
    ReleaseStalledTether(component,delta);
    SetLastError(nativeError);
}

__declspec(noinline) __declspec(guard(nocf))
void TraceTarget(void* object,const void* aim) {
    auto native=reinterpret_cast<NativeTarget>(TargetAddress(kPrimary));
#ifdef AXIOM_TESTING
    if (g_testTraceTarget) native=g_testTraceTarget;
#endif
    const DWORD beforeError=GetLastError();
    auto* frame=static_cast<TargetFrame*>(TlsGetValue(g_traceTls));
    if (frame && frame->component==object) frame->sample.path=Path::Primary;
    SetLastError(beforeError);
    native(object,aim);
}

__declspec(noinline) __declspec(guard(nocf))
void TraceDispatch(void* world,const void* query,void* collector,void* context) {
    auto native=reinterpret_cast<NativeDispatch>(TargetAddress(kRay));
#ifdef AXIOM_TESTING
    if (g_testTraceDispatch) native=g_testTraceDispatch;
#endif
    const DWORD beforeError=GetLastError();
    auto* frame=static_cast<TargetFrame*>(TlsGetValue(g_traceTls));
    SetLastError(beforeError);
    native(world,query,collector,context);
    if (!frame || frame->sample.path!=Path::Primary || frame->sample.queried) return;
    const DWORD nativeError=GetLastError();
    const auto input=reinterpret_cast<uintptr_t>(query);
    uint32_t mask=0,count=0;
    float points[2][3]{};
    if (Get(input+8,mask) && mask==0x70 && Get(input+0x28,points) &&
        Get(reinterpret_cast<uintptr_t>(collector)+0xC,count) &&
        axiom::trace::RecordQuery(frame->sample,mask,points[0],points[1],count)) {
        // Native primary query has no context pointer with external lifetime.
        // Refuse retry if its optional context/filter pointers are populated.
        if (count && count<=frame->sample.candidates.size() && g_searchReady.load(std::memory_order_acquire) &&
            Get(input,frame->searchQuery) &&
            std::all_of(frame->searchQuery.begin()+0x18,frame->searchQuery.begin()+0x28,
                        [](uint8_t byte){return byte==0;})) {
            frame->world=world;
            frame->expectedConversions=count;
            frame->effectiveRange=CurrentEffectiveRange();
            frame->unlimited=g_traceUnlimited.load(std::memory_order_relaxed);
        }
        const bool unlimited=g_traceUnlimited.load(std::memory_order_relaxed);
        const auto tick=frame->sample.tick;
        for (auto outcome : {Result::Raw,Result::Accepted}) {
            const auto slot=TraceIndex(Path::Primary,outcome,unlimited);
            if (tick>=g_traceNext[slot].load(std::memory_order_relaxed)) {
                frame->expectedConversions=count;
                break;
            }
        }
    }
    SetLastError(nativeError);
}

// The geometry is live only during the native conversion. Snapshot its
// plain values here, using the same call and never retaining a body pointer.
__declspec(noinline) __declspec(guard(nocf))
void TraceCandidateConvert(void* context,const void* hit,void* candidate,uint8_t absolute) {
    auto native=reinterpret_cast<NativeCandidateConvert>(TargetAddress(kCandidateConvert));
#ifdef AXIOM_TESTING
    if (g_testCandidateConvert) native=g_testCandidateConvert;
#endif
    const DWORD beforeError=GetLastError();
    auto* frame=static_cast<TargetFrame*>(TlsGetValue(g_traceTls));
    SetLastError(beforeError);
    native(context,hit,candidate,absolute);
    if (!frame) return;
    if (frame->retryVector) {
        const DWORD nativeError=GetLastError();
        const auto base=reinterpret_cast<uintptr_t>(frame->retryVector->entries);
        const auto output=reinterpret_cast<uintptr_t>(candidate);
        if (absolute==1 && base && output>=base) {
            const auto offset=output-base;
            const auto slot=offset/sizeof(NativeRayCandidate);
            if (slot<frame->retryKinds.size() && slot<frame->retryVector->capacity &&
                offset%sizeof(NativeRayCandidate)==0)
                Get(reinterpret_cast<uintptr_t>(hit)+0x88,frame->retryKinds[slot]);
        }
        SetLastError(nativeError);
        return;
    }
    if (!frame->expectedConversions) return;
    --frame->expectedConversions;
    auto& sample=frame->sample;
    if (sample.count>sample.candidates.size() || sample.converted>=sample.count || absolute!=1)
        return;
    const DWORD nativeError=GetLastError();
    const auto from=reinterpret_cast<uintptr_t>(hit);
    const auto to=reinterpret_cast<uintptr_t>(candidate);
    auto& record=sample.candidates[sample.converted];
    std::array<uint8_t,0x58> bytes{};
    if (Get(to,bytes) && Get(from+0x88,record.kind)) {
        uintptr_t body=0;
        std::memcpy(record.point,bytes.data()+8,sizeof(record.point));
        std::memcpy(&record.material,bytes.data()+0x48,sizeof(record.material));
        std::memcpy(&body,bytes.data()+0x50,sizeof(body));
        record.body=body!=0;
        if (!sample.converted) frame->firstCandidate=to;
        if (to!=frame->firstCandidate+sample.converted*bytes.size()) frame->contiguousConversions=false;
        ++sample.converted;
        if (!frame->expectedConversions && sample.converted==sample.count && frame->contiguousConversions) {
            SetLastError(nativeError);
            ProbeNearbySurface(*frame,reinterpret_cast<NativeRayCandidate*>(frame->firstCandidate));
        }
    }
    SetLastError(nativeError);
}

// This native query sweeps a shape. Its two placement vectors can coincide.
__declspec(noinline) __declspec(guard(nocf))
bool TraceSweep(void* world,const void* query,uint8_t kind,void* result,float epsilon) {
    auto native=reinterpret_cast<NativeSweep>(TargetAddress(kSweep));
#ifdef AXIOM_TESTING
    if (g_testTraceSweep) native=g_testTraceSweep;
#endif
    const DWORD beforeError=GetLastError();
    auto* frame=static_cast<TargetFrame*>(TlsGetValue(g_traceTls));
    if (!frame) {
        SetLastError(beforeError);
        return native(world,query,kind,result,epsilon);
    }
    frame->sample.path=Path::Alternate;
    SetLastError(beforeError);
    const bool hit=native(world,query,kind,result,epsilon);
    const DWORD nativeError=GetLastError();
    const auto input=reinterpret_cast<uintptr_t>(query);
    std::array<uintptr_t,2> vectors{};
    uint32_t count=0, mask=0;
    uint16_t shape=0;
    float points[2][3]{};
    if (Get(input+8,mask) && mask==0x70 && Get(input+0x28,shape) && Get(input+0x48,points) &&
        (!hit || (Get(reinterpret_cast<uintptr_t>(result),vectors) &&
                  (vectors[0] || vectors[1]) && Get((vectors[0]?vectors[0]:vectors[1])+8,count) && count)))
        axiom::trace::RecordSweep(frame->sample,count,shape,points[0],points[1]);
    SetLastError(nativeError);
    return hit;
}

__declspec(noinline) __declspec(guard(nocf))
void TracePreQueryReset(void* object,uint8_t resetFlag) {
    auto native=reinterpret_cast<NativeReset>(TargetAddress(kReset));
#ifdef AXIOM_TESTING
    if (g_testTraceReset) native=g_testTraceReset;
#endif
    const DWORD beforeError=GetLastError();
    auto* frame=static_cast<TargetFrame*>(TlsGetValue(g_traceTls));
    if (frame && frame->component==object) {
        frame->sample.reset=true;
        frame->sample.resetReason=axiom::trace::DecodeResetReason(resetFlag);
        const bool unlimited=g_traceUnlimited.load(std::memory_order_relaxed);
        const auto index=TraceResetIndex(unlimited,frame->sample.resetReason);
        if (frame->sample.beforeRaw || frame->sample.beforeMode<2 ||
            frame->sample.tick>=g_traceNext[index].load(std::memory_order_relaxed))
            frame->sample.sampledActorMode=ReadActorMode(reinterpret_cast<uintptr_t>(object));
    }
    SetLastError(beforeError);
    native(object);
}

// The action-state callback can reset the same target outside its frame update.
__declspec(noinline) __declspec(guard(nocf))
void TraceExternalReset(void* object) {
    auto native=reinterpret_cast<NativeReset>(TargetAddress(kReset));
#ifdef AXIOM_TESTING
    if (g_testTraceReset) native=g_testTraceReset;
#endif
    const auto component=reinterpret_cast<uintptr_t>(object);
    const DWORD beforeError=GetLastError();
    Sample sample{};
    const bool tracked=g_initialized.load(std::memory_order_acquire) &&
                       !g_stopping.load(std::memory_order_acquire) && ControlledCatch(component) &&
                       ReadCursor(component,sample.beforeMode,sample.beforeRaw);
    SetLastError(beforeError);
    native(object);
    if (!tracked) return;
    const DWORD nativeError=GetLastError();
    sample.tick=GetTickCount64();
    if (ControlledCatch(component) && ReadCursor(component,sample.mode,sample.raw) &&
        axiom::trace::CursorCleared(sample)) {
        const bool unlimited=g_traceUnlimited.load(std::memory_order_relaxed);
        QueueTraceRow(TraceExternalResetIndex(unlimited),sample,250,false);
    }
    SetLastError(nativeError);
}

#ifndef SAF_UNIFIED_RUNTIME
struct TraceHook {
    uintptr_t call;
    size_t before;
    const uint8_t* context;
    size_t size;
    uintptr_t callback;
    size_t relay;
};

template<size_t N,typename F>
TraceHook HookAt(uintptr_t call,size_t before,const std::array<uint8_t,N>& context,F callback,size_t relay) {
    return {call,before,context.data(),N,reinterpret_cast<uintptr_t>(callback),relay};
}

#endif

template<size_t N>
quiesced_patch::Check TraceWitness(uintptr_t game,uintptr_t at,const std::array<uint8_t,N>& bytes) {
#ifdef SAF_UNIFIED_RUNTIME
    (void)game;
    (void)bytes;
    const auto capture=g_nativeRoles.Witness(at,N);
    if (capture.empty()) return {};
    return {reinterpret_cast<const uint8_t*>(TargetAddress(at)),capture.data(),N};
#else
    return {reinterpret_cast<const uint8_t*>(game+at),bytes.data(),N};
#endif
}

enum class TraceInstall { Installed, Retry, Refused };

TraceInstall TraceRefusal(const char* stage,uintptr_t rva=0,DWORD detail=0) {
    char message[200]{};
    snprintf(message,sizeof(message),"Surface trace install: stage=%s rva=0x%llX detail=0x%lX.",
             stage,static_cast<unsigned long long>(rva),detail);
    Log(message);
    return TraceInstall::Refused;
}

#ifdef AXIOM_TESTING
DWORD g_testTraceMemoryType=MEM_IMAGE;
std::atomic<unsigned>* g_testTraceInstallCount=nullptr;
TraceInstall (*g_testTraceInstall)(unsigned)=nullptr;
#endif

bool VerifiedAllHitsCollector(uintptr_t game,DWORD imageType) {
    const std::array checks{
        TraceWitness(game,0x3D79389,kAllHitsDispatch),
        TraceWitness(game,0x3D36A14,kAllHitsConstructor),
        TraceWitness(game,0x3D36B7F,kCollectorMerge),
    };
    for (const auto& check:checks) {
        const auto address=reinterpret_cast<uintptr_t>(check.address);
        if ((!safcompat::Region(address,check.size,game,imageType,PAGE_EXECUTE_READ) &&
             !safcompat::Region(address,check.size,game,imageType,PAGE_EXECUTE_WRITECOPY)) ||
            !quiesced_patch::SameBytes(check.address,check.expected,check.size)) return false;
    }
    return true;
}

#ifndef SAF_UNIFIED_RUNTIME
TraceInstall InstallTargetTrace() {
    const auto game=g.game;
    const std::array<TraceHook,12> hooks{{
        HookAt(kUpdateACall,12,kUpdateAContext,TraceUpdate,0),
        HookAt(kUpdateBCall,12,kUpdateBContext,TraceUpdate,0),
        HookAt(kUpdateCCall,12,kUpdateCContext,TraceUpdate,0),
        HookAt(kPrimaryCall,7,kPrimaryContext,TraceTarget,32),
        HookAt(kRayCall,17,kRayContext,TraceDispatch,64),
        HookAt(kSweepCall,21,kSweepContext,TraceSweep,96),
        HookAt(kResetCall,6,kResetContext,TracePreQueryReset,128),
        HookAt(kExternalResetCall,6,kExternalResetContext,TraceExternalReset,160),
        HookAt(kCandidateConvertCall,17,kCandidateConvertContext,TraceCandidateConvert,256),
        HookAt(kSurfaceCommitCall,11,kSurfaceCommitContext,TraceSurfaceCommit,320),
        HookAt(kCommitAnchorCall,19,kCommitAnchorContext,TraceCommitAnchor,352),
        HookAt(kCastSurfaceCall,34,kCastSurfaceContext,TraceCastSurface,384),
    }};
    constexpr size_t decisionRelay=192;
    const auto decision=axiom::trace::ResetDecisionRelay(game+kResetBranch,game+kContinueTarget);
    for (size_t i=0;i<hooks.size();++i) {
        const auto& hook=hooks[i];
        const size_t width=axiom::trace::Relay(hook.callback).size()+(hook.call==kResetCall?3:0);
        if (hook.relay+width>4096 ||
            (hook.relay<decisionRelay+decision.size() && decisionRelay<hook.relay+width)) return TraceRefusal("relay-layout",hook.call);
        for (size_t j=0;j<i;++j) {
            const auto& previous=hooks[j];
            if (hook.relay==previous.relay && hook.callback==previous.callback &&
                hook.call!=kResetCall && previous.call!=kResetCall) continue;
            const size_t otherWidth=axiom::trace::Relay(previous.callback).size()+(previous.call==kResetCall?3:0);
            if (hook.relay<previous.relay+otherWidth && previous.relay<hook.relay+width) return TraceRefusal("relay-overlap",hook.call);
        }
    }
    std::array<near_allocation::Reference,hooks.size()+3+kObstaclePathGates.size()> references{};
    references.back()={game+kStallRadialCall+5,kStallPullRelay};
    references[hooks.size()]={game+kResetDecision+5,decisionRelay};
    references[hooks.size()+1]={game+kObstacleCall+5,kObstacleRelay};
    for (size_t i=0;i<kObstaclePathGates.size();++i)
        references[hooks.size()+2+i]={game+kObstaclePathGates[i].rva+5,
                                     kObstacleGateRelay+i*kObstacleGateStride};
    const std::array fixedWitnesses{
        TraceWitness(game,kUpdate,kUpdateEntry), TraceWitness(game,kPrimary,kPrimaryEntry),
        TraceWitness(game,kCatchControlWitnessRva,kCatchControlWitness),
        TraceWitness(game,kRay,kRayEntry), TraceWitness(game,kSweep,kSweepEntry),
        TraceWitness(game,kCandidateConvert,kCandidateConvertEntry),
        TraceWitness(game,0x3D78240,kCandidateFields), TraceWitness(game,0x3D78340,kCandidateBodyStore),
        TraceWitness(game,kSurfaceQuery,kSurfaceQueryEntry),
        TraceWitness(game,0x676516,kSurfaceQueryTransfer),
        TraceWitness(game,0x6764B4,kSurfaceQueryVector),
        TraceWitness(game,0x6763E6,kSurfaceQueryInit),
        TraceWitness(game,0x3D79423,kSurfaceQueryAppend),
        TraceWitness(game,0x3D79439,kRaySort),
        TraceWitness(game,0x676651,kRayBodyBranch),
        TraceWitness(game,0x67671F,kRayClassifierExit),
        TraceWitness(game,0x3DA58F0,kSurfaceQuerySizing),
        TraceWitness(game,0x140F96B,kRayGrowInlineArgument),
        TraceWitness(game,0x3E3860,kRayGrowThunk),
        TraceWitness(game,0x8AB5509,kRayGrowInlineCompare),
        TraceWitness(game,0x8AB55F6,kRayGrowFreePrevious),
        TraceWitness(game,0x676AAC,kSurfaceQueryFreeTls),
        TraceWitness(game,0x1595740,kAllocatorTls),
        TraceWitness(game,0x4B7EACC,kSurfaceQueryFreeAlternate),
        TraceWitness(game,0x4B7E9CC,kSurfaceQueryFreeMain),
        TraceWitness(game,kSurfaceCommit,kSurfaceCommitEntry),
        TraceWitness(game,0x685AA6,kReelDispatch),
        TraceWitness(game,0x68AAB3,kSurfaceCommitStage),
        TraceWitness(game,0x68AC16,kSurfaceCommitKind),
        TraceWitness(game,0x68AEEC,kSurfaceCommitDistance),
        TraceWitness(game,kCommitAnchor,kCommitAnchorEntry),
        TraceWitness(game,0x68CCE6,kCommitWorldPosition),
        TraceWitness(game,0x68ADB9,kCommitAnchorDistanceInput),
        TraceWitness(game,kCastDispatch,kCastDispatchEntry),
        TraceWitness(game,0x67D27A,kCastCursorFields),
        TraceWitness(game,0x67D56C,kCastDescriptor),
        TraceWitness(game,0x67D3F0,kCastWorldTargetIdentity),
        TraceWitness(game,0x67D465,kCastWorldHandle),
        TraceWitness(game,0x67D499,kCastNormalizeType),
        TraceWitness(game,0x698193,kBodyBackedWorldTarget),
        TraceWitness(game,kActorModeWitnessRva,kActorModeWitness),
        TraceWitness(game,kReset,kResetEntry), TraceWitness(game,0x69841E,kResetStores),
        TraceWitness(game,0x677666,kPrimaryStores), TraceWitness(game,0x678437,kSweepStores),
        TraceWitness(game,0x67807B,kSweepCountUse), TraceWitness(game,0x677ED0,kSweepShape),
        TraceWitness(game,0x675E6D,kParentBranch),
        TraceWitness(game,kResetDecision,kResetDecisionBytes),
        TraceWitness(game,0x675C41,kContextResetChecks),
        TraceWitness(game,0x675989,kModeFlagInit),
        TraceWitness(game,0x675A46,kModeFlagWithRequest),
        TraceWitness(game,0x675AF9,kModeFlagWithoutRequest),
    };
    std::array<quiesced_patch::Check,fixedWitnesses.size()+hooks.size()> witnesses{};
    std::copy(fixedWitnesses.begin(),fixedWitnesses.end(),witnesses.begin());
    for (size_t i=0;i<hooks.size();++i) {
        const auto& hook=hooks[i];
        references[i]={game+hook.call+5,hook.relay};
        witnesses[fixedWitnesses.size()+i]={reinterpret_cast<const uint8_t*>(game+hook.call-hook.before),
                         hook.context,hook.size};
    }
#ifdef AXIOM_TESTING
    const DWORD imageType=g_testTraceMemoryType;
#else
    constexpr DWORD imageType=MEM_IMAGE;
#endif
    for (const auto& witness:witnesses) {
        const auto address=reinterpret_cast<uintptr_t>(witness.address);
        // The game's executable .data1 code can be mapped copy-on-write.
        // Its owner and bytes must match just like an RX page.
        if (!safcompat::Region(address,witness.size,game,imageType,PAGE_EXECUTE_READ) &&
            !safcompat::Region(address,witness.size,game,imageType,PAGE_EXECUTE_WRITECOPY)) {
            MEMORY_BASIC_INFORMATION memory{};
            VirtualQuery(witness.address,&memory,sizeof(memory));
            const auto protect=memory.Protect;
            TraceRefusal("native-protection",address-game,protect);
            const bool writer=memory.State==MEM_COMMIT && memory.Type==imageType &&
                reinterpret_cast<uintptr_t>(memory.AllocationBase)==game &&
                protect==PAGE_EXECUTE_READWRITE;
            return writer ? TraceInstall::Retry : TraceInstall::Refused;
        }
        if (!quiesced_patch::SameBytes(witness.address,witness.expected,witness.size))
            return TraceRefusal("native-bytes",address-game);
    }

    const near_allocation::Api allocator{
        nullptr,
        [](void*,uintptr_t at,near_allocation::Region& region) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))) return false;
            region={reinterpret_cast<uintptr_t>(info.BaseAddress),info.RegionSize,info.State==MEM_FREE};
            return true;
        },
        [](void*,uintptr_t at,size_t size) {
            return reinterpret_cast<uintptr_t>(VirtualAlloc(reinterpret_cast<void*>(at),size,
                    MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        },
        [](void*,uintptr_t at,size_t) { VirtualFree(reinterpret_cast<void*>(at),0,MEM_RELEASE); }
    };
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto allocation=near_allocation::Allocate(allocator,references.data(),references.size(),
        system.dwPageSize,system.dwAllocationGranularity,
        reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress),
        reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress),game,0x144E9000);
    if (!allocation.address) return TraceRefusal("relay-allocation",0,allocation.allocationFailures);
    auto* page=reinterpret_cast<uint8_t*>(allocation.address);
    const auto release=[&] {
        if (g_commitTls!=TLS_OUT_OF_INDEXES) TlsFree(g_commitTls);
        g_commitTls=TLS_OUT_OF_INDEXES;
        if (g_traceTls!=TLS_OUT_OF_INDEXES) TlsFree(g_traceTls);
        g_traceTls=TLS_OUT_OF_INDEXES;
        VirtualFree(page,0,MEM_RELEASE);
    };
    std::array<quiesced_patch::Patch,hooks.size()+1> patches{};
    std::memcpy(page+decisionRelay,decision.data(),decision.size());
    const auto obstacle=axiom::trace::Relay(reinterpret_cast<uintptr_t>(TraceObstacleCorrection));
    for (const auto& hook:hooks)
        if (hook.relay+axiom::trace::Relay(hook.callback).size()+3>kObstacleRelay) {
            release();
            return TraceRefusal("obstacle-relay-layout",hook.call);
        }
    const auto stall=axiom::trace::Relay(reinterpret_cast<uintptr_t>(TraceStallRadialStep));
    if (decisionRelay+decision.size()>kObstacleRelay ||
        kObstacleRelay+obstacle.size()>kObstacleGateRelay ||
        kStallPullRelay+stall.size()>system.dwPageSize) {
        release();
        return TraceRefusal("obstacle-relay-layout",kObstacleCall);
    }
    std::memcpy(page+kObstacleRelay,obstacle.data(),obstacle.size());
    std::memcpy(page+kStallPullRelay,stall.data(),stall.size());
    for (size_t i=0;i<kObstaclePathGates.size();++i) {
        const auto gate=ObstacleGateRelay(kObstaclePathGates[i],game+kNativeObstacleFlag,
            reinterpret_cast<uintptr_t>(&g_obstacleOwner));
        if (gate.size()>kObstacleGateStride) {
            release();
            return TraceRefusal("obstacle-gate-size",kObstaclePathGates[i].rva);
        }
        std::memcpy(page+kObstacleGateRelay+i*kObstacleGateStride,gate.data(),gate.size());
    }
    std::array<uint8_t,5> branch{};
    if (!axiom::trace::RelativeBranch(references[hooks.size()].next,allocation.address+decisionRelay,0xE9,branch)) {
        release();
        return TraceRefusal("decision-displacement",kResetDecision);
    }
    auto& decisionPatch=patches.back();
    decisionPatch.address=reinterpret_cast<uint8_t*>(game+kResetDecision);
    decisionPatch.size=kResetDecisionBytes.size();
    std::memcpy(decisionPatch.before,kResetDecisionBytes.data(),decisionPatch.size);
    std::memset(decisionPatch.after,0x90,decisionPatch.size);
    std::memcpy(decisionPatch.after,branch.data(),branch.size());
    for (size_t i=0;i<hooks.size();++i) {
        const auto& hook=hooks[i];
        const auto relay=axiom::trace::Relay(hook.callback);
        // Pass the exact native branch marker in DL. Execute the displaced vzeroupper
        // in the relay before entering the same ABI-compatible observer.
        constexpr std::array<uint8_t,3> loadResetFlag{0x8A,0x55,0x67};
        const bool passesResetFlag=hook.call==kResetCall;
        const size_t prefix=passesResetFlag?loadResetFlag.size():0;
        if (passesResetFlag) std::memcpy(page+hook.relay,hook.context+hook.before-prefix,prefix);
        std::memcpy(page+hook.relay+prefix,relay.data(),relay.size());
        std::array<uint8_t,5> bytes{};
        if (!axiom::trace::RelativeCall(references[i].next,allocation.address+hook.relay,bytes)) {
            release();
            return TraceRefusal("call-displacement",hook.call);
        }
        auto& patch=patches[i];
        patch.address=reinterpret_cast<uint8_t*>(game+hook.call-prefix);
        patch.size=prefix+bytes.size();
        std::memcpy(patch.before,hook.context+hook.before-prefix,patch.size);
        if (passesResetFlag) std::memcpy(patch.after,loadResetFlag.data(),prefix);
        std::memcpy(patch.after+prefix,bytes.data(),bytes.size());
    }
    DWORD protection=0;
    if (!VirtualProtect(page,system.dwPageSize,PAGE_EXECUTE_READ,&protection) ||
        !FlushInstructionCache(GetCurrentProcess(),page,system.dwPageSize)) {
        const DWORD error=GetLastError();
        release();
        return TraceRefusal("relay-protection",0,error);
    }
    g_traceTls=TlsAlloc();
    g_commitTls=TlsAlloc();
    if (g_traceTls==TLS_OUT_OF_INDEXES || g_commitTls==TLS_OUT_OF_INDEXES) {
        const DWORD error=GetLastError();
        release();
        return TraceRefusal("tls",0,error);
    }
    quiesced_patch::Report report{};
    if (!quiesced_patch::Write(patches.data(),patches.size(),{},&report,witnesses.data(),witnesses.size())) {
        uintptr_t rva=0;
        if (report.stage==quiesced_patch::Stage::ContractBytes && report.patch<witnesses.size())
            rva=reinterpret_cast<uintptr_t>(witnesses[report.patch].address)-game;
        else if (report.patch<patches.size())
            rva=reinterpret_cast<uintptr_t>(patches[report.patch].address)-game;
        TraceRefusal(quiesced_patch::StageName(report.stage),rva,report.error);
        if (report.unchanged && report.rollbackOk && report.allResumed) release();
        return quiesced_patch::Recoverable(report) ? TraceInstall::Retry : TraceInstall::Refused;
    }
    const bool allHits=VerifiedAllHitsCollector(game,imageType);
    g_allHitsCollectorReady.store(allHits,std::memory_order_release);
    Log(allHits?"Native all-hits camera collector verified for grid-only targets.":
                "Native all-hits camera collector unavailable; standard targeting retained.");
    g_obstaclePathReady=InstallObstacleObserver(allocation.address+kObstacleRelay,
        allocation.address+kObstacleGateRelay,imageType);
    Log(g_obstaclePathReady
        ?"Straight Pull native obstacle path ready: solver, waypoint basis, rope length and display."
        :"Straight Pull native obstacle path refused; player obstacle override inactive.");
    const bool stallReady=g_obstaclePathReady &&
        InstallStalledTether(allocation.address+kStallPullRelay,imageType);
    g_stallReady.store(stallReady,std::memory_order_release);
    Log(stallReady
        ?"Straight Pull stalled-tether release ready (native release, active pull only)."
        :"Straight Pull stalled-tether release unavailable.");
    // BootstrapThread retains the bridge. Relays and TLS share its process lifetime.
    return TraceInstall::Installed;
}

#else
#include "TargetTraceHooks.h"
#endif
