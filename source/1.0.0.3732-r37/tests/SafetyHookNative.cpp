#define SAF_HOOK_FIXTURE
#include <filesystem>
#include <fstream>
#include "../targeting/src/UnifiedTargeting.cpp"

bool CallProbe(void* first, const void* second, uint8_t third, void* fourth, float fifth) {
    return reinterpret_cast<uintptr_t>(first)==1 && reinterpret_cast<uintptr_t>(second)==2 &&
        third==3 && reinterpret_cast<uintptr_t>(fourth)==4 && fifth==2.0f;
}

bool TestNativeCallDispatch() {
    // Win64 caller populates four register arguments and one stack argument.
    // The original CALL returns false. The hook must use the five typed values
    // and resume after the exact call with its own result in AL.
    constexpr std::array<uint8_t,49> program{
        0x48,0x83,0xEC,0x38,
        0xB9,1,0,0,0, 0xBA,2,0,0,0, 0x41,0xB8,3,0,0,0, 0x41,0xB9,4,0,0,0,
        0xC7,0x44,0x24,0x20,0,0,0,0x40,
        0xE8,5,0,0,0,
        0x48,0x83,0xC4,0x38,0xC3,
        0x31,0xC0,0xC3,
    };
    constexpr size_t call=34;
    auto* executable=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!executable) return false;
    std::memcpy(executable,program.data(),program.size());
    DWORD old=0;
    if (!VirtualProtect(executable,4096,PAGE_EXECUTE_READ,&old)) return false;
    const auto previous=g;
    g={reinterpret_cast<uintptr_t>(executable),4096};
    auto hook=saf_hooks::Prepare(GameImage(),g.game+call,{program.data()+call,5},TraceCall<call,CallProbe>);
    const auto run=reinterpret_cast<bool(*)()>(executable);
    const bool stock=!run();
    const bool observed=hook && saf_hooks::Enable(GameImage(),*hook) && run();
    hook.reset();
    g=previous;
    VirtualFree(executable,0,MEM_RELEASE);
    return stock && observed;
}

bool VerifyRejection(uintptr_t base,size_t size,uintptr_t preferred,const axiom::native::Roles& roles) {
    const auto updateCall=roles.Address(0x9CF3C5);
    const auto wrongCallee=roles.Address(0x675EA0);
    if (!updateCall || !wrongCallee) return false;
    auto* bytes=reinterpret_cast<uint8_t*>(updateCall);
    if (bytes[0]!=0xE8) return false;
    int32_t original=0;
    std::memcpy(&original,bytes+1,sizeof(original));
    const auto replacement=static_cast<int32_t>(wrongCallee-updateCall-5);
    if (replacement==original) return false;
    DWORD old=0,writable=0;
    if (!VirtualProtect(bytes,5,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(bytes+1,&replacement,sizeof(replacement));
    if (!VirtualProtect(bytes,5,old,&writable)) return false;
    axiom::native::Roles rejected;
    const bool refused=!rejected.Initialize(base,size,preferred);
    if (!VirtualProtect(bytes,5,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(bytes+1,&original,sizeof(original));
    return VirtualProtect(bytes,5,old,&writable) && refused;
}

bool VerifyRangeTransaction() {
    uint8_t* support=nullptr;
    const auto start=(g.game+g.size+0xFFFF)&~uintptr_t{0xFFFF};
    for (uintptr_t offset=0;offset<0x1000000 && !support;offset+=0x10000)
        support=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(start+offset),
                                                   8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!support) return false;
    const auto address=reinterpret_cast<uintptr_t>(support);
    const bool installed=g_catchRange.Update(g.game,address,Read);
    constexpr std::array<uintptr_t,4> keys{0x67BD5F,0x139662A,0x138DCE3,0x1391FD3};
    bool matches=installed;
    for (const auto key:keys) {
        const auto at=g_nativeRoles.Address(key);
        int32_t displacement=0;
        matches=Get(at+4,displacement) && at+8+displacement==address+0x1000 && matches;
    }
    const bool restored=g_catchRange.Restore(g.game,Read);
    for (const auto key:keys) {
        const auto source=g_nativeRoles.Witness(key,8);
        matches=source.size()==8 && !std::memcmp(reinterpret_cast<const void*>(g_nativeRoles.Address(key)),
                                                  source.data(),source.size()) && matches;
    }
    VirtualFree(support,0,MEM_RELEASE);
    return matches && restored;
}

int wmain(int argc,wchar_t** argv) {
    if (argc!=2 && argc!=3) return 2;
    std::ifstream stream(std::filesystem::path(argv[1]), std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length < 4096 || length > 0x24000000) return 2;
    std::vector<uint8_t> raw(static_cast<size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(raw.data()), length)) return 2;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) > raw.size() - sizeof(IMAGE_NT_HEADERS64)) return 2;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    if (nt->Signature != IMAGE_NT_SIGNATURE || imageSize > 0x24000000 ||
        nt->OptionalHeader.SizeOfHeaders > raw.size() || nt->OptionalHeader.SizeOfHeaders > imageSize) return 2;
    void* preferred=argc==3?reinterpret_cast<void*>(0x140000000ULL):nullptr;
    auto* image = static_cast<uint8_t*>(VirtualAlloc(preferred, imageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image) return 2;

    std::memcpy(image, raw.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& section = sections[i];
        const auto count = (std::min)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (section.PointerToRawData > raw.size() || count > raw.size() - section.PointerToRawData ||
            section.VirtualAddress > imageSize || count > imageSize - section.VirtualAddress) return 2;
        std::memcpy(image + section.VirtualAddress, raw.data() + section.PointerToRawData, count);
        if (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            DWORD old=0;
            if (!VirtualProtect(image+section.VirtualAddress,section.Misc.VirtualSize,PAGE_EXECUTE_READ,&old)) return 2;
        }
    }
    const auto base=reinterpret_cast<uintptr_t>(image);
    g={base,nt->OptionalHeader.SizeOfImage};
    g_fixturePreferredBase=nt->OptionalHeader.ImageBase;
    g_logSink=[](const char* message){ puts(message); };
    if (saf_targeting::Prepare(nullptr)) {
        puts("FAIL runtime refuses this renamed/non-game executable");
        return 1;
    }
    g_logSink=[](const char* message){ puts(message); };
    if (!g_nativeRoles.Initialize(base,imageSize,g_fixturePreferredBase)) {
        printf("REFUSED unrecognized native targeting role 0x%llX\n",
               static_cast<unsigned long long>(g_nativeRoles.FailedRole()));
        return 1;
    }
    if (!VerifyRejection(base,imageSize,g_fixturePreferredBase,g_nativeRoles)) {
        puts("FAIL adaptive binding rejects a mismatched relative call target");
        return 1;
    }
    puts("PASS adaptive binding rejects a mismatched relative call target");
    if (InstallTargetTrace()!=TraceInstall::Installed || !g_obstaclePathReady ||
        !g_stallReady.load() || !g_allHitsCollectorReady.load()) {
        puts("FAIL integrated SafetyHook group binds the game image");
        return 1;
    }
    printf("PASS %zu targeting hooks, %zu obstacle hooks, one stall hook\n",
        g_targetHooks->size(),g_obstacleHooks->size());
    const bool range=VerifyRangeTransaction();
    printf("%s four current-image range consumers redirect and restore exact original bytes\n",
        range?"PASS":"FAIL");
    const bool dispatch=TestNativeCallDispatch();
    printf("%s typed native bridge handles Win64 register and stack arguments, return value and continuation\n",
        dispatch?"PASS":"FAIL");
    return range && dispatch?0:1;
}

