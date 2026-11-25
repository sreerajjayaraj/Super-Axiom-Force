
#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "DistanceOwner.h"
#include "Profiles.generated.h"
#include "Menu601.generated.h"
#include "CatchRange.h"
#include "TargetTrace.h"
#include "StalledTether.h"
#include "contracts/NearAllocation.h"
#include "contracts/OwnedPatchClient.h"
#include "contracts/QuiescedPatch.h"

namespace {
using namespace axiom::contract;

constexpr uintptr_t kDistance=0x716B838;
struct MenuWitness {
    uintptr_t rva;
    const uint8_t* bytes;
    size_t size;
};

template<size_t N>
constexpr MenuWitness Witness(uintptr_t rva,const std::array<uint8_t,N>& bytes) {
    return {rva,bytes.data(),N};
}

struct MenuProfile {
    uint32_t stamp, imageSize;
    const char* name;
    const std::array<uint8_t,32>& hash;
    uintptr_t vtable, oldProbe, backendStarted, oldWorker, menuStage, status;
    uintptr_t enabled, range, unlimited, registryCount, registry;
    MenuWitness init, start, shutdown, readSettings, readFlag, registryInit, publishSettings;
    MenuWitness toggle, draw, probe, getter, stockWorker, setEnabled, initRow, startRow;
};

constexpr uintptr_t kVtable=0x410778;
constexpr uintptr_t kOldProbeCalled=0x6D0EF8;
constexpr uintptr_t kBackendStarted=0x6D0EF9;
constexpr uintptr_t kOldWorkerStarted=0x6D0EEC;
constexpr uintptr_t kMenuStage=0x644C38;
constexpr uintptr_t kStatus=0x6D0EE4;
constexpr uintptr_t kEnabled=0x6D0EE8;
constexpr uintptr_t kRange=0x63AC30;
constexpr uintptr_t kUnlimited=0x6D0EF4;
constexpr uintptr_t kRegistryCount=0x6487B0;
constexpr uintptr_t kRegistry=0x647BB0;
constexpr MenuProfile kMenu60{
    0x1927CF17, 0xC01000, "DLMM 6.0", kMenuSha,
    kVtable, kOldProbeCalled, kBackendStarted, kOldWorkerStarted,
    kMenuStage, kStatus, kEnabled, kRange, kUnlimited,
    kRegistryCount, kRegistry,
    Witness(kInitRva,kInit), Witness(kStartRva,kStart), Witness(kShutdownRva,kShutdown),
    Witness(kReadSettingsRva,kReadSettings), Witness(kReadFlagRva,kReadFlag), Witness(kMenuRegistryInitRva,kMenuRegistryInit),
    Witness(kPublishSettingsRva,kPublishSettings), Witness(kToggleRva,kToggle), Witness(kDrawRva,kDraw),
    Witness(kProbeRva,kProbe), Witness(kGetterRva,kGetter), Witness(kOldWorkerRva,kOldWorker),
    Witness(kSetEnabledRva,kSetEnabled),
    Witness(kInitRowRva,kInitRow), Witness(kStartRowRva,kStartRow)
};
constexpr MenuProfile kMenu601{
    0xDA60636F, 0xC00000, "DLMM 6.0.1", menu601::kMenuSha,
    menu601::kVtable, menu601::kOldProbeCalled, menu601::kBackendStarted, menu601::kOldWorkerStarted,
    menu601::kMenuStage, menu601::kStatus, menu601::kEnabled, menu601::kRange, menu601::kUnlimited,
    menu601::kRegistryCount, menu601::kRegistry,
    Witness(menu601::kInitRva,menu601::kInit), Witness(menu601::kStartRva,menu601::kStart), Witness(menu601::kShutdownRva,menu601::kShutdown),
    Witness(menu601::kReadSettingsRva,menu601::kReadSettings), Witness(menu601::kReadFlagRva,menu601::kReadFlag), Witness(menu601::kMenuRegistryInitRva,menu601::kMenuRegistryInit),
    Witness(menu601::kPublishSettingsRva,menu601::kPublishSettings), Witness(menu601::kToggleRva,menu601::kToggle), Witness(menu601::kDrawRva,menu601::kDraw),
    Witness(menu601::kProbeRva,menu601::kProbe), Witness(menu601::kGetterRva,menu601::kGetter), Witness(menu601::kOldWorkerRva,menu601::kOldWorker),
    Witness(menu601::kSetEnabledRva,menu601::kSetEnabled),
    Witness(menu601::kInitRowRva,menu601::kInitRow), Witness(menu601::kStartRowRva,menu601::kStartRow)
};

constexpr std::array<uint8_t,32> kDynamicSafSha{0x4D,0xE8,0x8E,0xCA,0xF5,0x12,0x54,0x73,0x80,0x71,0x0C,0x84,0x2F,0x6F,0x6A,0x24,0x8D,0x31,0x38,0x88,0x07,0xEB,0xA8,0xF1,0x68,0x47,0xE5,0xB3,0x69,0x18,0xDD,0xEF};
constexpr uint32_t kReadyMask=0x3F; // SAF range lanes 0 through 5.
constexpr float kMenuUnlimitedReach=50000.f;

struct ModuleState {
    uintptr_t game=0, menu=0;
    const MenuProfile* profile=nullptr;
    HANDLE stop=nullptr, worker=nullptr;
    volatile LONG* safUnlimited=nullptr;
    safcompat::Client saf{};
};
ModuleState g;
SRWLOCK g_lifecycle=SRWLOCK_INIT;
std::atomic<bool> g_installed{false},g_initialized{false},g_stopping{false};
#ifdef AXIOM_TESTING
int (*g_testSafReady)(safcompat::Lease&)=nullptr;
bool (*g_testGameWitnesses)(uintptr_t)=nullptr;
void (*g_testLoadMenuSettings)(void*)=nullptr;
void (*g_testPublishMenuSettings)(const void*)=nullptr;
bool (*g_testMenuEnabled)(const wchar_t*,const wchar_t*,bool)=nullptr;
#endif
HMODULE g_self=nullptr;
HANDLE g_log=INVALID_HANDLE_VALUE;
const char kReason[]="Menu Reach controls its secondary catch radius. SAF supplies dynamic aim and tether reach, pull speed, Instant Axiom and Straight Pull.";
const char kRefusal[]="Axiom coexistence refused its native or lifecycle contract.";

void Log(const char* text) {
    if (g_log==INVALID_HANDLE_VALUE) return;
    char message[880]{};
    const int size=snprintf(message,sizeof(message),"[AxiomCoexist] %s\r\n",text);
    if (size>0 && size<static_cast<int>(sizeof(message))) {
        DWORD bytes=0;
        WriteFile(g_log,message,static_cast<DWORD>(size),&bytes,nullptr);
    }
}

bool Read(void*,uintptr_t at,void* out,size_t n) {
    SIZE_T copied=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),out,n,&copied) && copied==n;
}

