#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
namespace safcompat {
inline constexpr uint64_t kMagic=0x31564E574F464153ULL;
inline constexpr uint32_t kVersion=0x35760001,kWriter=0x80000000,kMaxReaders=64;
enum class Status:uint32_t{Native=0,Ready=1,Poison=2};
enum class Result{Valid,Busy,Invalid};
struct Descriptor {
 uint64_t magic=kMagic;uint32_t version=kVersion,bytes=sizeof(Descriptor);
 std::atomic<uint32_t>gate{0};uint32_t status=0;uint64_t generation=0;
 uintptr_t gameBase=0,support=0;uint32_t ownedMask=0,rangeBits=0,pullBits=0,reserved=0;
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
struct Snapshot{uint32_t status=0;uint64_t generation=0;uintptr_t gameBase=0,support=0;uint32_t ownedMask=0,rangeBits=0,pullBits=0,reserved=0;uintptr_t producer=0;};
class Lease {
 Descriptor*d_=nullptr;
public:
 Snapshot state{};
 Lease()=default;Lease(const Lease&)=delete;Lease&operator=(const Lease&)=delete;
 ~Lease(){Release();}
 void Release(){if(d_){d_->gate.fetch_sub(1,std::memory_order_release);d_=nullptr;}}
 Result Acquire(Descriptor*d){
  Release();if(!d)return Result::Valid;
  if(d->magic!=kMagic||d->version!=kVersion||d->bytes!=sizeof(Descriptor))return Result::Invalid;
  uint32_t gate=d->gate.load(std::memory_order_acquire);
  if(gate&kWriter)return gate==kWriter?Result::Busy:Result::Invalid;
  if(gate>=kMaxReaders)return Result::Invalid;
  if(!d->gate.compare_exchange_strong(gate,gate+1,std::memory_order_acq_rel))return gate==kWriter||gate<kMaxReaders?Result::Busy:Result::Invalid;
  d_=d;state={d->status,d->generation,d->gameBase,d->support,d->ownedMask,d->rangeBits,d->pullBits,d->reserved};
  if((state.generation&1)||state.status>static_cast<uint32_t>(Status::Ready)||state.reserved){Release();return Result::Invalid;}
  return Result::Valid;
 }
};
inline bool BeginWrite(Descriptor&d){uint32_t expected=0;if(!d.gate.compare_exchange_strong(expected,kWriter,std::memory_order_acq_rel))return false;++d.generation;return true;}
inline void EndWrite(Descriptor&d){++d.generation;d.gate.store(0,std::memory_order_release);}
}
