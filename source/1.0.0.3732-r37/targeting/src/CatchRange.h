#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "contracts/OwnedPatchClient.h"
#include "contracts/QuiescedPatch.h"

namespace axiom {
class CatchRange {
    using Bytes=std::array<uint8_t,8>;
    struct Input {
        uintptr_t rva;
        Bytes native;
    };
    // Case 5 verification, RemoteCatchFunctions query and the two
    // pull-distance clamps feeding the same traversal helper.
    inline static constexpr std::array<Input,4> kInputs{{
        {0x67BD5F,{0xC5,0x7A,0x10,0x05,0x31,0xFA,0xAE,0x06}},
        {0x139662A,{0xC5,0xFA,0x10,0x35,0xB6,0x51,0xDD,0x05}},
        {0x138DCE3,{0xC5,0xFA,0x5D,0x0D,0xAD,0xDA,0xDD,0x05}},
        {0x1391FD3,{0xC5,0xFA,0x5D,0x0D,0xBD,0x97,0xDD,0x05}},
    }};
    inline static constexpr std::array<uint8_t,16> kVerifyDistance{
        0xC5,0xBA,0x58,0x05,0x14,0x30,0xB4,0x05,
        0xC5,0xFA,0x11,0x85,0xB0,0x00,0x00,0x00};
    inline static constexpr std::array<uint8_t,40> kQueryEndpoint{
        0xC5,0xCA,0x59,0x40,0x08,0xC4,0xC1,0x7A,0x58,0x7D,0x08,
        0xC5,0xCA,0x59,0x40,0x04,0xC4,0x41,0x7A,0x58,0x45,0x04,
        0xC5,0xCA,0x59,0x00,0xC4,0xC1,0x7A,0x58,0x75,0x00,
        0xC5,0xFA,0x11,0xB5,0xF4,0x00,0x00,0x00};
    inline static constexpr std::array<uint8_t,16> kPullClampA{
        0xC5,0xFA,0x10,0x05,0xA9,0x10,0xE3,0x04,0xC5,0xFA,0x5F,0xF1,0xC5,0xFA,0x10,0x4F};
    inline static constexpr std::array<uint8_t,16> kPullClampB{
        0xC5,0xFA,0x10,0x05,0xB9,0xCD,0xE2,0x04,0xC5,0xFA,0x5F,0xF1,0xC5,0xFA,0x10,0x4B};
    std::array<Input,kInputs.size()> inputs_=kInputs;
    std::array<quiesced_patch::Check,4> resolvedChecks_{};
    bool configured_=false;
    std::array<Bytes,kInputs.size()> owned_{};
    uintptr_t support_=0;
    bool fault_=false;

    static bool Redirect(uintptr_t address,uintptr_t scalar,const Bytes& original,Bytes& result) {
        const auto offset=static_cast<int64_t>(scalar)-static_cast<int64_t>(address+original.size());
        if (offset<(std::numeric_limits<int32_t>::min)() ||
            offset>(std::numeric_limits<int32_t>::max)()) return false;
        const auto displacement=static_cast<int32_t>(offset);
        result=original;
        std::memcpy(result.data()+4,&displacement,sizeof(displacement));
        return true;
    }

    static quiesced_patch::Patch Change(uintptr_t address,const Bytes& from,const Bytes& to) {
        quiesced_patch::Patch patch{};
        patch.address=reinterpret_cast<uint8_t*>(address);
        patch.size=from.size();
        std::memcpy(patch.before,from.data(),from.size());
        std::memcpy(patch.after,to.data(),to.size());
        return patch;
    }

    bool OwnedBytes(uintptr_t game,safcompat::ReadFn read) const {
        for (size_t i=0;i<kInputs.size();++i) {
            if (!safcompat::Equal(read,nullptr,game+inputs_[i].rva,owned_[i].data(),owned_[i].size()))
                return false;
        }
        return true;
    }

public:
    template<class Roles> bool Configure(const Roles& roles,uintptr_t game) {
        if (configured_) return true;
        if (support_) return false;
        auto inputs=inputs_;
        for (size_t i=0;i<inputs.size();++i) {
            const auto role=kInputs[i].rva;
            const auto address=roles.Address(role);
            const auto bytes=roles.Witness(role,inputs[i].native.size());
            if (address<=game || bytes.empty()) return false;
            inputs[i].rva=address-game;
            std::memcpy(inputs[i].native.data(),bytes.data(),bytes.size());
        }
        constexpr std::array<uintptr_t,4> keys{0x67BF48,0x1396783,0x138DCEB,0x1391FDB};
        constexpr std::array sizes{kVerifyDistance.size(),kQueryEndpoint.size(),kPullClampA.size(),kPullClampB.size()};
        for (size_t i=0;i<keys.size();++i) {
            const auto address=roles.Address(keys[i]);
            const auto bytes=roles.Witness(keys[i],sizes[i]);
            if (address<=game || bytes.empty()) return false;
            resolvedChecks_[i]={reinterpret_cast<const uint8_t*>(address),bytes.data(),bytes.size()};
        }
        inputs_=inputs;
        configured_=true;
        return true;
    }

    bool Faulted() const { return fault_; }

    bool Restore(uintptr_t game,safcompat::ReadFn read) {
        if (!support_) return true;
        std::array<quiesced_patch::Patch,kInputs.size()> patches{};
        size_t count=0;
        for (size_t i=0;i<kInputs.size();++i) {
            const auto at=game+inputs_[i].rva;
            Bytes current{};
            if (!read(nullptr,at,current.data(),current.size())) return false;
            if (current==owned_[i])
                patches[count++]=Change(at,owned_[i],inputs_[i].native);
        }
        if (count && !quiesced_patch::Write(patches.data(),count)) return false;
        support_=0;
        return true;
    }

    bool Update(uintptr_t game,uintptr_t support,safcompat::ReadFn read) {
        if (!support) return Restore(game,read);
        if (fault_) return false;
        if (support_) {
            if (support_==support && OwnedBytes(game,read)) return true;
            fault_=true;
            return false;
        }
        std::array<quiesced_patch::Patch,kInputs.size()> patches{};
        for (size_t i=0;i<kInputs.size();++i) {
            const auto at=game+inputs_[i].rva;
            if (!Redirect(at,support+0x1000,inputs_[i].native,owned_[i])) {
                fault_=true;
                return false;
            }
            patches[i]=Change(at,inputs_[i].native,owned_[i]);
        }
        const std::array<quiesced_patch::Check,4> legacyChecks{{
            {reinterpret_cast<const uint8_t*>(game+0x67BF48),kVerifyDistance.data(),kVerifyDistance.size()},
            {reinterpret_cast<const uint8_t*>(game+0x1396783),kQueryEndpoint.data(),kQueryEndpoint.size()},
            {reinterpret_cast<const uint8_t*>(game+0x138DCEB),kPullClampA.data(),kPullClampA.size()},
            {reinterpret_cast<const uint8_t*>(game+0x1391FDB),kPullClampB.data(),kPullClampB.size()},
        }};
        const auto& checks=configured_?resolvedChecks_:legacyChecks;
        quiesced_patch::Report report{};
        if (!quiesced_patch::Write(patches.data(),patches.size(),{},&report,checks.data(),checks.size())) {
            if (!report.unchanged) support_=support;
            fault_=!quiesced_patch::Recoverable(report);
            return false;
        }
        support_=support;
        return true;
    }
};
}