template<class T>
bool Get(uintptr_t at,T& value) { return Read(nullptr,at,&value,sizeof(value)); }

template<class T>
bool Set(uintptr_t at,const T& value) {
    SIZE_T bytes=0;
    return WriteProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),&value,sizeof(value),&bytes) &&
           bytes==sizeof(value);
}

bool Writeable(uintptr_t at,size_t n,uintptr_t owner,DWORD type) {
    return safcompat::Region(at,n,owner,type,PAGE_READWRITE) ||
           safcompat::Region(at,n,owner,type,PAGE_WRITECOPY);
}

template<size_t N>
bool Block(uintptr_t owner,uintptr_t at,const std::array<uint8_t,N>& bytes,DWORD protection) {
    return safcompat::Region(owner+at,N,owner,MEM_IMAGE,protection) &&
           safcompat::Equal(Read,nullptr,owner+at,bytes.data(),N);
}

bool HashFile(const wchar_t* path,const std::array<uint8_t,32>& expected) {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if (file==INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    std::array<uint8_t,32> digest{};
    std::array<uint8_t,65536> data{};
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0 &&
            BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    while (ok) {
        DWORD bytes=0;
        if (!ReadFile(file,data.data(),static_cast<DWORD>(data.size()),&bytes,nullptr)) {
            ok=false;
            break;
        }
        if (!bytes) break;
        ok=BCryptHashData(hash,data.data(),bytes,0)>=0;
    }
    ok=ok && BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)>=0 &&
       digest==expected;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    CloseHandle(file);
    return ok;
}

bool HashModule(HMODULE module,const std::array<uint8_t,32>& expected) {
    wchar_t path[1024]{};
    const auto count=GetModuleFileNameW(module,path,static_cast<DWORD>(std::size(path)));
    return count && count<std::size(path) && HashFile(path,expected);
}

