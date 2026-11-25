#define AXIOM_TESTING 1
#include "../src/AxiomCoexist.cpp"

int main() {
    constexpr uintptr_t predicate=0x5E0C50;
    auto* game=static_cast<uint8_t*>(VirtualAlloc(nullptr,0xC00000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    auto* menu=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x690000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if (!game || !menu) return 2;
    const auto gameBase=reinterpret_cast<uintptr_t>(game);
    const auto menuBase=reinterpret_cast<uintptr_t>(menu);
    constexpr uintptr_t pageOffset=0x689000;
    auto* recordPage=menu+pageOffset;
    memset(recordPage,0xA5,4096);
    memcpy(game+predicate,kFlashPredicate.data(),kFlashPredicate.size());
    for (const auto& guard:kFlash605Guards) {
        std::memcpy(game+guard.current,guard.relocated.data(),guard.length);
        const uintptr_t label=menuBase+guard.label;
        auto* record=menu+guard.descriptor;
        std::memcpy(record,&guard.before,8);
        std::memcpy(record+8,&label,8);
        std::memcpy(record+16,&guard.length,4);
        std::memcpy(record+20,guard.original.data(),guard.length);
    }
    std::array<uint8_t,4096> before{},after{};
    std::memcpy(before.data(),recordPage,before.size());
    after=before;
    const auto checkGuards=[&]() {
        unsigned passes=0;
        for (const auto& guard:kFlash605Guards) {
            uintptr_t rva=0;
            uint32_t length=0;
            auto* record=menu+guard.descriptor;
            std::memcpy(&rva,record,8);
            std::memcpy(&length,record+16,4);
            if (length==guard.length && (rva==guard.before || rva==guard.current) &&
                std::memcmp(game+rva,record+20,length)==0) ++passes;
        }
        return passes;
    };
    DWORD old=0;
    if (!VirtualProtect(recordPage,4096,PAGE_READONLY,&old) ||
        !VirtualProtect(game+predicate,kFlashPredicate.size(),PAGE_EXECUTE_READ,&old)) return 2;
    const auto& mutated=kFlash605Guards.back();
    game[mutated.current+5]^=1;
    for (const auto& guard:kFlash605Guards)
        if (!VirtualProtect(game+guard.current,guard.length,PAGE_EXECUTE_READ,&old)) return 2;
    const bool rejected=RepairFlashGuards605(gameBase,menuBase,MEM_PRIVATE)==FlashRepair::Refused &&
                        std::memcmp(recordPage,before.data(),before.size())==0;
    printf("%s foreign native timing bytes refused with both descriptors unchanged\n",rejected?"PASS":"FAIL");
    if (!VirtualProtect(game+mutated.current,mutated.length,PAGE_READWRITE,&old)) return 2;
    game[mutated.current+5]^=1;
    if (!VirtualProtect(game+mutated.current,mutated.length,PAGE_EXECUTE_READ,&old)) return 2;
    const bool stale=checkGuards()==0;
    const bool installed=RepairFlashGuards605(gameBase,menuBase,MEM_PRIVATE)==FlashRepair::Applied;
    for (const auto& guard:kFlash605Guards) {
        auto* expected=after.data()+(guard.descriptor-pageOffset);
        std::memcpy(expected,&guard.current,8);
        std::memcpy(expected+21,guard.relocated.data()+1,4);
    }
    MEMORY_BASIC_INFORMATION page{};
    const bool precise=VirtualQuery(recordPage,&page,sizeof(page)) && page.Protect==PAGE_READONLY &&
        std::memcmp(recordPage,after.data(),after.size())==0;
    const bool pass=stale && installed && precise && checkGuards()==2;
    printf("%s relocated descriptors match both native timing guards and restore read-only protection\n",
           pass?"PASS":"FAIL");
    VirtualFree(game,0,MEM_RELEASE);
    VirtualFree(menu,0,MEM_RELEASE);
    return rejected && pass?0:1;
}
