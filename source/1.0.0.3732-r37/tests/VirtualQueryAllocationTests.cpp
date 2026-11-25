// Only this executable's own virtual address space is queried/reserved.
// A fixed high image base keeps the inspected game-address window unoccupied.
#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"
static unsigned checks=0,failures=0;
static void Check(bool yes,const char* name){++checks;printf("%s %s\n",yes?"PASS":"FAIL",name);if(!yes)++failures;}
static std::array<near_allocation::Reference,10> Refs(){
 constexpr uintptr_t b=0x140000000;
 return {{{b+0x64B1F2,0x1000},{b+0x64C161,0x1000},{b+0x134D251,0x1000},
 {b+0x649901,0x1000},{b+0x64B1F9,0x1000},{b+0x134D258,0x1000},
 {b+0x65CCE3,0x1008},{b+0x65A38F,0},{b+0x65C1E5,0x200},{b+0x137BAE4,0x400}}};
}
static void* Reserve(uintptr_t at,size_t n){return VirtualAlloc(reinterpret_cast<void*>(at),n,MEM_RESERVE,PAGE_NOACCESS);}
static void Release(void* p){if(p)Check(VirtualFree(p,0,MEM_RELEASE)!=0,"private reservation releases");}
int main(){
 constexpr uintptr_t base=0x140000000,low=0xC1380000,highEnd=0x1C0650000,imageEnd=0x153EE0000;
 printf("OwnImage=%p MBI_size=%zu state_offset=%zu region_size_offset=%zu\n",GetModuleHandleW(nullptr),sizeof(MEMORY_BASIC_INFORMATION),offsetof(MEMORY_BASIC_INFORMATION,State),offsetof(MEMORY_BASIC_INFORMATION,RegionSize));
 Check(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))>=0x4000000000,"fixture executable lies outside inspected address window");
 auto* image=Reserve(base,0x13EE0000);auto* above=Reserve(imageEnd,highEnd-imageEnd);
 auto* ff=Reserve(0x13FFE0000,0x10000);auto* sj=Reserve(0x13FFD0000,0x10000);auto* psm=Reserve(0x13FFC0000,0x10000);
 Check(image==reinterpret_cast<void*>(base)&&above==reinterpret_cast<void*>(imageEnd)&&ff&&sj&&psm,"real image, occupied upper arena and three peer reservations exist");
 if(!image||!above||!ff||!sj||!psm)return 2;
 NearSupportErrors errors{};near_allocation::Region r{};
 Check(QueryNearSupport(&errors,0x13FFF0000,r)&&r.free&&r.base==0x13FFF0000&&r.size==0x10000,"real VirtualQuery at aligned last free slot returns entire64KiB tail");
 Check(QueryNearSupport(&errors,0x13FFFF000,r)&&r.free&&r.base==0x13FFFF000&&r.size==0x1000,"real VirtualQuery inside free span returns remaining4KiB only");
 const auto refs=Refs();SYSTEM_INFO sys{};GetSystemInfo(&sys);
 auto allocate=[&](){return near_allocation::Allocate({&errors,QueryNearSupport,AllocateNearSupport,ReleaseNearSupport},refs.data(),refs.size(),0x2000,sys.dwAllocationGranularity,
 reinterpret_cast<uintptr_t>(sys.lpMinimumApplicationAddress),reinterpret_cast<uintptr_t>(sys.lpMaximumApplicationAddress),base,0x13EE0000);};
 auto found=allocate();printf("peer_layout result=%p queries=%u attempts=%u above=%d below=%d\n",reinterpret_cast<void*>(found.address),found.queries,found.attempts,found.searchedAbove,found.searchedBelow);
 Check(found.address==0x13FFF0000&&found.queries==2&&found.attempts==1,"production allocator finds nearest real free lower slot despite occupied upper arena and peer caves");
 if(found.address){Check(QueryNearSupport(&errors,found.address,r)&&!r.free,"production query preserves committed owned-memory classification");ReleaseNearSupport(&errors,found.address,0x2000);}
 Release(image);Release(above);Release(ff);Release(sj);Release(psm);
 auto* occupied=Reserve(low,highEnd-low);Check(occupied==reinterpret_cast<void*>(low),"full rel32 window reserves privately without committing physical memory");if(!occupied)return 2;
 found=allocate();printf("occupied_layout result=%p queries=%u attempts=%u above=%d below=%d errors=%lu/%lu\n",reinterpret_cast<void*>(found.address),found.queries,found.attempts,found.searchedAbove,found.searchedBelow,errors.query,errors.allocation);
 Check(!found.address&&found.window.low==low&&found.window.high==0x1C0640000&&found.queries==32457&&!found.attempts&&!found.queryFailures&&!found.invalidRegions&&!found.budgetExceeded,"real wholly reserved window exactly reproduces live32457queries zeroattempts signature");
 Check(errors.queries==32462&&errors.free==3&&errors.reserved==32458&&errors.committed==1&&!errors.other,"raw diagnostic counters distinguish free, reserved and committed query results");
 Check(errors.eligibleFree==2&&errors.largestFreeTail==0x10000,"raw diagnostic spans distinguish4KiB tail from usable8KiB region");
 Check(errors.first[0].at==0x13FFF0000&&errors.first[0].info.State==MEM_FREE&&errors.first[0].info.RegionSize==0x10000&&errors.first[0].returned==48,"bounded raw sample retains actual Windows address, state, span and ABI size");
 Release(occupied);printf("VirtualQueryAllocation checks=%u failures=%u\n",checks,failures);return failures?1:0;
}