bool ExactHost(uintptr_t game) {
    wchar_t name[32768]{};
    const auto count=GetModuleFileNameW(nullptr,name,static_cast<DWORD>(std::size(name)));
    if (!count || count>=std::size(name)) return false;
    const auto* slash=wcsrchr(name,L'\\');
    if (!slash || _wcsicmp(slash+1,L"CrimsonDesert.exe") || game!=0x140000000) return false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    return Get(game,dos) && dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>=64 &&
           dos.e_lfanew<=0x1000 && Get(game+dos.e_lfanew,nt) && nt.Signature==IMAGE_NT_SIGNATURE &&
           nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64 && nt.FileHeader.TimeDateStamp==0x6AF5D198 &&
           nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           nt.OptionalHeader.SizeOfImage==0x144E9000 && nt.OptionalHeader.CheckSum==0x13FF2BD0;
}

bool ReadMenuHeaders(uintptr_t menu,IMAGE_NT_HEADERS64& nt) {
    IMAGE_DOS_HEADER dos{};
    return Get(menu,dos) && dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>=64 &&
           dos.e_lfanew<=0x1000 && Get(menu+dos.e_lfanew,nt) && nt.Signature==IMAGE_NT_SIGNATURE &&
           nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64 &&
           nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC && !nt.OptionalHeader.CheckSum;
}

const MenuProfile* ExactMenu(uintptr_t menu) {
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadMenuHeaders(menu,nt)) return nullptr;
    for (const auto* profile : {&kMenu60,&kMenu601}) {
        if (nt.FileHeader.TimeDateStamp==profile->stamp && nt.OptionalHeader.SizeOfImage==profile->imageSize)
            return profile;
    }
    return nullptr;
}

bool MenuBlock(uintptr_t menu,const MenuWitness& witness,DWORD protection) {
    return safcompat::Region(menu+witness.rva,witness.size,menu,MEM_IMAGE,protection) &&
           safcompat::Equal(Read,nullptr,menu+witness.rva,witness.bytes,witness.size);
}

bool GameWitnesses(uintptr_t game) {
#ifdef AXIOM_TESTING
    if (g_testGameWitnesses) return g_testGameWitnesses(game);
#endif
    return Block(game,kDistanceUseRva,kDistanceUse,PAGE_EXECUTE_READ) &&
           Block(game,kDistanceFallbackRva,kDistanceFallback,PAGE_EXECUTE_READ) &&
           (Block(game,kDistanceInitRva,kDistanceInit,PAGE_EXECUTE_READWRITE) ||
            Block(game,kDistanceInitRva,kDistanceInit,PAGE_EXECUTE_WRITECOPY)) &&
           Block(game,kDistanceSeedRva,kDistanceSeed,PAGE_READONLY) &&
           Block(game,kDistanceUseRowRva,kDistanceUseRow,PAGE_READONLY) &&
           Block(game,kDistanceInitRowRva,kDistanceInitRow,PAGE_READONLY) &&
           Writeable(game+kDistance,sizeof(uint32_t),game,MEM_IMAGE);
}

bool MenuWitnesses(uintptr_t menu) {
    const auto& profile=*g.profile;
    for (const auto& block : {profile.init, profile.start, profile.shutdown, profile.readSettings,
                              profile.readFlag, profile.registryInit, profile.publishSettings, profile.toggle,
                              profile.draw, profile.probe, profile.getter, profile.stockWorker, profile.setEnabled}) {
        if (!MenuBlock(menu,block,PAGE_EXECUTE_READ)) return false;
    }
    uintptr_t init=0,start=0,stop=0;
    return MenuBlock(menu,profile.initRow,PAGE_READONLY) &&
           MenuBlock(menu,profile.startRow,PAGE_READONLY) &&
           Get(menu+profile.vtable+0x50,init) && init==menu+profile.init.rva &&
           Get(menu+profile.vtable+0x58,stop) && stop==menu+profile.shutdown.rva &&
           Get(menu+profile.vtable+0x90,start) && start==menu+profile.toggle.rva &&
           Writeable(menu+profile.backendStarted,sizeof(uint8_t),menu,MEM_IMAGE) &&
           Writeable(menu+profile.status,sizeof(uint32_t),menu,MEM_IMAGE) &&
           Writeable(menu+profile.enabled,sizeof(uint32_t),menu,MEM_IMAGE) &&
           Writeable(menu+profile.range,sizeof(float),menu,MEM_IMAGE) &&
           Writeable(menu+profile.unlimited,sizeof(uint32_t),menu,MEM_IMAGE);
}

bool Pin(HMODULE module) {
    HMODULE pinned=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
           reinterpret_cast<LPCWSTR>(module),&pinned) && pinned==module;
}

