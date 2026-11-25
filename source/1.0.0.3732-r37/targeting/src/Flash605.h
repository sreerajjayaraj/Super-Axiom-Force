// Included inside the bridge namespace after the native span helpers.
enum class FlashRepair { Applied, Refused, Retry };

struct FlashGuard605 {
    uintptr_t descriptor;
    uintptr_t before;
    uintptr_t current;
    uintptr_t label;
    uint32_t length;
    std::array<uint8_t,20> original;
    std::array<uint8_t,20> relocated;
};

constexpr std::array<FlashGuard605,2> kFlash605Guards{{
    {0x6899A8,0xBC44B2,0xBFE122,0x68B988,20,
     {0xE8,0x29,0xF9,0x9E,0xFF,0x84,0xC0,0x74,0x0B,0xC5,0xCA,0x59,0xC7,0xC4,0xC1,0x7A,0x59,0xC8,0xEB,0x05},
     {0xE8,0x29,0x2B,0x9E,0xFF,0x84,0xC0,0x74,0x0B,0xC5,0xCA,0x59,0xC7,0xC4,0xC1,0x7A,0x59,0xC8,0xEB,0x05}},
    {0x689A00,0xBC4C66,0xBFE8D6,0x68B9C0,11,
     {0xE8,0x75,0xF1,0x9E,0xFF,0x88,0x85,0xE0,0x00,0x00,0x00},
     {0xE8,0x75,0x23,0x9E,0xFF,0x88,0x85,0xE0,0x00,0x00,0x00}},
}};
constexpr std::array<uint8_t,17> kFlashPredicate{
    0x48,0x8B,0x81,0x88,0x00,0x00,0x00,0x44,0x0F,0xB6,0x40,0x01,0x41,0x8D,0x40,0xFD,0x83,
};

FlashRepair RepairFlashGuards605(uintptr_t game,uintptr_t menu,DWORD imageType=MEM_IMAGE) {
    constexpr uintptr_t predicate=0x5E0C50;
    constexpr size_t recordHeader=20;
    std::array<quiesced_patch::Patch,4> patches{};
    std::array<quiesced_patch::Check,5> checks{};
    std::array<std::array<uint8_t,40>,2> records{};
    const auto readOnly=[&](uintptr_t at,size_t size,uintptr_t owner,DWORD protection){
        const DWORD copy=protection==PAGE_READONLY?PAGE_WRITECOPY:PAGE_EXECUTE_WRITECOPY;
        return safcompat::Region(at,size,owner,imageType,protection) ||
               safcompat::Region(at,size,owner,imageType,copy);
    };
    if (!readOnly(game+predicate,kFlashPredicate.size(),game,PAGE_EXECUTE_READ) ||
        !quiesced_patch::SameBytes(reinterpret_cast<const uint8_t*>(game+predicate),
                                  kFlashPredicate.data(),kFlashPredicate.size())) return FlashRepair::Refused;
    checks.back()={reinterpret_cast<const uint8_t*>(game+predicate),kFlashPredicate.data(),kFlashPredicate.size()};
    for (size_t i=0;i<kFlash605Guards.size();++i) {
        const auto& guard=kFlash605Guards[i];
        const auto record=menu+guard.descriptor;
        const auto target=game+guard.current;
        int32_t displacement=0;
        std::memcpy(&displacement,guard.relocated.data()+1,sizeof(displacement));
        if (static_cast<int64_t>(guard.current)+5+displacement!=static_cast<int64_t>(predicate) ||
            !readOnly(record,recordHeader+guard.length,menu,PAGE_READONLY) ||
            !readOnly(target,guard.length,game,PAGE_EXECUTE_READ) ||
            !quiesced_patch::SameBytes(reinterpret_cast<const uint8_t*>(target),
                                      guard.relocated.data(),guard.length)) return FlashRepair::Refused;

        const auto nativeLabel=menu+guard.label;
        auto& before=records[i];
        std::memcpy(before.data(),&guard.before,8);
        std::memcpy(before.data()+8,&nativeLabel,8);
        std::memcpy(before.data()+16,&guard.length,4);
        std::memcpy(before.data()+recordHeader,guard.original.data(),guard.length);
        if (!quiesced_patch::SameBytes(reinterpret_cast<const uint8_t*>(record),before.data(),
                                      recordHeader+guard.length)) return FlashRepair::Refused;
        checks[2*i]={reinterpret_cast<const uint8_t*>(record),before.data(),recordHeader+guard.length};
        checks[2*i+1]={reinterpret_cast<const uint8_t*>(target),guard.relocated.data(),guard.length};

        auto& addressPatch=patches[2*i];
        addressPatch.address=reinterpret_cast<uint8_t*>(record);
        addressPatch.size=8;
        std::memcpy(addressPatch.before,&guard.before,8);
        std::memcpy(addressPatch.after,&guard.current,8);
        auto& callPatch=patches[2*i+1];
        callPatch.address=reinterpret_cast<uint8_t*>(record+recordHeader+1);
        callPatch.size=4;
        std::memcpy(callPatch.before,guard.original.data()+1,4);
        std::memcpy(callPatch.after,guard.relocated.data()+1,4);
    }
    quiesced_patch::Report result{};
    if (quiesced_patch::Write(patches.data(),patches.size(),{},&result,checks.data(),checks.size()))
        return FlashRepair::Applied;
    return quiesced_patch::Recoverable(result)?FlashRepair::Retry:FlashRepair::Refused;
}

void TryFlash605(HMODULE menu) {
    constexpr std::array<uint8_t,32> menuHash{
        0x21,0xEB,0xE7,0x92,0x0D,0x19,0xDA,0xA6,0x7A,0x9A,0xD9,0xE5,0x53,0x2B,0x03,0xD8,
        0x11,0x5D,0x0B,0x97,0x36,0x7F,0x6E,0xBD,0x06,0x2C,0x97,0x16,0xE6,0x3A,0xA6,0xED,
    };
    if (!menu || !HashModule(menu,menuHash)) return;
    auto result=FlashRepair::Retry;
    for (unsigned attempt=0;attempt<3 && result==FlashRepair::Retry;++attempt) {
        result=RepairFlashGuards605(g.game,reinterpret_cast<uintptr_t>(menu));
        if (result==FlashRepair::Retry) Sleep(10);
    }
    Log(result==FlashRepair::Applied?
        "Flash Mode 6.0.5: relocated both guard records to native BFE122/BFE8D6; Menu retains its timing checks.":
        "Flash Mode 6.0.5: relocation refused; guard or native bytes changed or thread transaction unavailable.");
}
