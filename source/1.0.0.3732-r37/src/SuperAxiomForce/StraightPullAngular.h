#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
namespace straight_pull_angular {
inline constexpr uint32_t kSiteRva=0x137CDF5,kResumeRva=0x137CE04,kHelperRva=0x137BA70;
inline constexpr uint32_t kSelectedReturnRvas[]={0x137EB03,0x137EBE3};
inline constexpr size_t kCallerReturnOffset=0x1D0;
inline constexpr uint8_t kOriginal[]={0xC4,0xC1,0x22,0x59,0xDE,0xC4,0xC1,0x32,0x59,0xD6,0xC4,0xC1,0x2A,0x59,0xCE};
inline constexpr size_t kCodeSize=78,kDataOffset=80,kBufferSize=112;
inline constexpr uint8_t kUnwind[]={1,0,0,0};
inline constexpr uint8_t kHelperPrologue[]={0x48,0x8B,0xC4,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0xA8,0x38,0xFF,0xFF,0xFF,0x48,0x81,0xEC,0x88,0x01,0,0};
inline constexpr uint8_t kFirstArguments[]={0x4C,0x89,0x74,0x24,0x30,0x4C,0x89,0x7C,0x24,0x28,0x4C,0x89,0x64,0x24,0x20,0x4C,0x8D,0x4F,0x50,0x4C,0x8D,0x87,0xA0,0,0,0,0x48,0x8B,0xD7,0xC4,0xC1,0x78,0x28,0xC0};
inline constexpr uint8_t kSecondArguments[]={0x48,0x89,0x74,0x24,0x30,0x4C,0x89,0x74,0x24,0x28,0x4C,0x89,0x7C,0x24,0x20,0x4C,0x8D,0x4F,0x50,0x4C,0x8D,0x87,0xA0,0,0,0,0x48,0x8B,0xD7,0xC4,0xC1,0x78,0x28,0xC0};
inline constexpr uint8_t kQuaternionCall[]={0x48,0x8D,0x4C,0x24,0x40,0xE8,0x42,0x72,0x06,0xFF};
inline bool ValidateNative(const uint8_t* image,size_t imageSize) {
    if(!image || imageSize<0x13801F3)return false;
    if(std::memcmp(image+kSiteRva,kOriginal,sizeof(kOriginal)) ||
       std::memcmp(image+kHelperRva,kHelperPrologue,sizeof(kHelperPrologue)) ||
       std::memcmp(image+0x137EADC,kFirstArguments,sizeof(kFirstArguments)) ||
       std::memcmp(image+0x137EBBC,kSecondArguments,sizeof(kSecondArguments)) ||
       std::memcmp(image+kResumeRva,kQuaternionCall,sizeof(kQuaternionCall)))return false;
    for(uint32_t ret:{kSelectedReturnRvas[0],kSelectedReturnRvas[1],uint32_t(0x13801F3)}) {
        int32_t delta=0;std::memcpy(&delta,image+ret-4,4);
        if(image[ret-5]!=0xE8 || static_cast<int64_t>(ret)+delta!=kHelperRva)return false;
    }
    return true;
}
// Replace the original15-byte span by10NOP + CALL rel32 at its end. The
// adapter is a true leaf CALL target; RET resumes at native137B384. Its only
// memory read from the native frame is the helper's original return address:
// eight nonvolatile pushes+188 allocation+our CALL8 => RSP+1D0.
// XMM0/XMM4 are dead scalar scratch at this exact native boundary: the original
// identity quaternion was already stored at[rsp+40], and the immediately
// following3E3770 takes only RCX/XMM1/XMM2/XMM3. No GPR/RFLAGS/RSP is changed.
// XMM1/2/3 retain native upper lanes; only the low angular-increment DWORD is
// zeroed for the ordinary pull step. The second selected return is inside
// the native obstacle-correction loop and must retain native rotation.
inline bool EmitSelected(uint8_t* out, size_t capacity, const std::array<uintptr_t, 2>& selectedReturns, size_t* size) {
    if(size)*size=0;
    if (!out || capacity < kBufferSize || selectedReturns[0] < 0x10000 ||
        selectedReturns[0] >= selectedReturns[1] || selectedReturns[1] >= 0x0000800000000000ULL) return false;
    std::array<uint8_t,kBufferSize> code{};
    size_t at=0;
    auto bytes=[&](std::initializer_list<uint8_t> values){for(auto v:values)code[at++]=v;};
    auto u32=[&](uint32_t value){std::memcpy(code.data()+at,&value,4);at+=4;};
    bytes({0xC5,0xFA,0x7E,0x84,0x24});u32(static_cast<uint32_t>(kCallerReturnOffset)); // VMOVQ XMM0,[RSP+1D0]
    bytes({0xC4,0xE2,0x79,0x29,0x05});const size_t first=at;u32(0); // VPCMPEQQ XMM0,XMM0,[return1]
    bytes({0xC5,0xFA,0x7E,0xA4,0x24});u32(static_cast<uint32_t>(kCallerReturnOffset));
    bytes({0xC4,0xE2,0x59,0x29,0x25});const size_t second=at;u32(0); // XMM4 versus return2
    bytes({0xC5,0xF9,0xEB,0xC4}); // VPOR XMM0,XMM0,XMM4
    bytes({0xC5,0xF9,0x73,0xF8,0x0C,0xC5,0xF9,0x73,0xD8,0x0C}); // retain only low DWORD mask
    for(auto v:kOriginal)code[at++]=v;
    bytes({0xC5,0xF8,0x55,0xC9,0xC5,0xF8,0x55,0xD2,0xC5,0xF8,0x55,0xDB}); // VANDNPS scalar mask
    bytes({0xC3});
    if(at!=kCodeSize)return false;
    const int32_t d1=static_cast<int32_t>(kDataOffset-first-4),d2=static_cast<int32_t>(kDataOffset+16-second-4);
    std::memcpy(code.data()+first,&d1,4);std::memcpy(code.data()+second,&d2,4);
    std::memcpy(code.data() + kDataOffset, &selectedReturns[0], sizeof(uintptr_t));
    // Preserve the leaf relay layout. A noncanonical return can never select
    // the second mask, including when the game is rebased.
    constexpr uintptr_t nativeCorrection=UINT64_MAX;
    std::memcpy(code.data() + kDataOffset + 16, &nativeCorrection, sizeof(nativeCorrection));
    std::memcpy(out,code.data(),code.size());if(size)*size=code.size();return true;
}
inline bool Emit(uint8_t* out, size_t capacity, uintptr_t gameBase, uintptr_t, size_t* size) {
    if (size) *size = 0;
    if (gameBase < 0x10000 || gameBase >= 0x0000800000000000ULL - kSelectedReturnRvas[1]) return false;
    return EmitSelected(out, capacity, {gameBase + kSelectedReturnRvas[0], gameBase + kSelectedReturnRvas[1]}, size);
}
}