enum class SafState {Ready, Busy, Missing};
SafState SafReady(safcompat::Lease& lease) {
#ifdef AXIOM_TESTING
    if (g_testSafReady) return static_cast<SafState>(g_testSafReady(lease));
#endif
    safcompat::View view;
    const auto result=g.saf.Open(Read,nullptr,g.game,lease,view);
    if (result==safcompat::Result::Busy) return SafState::Busy;
    return result==safcompat::Result::Valid &&
           lease.state.status==static_cast<uint32_t>(safcompat::Status::Ready) &&
           (lease.state.ownedMask&kReadyMask)==kReadyMask ? SafState::Ready:SafState::Missing;
}

bool LoadSettings(axiom::Settings& value) {
    axiom::Settings again{};
    const auto snapshot=[](axiom::Settings& state) {
        return Get(g.menu+g.profile->enabled,state.enabled) && Get(g.menu+g.profile->range,state.range) &&
               Get(g.menu+g.profile->unlimited,state.unlimited);
    };
    return snapshot(value) && snapshot(again) && value.enabled==again.enabled &&
           value.unlimited==again.unlimited && axiom::FloatBits(value.range)==axiom::FloatBits(again.range);
}

bool Apply(axiom::DistanceOwner& owner,const axiom::Settings& settings,bool ready,bool* qualified=nullptr) {
    uint32_t current=0;
    if (!Get(g.game+kDistance,current)) return false;
    const auto op=owner.Decide(current,settings,ready);
    if (op.stop) {
        Log("Foreign writer changed the scoped fallback radius. No overwrite. Restart required.");
        return false;
    }
    if (!op.write) {
        if (qualified) *qualified=owner.Owned() || current==axiom::kNativeDistance;
        return true;
    }
#ifdef AXIOM_TESTING
    const DWORD allocationType=g_testGameWitnesses?MEM_PRIVATE:MEM_IMAGE;
#else
    constexpr DWORD allocationType=MEM_IMAGE;
#endif
    if (!Writeable(g.game+kDistance,sizeof(uint32_t),g.game,allocationType)) return false;
    const auto address=reinterpret_cast<volatile LONG*>(g.game+kDistance);
    const bool success=static_cast<uint32_t>(InterlockedCompareExchange(address,
                       static_cast<LONG>(op.desired),static_cast<LONG>(op.expected)))==op.expected;
    owner.Complete(op,success);
    if (qualified) *qualified=success;
    if (!success) return true;
    char message[192]{};
    float from=0, to=0;
    memcpy(&from,&op.expected,4); memcpy(&to,&op.desired,4);
    snprintf(message,sizeof(message),"Menu fallback radius %.1f -> %.1f at game+716B838.",from,to);
    Log(message);
    return true;
}

#include "TargetTraceRuntime.h"

