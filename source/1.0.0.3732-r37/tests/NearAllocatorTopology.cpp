#include <Windows.h>
#include <safetyhook.hpp>
#include <cstdio>

int main() {
    void* mainReservation=nullptr;
    void* lowerReservation=nullptr;
    uintptr_t base=0;
    for (uintptr_t region=0x180000000ULL;region<0x1C0000000ULL;region+=0x1000000) {
        auto* below=VirtualAlloc(reinterpret_cast<void*>(region-0x10000),0x10000,MEM_RESERVE,PAGE_NOACCESS);
        if (!below) continue;
        auto* center=VirtualAlloc(reinterpret_cast<void*>(region),0x29000,MEM_RESERVE,PAGE_NOACCESS);
        if (center) {
            base=region;
            lowerReservation=below;
            mainReservation=center;
            break;
        }
        VirtualFree(below,0,MEM_RELEASE);
    }
    if (!base) return 2;
    const auto allocator=safetyhook::Allocator::create();
    auto result=allocator->allocate_near({reinterpret_cast<uint8_t*>(base+0x18000)},4096,0x2A000);
    const auto received=result?result->address():0;
    const bool matches=received==base+0x30000;
    std::printf("%s free gap reached at 0x%llX, expected 0x%llX\n",matches?"PASS":"FAIL",
                static_cast<unsigned long long>(received),static_cast<unsigned long long>(base+0x30000));
    if (result) result->free();
    VirtualFree(mainReservation,0,MEM_RELEASE);
    VirtualFree(lowerReservation,0,MEM_RELEASE);
    return matches?0:1;
}

