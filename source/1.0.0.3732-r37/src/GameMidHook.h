#pragma once

#include <Windows.h>
#include <safetyhook.hpp>
#include <cstring>
#include <memory>
#include <span>
#include "GameHookAllocator.h"

namespace saf_hooks {

struct Image {
    uintptr_t base = 0;
    size_t size = 0;
#ifdef SAF_HOOK_FIXTURE
    DWORD type = MEM_PRIVATE;
#else
    static constexpr DWORD type = MEM_IMAGE;
#endif
};

inline bool Executable(const Image& image, uintptr_t address, size_t length) {
    if (!image.base || !length || address < image.base || address - image.base >= image.size ||
        length > image.size - (address - image.base)) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &memory, sizeof(memory)) ||
        memory.State != MEM_COMMIT || memory.Type != image.type ||
        reinterpret_cast<uintptr_t>(memory.AllocationBase) != image.base ||
        (memory.Protect != PAGE_EXECUTE_READ && memory.Protect != PAGE_EXECUTE_WRITECOPY)) return false;
    const auto offset = address - reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return offset < memory.RegionSize && length <= memory.RegionSize - offset;
}

struct PreparationFailure {
    const char* stage=nullptr;
    MEMORY_BASIC_INFORMATION memory{};
    unsigned libraryType=0;
    unsigned libraryDetail=0;
};

inline std::unique_ptr<safetyhook::MidHook> Prepare(const Image& image, uintptr_t address,
    std::span<const uint8_t> original, safetyhook::MidHookFn callback,PreparationFailure* failure=nullptr) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    if (!Executable(image,address,original.size())) {
        if (failure) {
            failure->stage="image-page";
            VirtualQuery(bytes,&failure->memory,sizeof(failure->memory));
        }
        return {};
    }
    if (std::memcmp(bytes,original.data(),original.size())) {
        if (failure) failure->stage="native-bytes";
        return {};
    }
    const auto allocator=game_hooks::AllocatorFor(image.base,image.size);
    if (!allocator) {
        if (failure) failure->stage="near-game-pool";
        return {};
    }
    auto hook=safetyhook::MidHook::create(allocator,address,callback,safetyhook::MidHook::StartDisabled);
    if (!hook) {
        if (failure) {
            const auto& error=hook.error();
            failure->stage="safetyhook-create";
            failure->libraryType=error.type;
            if (error.type==safetyhook::MidHook::Error::BAD_INLINE_HOOK)
                failure->libraryDetail=error.inline_hook_error.type;
            else failure->libraryDetail=static_cast<unsigned>(error.allocator_error);
        }
        return {};
    }
    if (hook->target_address()!=address || hook->original_bytes().size()!=original.size() ||
        std::memcmp(hook->original_bytes().data(),original.data(),original.size())) {
        if (failure) {
            failure->stage="stolen-instruction-span";
            failure->libraryDetail=static_cast<unsigned>(hook->original_bytes().size());
        }
        return {};
    }
    return std::make_unique<safetyhook::MidHook>(std::move(*hook));
}

inline bool Enable(const Image& image, safetyhook::MidHook& hook) {
    const auto& bytes = hook.original_bytes();
    if (!Executable(image, hook.target_address(), bytes.size()) ||
        std::memcmp(hook.target(), bytes.data(), bytes.size()) || !hook.enable()) return false;
    // The library's near branch points to its trampoline's FF25 stub entry.
    // Validate both steps before permitting the feature to use this hook.
    int32_t displacement=0;
    if (hook.target()[0]!=0xE9) return false;
    std::memcpy(&displacement,hook.target()+1,sizeof(displacement));
    const auto entry=hook.target_address()+5+displacement;
    const auto trampoline=hook.owned_trampoline_address();
    if (entry<trampoline || entry-trampoline>64) return false;
    constexpr uint8_t indirectJump[]={0xFF,0x25,0x00,0x00,0x00,0x00};
    const auto* detour=reinterpret_cast<const uint8_t*>(entry);
    uintptr_t destination=0;
    if (std::memcmp(detour,indirectJump,sizeof(indirectJump))) return false;
    std::memcpy(&destination,detour+sizeof(indirectJump),sizeof(destination));
    return destination==hook.owned_stub_address();
}

}
