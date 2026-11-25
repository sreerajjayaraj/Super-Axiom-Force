// Included after the unified targeting callbacks and native witnesses.
std::atomic<bool> g_traceHooksReady{false};
std::atomic<bool> g_obstacleHooksReady{false};
// Published once by the worker. Keep trampolines for the lifetime of the pinned ASI.
using Hook = std::unique_ptr<safetyhook::MidHook>;
std::vector<Hook>* g_targetHooks=nullptr;
std::vector<Hook>* g_obstacleHooks=nullptr;
Hook* g_stallHook=nullptr;

template<class T> T Arg(uintptr_t value) {
    return static_cast<T>(value);
}
template<> void* Arg<void*>(uintptr_t value) { return reinterpret_cast<void*>(value); }
template<> const void* Arg<const void*>(uintptr_t value) { return reinterpret_cast<const void*>(value); }

template<size_t index, class T> T Arg(const safetyhook::Context& context) {
    if constexpr (index<4) {
        if constexpr (std::is_same_v<T,float>) {
            const safetyhook::Xmm* registers[]={&context.xmm0,&context.xmm1,&context.xmm2,&context.xmm3};
            return registers[index]->f32[0];
        } else {
            const uintptr_t registers[]={context.rcx,context.rdx,context.r8,context.r9};
            return Arg<T>(registers[index]);
        }
    } else {
        // At the original call site, the caller's shadow space starts at RSP.
        T value{};
        std::memcpy(&value,reinterpret_cast<const void*>(context.rsp+index*sizeof(uintptr_t)),sizeof(T));
        return value;
    }
}

template<class R,class... A,size_t... I>
void Invoke(safetyhook::Context& context,R(*callback)(A...),std::index_sequence<I...>) {
    if constexpr (std::is_same_v<R,void>) callback(Arg<I,A>(context)...);
    else if constexpr (std::is_same_v<R,bool>) {
        const auto result=callback(Arg<I,A>(context)...);
        context.rax=(context.rax&~uintptr_t{0xFF})|static_cast<uintptr_t>(result);
    } else {
        static_assert(std::is_pointer_v<R>);
        context.rax=reinterpret_cast<uintptr_t>(callback(Arg<I,A>(context)...));
    }
}

template<uintptr_t call,class R,class... A>
void Invoke(safetyhook::Context& context,R(*target)(A...)) {
    Invoke(context,target,std::index_sequence_for<A...>{});
    context.rip=TargetAddress(call)+5;
}

template<uintptr_t call,auto callback>
void TraceCall(safetyhook::Context& context) {
    if (g_traceHooksReady.load(std::memory_order_acquire)) Invoke<call>(context,callback);
}

// The native vzeroupper immediately before the reset CALL still executes.
void TraceResetCall(safetyhook::Context& context) {
    if (!g_traceHooksReady.load(std::memory_order_acquire)) return;
    const auto flag=*reinterpret_cast<const uint8_t*>(context.rbp+0x67);
    TracePreQueryReset(reinterpret_cast<void*>(context.rcx),flag);
    context.rip=TargetAddress(kResetCall)+5;
}

constexpr uintptr_t kZeroFlag=0x40;
void MarkResetDecision(safetyhook::Context& context) {
    // At the JE, the register context holds the exact flags from the native CMP.
    if (g_traceHooksReady.load(std::memory_order_acquire) && !(context.rflags&kZeroFlag))
        *reinterpret_cast<uint8_t*>(context.rbp+0x67)=axiom::trace::kActorResetMarker;
}

saf_hooks::Image GameImage() {
#if defined(AXIOM_TESTING) && defined(SAF_HOOK_FIXTURE)
    return {g.game,g.size,g_testTraceMemoryType};
#else
    return {g.game,g.size};
#endif
}

template<class Checks>
bool VerifyWitnesses(const Checks& checks) {
    for (const auto& check : checks) {
        const auto address = reinterpret_cast<uintptr_t>(check.address);
        const auto rva = address >= g.game ? address - g.game : 0;
        if (!saf_hooks::Executable(GameImage(), address, check.size)) {
            TraceRefusal("native-witness-page", rva, static_cast<DWORD>(check.size));
            return false;
        }
        if (std::memcmp(check.address, check.expected, check.size)) {
            TraceRefusal("native-witness-bytes", rva, static_cast<DWORD>(check.size));
            return false;
        }
    }
    return true;
}

template<size_t N>
bool PrepareTrace(std::vector<Hook>& hooks,uintptr_t offset,const std::array<uint8_t,N>& context,
                  size_t before,size_t width,safetyhook::MidHookFn callback) {
    const auto check=TraceWitness(g.game,offset-before,context);
    if (!VerifyWitnesses(std::array{check})) return false;
    if (before>N || width>N-before || offset<before) return false;
    auto hook=saf_hooks::Prepare(GameImage(),
        reinterpret_cast<uintptr_t>(check.address)+before,{check.expected+before,width},callback);
    if (!hook) {
        TraceRefusal("safetyhook-prepare",offset);
        return false;
    }
    hooks.push_back(std::move(hook));
    return true;
}

