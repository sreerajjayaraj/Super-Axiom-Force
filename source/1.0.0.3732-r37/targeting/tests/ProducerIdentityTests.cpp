#include "../src/contracts/OwnedPatchClient.h"
#include <cstdio>

static bool ReadImage(void*, uintptr_t at, void* out, size_t size) {
    SIZE_T copied=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),out,size,&copied) && copied==size;
}

int wmain(int argc,wchar_t** argv) {
    if (argc!=2) return 2;
    HMODULE saf=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
    if (!saf) return 2;

    safcompat::Client client;
    bool passed=client.Resolve(ReadImage,nullptr) && client.Present() &&
        client.Owner()==reinterpret_cast<uintptr_t>(saf);
    safcompat::Lease lease;
    safcompat::View view;
    passed=passed && client.Open(ReadImage,nullptr,0x140000000ULL,lease,view)==safcompat::Result::Valid &&
        lease.state.status==0;
    std::printf("%s exact producer image, export layout and dormant lease (entry point never executed)\n",
                passed?"PASS":"FAIL");
    return passed?0:1;
}
