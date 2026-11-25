#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace near_allocation {
struct Reference { uintptr_t next=0; size_t targetOffset=0; };
struct Window { uintptr_t low=0,high=0; bool valid=false; };
struct Region { uintptr_t base=0; size_t size=0; bool free=false; };
struct Api {
 void* context=nullptr;
 bool (*query)(void*,uintptr_t,Region&)=nullptr;
 uintptr_t (*allocate)(void*,uintptr_t,size_t)=nullptr;
 void (*release)(void*,uintptr_t,size_t)=nullptr;
};
struct Result {
 uintptr_t address=0;
 Window window{};
 uint32_t queries=0,attempts=0,allocationFailures=0,queryFailures=0,invalidRegions=0,releasedUnexpected=0;
 bool searchedAbove=false,searchedBelow=false,budgetExceeded=false;
};
inline bool AlignUp(uintptr_t value,size_t granularity,uintptr_t& aligned){
 if(!granularity)return false;const uintptr_t rem=value%granularity;
 const uintptr_t add=rem?granularity-rem:0;
 if(add>UINTPTR_MAX-value)return false;aligned=value+add;return true;
}
inline uintptr_t AlignDown(uintptr_t value,size_t granularity){return value-value%granularity;}
inline Window Intersect(const Reference* references,size_t count,size_t bytes,size_t granularity,
                        uintptr_t minimum,uintptr_t maximum){
 Window w{};
 if(!references||!count||!bytes||!granularity||maximum<minimum||bytes-1>maximum-minimum)return w;
 w.low=minimum;w.high=maximum-(bytes-1);
 // Keep exclusive allocation ends representable as well as its last byte.
 if(w.high>UINTPTR_MAX-bytes)w.high=UINTPTR_MAX-bytes;
 for(size_t i=0;i<count;++i){
  const auto& r=references[i];
  const uintptr_t minTarget=r.next>=0x80000000ULL?r.next-0x80000000ULL:0;
  const uintptr_t maxTarget=r.next<=UINTPTR_MAX-0x7FFFFFFFULL?r.next+0x7FFFFFFFULL:UINTPTR_MAX;
  if(maxTarget<r.targetOffset)return Window{};
  const uintptr_t low=minTarget>r.targetOffset?minTarget-r.targetOffset:0;
  const uintptr_t high=maxTarget-r.targetOffset;
  if(w.low<low)w.low=low;if(w.high>high)w.high=high;
 }
 uintptr_t first=0;if(w.low>w.high||!AlignUp(w.low,granularity,first))return Window{};
 w.low=first;w.high=AlignDown(w.high,granularity);w.valid=w.low<=w.high;return w;
}
inline bool Fits(uintptr_t base,const Reference* references,size_t count){
 if(!references||!count)return false;
 for(size_t i=0;i<count;++i){const auto&r=references[i];if(r.targetOffset>UINTPTR_MAX-base)return false;
  const uintptr_t target=base+r.targetOffset;
  if(target>=r.next){if(target-r.next>0x7FFFFFFFULL)return false;}
  else if(r.next-target>0x80000000ULL)return false;
 }
 return true;
}
inline Result Allocate(const Api& api,const Reference* references,size_t count,
                       size_t bytes,size_t granularity,uintptr_t minimum,uintptr_t maximum,
                       uintptr_t imageBase,size_t imageSize){
 Result out{};out.window=Intersect(references,count,bytes,granularity,minimum,maximum);
 if(!out.window.valid||!api.query||!api.allocate||!api.release||imageSize>UINTPTR_MAX-imageBase)return out;
 const auto w=out.window;const uintptr_t imageEnd=imageBase+imageSize;
 const uint64_t slots=static_cast<uint64_t>((w.high-w.low)/granularity)+1;
 const uint64_t budget=slots<=((std::numeric_limits<uint32_t>::max)()-8)/2?slots*2+8:(std::numeric_limits<uint32_t>::max)();
 auto query=[&](uintptr_t at,Region& region){
  if(out.queries>=budget){out.budgetExceeded=true;return false;}++out.queries;
  if(!api.query(api.context,at,region)){++out.queryFailures;return false;}
  if(!region.size||region.base>at||region.size>UINTPTR_MAX-region.base||region.base+region.size<=at){++out.invalidRegions;return false;}
  return true;
 };
 auto attempt=[&](uintptr_t candidate){
  // Revalidate and retry once after an allocation race/transient failure.
  // A failed page never causes the remainder of a free region to be skipped.
  for(unsigned retry=0;retry<2;++retry){
   if(retry){Region now{};if(!query(candidate,now)||!now.free||candidate<now.base||bytes>now.base+now.size-candidate)return false;}
   ++out.attempts;const uintptr_t actual=api.allocate(api.context,candidate,bytes);
   if(!actual){++out.allocationFailures;continue;}
   if(actual!=candidate||actual<w.low||actual>w.high||!Fits(actual,references,count)){
    api.release(api.context,actual,bytes);++out.releasedUnexpected;return false;
   }
   out.address=actual;return true;
  }
  return false;
 };
 auto scan=[&](uintptr_t start,bool upward){
  uintptr_t at=start;
  while(at>=w.low&&at<=w.high){
   Region region{};if(!query(at,region))break;
   const uintptr_t end=region.base+region.size;
   if(region.free&&region.size>=bytes){
    uintptr_t candidate=at;
    if(upward){if(candidate<region.base)candidate=region.base;if(!AlignUp(candidate,granularity,candidate))break;}
    else {const uintptr_t last=end-bytes;if(candidate>last)candidate=last;candidate=AlignDown(candidate,granularity);}
    if(candidate>=region.base&&candidate>=w.low&&candidate<=w.high&&candidate<=end&&bytes<=end-candidate){
     if(attempt(candidate))return;
     if(out.releasedUnexpected)return; // A broken allocator response fails closed.
     if(upward){if(granularity>UINTPTR_MAX-candidate)break;at=candidate+granularity;}
     else {if(candidate<granularity||candidate-granularity<w.low)break;at=candidate-granularity;}
     continue;
    }
   }
   if(upward){if(!AlignUp(end,granularity,at))break;}
   else {if(region.base==0)break;at=AlignDown(region.base-1,granularity);}
  }
 };
 uintptr_t above=0;
 const uintptr_t firstAbove=imageEnd>w.low?imageEnd:w.low;
 if(AlignUp(firstAbove,granularity,above)&&above<=w.high){out.searchedAbove=true;scan(above,true);}
 if(out.address||out.releasedUnexpected)return out;
 if(imageBase>=bytes){uintptr_t below=imageBase-bytes;if(below>w.high)below=w.high;below=AlignDown(below,granularity);
  if(below>=w.low){out.searchedBelow=true;scan(below,false);}}
 return out;
}
} // namespace near_allocation