bool EnableTraceGroup(std::vector<Hook>& hooks) {
    for (const auto& hook:hooks) {
        if (saf_hooks::Enable(GameImage(),*hook)) continue;
        TraceRefusal("safetyhook-enable",hook->target_address()-g.game);
        return false;
    }
    return true;
}

void ObserveObstacle(safetyhook::Context& context) {
    if (g_obstacleHooksReady.load(std::memory_order_acquire))
        Invoke<kObstacleCall>(context,TraceObstacleCorrection);
}

template<class T> bool ReadGateValue(uintptr_t address,T& value) {
    const auto error=GetLastError();
    const bool read=Get(address,value);
    SetLastError(error);
    return read;
}

template<ObstaclePathStage stage> void ObstacleGate(safetyhook::Context& context) {
    if (!g_obstacleHooksReady.load(std::memory_order_acquire)) return;
    uint8_t nativeFlag=0;
    if (!ReadGateValue(TargetAddress(kNativeObstacleFlag),nativeFlag)) return;
    if (nativeFlag) {
        context.rflags&=~kZeroFlag;
        return;
    }
    context.rflags|=kZeroFlag;
    const auto owner=g_obstacleOwner.load(std::memory_order_acquire);
    if (!owner) return;
    uintptr_t actor=0;
    if constexpr (stage==ObstaclePathStage::Correction) ReadGateValue(context.rdi+0x20,actor);
    else if constexpr (stage==ObstaclePathStage::Basis) actor=context.rsi;
    else if constexpr (stage==ObstaclePathStage::Length) actor=context.rbx;
    else ReadGateValue(context.r13+8,actor);
    if (actor!=owner) return;
    if constexpr (stage==ObstaclePathStage::Correction) g_obstacleGateEntries.fetch_add(1,std::memory_order_relaxed);
    else if constexpr (stage==ObstaclePathStage::Basis) {
        uint32_t waypoints=0;
        if (ReadGateValue(context.rbx+0xD78,waypoints) && waypoints)
            g_obstacleWaypointReads.fetch_add(1,std::memory_order_relaxed);
    }
    context.rflags&=~kZeroFlag;
}

bool InstallObstacleHooks() {
    const auto game=g.game;
    const std::array checks{
        TraceWitness(game,kObstacleQuaternion,kObstacleQuaternionEntry),
        TraceWitness(game,0x13B8C93,kWaypointBasisOwner),
        TraceWitness(game,0x13B8CE4,kWaypointBasisLookup),
        TraceWitness(game,0x3C89BA,kWaypointLengthOwner),
        TraceWitness(game,0x3C8A3D,kWaypointLengthLookup),
        TraceWitness(game,0x69656E,kWaypointDisplayOwner),
        TraceWitness(game,0x6968C5,kWaypointDisplayLookup),
    };
    if (!VerifyWitnesses(checks)) return false;
    auto hooks=std::make_unique<std::vector<Hook>>();
    if (!PrepareTrace(*hooks,kObstacleCall,kObstacleContext,17,5,ObserveObstacle)) return false;
    constexpr safetyhook::MidHookFn callbacks[]{ObstacleGate<ObstaclePathStage::Correction>,
        ObstacleGate<ObstaclePathStage::Basis>,ObstacleGate<ObstaclePathStage::Length>,ObstacleGate<ObstaclePathStage::Display>};
    for (size_t i=0;i<kObstaclePathGates.size();++i) {
        const auto& gate=kObstaclePathGates[i];
        if (!PrepareTrace(*hooks,gate.rva+7,gate.context,7,6,callbacks[i])) return false;
    }
    g_obstacleHooks=hooks.release();
    return EnableTraceGroup(*g_obstacleHooks);
}

void ObserveStall(safetyhook::Context& context) {
    if (g_stallReady.load(std::memory_order_acquire))
        Invoke<kStallRadialCall>(context,TraceStallRadialStep);
}

bool InstallStallHook() {
    const auto game=g.game;
    const std::array checks{
        TraceWitness(game,kStallRadialCall-7,kStallRadialContext),
        TraceWitness(game,kNativeRadialStep,kStallRadialEntry),
        TraceWitness(game,0x13BC3AB,kStallReelInput),
        TraceWitness(game,0x674161,kTetherReleaseCall),
        TraceWitness(game,kNativeTetherRelease,kTetherReleaseEntry),
    };
    if (!VerifyWitnesses(checks)) return false;
    const auto& context=checks.front();
    auto hook=saf_hooks::Prepare(GameImage(),
        reinterpret_cast<uintptr_t>(context.address)+7,{context.expected+7,5},ObserveStall);
    if (!hook) return false;
    g_stallHook=new Hook(std::move(hook));
    return saf_hooks::Enable(GameImage(),**g_stallHook);
}