DWORD WINAPI DistanceWorker(void*) {
    axiom::DistanceOwner owner;
    axiom::CatchRange catchRange;
    bool reportedCatchRange=false;
    bool wasReady=false;
    uint32_t previousStatus=1;
    unsigned traceAttempts=0;
    bool ownsReachRequest=false;
    while (WaitForSingleObject(g.stop,250)==WAIT_TIMEOUT) {
        const bool initialized=g_initialized.load(std::memory_order_acquire);
        if (g.profile && !initialized) continue;
        DrainTargetTrace();
        safcompat::Lease lease;
        axiom::Settings settings{};
        const auto safState=SafReady(lease);
        if (safState==SafState::Busy) continue;
        const bool ready=safState==SafState::Ready;
        constexpr uint32_t straightPullMask=0x80;
        g_straightPullOwned.store(ready && (lease.state.ownedMask&straightPullMask)!=0,
                                  std::memory_order_relaxed);
        if (!initialized) {
            if (!ready) continue;
            ++traceAttempts;
#ifdef AXIOM_TESTING
            if (g_testTraceInstallCount) g_testTraceInstallCount->store(traceAttempts);
            const auto result=g_testTraceInstall?g_testTraceInstall(traceAttempts):InstallTargetTrace();
#else
            const auto result=InstallTargetTrace();
#endif
            if (result!=TraceInstall::Installed) {
                if (result==TraceInstall::Retry && traceAttempts<8) continue;
                Log("SAF-only surface recovery unavailable. SAF keeps its native patches.");
                break;
            }
            g_initialized.store(true,std::memory_order_release);
            Log("SAF-only surface recovery and native attachment tracing installed after SAF acquired ownership.");
        }
        bool qualified=false,requested=false;
        if (g.profile) {
            if (!g.safUnlimited || !GameWitnesses(g.game)) break;
            if (!LoadSettings(settings)) continue;
            if (!Apply(owner,settings,ready,&qualified)) break;
            requested=ready && settings.enabled==1 && settings.unlimited==1 &&
                      std::isfinite(settings.range) && qualified;
        }
        float effectiveRange=0.f;
        memcpy(&effectiveRange,&lease.state.rangeBits,sizeof(effectiveRange));
        // Both confirmation queries follow the same scalar as the aiming ray.
        // The live lease keeps ownership stable during each installation.
        const bool catchReady=catchRange.Update(g.game,ready?lease.state.support:0,Read);
        if (!catchReady && catchRange.Faulted()) {
            Log("Additional Remote Catch references changed. Restart required; foreign bytes retained.");
            break;
        }
        const bool wantReach=ready && catchReady;
        if (wantReach!=ownsReachRequest) {
            const LONG expected=ownsReachRequest?1:0;
            const LONG desired=wantReach?1:0;
            if (!g.safUnlimited || InterlockedCompareExchange(g.safUnlimited,desired,expected)!=expected) {
                Log("Native SAF reach request ownership changed. Surface expansion stopped.");
                break;
            }
            ownsReachRequest=wantReach;
        }
        if (ownsReachRequest && InterlockedCompareExchange(g.safUnlimited,1,1)!=1) {
            ownsReachRequest=false;
            Log("SAF reach request changed while active. Surface expansion stopped.");
            break;
        }
        const bool expanded=ownsReachRequest && effectiveRange>=kMenuUnlimitedReach;
        const bool unlimitedActive=requested && expanded;
        g_effectiveRangeBits.store(expanded?lease.state.rangeBits:0,std::memory_order_relaxed);
        g_traceUnlimited.store(unlimitedActive,std::memory_order_relaxed);
        g_searchReady.store(expanded,std::memory_order_release);
        if (expanded && !reportedCatchRange) {
            Log("Dynamic Axiom targeting active: native long ray, confirmation and tether inputs verified.");
            reportedCatchRange=true;
        } else if (!ready && catchReady && reportedCatchRange) {
            Log("Remote Catch confirmation and pull clamps restored after SAF ownership ended.");
            reportedCatchRange=false;
        }
        if (!g.profile) continue;

        const bool enabled=ready && settings.enabled==1 && settings.unlimited<=1 &&
                           std::isfinite(settings.range) && qualified &&
                           (!requested || unlimitedActive);
        const uint32_t status=enabled?2u:(ready && settings.enabled==0?0u:1u);
        if (status!=previousStatus) {
            if (!Set(g.menu+g.profile->status,status) || !Set(g.menu+g.profile->backendStarted,uint8_t{status?1u:0u})) break;
            previousStatus=status;
        }
        if (ready!=wasReady) {
            wasReady=ready;
            Log(ready?kReason:"SAF scoped ownership unavailable. Menu fallback radius restored.");
        }
    }
    g_searchReady.store(false,std::memory_order_release);
    g_straightPullOwned.store(false,std::memory_order_relaxed);
    g_effectiveRangeBits.store(0,std::memory_order_relaxed);
    DrainTargetTrace();
    bool restored=catchRange.Restore(g.game,Read);
    for (unsigned attempt=0;!restored && attempt<3;++attempt) {
        Sleep(10);
        restored=catchRange.Restore(g.game,Read);
    }
    if (!restored) Log("Additional Remote Catch reference restoration incomplete; restart required.");
    if (ownsReachRequest) InterlockedCompareExchange(g.safUnlimited,0,1);
    if (owner.Owned() && GameWitnesses(g.game)) Apply(owner,{},false);
    if (g.profile) {
        const bool stopping=g_stopping.load(std::memory_order_acquire);
        Set(g.menu+g.profile->status,uint32_t{stopping?0u:3u});
        Set(g.menu+g.profile->backendStarted,uint8_t{stopping?0u:1u});
    }
    return 0;
}

bool StartWorkerLocked() {
    if (g_stopping.load(std::memory_order_acquire)) return false;
    if (!g.worker) g.worker=CreateThread(nullptr,0,DistanceWorker,nullptr,0,nullptr);
    return g.worker && WaitForSingleObject(g.worker,0)==WAIT_TIMEOUT;
}

extern "C" bool HookStart(void*) {
    AcquireSRWLockExclusive(&g_lifecycle);
    const bool ready=g_installed.load(std::memory_order_acquire) &&
                     g_initialized.load(std::memory_order_acquire) && StartWorkerLocked();
    ReleaseSRWLockExclusive(&g_lifecycle);
    return ready;
}

