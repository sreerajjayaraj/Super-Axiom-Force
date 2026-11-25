#pragma once
#include <Windows.h>
#include <array>
#include <cstring>
#include "OwnedPatchProtocol.h"
#include "PatchTemplates.h"
#include "ProducerIdentity.h"
#include "ProducerMenuReachIdentity.h"
#include "ProducerDynamicIdentity.h"
namespace safcompat {
using ReadFn=bool(*)(void*,uintptr_t,void*,size_t);
enum class FaultPart:uint32_t{None,Shape,ProducerCode,Support,Scalar,Site};
struct Fault{FaultPart part=FaultPart::None;uintptr_t address=0;size_t size=0;bool readOk=false;std::array<uint8_t,16>expected{},actual{};};
inline bool Equal(ReadFn read,void*context,uintptr_t at,const uint8_t*expected,size_t size,Fault*f=nullptr,FaultPart part=FaultPart::Shape){
 std::array<uint8_t,4096>actual{};for(size_t o=0;o<size;){const size_t n=(size-o)<actual.size()?size-o:actual.size();const bool ok=read(context,at+o,actual.data(),n);if(!ok||memcmp(actual.data(),expected+o,n)){if(f){size_t first=0;if(ok)while(first<n&&actual[first]==expected[o+first])++first;f->part=part;f->address=at+o+first;f->size=(n-first)<16?n-first:16;f->readOk=ok;memcpy(f->expected.data(),expected+o+first,f->size);if(ok)memcpy(f->actual.data(),actual.data()+first,f->size);}return false;}o+=n;}return true;
}
inline bool Region(uintptr_t at,size_t n,uintptr_t owner,DWORD type,DWORD protection){
 MEMORY_BASIC_INFORMATION m{};return VirtualQuery(reinterpret_cast<void*>(at),&m,sizeof(m))==sizeof(m)&&m.State==MEM_COMMIT&&m.Type==type&&m.Protect==protection&&reinterpret_cast<uintptr_t>(m.AllocationBase)==owner&&at>=reinterpret_cast<uintptr_t>(m.BaseAddress)&&at-reinterpret_cast<uintptr_t>(m.BaseAddress)<=m.RegionSize&&n<=m.RegionSize-(at-reinterpret_cast<uintptr_t>(m.BaseAddress));
}
inline bool FinitePositive(uint32_t bits){return bits>0&&bits<0x7F800000u;}
class View {
 ReadFn read_=nullptr;void*context_=nullptr;uintptr_t base_=0;Snapshot state_{};
public:
 Result Configure(ReadFn read,void*context,uintptr_t base,const Snapshot&state,Fault*f=nullptr,bool dynamic=false){
  if(f){*f={};f->part=FaultPart::Shape;}
  read_=read;context_=context;base_=base;state_=state;
  if(!read||base!=0x140000000ULL||state.reserved||(state.generation&1)||state.status>1)return Result::Invalid;
  if(state.status==0){const bool valid=!state.gameBase&&!state.support&&!state.ownedMask&&!state.rangeBits&&!state.pullBits;if(valid&&f)f->part=FaultPart::None;return valid?Result::Valid:Result::Invalid;}
  if(state.gameBase!=base||state.support<0x10000||state.support>=0x800000000000ULL-0x2000||(state.support&0xFFF)||state.ownedMask&~0x7FFu||!FinitePositive(state.rangeBits)||!FinitePositive(state.pullBits))return Result::Invalid;
  for(uint32_t group:{0x3Fu,0x40u,0x80u,0x700u})if((state.ownedMask&group)&&(state.ownedMask&group)!=group)return Result::Invalid;
  // One owned allocation, with code/unwind/table RX and two separate RW scalar slots.
  if(!Region(state.support,0x1000,state.support,MEM_PRIVATE,PAGE_EXECUTE_READ)||!Region(state.support+0x1000,0x1000,state.support,MEM_PRIVATE,PAGE_READWRITE))return Result::Invalid;
  MEMORY_BASIC_INFORMATION after{};if(VirtualQuery(reinterpret_cast<void*>(state.support+0x2000),&after,sizeof(after))!=sizeof(after)||reinterpret_cast<uintptr_t>(after.AllocationBase)==state.support)return Result::Invalid;
  if(dynamic){
   constexpr size_t correctionSlot=0x460;
   constexpr std::array<uint8_t,8> nativeCorrection{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
   constexpr size_t tail=correctionSlot+nativeCorrection.size();
   if(!Equal(read,context,state.support,kSupportCode,correctionSlot,f,FaultPart::Support)||
      !Equal(read,context,state.support+correctionSlot,nativeCorrection.data(),nativeCorrection.size(),f,FaultPart::Support)||
      !Equal(read,context,state.support+tail,kSupportCode+tail,sizeof(kSupportCode)-tail,f,FaultPart::Support))return Result::Invalid;
  }else if(!Equal(read,context,state.support,kSupportCode,sizeof(kSupportCode),f,FaultPart::Support))return Result::Invalid;
  if(!Equal(read,context,state.support+0x1000,reinterpret_cast<const uint8_t*>(&state.rangeBits),4,f,FaultPart::Scalar)||!Equal(read,context,state.support+0x1008,reinterpret_cast<const uint8_t*>(&state.pullBits),4,f,FaultPart::Scalar))return Result::Invalid;
  for(size_t i=0;i<std::size(kSites);++i){const auto&s=kSites[i];uint8_t patch[16]{};if((state.ownedMask&(1u<<i))&&!ExpectedPatch(s,base,state.support,patch))return Result::Invalid;if(!Equal(read,context,base+s.rva,(state.ownedMask&(1u<<i))?patch:s.original.data(),s.size,f,FaultPart::Site))return Result::Invalid;}
  if(f)f->part=FaultPart::None;return Result::Valid;
 }
 static bool Read(void*opaque,uintptr_t at,void*out,size_t n){
  auto&v=*static_cast<View*>(opaque);if(!v.read_||!out||!n||n>UINTPTR_MAX-at||!v.read_(v.context_,at,out,n))return false;
  if(v.state_.status!=1)return true;auto*bytes=static_cast<uint8_t*>(out);
  for(size_t i=0;i<std::size(kSites);++i){if(!(v.state_.ownedMask&(1u<<i)))continue;const auto&s=kSites[i];const uintptr_t begin=v.base_+s.rva,end=begin+s.size;if(at>=end||at+n<=begin)continue;uint8_t patch[16]{};if(!ExpectedPatch(s,v.base_,v.state_.support,patch))return false;const uintptr_t first=at>begin?at:begin,last=at+n<end?at+n:end;for(uintptr_t a=first;a<last;++a){if(bytes[a-at]!=patch[a-begin])return false;bytes[a-at]=s.original[a-begin];}}
  return true;
 }
};
class Client {
 template<class Blocks>
 bool ValidateProducer(ReadFn read,void* context,const Blocks& blocks,uint32_t imageBaseOffset,
                       uint32_t descriptorRva,Fault* fault) const {
  for(const auto& block:blocks){
   if(!Region(module_+block.rva,block.size,module_,MEM_IMAGE,block.executable?PAGE_EXECUTE_READ:PAGE_READONLY))return false;
   if(block.rva){
    if(!Equal(read,context,module_+block.rva,block.bytes,block.size,fault,FaultPart::ProducerCode))return false;
    continue;
   }
   std::array<uint8_t,4096> header{};
   if(block.size>header.size()||imageBaseOffset+sizeof(module_)>block.size)return false;
   memcpy(header.data(),block.bytes,block.size);
   memcpy(header.data()+imageBaseOffset,&module_,sizeof(module_));
   if(!Equal(read,context,module_,header.data(),block.size,fault,FaultPart::ProducerCode))return false;
  }
  return Region(module_+descriptorRva,sizeof(Descriptor),module_,MEM_IMAGE,PAGE_READWRITE);
 }
 uintptr_t module_=0;Descriptor*descriptor_=nullptr;bool invalid_=false,resolved_=false,menuReach_=false,dynamic_=false;
public:
 explicit Client(bool nativeOnly=false):resolved_(nativeOnly){}
 bool Present()const{return module_&&descriptor_&&!invalid_;}
 bool Invalid()const{return invalid_;}
 uintptr_t Owner()const{return module_;}
 bool Identity(ReadFn read,void*context,Fault*f=nullptr)const{
  if(invalid_)return false;if(!module_)return true;
  if(dynamic_)return ValidateProducer(read,context,dynamic_producer::kBlocks,dynamic_producer::kImageBaseHeaderOffset,dynamic_producer::kDescriptorRva,f);
  return menuReach_
      ? ValidateProducer(read,context,menu_reach_producer::kBlocks,menu_reach_producer::kImageBaseHeaderOffset,menu_reach_producer::kDescriptorRva,f)
      : ValidateProducer(read,context,producer::kBlocks,producer::kImageBaseHeaderOffset,producer::kDescriptorRva,f);
 }
 bool Resolve(ReadFn read,void*context){
  if(resolved_)return !invalid_;resolved_=true;HMODULE module=nullptr;
  // Own a strong reference before reading an export, header or code byte. This
  // worker-only acquisition closes discovery/unload races before the pin.
  if(!GetModuleHandleExW(0,L"SuperAxiomForce.asi",&module)){if(GetLastError()==ERROR_MOD_NOT_FOUND)return true;invalid_=true;return false;}
  module_=reinterpret_cast<uintptr_t>(module);
  const auto symbol=GetProcAddress(module,"SAFOwnedPatchStateV1");
  const auto address=reinterpret_cast<uintptr_t>(symbol);
  const auto request=reinterpret_cast<uintptr_t>(GetProcAddress(module,"SAFMenuUnlimitedReachV1"));
  menuReach_=request==module_+menu_reach_producer::kMenuUnlimitedRva;
  const auto descriptorRva=menuReach_?menu_reach_producer::kDescriptorRva:producer::kDescriptorRva;
  if(menuReach_ && address==module_+descriptorRva && !Identity(read,context))
   dynamic_=true;
  if((request && !menuReach_)||address!=module_+descriptorRva||!Identity(read,context)){
   invalid_=true;FreeLibrary(module);return false;
  }
  HMODULE pinned=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(module_),&pinned)||pinned!=module){invalid_=true;FreeLibrary(module);return false;}
  descriptor_=reinterpret_cast<Descriptor*>(symbol);FreeLibrary(module);return true;
 }
 Result Open(ReadFn read,void*context,uintptr_t base,Lease&lease,View&view,Fault*f=nullptr)const{
  if(f)*f={};lease.state.producer=module_;
  if(!resolved_||!Identity(read,context,f)){if(f&&f->part==FaultPart::None)f->part=FaultPart::Shape;return Result::Invalid;}
  const auto status=lease.Acquire(descriptor_);lease.state.producer=module_;if(status!=Result::Valid){if(status==Result::Invalid&&f)f->part=FaultPart::Shape;return status;}
  lease.state.producer=module_;return view.Configure(read,context,base,lease.state,f,dynamic_);
 }
};
// Worker discovers/pins a provider into unpublished storage. A single release
// publication makes the fully immutable client visible to every callback.
// The native-only client stays process-lifetime stable while discovery is absent.
class PublishedClient {
 Client native_{true},producer_{},pending_{};std::atomic<const Client*>current_{&native_};bool pendingReady_=false,finished_=false;
public:
 bool Discover(ReadFn read,void*context){
  if(finished_)return false;if(pendingReady_)return true;
  Client candidate;if(!candidate.Resolve(read,context)||candidate.Present()){
   pending_=candidate;pendingReady_=true;return true;
  }
  return false;
 }
 bool Publish(){if(!pendingReady_||finished_)return false;producer_=pending_;current_.store(&producer_,std::memory_order_release);finished_=true;pendingReady_=false;return true;}
 bool Poll(ReadFn read,void*context){if(Discover(read,context))Publish();return !current_.load(std::memory_order_acquire)->Invalid();}
 Result Open(ReadFn read,void*context,uintptr_t base,Lease&lease,View&view,Fault*f=nullptr)const{return current_.load(std::memory_order_acquire)->Open(read,context,base,lease,view,f);}
 bool Current(uintptr_t owner)const{return current_.load(std::memory_order_acquire)->Owner()==owner;}
};
}
