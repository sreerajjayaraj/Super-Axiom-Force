#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace clock_acceleration {
enum class Kind { Reel, Propell };
inline constexpr uint8_t kReelAdd[]={0xC4,0xC1,0x72,0x58,0x84,0x0E,0x20,0x0B,0,0};
inline constexpr uint8_t kPropellAdd[]={0xC5,0xF2,0x58,0x84,0x0F,0x20,0x0B,0,0};
// A near-complete native clock gives the native movement block one processed
// frame. The next native elapsed-add reaches completion without bypassing any
// acquisition, target-validity or completion branch.
inline std::vector<uint8_t> Emit(Kind kind, const uint8_t* original, size_t length,
                                 uintptr_t resume, uintptr_t keepTravelClock=0) {
    (void)resume; // CALL occupies the final5 bytes of the stolen span.
    std::vector<uint8_t> b;
    std::vector<size_t> fallback;
    auto bytes = [&](std::initializer_list<uint8_t> x) { b.insert(b.end(), x); };
    auto u32 = [&](uint32_t x) { const auto* p = reinterpret_cast<const uint8_t*>(&x); b.insert(b.end(), p, p+4); };
    auto branch = [&](uint8_t condition) { bytes({0x0F,condition}); fallback.push_back(b.size()); u32(0); };
    auto epilog = [&]() {
        // Restore OF then the five LAHF flags without changingRSP. The final
        // POP/POP/POP/POP/JMP sequence is a standard Windows unwind epilog.
        bytes({0x4C,0x8B,0xE0,0x49,0x8B,0xC5,0x04,0x7F,0x9E,0x49,0x8B,0xC4});
        bytes({0x41,0x5F,0x41,0x5E,0x41,0x5D,0x41,0x5C});
    };
    auto jump = [&]() { bytes({0xC3}); };
    // Registered unwind metadata describes four nonvolatile pushes. Flags are
    // kept inR13 viaLAHF/SETO; no transient stack slot or game call is needed.
    bytes({0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57});
    bytes({0x4C,0x8B,0xE0,0x9F,0x0F,0x90,0xC0,0x4C,0x8B,0xE8,0x49,0x8B,0xC4});
    if (kind==Kind::Propell && keepTravelClock) {
        bytes({0x49,0xBC}); // MOV R12, policy slot
        for (unsigned i=0;i<sizeof(keepTravelClock);++i)
            b.push_back(static_cast<uint8_t>(keepTravelClock>>(8*i)));
        bytes({0x41,0x83,0x3C,0x24,0x00}); // CMP DWORD PTR [R12],0
        branch(0x85);
    }
    // Acceleration is eligible only when the exact objects used by the native
    // movement block are live. An invalid acquisition frame stays native and
    // cannot consume the adapter's first valid movement phase.
    for(uint32_t member:{uint32_t(0x9C0),uint32_t(0x9B8)}) {
        if(kind==Kind::Reel) bytes({0x4E,0x8B,0xA4,0x31});
        else bytes({0x4C,0x8B,0xA4,0x0F});
        u32(member); bytes({0x4D,0x85,0xE4}); branch(0x84);
        bytes({0x4D,0x8B,0x64,0x24,0x08,0x4D,0x85,0xE4}); branch(0x84);
        bytes({0x41,0x80,0x7C,0x24,0x15,0}); branch(0x85);
    }
    if(kind==Kind::Propell) { bytes({0x80,0xBC,0x0F});u32(0xB28);bytes({0});branch(0x85); }
    if(kind==Kind::Reel) {
        bytes({0x46,0x8B,0xA4,0x31}); u32(0xB20); // R12D=old elapsed [RCX+R14+B20]
        bytes({0x46,0x8B,0xB4,0x31}); u32(0xB24);
        bytes({0xC4,0xC1,0x79,0x7E,0xCF}); // R15D=dt bits fromXMM1
    } else {
        bytes({0x44,0x8B,0xA4,0x0F}); u32(0xB20); // [RCX+RDI+B20], dedicated65B170 owner
        bytes({0x44,0x8B,0xB4,0x0F}); u32(0xB24);
        bytes({0xC4,0xC1,0x79,0x7E,0xCF}); // R15D=dt bits fromXMM1
    }
    bytes({0x41,0x81,0xFE}); u32(0x00800000); branch(0x86); // predecessor must also be normal (DAZ/FTZ safe)
    bytes({0x41,0x81,0xFE}); u32(0x7F800000); branch(0x83); // finite duration
    bytes({0x45,0x85,0xFF}); branch(0x84); // positive dt
    bytes({0x41,0x81,0xFF}); u32(0x00800000); branch(0x82); // subnormal dt stays native
    bytes({0x41,0x81,0xFF}); u32(0x7F800000); branch(0x83); // finite positive dt
    bytes({0x41,0xFF,0xCE}); // R14D=predecessor(duration)
    bytes({0x41,0x81,0xFC});u32(0x7F800000);branch(0x83); // old must be finite nonnegative
    bytes({0x45,0x3B,0xE6}); branch(0x83); // already near-complete/expired: preserve native next update
    // Conservatively require dt >= one predecessorULP. Then a normal next
    // update can advance the near-complete clock rather than rounding forever.
    bytes({0x45,0x8B,0xE6,0x41,0xC1,0xEC,0x17,0x41,0x83,0xFC,0x18});
    bytes({0x72,0x11}); // tiny duration: any normal dt exceeds its predecessorULP
    bytes({0x41,0x83,0xEC,0x17,0x41,0xC1,0xE4,0x17,0x45,0x3B,0xFC}); branch(0x82);
    bytes({0xC4,0xC1,0x79,0x6E,0xC6,0xC5,0xF2,0x10,0xC0});
    epilog(); jump();
    const size_t destination=b.size();
    // R14 may hold duration bits; restore its native index before replaying the
    // original memoryADD, while the full registered stack frame remains live.
    bytes({0x4C,0x8B,0x74,0x24,0x08});
    b.insert(b.end(),original,original+length); epilog(); jump();
    for(auto field:fallback) { const int32_t relative=static_cast<int32_t>(destination-field-4); std::memcpy(b.data()+field,&relative,4); }
    return b;
}
inline constexpr uint8_t kUnwind[] = {
    1,8,4,0, 8,0xF0, 6,0xE0, 4,0xD0, 2,0xC0
};
} // namespace clock_acceleration
