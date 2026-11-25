#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>
struct Hole {uintptr_t start,end;};
struct World {
 uintptr_t low=0xC0000000,high=0x1D0000000,failAddress=0,wrongReturn=0,lastRequest=0;
 std::vector<Hole> holes;unsigned queries=0,attempts=0,releases=0,transient=0,queryMode=0;
 bool Query(uintptr_t at,MEMORY_BASIC_INFORMATION& out){
  ++queries;if(queryMode==1||at<low||at>=high)return false;
  if(queryMode==2){out={};out.BaseAddress=reinterpret_cast<void*>(at+1);out.RegionSize=0x1000;out.State=MEM_FREE;return true;}
  uintptr_t end=high;bool free=false;
  for(auto h:holes){if(at>=h.start&&at<h.end){end=h.end;free=true;break;}if(h.start>at)end=(std::min)(end,h.start);}
  const auto page=at&~uintptr_t(0xFFF);out={};out.BaseAddress=reinterpret_cast<void*>(page);out.RegionSize=end-page;out.State=free?MEM_FREE:MEM_RESERVE;return true;
 }
 uintptr_t Allocate(uintptr_t at,size_t size){++attempts;lastRequest=at;if(at==failAddress)return 0;if(transient){--transient;return 0;}
  for(auto h:holes)if(at>=h.start&&at<=h.end&&size<=h.end-at)return wrongReturn?wrongReturn:at;return 0;}
};
static unsigned checks=0,failures=0;
static void Check(bool yes,const char*n){++checks;printf("%s %s\n",yes?"PASS":"FAIL",n);if(!yes)++failures;}
#ifdef SAF_NEAR_LEGACY_RED
static World* active=nullptr;
static SIZE_T FakeQuery(const void*p,MEMORY_BASIC_INFORMATION*out,SIZE_T){return active->Query(reinterpret_cast<uintptr_t>(p),*out)?sizeof(*out):0;}
static void* FakeAllocate(void*p,SIZE_T n,DWORD,DWORD){return reinterpret_cast<void*>(active->Allocate(reinterpret_cast<uintptr_t>(p),n));}
#include "../gameplay-3446/r13/legacy_allocator.h"
int main(){
 constexpr uintptr_t base=0x140000000;constexpr size_t imageSize=0x13EE0000;
 World below;below.holes={{base-0x10000,base}};active=&below;
 Check(LegacyAllocate(base,imageSize)==base-0x10000,"busy above image still finds available below-image rel32 hole");
 const uintptr_t start=(base+imageSize+0xFFFF)&~uintptr_t(0xFFFF);
 World fragmented;fragmented.holes={{start,start+0x30000}};fragmented.failAddress=start;active=&fragmented;
 Check(LegacyAllocate(base,imageSize)==start+0x10000,"one failed address does not discard rest of free region");
 World transient;transient.holes={{start,start+0x10000}};transient.transient=1;active=&transient;
 Check(LegacyAllocate(base,imageSize)==start,"transient failure retries a revalidated address");
 printf("NearAllocation legacy checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
#else
#include "../src/SuperAxiomForce/NearAllocation.h"
static bool ModelQuery(void*c,uintptr_t at,near_allocation::Region&r){MEMORY_BASIC_INFORMATION m{};if(!static_cast<World*>(c)->Query(at,m))return false;r={reinterpret_cast<uintptr_t>(m.BaseAddress),m.RegionSize,m.State==MEM_FREE};return true;}
static uintptr_t ModelAllocate(void*c,uintptr_t at,size_t n){return static_cast<World*>(c)->Allocate(at,n);}
static void ModelRelease(void*c,uintptr_t,size_t){++static_cast<World*>(c)->releases;}
static std::vector<near_allocation::Reference> References(uintptr_t base){
 return {{base+0x64B1F2,0x1000},{base+0x64C161,0x1000},{base+0x134D251,0x1000},
         {base+0x649901,0x1000},{base+0x64B1F9,0x1000},{base+0x134D258,0x1000},
         {base+0x65CCE3,0x1008},{base+0x65A38F,0},{base+0x65C1E5,0x200},{base+0x137BAE4,0x400}};
}
static near_allocation::Result Run(World& world,uintptr_t base=0x140000000,size_t imageSize=0x13EE0000){
 const auto refs=References(base);return near_allocation::Allocate({&world,ModelQuery,ModelAllocate,ModelRelease},refs.data(),refs.size(),0x2000,0x10000,world.low,world.high-1,base,imageSize);
}
static bool RealQuery(void*,uintptr_t at,near_allocation::Region&r){MEMORY_BASIC_INFORMATION m{};if(!VirtualQuery(reinterpret_cast<void*>(at),&m,sizeof(m)))return false;r={reinterpret_cast<uintptr_t>(m.BaseAddress),m.RegionSize,m.State==MEM_FREE};return true;}
static uintptr_t RealAllocate(void*,uintptr_t at,size_t n){return reinterpret_cast<uintptr_t>(VirtualAlloc(reinterpret_cast<void*>(at),n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));}
static void RealRelease(void*,uintptr_t at,size_t){VirtualFree(reinterpret_cast<void*>(at),0,MEM_RELEASE);}
int main(){
 constexpr uintptr_t base=0x140000000;constexpr size_t imageSize=0x13EE0000;const auto refs=References(base);
 const uintptr_t start=(base+imageSize+0xFFFF)&~uintptr_t(0xFFFF);
 World below;below.holes={{base-0x10000,base}};auto found=Run(below);
 Check(found.address==base-0x10000&&found.searchedAbove&&found.searchedBelow,"busy above image still finds available below-image rel32 hole");
 Check(near_allocation::Fits(found.address,refs.data(),refs.size()),"below-image result reaches every exact data and code targetoffset");
 World fragmented;fragmented.holes={{start,start+0x30000}};fragmented.failAddress=start;found=Run(fragmented);
 Check(found.address==start+0x10000&&found.allocationFailures==2,"one failed address does not discard rest of free region");
 World transient;transient.holes={{start,start+0x10000}};transient.transient=1;found=Run(transient);
 Check(found.address==start&&found.attempts==2&&found.queries>=2,"transient failure retries only after revalidation");
 World upper;upper.holes={{base+0x71000000,base+0x71010000}};found=Run(upper);
 Check(found.address==base+0x71000000,"search covers valid signed-rel32 window beyond old fixed70000000 cap");
 World both;both.holes={{start,start+0x10000},{base-0x10000,base}};found=Run(both);
 Check(found.address==start&&!found.searchedBelow,"valid above-image allocation remains supported");
 World shortHole;shortHole.holes={{start,start+0x1000}};found=Run(shortHole);
 Check(!found.address&&!found.attempts,"allocation never crosses a too-small free region");
 World unaligned;unaligned.holes={{start+0x1000,start+0x9000},{base-0x10000,base}};found=Run(unaligned);
 Check(found.address==base-0x10000,"unaligned reservation fragments are skipped without losing lower holes");
 World wrong;wrong.holes={{start,start+0x10000}};wrong.wrongReturn=0x10000;found=Run(wrong);
 Check(!found.address&&found.releasedUnexpected==1&&wrong.releases==1,"unexpected allocator return is rejected and released");
 World failed;failed.queryMode=1;found=Run(failed);Check(!found.address&&found.queryFailures==2&&!found.attempts,"query failure cannot produce an unvalidated allocation");
 World malformed;malformed.queryMode=2;found=Run(malformed);Check(!found.address&&found.invalidRegions==2&&!found.attempts,"invalid queried region is rejected without allocation");
 World empty;found=Run(empty);Check(!found.address&&found.queries<=(found.window.high-found.window.low)/0x10000+4&&!found.budgetExceeded&&!found.attempts,"busy queried-page regions terminate in a bounded scan");
 for(size_t offset:{size_t(0x1000),size_t(0x1008),size_t(0),size_t(0x200),size_t(0x400)}){
  const near_allocation::Reference ref{0x240000000,offset};const auto low=ref.next-0x80000000ULL-offset,high=ref.next+0x7FFFFFFFULL-offset;
  Check(near_allocation::Fits(low,&ref,1)&&!near_allocation::Fits(low-1,&ref,1),"inclusive INT32_MIN displacement edge and one-byte outside");
  Check(near_allocation::Fits(high,&ref,1)&&!near_allocation::Fits(high+1,&ref,1),"inclusive INT32_MAX displacement edge and one-byte outside");
  const auto window=near_allocation::Intersect(&ref,1,0x2000,1,0x10000,0x7FFFFFFEFFFF);
  Check(window.valid&&window.low==low&&window.high==high,"intersection includes exact signed edges for each targetoffset");
 }
 const auto window=near_allocation::Intersect(refs.data(),refs.size(),0x2000,0x10000,0x10000,0x7FFFFFFEFFFF);
 Check(window.valid&&window.low%0x10000==0&&window.high%0x10000==0&&near_allocation::Fits(window.low,refs.data(),refs.size())&&near_allocation::Fits(window.high,refs.data(),refs.size()),"all ten consumers share one aligned validated window");
 uintptr_t aligned=0;Check(!near_allocation::AlignUp(UINTPTR_MAX-7,0x10000,aligned),"alignment overflow rejected");
 near_allocation::Reference overflow{1,16};Check(!near_allocation::Fits(UINTPTR_MAX-7,&overflow,1),"targetoffset addition overflow rejected");
 overflow={UINTPTR_MAX,UINTPTR_MAX};Check(!near_allocation::Intersect(&overflow,1,0x2000,0x10000,0x10000,UINTPTR_MAX-1).valid,"impossible targetoffset window rejected without wrap");
 Check(!near_allocation::Intersect(refs.data(),refs.size(),0x2000,0,0x10000,0x7FFFFFFEFFFF).valid,"zero allocation granularity rejected");
 World imageOverflow;found=Run(imageOverflow,UINTPTR_MAX-7,16);Check(!found.address&&!found.queries,"image endpoint overflow cannot query or allocate");
 SYSTEM_INFO sys{};GetSystemInfo(&sys);auto* own=static_cast<uint8_t*>(static_cast<void*>(GetModuleHandleW(nullptr)));
 auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(own);auto* nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(own+dos->e_lfanew);
 const auto ownBase=reinterpret_cast<uintptr_t>(own);const auto ownRefs=References(ownBase);
 auto actual=near_allocation::Allocate({nullptr,RealQuery,RealAllocate,RealRelease},ownRefs.data(),ownRefs.size(),0x2000,sys.dwAllocationGranularity,
  reinterpret_cast<uintptr_t>(sys.lpMinimumApplicationAddress),reinterpret_cast<uintptr_t>(sys.lpMaximumApplicationAddress),ownBase,nt->OptionalHeader.SizeOfImage);
 Check(actual.address&&near_allocation::Fits(actual.address,ownRefs.data(),ownRefs.size()),"real own-process allocation satisfies every rel32 consumer");
 if(actual.address){MEMORY_BASIC_INFORMATION m{};VirtualQuery(reinterpret_cast<void*>(actual.address),&m,sizeof(m));Check(m.State==MEM_COMMIT&&m.Protect==PAGE_READWRITE,"real allocation has expected committed writable protection");RealRelease(nullptr,actual.address,0x2000);}
 printf("NearAllocation checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
#endif
