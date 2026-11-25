// Included after the SAF state definitions.
struct CoreHook {
    std::unique_ptr<safetyhook::MidHook> hook;
    std::array<uint8_t,16> branch{};
};

struct CoreSite {
    size_t rva=0;
    std::span<const uint8_t> bytes{};
    safetyhook::MidHookFn callback=nullptr;
};

static CoreHook* g_coreHooks=nullptr;
static std::array<CoreSite,3> g_coreSites{};
static uintptr_t g_ordinaryPullReturn=0;
static std::atomic<bool> g_straightPolicy{false};
static std::atomic<bool> g_instantPolicy{false};

static void ApplyAngularPolicy(safetyhook::Context& context) {
    // All three native scalar multiplies have already executed. The helper's
    // original caller return is above its eight pushes and 0x188-byte frame.
    constexpr size_t callerReturn=straight_pull_angular::kCallerReturnOffset-8;
    if (!g_straightPolicy.load(std::memory_order_relaxed) ||
        *reinterpret_cast<const uintptr_t*>(context.rsp+callerReturn)!=g_ordinaryPullReturn) return;
    context.xmm1.u32[0]=0;
    context.xmm2.u32[0]=0;
    context.xmm3.u32[0]=0;
}

static bool ClockInputsLive(uintptr_t lane) {
    for (const uintptr_t field:{uintptr_t{0x9C0},uintptr_t{0x9B8}}) {
        const auto state=*reinterpret_cast<const uintptr_t*>(lane+field);
        if (!state) return false;
        const auto object=*reinterpret_cast<const uintptr_t*>(state+8);
        if (!object || *reinterpret_cast<const uint8_t*>(object+0x15)) return false;
    }
    return true;
}

template<bool Propell> static void ApplyClockPolicy(safetyhook::Context& context) {
    if (!g_instantPolicy.load(std::memory_order_relaxed)) return;
    const uintptr_t lane=context.rcx+(Propell?context.rdi:context.r14);
    if constexpr (Propell) {
        if (InterlockedCompareExchange(g_keepTravelClock,0,0) ||
            *reinterpret_cast<const uint8_t*>(lane+0xB28)) return;
    }
    if (!ClockInputsLive(lane)) return;
    // Only normal positive values may supply the predecessor time. The
    // ordinary result and upper register lanes already come from VADDSS.
    const auto elapsed=*reinterpret_cast<const uint32_t*>(lane+0xB20);
    const auto duration=*reinterpret_cast<const uint32_t*>(lane+0xB24);
    const auto delta=context.xmm1.u32[0];
    constexpr uint32_t minNormal=0x00800000, infinity=0x7F800000;
    if (duration<=minNormal || duration>=infinity || delta<minNormal || delta>=infinity) return;
    const auto predecessor=duration-1;
    if (elapsed>=predecessor) return;
    const auto exponent=predecessor>>23;
    if (exponent>=24 && delta<((exponent-23)<<23)) return;
    context.xmm0.u32[0]=predecessor;
}

static bool BindCoreHooks(const native::Result& result,size_t angularSite,
                          const std::array<size_t,2>& selectedReturns) {
    g_ordinaryPullReturn=g_gameBase+selectedReturns[0];
    if (g_ordinaryPullReturn<=g_gameBase || selectedReturns[0]>=g_gameSize) return false;
    static constexpr std::array<uint8_t,5> quaternionArg{0x48,0x8D,0x4C,0x24,0x40};
    static constexpr std::array<uint8_t,10> reelStore{0xC4,0xC1,0x7A,0x11,0x84,0x0E,0x20,0x0B,0,0};
    static constexpr std::array<uint8_t,9> propellStore{0xC5,0xFA,0x11,0x84,0x0F,0x20,0x0B,0,0};
    if (!result.clock[0].valid || !result.clock[1].valid) return false;
    g_coreSites={{
        {angularSite+sizeof(straight_pull_angular::kOriginal),quaternionArg,ApplyAngularPolicy},
        {result.clock[0].instruction+sizeof(clock_acceleration::kReelAdd),reelStore,ApplyClockPolicy<false>},
        {result.clock[1].instruction+sizeof(clock_acceleration::kPropellAdd),propellStore,ApplyClockPolicy<true>},
    }};
    return true;
}

static bool PrepareCoreHooks(size_t first,size_t end) {
    if (!g_coreSites[first].callback) return false;
    if (!g_coreHooks) g_coreHooks=new CoreHook[3];
    const saf_hooks::Image image{g_gameBase,g_gameSize};
    for (size_t i=first;i<end;++i) {
        const auto& site=g_coreSites[i];
        auto& hook=g_coreHooks[i];
        if (hook.hook) continue;
        saf_hooks::PreparationFailure failure{};
        hook.hook=saf_hooks::Prepare(image,g_gameBase+site.rva,site.bytes,site.callback,&failure);
        if (!hook.hook) {
            Logf("Core hook preparation refused site=%zu rva=0x%llX stage=%s memory_type=0x%lX protection=0x%lX state=0x%lX library_type=%u detail=%u",
                 i,static_cast<unsigned long long>(site.rva),failure.stage,
                 failure.memory.Type,failure.memory.Protect,failure.memory.State,
                 failure.libraryType,failure.libraryDetail);
            return false;
        }
    }
    return true;
}

static bool EnableCoreHooks(size_t first,size_t end) {
    if (!g_coreHooks) return false;
    const saf_hooks::Image image{g_gameBase,g_gameSize};
    for (size_t i=first;i<end;++i) {
        auto& site=g_coreHooks[i];
        if (!site.hook) return false;
        const auto length=site.hook->original_bytes().size();
        if (site.hook->enabled()) {
            if (!saf_hooks::Executable(image,site.hook->target_address(),length) ||
                std::memcmp(site.hook->target(),site.branch.data(),length)) return false;
            continue;
        }
        if (!saf_hooks::Enable(image,*site.hook)) return false;
        std::memcpy(site.branch.data(),site.hook->target(),length);
    }
    return true;
}

static void AccountForCoreHook(uintptr_t address,uint8_t& expected) {
    if (!g_coreHooks) return;
    for (size_t i=0;i<3;++i) {
        const auto& site=g_coreHooks[i];
        if (!site.hook || !site.hook->enabled()) continue;
        const auto target=site.hook->target_address();
        if (address>=target && address-target<site.hook->original_bytes().size())
            expected=site.branch[address-target];
    }
}