extern "C" void HookShutdown(void*) {
    AcquireSRWLockExclusive(&g_lifecycle);
    g_stopping.store(true,std::memory_order_release);
    if (g.stop) SetEvent(g.stop);
    if (g.worker) {
        WaitForSingleObject(g.worker,INFINITE);
        CloseHandle(g.worker);
        g.worker=nullptr;
    }
    if (g.stop) {
        CloseHandle(g.stop);
        g.stop=nullptr;
    }
    ReleaseSRWLockExclusive(&g_lifecycle);
    Log("Axiom shutdown joined the scoped worker.");
}

bool ExactObject(uintptr_t object) {
    MEMORY_BASIC_INFORMATION info{};
    uintptr_t vtable=0;
    return object && !(object%8) &&
           VirtualQuery(reinterpret_cast<void*>(object),&info,sizeof(info))==sizeof(info) &&
           Writeable(object,0x28,reinterpret_cast<uintptr_t>(info.AllocationBase),info.Type) &&
           Get(object,vtable) && vtable==g.menu+g.profile->vtable;
}

// The exact Menu build calls these two local helpers directly and omits
// them from its CFG table. Keep the exception on these typed calls only.
using PublishSettings=void(*)(const void*);
using ReadFlag=bool(*)(const wchar_t*,const wchar_t*,bool);

__declspec(noinline) __declspec(guard(nocf))
void CallMenuPublishSettings(const void* fields) {
    auto publish=reinterpret_cast<PublishSettings>(g.menu+g.profile->publishSettings.rva);
#ifdef AXIOM_TESTING
    if (g_testPublishMenuSettings) publish=g_testPublishMenuSettings;
#endif
    publish(fields);
}

__declspec(noinline) __declspec(guard(nocf))
bool CallMenuReadEnabled() {
    auto read=reinterpret_cast<ReadFlag>(g.menu+g.profile->readFlag.rva);
#ifdef AXIOM_TESTING
    if (g_testMenuEnabled) read=g_testMenuEnabled;
#endif
    return read(L"AxiomForce",L"Enabled",false);
}

extern "C" bool HookInit(void* instance) {
    const auto object=reinterpret_cast<uintptr_t>(instance);
    if (!g_installed.load(std::memory_order_acquire) || !ExactObject(object)) return false;
    AcquireSRWLockExclusive(&g_lifecycle);
    if (g_stopping.load(std::memory_order_acquire) || g_initialized.load(std::memory_order_acquire)) {
        const bool ready=g_initialized.load(std::memory_order_relaxed) && StartWorkerLocked();
        ReleaseSRWLockExclusive(&g_lifecycle);
        return ready;
    }
#ifndef AXIOM_TESTING
    if (!MenuBlock(g.menu,g.profile->readSettings,PAGE_EXECUTE_READ) ||
        !MenuBlock(g.menu,g.profile->publishSettings,PAGE_EXECUTE_READ) ||
        !MenuBlock(g.menu,g.profile->readFlag,PAGE_EXECUTE_READ)) {
        ReleaseSRWLockExclusive(&g_lifecycle);
        Log(kRefusal);
        return false;
    }
#endif
    using ReadSettings=void(*)(void*);
    auto readSettings=reinterpret_cast<ReadSettings>(g.menu+g.profile->readSettings.rva);
#ifdef AXIOM_TESTING
    if (g_testLoadMenuSettings) readSettings=g_testLoadMenuSettings;
#endif
    readSettings(instance);
    CallMenuPublishSettings(reinterpret_cast<void*>(object+0x1C));
    const bool enabled=CallMenuReadEnabled();
    const char* reason=kReason;
    const uint32_t compatibility=1,phase=2;
    const bool ready=Set(g.menu+g.profile->enabled,uint32_t{enabled?1u:0u}) &&
                     Set(g.menu+g.profile->status,uint32_t{1}) && StartWorkerLocked() &&
                     Set(object+0x10,reason) && Set(object+0x0C,compatibility) && Set(object+0x18,phase) &&
                     Set(g.menu+g.profile->backendStarted,uint8_t{1});
    if (ready) g_initialized.store(true,std::memory_order_release);
    ReleaseSRWLockExclusive(&g_lifecycle);
    if (!ready) {
        const uint32_t failedCompatibility=2, failedPhase=3;
        reason=kRefusal;
        Set(object+0x10,reason); Set(object+0x0C,failedCompatibility); Set(object+0x18,failedPhase);
        HookShutdown(instance);
        Set(g.menu+g.profile->status,uint32_t{3});
        Set(g.menu+g.profile->backendStarted,uint8_t{1});
        Log(kRefusal);
        return false;
    }
    Log("Menu backend initialized. Awaiting SAF ownership before any game write.");
    Log(kReason);
    return true;
}

