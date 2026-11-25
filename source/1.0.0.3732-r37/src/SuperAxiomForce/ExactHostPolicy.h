#pragma once
#include <cstdint>
#include <cstddef>
namespace saf_host {
// Bounded, allocation-free game-family gate. The delayed resolver supplies the
// independent native identity before any game code or data may be changed.
template<class Read> inline bool CompatibleImage(uintptr_t base,const Read& read) noexcept {
    if (base < 0x10000 || (base & 0xFFFF) || base > UINTPTR_MAX - 0x2000) return false;
    uint16_t mz = 0, machine = 0, sections = 0, optional = 0, coff = 0, magic = 0, subsystem = 0;
    uint32_t pe = 0, signature = 0, size = 0, alignment = 0, entry = 0;
    uint32_t directories = 0, exceptionRva = 0, exceptionSize = 0;
    uint64_t preferred = 0;
    return read(base, &mz, sizeof(mz)) && mz == 0x5A4D &&
        read(base + 0x3C, &pe, sizeof(pe)) && pe >= 0x40 && pe <= 0x1000 &&
        read(base + pe, &signature, sizeof(signature)) && signature == 0x4550 &&
        read(base + pe + 4, &machine, sizeof(machine)) && machine == 0x8664 &&
        read(base + pe + 6, &sections, sizeof(sections)) && sections >= 6 && sections <= 32 &&
        read(base + pe + 20, &optional, sizeof(optional)) && optional >= 0xF0 &&
        read(base + pe + 22, &coff, sizeof(coff)) && (coff & 0x2002) == 0x0002 &&
        read(base + pe + 24, &magic, sizeof(magic)) && magic == 0x20B &&
        read(base + pe + 40, &entry, sizeof(entry)) && entry != 0 &&
        read(base + pe + 48, &preferred, sizeof(preferred)) && preferred == 0x140000000ULL &&
        read(base + pe + 56, &alignment, sizeof(alignment)) && alignment == 0x1000 &&
        read(base + pe + 80, &size, sizeof(size)) && size >= 0x8000000 && size <= 0x30000000 &&
        (size & 0xFFF) == 0 && entry < size && size <= UINTPTR_MAX - base &&
        read(base + pe + 92, &subsystem, sizeof(subsystem)) && subsystem == 2 &&
        read(base + pe + 132, &directories, sizeof(directories)) && directories >= 4 &&
        read(base + pe + 160, &exceptionRva, sizeof(exceptionRva)) && exceptionRva != 0 && exceptionRva < size &&
        read(base + pe + 164, &exceptionSize, sizeof(exceptionSize)) &&
        exceptionSize >= 12 * 100000 && exceptionSize % 12 == 0 && exceptionSize <= size - exceptionRva;
}
}