TraceInstall InstallTargetTrace() {
    const auto game=g.game;
#if defined(SAF_HOOK_FIXTURE) || defined(SAF_MAPPED_IMAGE_TESTING)
    const auto pointerBase=g_fixturePreferredBase;
#else
    const auto pointerBase=game;
#endif
    if (!g_nativeRoles.Initialize(game,g.size,pointerBase))
        return TraceRefusal("native-role-contract",g_nativeRoles.FailedRole());
    if (!g_catchRange.Configure(g_nativeRoles,game))
        return TraceRefusal("native-range-contract");
    Log("Axiom adaptive native contracts verified for targeting, range, obstacles and tether release.");
    const std::array checks{
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
    if (!VerifyWitnesses(checks)) return TraceInstall::Refused;
    auto hooks=std::make_unique<std::vector<Hook>>();
    if (!PrepareTrace(*hooks,kUpdateACall,kUpdateAContext,12,5,TraceCall<kUpdateACall,TraceUpdate>) ||
        !PrepareTrace(*hooks,kUpdateBCall,kUpdateBContext,12,5,TraceCall<kUpdateBCall,TraceUpdate>) ||
        !PrepareTrace(*hooks,kUpdateCCall,kUpdateCContext,12,5,TraceCall<kUpdateCCall,TraceUpdate>) ||
        !PrepareTrace(*hooks,kPrimaryCall,kPrimaryContext,7,5,TraceCall<kPrimaryCall,TraceTarget>) ||
        !PrepareTrace(*hooks,kRayCall,kRayContext,17,5,TraceCall<kRayCall,TraceDispatch>) ||
        !PrepareTrace(*hooks,kSweepCall,kSweepContext,21,5,TraceCall<kSweepCall,TraceSweep>) ||
        !PrepareTrace(*hooks,kResetCall,kResetContext,6,5,TraceResetCall) ||
        !PrepareTrace(*hooks,kExternalResetCall,kExternalResetContext,6,5,TraceCall<kExternalResetCall,TraceExternalReset>) ||
        !PrepareTrace(*hooks,kCandidateConvertCall,kCandidateConvertContext,17,5,TraceCall<kCandidateConvertCall,TraceCandidateConvert>) ||
        !PrepareTrace(*hooks,kSurfaceCommitCall,kSurfaceCommitContext,11,5,TraceCall<kSurfaceCommitCall,TraceSurfaceCommit>) ||
        !PrepareTrace(*hooks,kCommitAnchorCall,kCommitAnchorContext,19,5,TraceCall<kCommitAnchorCall,TraceCommitAnchor>) ||
        !PrepareTrace(*hooks,kCastSurfaceCall,kCastSurfaceContext,34,5,TraceCall<kCastSurfaceCall,TraceCastSurface>) ||
        !PrepareTrace(*hooks,kResetDecision+4,kResetDecisionBytes,4,6,MarkResetDecision)) return TraceInstall::Refused;

    g_traceTls=TlsAlloc();
    g_commitTls=TlsAlloc();
    if (g_traceTls==TLS_OUT_OF_INDEXES || g_commitTls==TLS_OUT_OF_INDEXES) {
        if (g_traceTls!=TLS_OUT_OF_INDEXES) TlsFree(g_traceTls);
        if (g_commitTls!=TLS_OUT_OF_INDEXES) TlsFree(g_commitTls);
        g_traceTls=g_commitTls=TLS_OUT_OF_INDEXES;
        return TraceRefusal("tls",0,GetLastError());
    }
    g_targetHooks=hooks.release();
    if (!EnableTraceGroup(*g_targetHooks)) return TraceInstall::Refused;

    const bool allHits=VerifiedAllHitsCollector(game,GameImage().type);
    g_allHitsCollectorReady.store(allHits,std::memory_order_release);
    Log(allHits?"Native all-hits camera collector verified for grid-only targets.":
                "Native all-hits camera collector unavailable; standard targeting retained.");
    g_obstaclePathReady=InstallObstacleHooks();
    g_obstacleHooksReady.store(g_obstaclePathReady,std::memory_order_release);
    Log(g_obstaclePathReady
        ?"Straight Pull native obstacle path ready: solver, waypoint basis, rope length and display."
        :"Straight Pull native obstacle path refused; player obstacle override inactive.");
    const bool stallReady=g_obstaclePathReady && InstallStallHook();
    g_stallReady.store(stallReady,std::memory_order_release);
    Log(stallReady
        ?"Straight Pull stalled-tether release ready (native release, active pull only)."
        :"Straight Pull stalled-tether release unavailable.");
    g_traceHooksReady.store(true,std::memory_order_release);
    return TraceInstall::Installed;
}