template<class F>
quiesced_patch::Patch Detour(uintptr_t at,F callback,const MenuWitness& original) {
    quiesced_patch::Patch p{};
    p.address=reinterpret_cast<uint8_t*>(at);

    // Windows x64 entry permits clobbering RAX. The three detours are
    // tail jumps to CFG-compiled callback entry points.
    p.size=12;
    memcpy(p.before,original.bytes,p.size);
    p.after[0]=0x48; p.after[1]=0xB8;
    const auto address=reinterpret_cast<uintptr_t>(callback);
    memcpy(p.after+2,&address,sizeof(address));
    p.after[10]=0xFF; p.after[11]=0xE0;
    return p;
}

bool BeforeAxiomInit(uintptr_t menu) {
    uint8_t probe=0, installed=0;
    uint32_t worker=0, phase=0, count=0;
    if (!Get(menu+g.profile->oldProbe,probe) || !Get(menu+g.profile->backendStarted,installed) ||
        !Get(menu+g.profile->oldWorker,worker) || !Get(menu+g.profile->menuStage,phase) ||
        !Get(menu+g.profile->registryCount,count) || probe || installed || worker || phase>=5 || count>64) return false;
    for (uint32_t i=0;i<count;++i) {
        uintptr_t object=0,vtable=0;
        if (!Get(menu+g.profile->registry+0x30*i,object) || !object || !Get(object,vtable)) return false;
        if (vtable!=menu+g.profile->vtable) continue;
        uint8_t initialized=0;
        uint32_t modulePhase=0;
        if (!Get(object+8,initialized) || !Get(object+0x18,modulePhase) ||
            initialized || modulePhase>=2) return false;
    }
    return true;
}

bool StartStandalone() {
    g.stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!g.stop) return false;
    if (!StartWorkerLocked()) {
        HookShutdown(nullptr);
        return false;
    }
    Log("SAF-only mode. Waiting for verified SAF range ownership.");
    return true;
}

enum class MenuStartup { Declined, Installed, Inconclusive };

MenuStartup TryLegacyMenu(HMODULE menu) {
    g.menu=reinterpret_cast<uintptr_t>(menu);
    g.profile=ExactMenu(g.menu);
    if (!g.profile || !HashModule(menu,g.profile->hash) || !BeforeAxiomInit(g.menu) ||
        !MenuWitnesses(g.menu) || !GameWitnesses(g.game))
        return MenuStartup::Declined;

    g.stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!g.stop) {
        return MenuStartup::Declined;
    }
    const std::array<quiesced_patch::Patch,3> patches{
        Detour(g.menu+g.profile->init.rva,HookInit,g.profile->init),
        Detour(g.menu+g.profile->start.rva,HookStart,g.profile->start),
        Detour(g.menu+g.profile->shutdown.rva,HookShutdown,g.profile->shutdown)
    };
    const uint8_t zero=0;
    const uint32_t zero32=0;
    uint32_t menuPhase=0;
    if (!Get(g.menu+g.profile->menuStage,menuPhase) || menuPhase>=5) {
        CloseHandle(g.stop);
        g.stop=nullptr;
        return MenuStartup::Declined;
    }
    const std::array<quiesced_patch::Check,11> checks{{
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->oldProbe),&zero,1},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->backendStarted),&zero,1},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->oldWorker),reinterpret_cast<const uint8_t*>(&zero32),4},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->probe.rva),g.profile->probe.bytes,g.profile->probe.size},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->menuStage),reinterpret_cast<const uint8_t*>(&menuPhase),4},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->init.rva),g.profile->init.bytes,g.profile->init.size},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->start.rva),g.profile->start.bytes,g.profile->start.size},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->shutdown.rva),g.profile->shutdown.bytes,g.profile->shutdown.size},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->readSettings.rva),g.profile->readSettings.bytes,g.profile->readSettings.size},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->publishSettings.rva),g.profile->publishSettings.bytes,g.profile->publishSettings.size},
        {reinterpret_cast<const uint8_t*>(g.menu+g.profile->readFlag.rva),g.profile->readFlag.bytes,g.profile->readFlag.size},
    }};
    quiesced_patch::Report report{};
    g_installed.store(true,std::memory_order_release);
    if (!quiesced_patch::Write(patches.data(),patches.size(),{},&report,checks.data(),checks.size())) {
        g_installed.store(false,std::memory_order_release);
        Log("Axiom startup transaction refused. No compatibility status advertised.");
        if (!report.unchanged || !report.protectionsRestored || !report.rollbackOk ||
            !report.allResumed) return MenuStartup::Inconclusive;
        CloseHandle(g.stop);
        g.stop=nullptr;
        return MenuStartup::Declined;
    }
    char installed[128]{};
    snprintf(installed,sizeof(installed),"Verified %s Axiom coexistence installed before Menu initialization.",g.profile->name);
    Log(installed);
    Log(InstallTargetTrace()==TraceInstall::Installed?"Axiom surface search and native attachment tracing installed.":
                             "Axiom surface search unavailable; coexistence remains active.");
    return MenuStartup::Installed;
}

