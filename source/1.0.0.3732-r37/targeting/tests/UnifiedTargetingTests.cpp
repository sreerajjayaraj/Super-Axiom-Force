#define AXIOM_TESTING 1
#define SAF_HOOK_FIXTURE 1
#include "../src/UnifiedTargeting.cpp"
#include <filesystem>
#include <fstream>
#include <vector>

int wmain(int argc,wchar_t** argv) {
    using namespace saf_targeting;
    if (argc!=2) return 2;
    const bool guarded=!Prepare(nullptr) && Sync({})==saf_targeting::Result::Inactive &&
        Sync({1,50000.f,true})==saf_targeting::Result::Unavailable;
    printf("%s combined targeting rejects a foreign process and remains dormant without ownership\n",
           guarded?"PASS":"FAIL");
    if (!guarded) return 1;
    std::ifstream file(std::filesystem::path(argv[1]),std::ios::binary|std::ios::ate);
    const auto length=file.tellg();
    if (length<4096 || length>0x24000000) return 2;
    std::vector<uint8_t> raw(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(raw.data()),length)) return 2;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<64 || dos->e_lfanew>0x1000) return 2;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data()+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage!=0x144E9000) return 2;
    const auto bytes=nt->OptionalHeader.SizeOfImage;
    const HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_EXECUTE_READWRITE,0,bytes+0x2000,nullptr);
    if (!mapping) return 2;
    auto* image=static_cast<uint8_t*>(MapViewOfFile(mapping,FILE_MAP_WRITE|FILE_MAP_EXECUTE,0,0,0));
    CloseHandle(mapping);
    if (!image) return 2;
    const auto* sections=IMAGE_FIRST_SECTION(nt);
    for (size_t i=0;i<nt->FileHeader.NumberOfSections;++i) {
        const auto& section=sections[i];
        if (!(section.Characteristics&IMAGE_SCN_MEM_EXECUTE)) continue;
        const size_t count=(std::min)(section.Misc.VirtualSize,section.SizeOfRawData);
        if (section.PointerToRawData>raw.size() || count>raw.size()-section.PointerToRawData ||
            section.VirtualAddress>bytes || count>bytes-section.VirtualAddress) return 2;
        auto* const base=image+section.VirtualAddress;
        std::memcpy(base,raw.data()+section.PointerToRawData,count);
        DWORD previous=0;
        if (!VirtualProtect(base,count,PAGE_EXECUTE_READ,&previous)) return 2;
    }
    g={reinterpret_cast<uintptr_t>(image),bytes};
    g_testTraceMemoryType=MEM_MAPPED;
    g_hostValid=true;
    const auto support=reinterpret_cast<uintptr_t>(image+bytes);
    std::array<uint8_t,8> originalRange{};
    std::memcpy(originalRange.data(),image+0x67BD5F,originalRange.size());
    const auto requested=State{support,50000.f,true};
    const bool activated=Sync(requested)==saf_targeting::Result::Active && g_searchReady.load() &&
        g_allHitsCollectorReady.load() && g_straightPullOwned.load() &&
        std::memcmp(image+0x67BD5F,originalRange.data(),originalRange.size())!=0;
    constexpr auto resetBranch=kResetDecision+4;
    const auto relayBranch=image[resetBranch];
    const bool stopped=Sync({})==saf_targeting::Result::Inactive && !g_searchReady.load() &&
        !g_straightPullOwned.load() &&
        std::memcmp(image+0x67BD5F,originalRange.data(),originalRange.size())==0;
    const bool resumed=Sync({support,50000.f,false})==saf_targeting::Result::Active &&
        g_searchReady.load() && !g_straightPullOwned.load() &&
        relayBranch==0xE9 && image[resetBranch]==relayBranch && Sync({})==saf_targeting::Result::Inactive;
    printf("%s exact-game integration installs once, restores range inputs on disable and resumes without DLMM\n",
           activated&&stopped&&resumed?"PASS":"FAIL");
    UnmapViewOfFile(image);
    return activated&&stopped&&resumed?0:1;
}
