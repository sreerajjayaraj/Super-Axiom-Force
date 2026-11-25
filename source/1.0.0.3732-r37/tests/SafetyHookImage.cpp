#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"

int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    const auto file=CreateFileW(argv[1],GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                                nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) return 2;
    const auto mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY|SEC_IMAGE,0,0,nullptr);
    CloseHandle(file);
    if (!mapping) return 2;
    // The file is mapped as image data. No thread invokes an archived address.
    const auto* image=static_cast<const uint8_t*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
    CloseHandle(mapping);
    if (!image) {
        std::printf("Cannot map inert image: %lu\n",GetLastError());
        return 2;
    }
    const auto& dos=*reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>4096) return 2;
    const auto& nt=*reinterpret_cast<const IMAGE_NT_HEADERS64*>(image+dos.e_lfanew);
    const auto base=reinterpret_cast<uintptr_t>(image);
    const auto imageSize=nt.OptionalHeader.SizeOfImage;
    if (nt.Signature!=IMAGE_NT_SIGNATURE || !imageSize) return 2;
    native::Image game{image,imageSize,{},{},{},{}};
    const auto* sections=IMAGE_FIRST_SECTION(&nt);
    for (unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        const auto& section=sections[i];
        const native::Range span{section.VirtualAddress,section.Misc.VirtualSize};
        if (!span.size || span.offset>=game.size || span.size>game.size-span.offset) continue;
        if (section.Characteristics&IMAGE_SCN_MEM_READ) game.readable.push_back(span);
        if (section.Characteristics&IMAGE_SCN_MEM_EXECUTE) game.code.push_back(span);
        else if (section.Characteristics&IMAGE_SCN_MEM_WRITE) game.writable.push_back(span);
    }
    const auto& exception=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    game.exceptions={exception.VirtualAddress,exception.Size};
    g_gameBase=base;
    g_gameSize=imageSize;
    g_image=game;
    g_logPath="SafetyHookImage.fixture.log";
    saf_targeting::PrepareMappedImage([](const char* message){ std::puts(message); },base,imageSize,nt.OptionalHeader.ImageBase);
    auto plan=saf_adaptive::Resolve(game,base);
    if (plan.refusal) {
        std::printf("FAIL core native image binding %s\n",plan.refusal);
        return 1;
    }
    BindFeatureSites(plan.bindings,plan.angularSite,plan.selectedReturns);
    g_retryContractReady=kAllFeatureBits;
    for (auto& site:plan.witnesses)
        g_retryContracts.push_back({image+site.rva,std::move(site.bytes),site.mask});
    g_adaptiveWitnessReady=true;
    std::printf("Image clock_prepared=%u angular_prepared=%u page_type=MEM_IMAGE\n",
                g_clockSupportReady,g_angularReady);
    ApplyConfig config{};
    const bool allCore=ApplyBatch(config,kAllFeatureBits,0,false);
    ServiceUnifiedTargeting();
    const bool targeting=saf_targeting::MappedImageFeaturesReady();
    std::printf("Image all_core=%u targeting_and_obstacles=%u archived_code_executed=0\n",
                allCore,targeting);
    return allCore && targeting ? 0 : 1;
}