// Standalone only owns native game spans. Menu presence, identity, and private
// state do not participate once optional legacy integration is declined.
bool StartSafBackend(HMODULE menu,bool& retainMenu) {
    const auto legacy=menu?TryLegacyMenu(menu):MenuStartup::Declined;
    retainMenu=legacy!=MenuStartup::Declined;
    if (legacy==MenuStartup::Installed) return true;
    if (legacy==MenuStartup::Inconclusive) {
        Log("Indeterminate Menu transaction. Standalone fallback withheld for this launch.");
        return true; // Keep the bridge and Menu resident if any detours remain.
    }
    g.menu=0;
    g.profile=nullptr;
    return StartStandalone();
}

#include "Flash605.h"

bool Bootstrap() {
    g.game=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!ExactHost(g.game)) return false;
    wchar_t logPath[1024]{};
    const auto size=GetModuleFileNameW(g_self,logPath,static_cast<DWORD>(std::size(logPath)));
    auto* slash=(size && size<std::size(logPath))?wcsrchr(logPath,L'\\'):nullptr;
    if (slash) {
        wcscpy_s(slash+1,std::size(logPath)-(slash+1-logPath),L"DesertLinkAxiomCoexist.log");
        g_log=CreateFileW(logPath,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    HMODULE saf=nullptr;
    const auto limit=GetTickCount64()+5000;
    do {
        if (GetModuleHandleExW(0,L"SuperAxiomForce.asi",&saf)) break;
        Sleep(10);
    } while (GetTickCount64()<limit);
    if (!saf) {
        Log("SAF was not present in the startup window.");
        return false;
    }
    if (!HashModule(nullptr,kGameSha) || (!HashModule(saf,kSAFSha) && !HashModule(saf,kDynamicSafSha)) ||
        !g.saf.Resolve(Read,nullptr) || !g.saf.Present()) {
        Log("Axiom startup refused: game or SAF identity unmatched. No hooks installed.");
        FreeLibrary(saf);
        return false;
    }

    const auto safBase=reinterpret_cast<uintptr_t>(saf);
    const auto request=reinterpret_cast<uintptr_t>(GetProcAddress(saf,"SAFMenuUnlimitedReachV1"));
    if (request!=safBase+safcompat::menu_reach_producer::kMenuUnlimitedRva ||
        request%alignof(LONG) || !Writeable(request,sizeof(LONG),safBase,MEM_IMAGE)) {
        Log("Exact SAF reach request slot unavailable.");
        FreeLibrary(saf);
        return false;
    }
    g.safUnlimited=reinterpret_cast<volatile LONG*>(request);

    HMODULE menu=nullptr;
    GetModuleHandleExW(0,L"DesertLinkModMenu.asi",&menu);
    TryFlash605(menu);
    bool retainMenu=false;
    const bool started=StartSafBackend(menu,retainMenu);
    // The strong reference already protects Menu through legacy publication.
    if (menu && (!retainMenu || Pin(menu))) FreeLibrary(menu);
    FreeLibrary(saf);
    return started;
}

DWORD WINAPI BootstrapThread(void* retained) {
    const auto module=static_cast<HMODULE>(retained);
    if (Bootstrap()) {
        // If pinning fails, keep the strong reference for process lifetime.
        if (Pin(module)) FreeLibrary(module);
        return 0;
    }
    FreeLibraryAndExitThread(module,0);
}

}

BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_self=module;
        wchar_t image[MAX_PATH]{};
        const auto count=GetModuleFileNameW(nullptr,image,static_cast<DWORD>(std::size(image)));
        const auto* slash=count && count<std::size(image)?wcsrchr(image,L'\\'):nullptr;
        if (!slash || _wcsicmp(slash+1,L"CrimsonDesert.exe")) return TRUE;
        HMODULE retained=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(module),&retained)) return TRUE;
        if (const HANDLE thread=CreateThread(nullptr,0,BootstrapThread,retained,0,nullptr))
            CloseHandle(thread);
        else
            FreeLibrary(retained);
    }
    return TRUE;
}

