#pragma once

#include <Windows.h>
#include <safetyhook.hpp>
#include <memory>

namespace game_hooks {

// Seed SafetyHook's private allocator from the edges of the main image. This
// keeps its short detours and their relocated native operands inside rel32
// range even if the library's first search from an interior code site fails.
inline std::shared_ptr<safetyhook::Allocator> CreateAllocator(uintptr_t base,size_t size) {
    constexpr size_t reach=0x10000000;
    constexpr size_t rel32Margin=0x60000000;
    if (!base || !size || size>=rel32Margin || size>UINTPTR_MAX-base) return {};
    auto allocator=safetyhook::Allocator::create();
    for (const auto anchor:{base+size,base}) {
        auto slot=allocator->allocate_near({reinterpret_cast<uint8_t*>(anchor)},4096,reach);
        if (!slot) continue;
        const auto address=slot->address();
        const auto end=base+size;
        const auto startDistance=address>=base?address-base:base-address;
        const auto endDistance=address>=end?address-end:end-address;
        if (startDistance>rel32Margin || endDistance>rel32Margin) continue;
        slot->free();
        return allocator;
    }
    return {};
}

inline std::shared_ptr<safetyhook::Allocator> AllocatorFor(uintptr_t base,size_t size) {
    // A fixture may bind a different inert image later in the same process.
    // Installed hooks hold their own shared allocation references.
    static uintptr_t boundBase=0;
    static size_t boundSize=0;
    static std::shared_ptr<safetyhook::Allocator> allocation;
    if (!allocation || base!=boundBase || size!=boundSize) {
        allocation=CreateAllocator(base,size);
        boundBase=base;
        boundSize=size;
    }
    return allocation;
}

}

