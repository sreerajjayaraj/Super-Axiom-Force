#include <Windows.h>
#include "../src/SuperAxiomForce/OwnedPatchProtocol.h"
#include <filesystem>
#include <cstdio>

int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    const auto file=std::filesystem::absolute(argv[1]);
    const auto log=file.parent_path()/L"SuperAxiomForce.log";
    if (std::filesystem::exists(log)) return 2;
    HMODULE saf=LoadLibraryW(file.c_str());
    if (!saf) {
        printf("LoadLibrary error=%lu\n",GetLastError());
        return 1;
    }
    const auto address=reinterpret_cast<uintptr_t>(GetProcAddress(saf,"SAFOwnedPatchStateV1"));
    safcompat::Lease state;
    Sleep(250);
    const bool dormant=address && state.Acquire(reinterpret_cast<safcompat::Descriptor*>(address))==safcompat::Result::Valid &&
        state.state.status==static_cast<uint32_t>(safcompat::Status::Native) && !state.state.support &&
        !std::filesystem::exists(log);
    state.Release();
    const bool released=FreeLibrary(saf)!=0;
    printf("%s exported ABI loads through Windows and the ASI stays dormant in a foreign process\n",
           dormant&&released?"PASS":"FAIL");
    return dormant&&released?0:1;
}
