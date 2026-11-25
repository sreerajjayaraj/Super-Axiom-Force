#pragma once
#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>
#include <cstdint>
#include <climits>
#include <type_traits>
#include <cstring>
#include <vector>
#define QUIESCED_DIAGNOSTICS_AVAILABLE 1
namespace quiesced_patch {
using QuerySystem=NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS,PVOID,ULONG,PULONG);
static_assert(sizeof(SYSTEM_PROCESS_INFORMATION)==0x100&&sizeof(SYSTEM_THREAD_INFORMATION)==0x50,"Reviewed x64 process/thread layout");
struct Check{const uint8_t*address=nullptr;const uint8_t*expected=nullptr;size_t size=0;};
struct Patch{uint8_t*address=nullptr;size_t size=0;uint8_t before[16]{},after[16]{};};
enum class Stage:uint8_t{None,Arguments,Resolver,BufferLimit,Snapshot,Enumeration,OpenThread,ThreadIdentity,Suspend,Context,BusyInstruction,NativeQuery,ThreadSet,NativeLayout,ExpectedBytes,ContractBytes,Protect,Flush,Restore};
inline const char* StageName(Stage s){
 switch(s){case Stage::None:return "none";case Stage::Arguments:return "arguments";case Stage::Resolver:return "query-resolver";case Stage::BufferLimit:return "query-buffer-limit";case Stage::Snapshot:return "thread-snapshot";case Stage::Enumeration:return "thread-enumeration";case Stage::OpenThread:return "thread-open";case Stage::ThreadIdentity:return "thread-owner";case Stage::Suspend:return "thread-suspend";case Stage::Context:return "thread-context";case Stage::BusyInstruction:return "instruction-busy";case Stage::NativeQuery:return "thread-query";case Stage::ThreadSet:return "thread-set-changed";case Stage::NativeLayout:return "thread-query-layout";case Stage::ExpectedBytes:return "expected-bytes";case Stage::ContractBytes:return "native-contract";case Stage::Protect:return "write-protect";case Stage::Flush:return "instruction-cache";case Stage::Restore:return "restore-protect";}
 return "unknown";
}
struct Report{
 Stage stage=Stage::None;DWORD error=0;LONG status=0;unsigned patch=UINT_MAX;DWORD thread=0;unsigned attempts=0,resumeFailures=0;DWORD resumeError=0,rollbackError=0;
 bool success=false,unchanged=true,protectionsRestored=true,rollbackOk=true,allResumed=false;
 void Fail(Stage where,DWORD code=0,LONG native=0,unsigned site=UINT_MAX,DWORD id=0){stage=where;error=code;status=native;patch=site;thread=id;}
};
static_assert(std::is_trivially_copyable<Report>::value&&std::is_standard_layout<Report>::value,"Frozen transaction diagnostics must remain plain bounded fields");
inline bool Recoverable(const Report&r){
 if(r.success||!r.unchanged||!r.protectionsRestored||!r.rollbackOk||!r.allResumed)return false;
 switch(r.stage){case Stage::Snapshot:case Stage::Enumeration:case Stage::OpenThread:case Stage::Suspend:case Stage::Context:case Stage::BusyInstruction:case Stage::NativeQuery:case Stage::ThreadSet:case Stage::Protect:case Stage::Flush:case Stage::Restore:return true;default:return false;}
}
struct Ops{
 decltype(&VirtualProtect)protect=&VirtualProtect;decltype(&FlushInstructionCache)flush=&FlushInstructionCache;
 decltype(&SuspendThread)suspend=&SuspendThread;decltype(&ResumeThread)resume=&ResumeThread;decltype(&GetThreadContext)context=&GetThreadContext;QuerySystem query=nullptr;
 decltype(&CreateToolhelp32Snapshot)snapshot=&CreateToolhelp32Snapshot;decltype(&Thread32First)first=&Thread32First;decltype(&Thread32Next)next=&Thread32Next;decltype(&OpenThread)open=&OpenThread;
};
class Threads{
 std::vector<HANDLE>handles;std::vector<unsigned char>suspended;std::vector<uint8_t>buffer;QuerySystem query=nullptr;ULONG needed=0;const Ops&ops;Report&report;
public:
 Threads(const Ops&o,Report&r):ops(o),report(r){}
 bool Prepare(){
  for(size_t i=0;i<handles.size();++i){if(i<suspended.size()&&suspended[i]){report.Fail(Stage::ThreadIdentity);return false;}CloseHandle(handles[i]);}
  handles.clear();suspended.clear();
  if(!query)query=ops.query?ops.query:reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));
  if(!query){report.Fail(Stage::Resolver,GetLastError());return false;}
  const size_t capacity=needed?static_cast<size_t>(needed)+0x100000:0x1000000;
  if(capacity>0x8000000){report.Fail(Stage::BufferLimit);return false;}
  if(buffer.size()<capacity)buffer.assign(capacity,0); // Allocate/touch only before any suspend.
  const DWORD process=GetCurrentProcessId(),self=GetCurrentThreadId();HANDLE snap=ops.snapshot(TH32CS_SNAPTHREAD,0);
  if(snap==INVALID_HANDLE_VALUE){report.Fail(Stage::Snapshot,GetLastError());return false;}
  THREADENTRY32 entry{};entry.dwSize=sizeof(entry);bool ok=true;
  if(ops.first(snap,&entry))do{
   if(entry.th32OwnerProcessID!=process||entry.th32ThreadID==self)continue;
   HANDLE h=ops.open(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION|SYNCHRONIZE,FALSE,entry.th32ThreadID);
   if(!h){const DWORD e=GetLastError();if(e==ERROR_INVALID_PARAMETER)continue;report.Fail(Stage::OpenThread,e,0,UINT_MAX,entry.th32ThreadID);ok=false;break;}
   if(GetProcessIdOfThread(h)!=process){const DWORD e=GetLastError();CloseHandle(h);report.Fail(Stage::ThreadIdentity,e,0,UINT_MAX,entry.th32ThreadID);ok=false;break;}
   handles.push_back(h);
  }while(ops.next(snap,&entry));
  else{report.Fail(Stage::Enumeration,GetLastError());ok=false;}
  if(ok){const DWORD e=GetLastError();if(e!=ERROR_NO_MORE_FILES){report.Fail(Stage::Enumeration,e);ok=false;}}
  CloseHandle(snap);suspended.assign(handles.size(),0);return ok;
 }
 bool Freeze(const Patch*patches,size_t count,bool&busy){
  busy=false;report.allResumed=false;
  for(size_t i=0;i<handles.size();++i){
   if(ops.suspend(handles[i])==DWORD(-1)){const DWORD e=GetLastError();report.Fail(Stage::Suspend,e,0,UINT_MAX,GetThreadId(handles[i]));return false;}suspended[i]=1;
  }
  for(HANDLE h:handles){
   CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL;
   if(!ops.context(h,&c)){const DWORD e=GetLastError();report.Fail(Stage::Context,e,0,UINT_MAX,GetThreadId(h));return false;}
   for(size_t i=0;i<count;++i){const auto begin=reinterpret_cast<uintptr_t>(patches[i].address);
    if(c.Rip>=begin&&c.Rip-begin<patches[i].size){busy=true;report.Fail(Stage::BusyInstruction,0,0,static_cast<unsigned>(i),GetThreadId(h));return false;}}
  }
  ULONG length=0;const auto status=query(SystemProcessInformation,buffer.data(),static_cast<ULONG>(buffer.size()),&length);
  if(status<0||length>buffer.size()){needed=length;busy=true;report.Fail(Stage::NativeQuery,0,status);return false;}
  const DWORD process=GetCurrentProcessId(),self=GetCurrentThreadId();bool found=false;
  for(size_t at=0;at<length;){
   if(length-at<sizeof(SYSTEM_PROCESS_INFORMATION)){report.Fail(Stage::NativeLayout);return false;}
   const auto*r=reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data()+at);
   const size_t n=r->NextEntryOffset?r->NextEntryOffset:length-at;
   if(n<sizeof(*r)||n>length-at){report.Fail(Stage::NativeLayout);return false;}
   if(reinterpret_cast<uintptr_t>(r->UniqueProcessId)==process){
    found=true;if(r->NumberOfThreads>(n-sizeof(*r))/sizeof(SYSTEM_THREAD_INFORMATION)){report.Fail(Stage::NativeLayout);return false;}
    const auto*list=reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(r+1);
    for(ULONG i=0;i<r->NumberOfThreads;++i){const auto id=reinterpret_cast<uintptr_t>(list[i].ClientId.UniqueThread);if(id==self)continue;
     bool known=false;for(HANDLE h:handles)if(GetThreadId(h)==id){known=true;break;}
     if(!known){busy=true;report.Fail(Stage::ThreadSet,0,0,UINT_MAX,static_cast<DWORD>(id));return false;}}
    break;
   }
   if(!r->NextEntryOffset)break;at+=r->NextEntryOffset;
  }
  if(!found){report.Fail(Stage::NativeLayout);return false;}return true;
 }
 bool Thaw(){
  // Do not return to logging, allocation, or heap destruction until every
  // owned increment is resumed or its thread is confirmed terminated.
  for(;;){bool pending=false;
   for(size_t i=0;i<handles.size();++i)if(suspended[i]){
    if(ops.resume(handles[i])!=DWORD(-1))suspended[i]=0;
    else{const DWORD e=GetLastError();if(report.resumeFailures<UINT_MAX)++report.resumeFailures;report.resumeError=e;
     if(WaitForSingleObject(handles[i],0)==WAIT_OBJECT_0)suspended[i]=0;else pending=true;}}
   if(!pending){report.allResumed=true;return true;}Sleep(0);
  }
 }
 ~Threads(){Thaw();for(HANDLE h:handles)CloseHandle(h);}
};
inline bool SameBytes(const uint8_t* address, const uint8_t* expected, size_t size) noexcept {
 MEMORY_BASIC_INFORMATION region{};
 constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
 if(!VirtualQuery(address,&region,sizeof(region)) || region.State!=MEM_COMMIT ||
     !(region.Protect&readable) || (region.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
 const auto offset=reinterpret_cast<uintptr_t>(address)-reinterpret_cast<uintptr_t>(region.BaseAddress);
 if(offset>=region.RegionSize||size>region.RegionSize-offset)return false;
 __try{return std::memcmp(address,expected,size)==0;}
 __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION||GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR?
     EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
inline bool Write(const Patch*patches,size_t count,const Ops&ops=Ops{},Report*out=nullptr,const Check*checks=nullptr,size_t checkCount=0){
 Report local{};Report&r=out?*out:local;r={};
 if(!patches||!count||count>32){r.Fail(Stage::Arguments);r.allResumed=true;return false;}
 for(size_t i=0;i<count;++i)if(!patches[i].address||!patches[i].size||patches[i].size>16){r.Fail(Stage::Arguments,0,0,static_cast<unsigned>(i));r.allResumed=true;return false;}
 // Adaptive checks include whole function bodies and their branch witnesses.
 // Bound the total frozen read, while retaining the 16-byte write limit.
 constexpr size_t maxChecks = 512, maxCheckSize = 0x4000, maxCheckedBytes = 0x10000;
 if(checkCount>maxChecks||(checkCount&&!checks)){r.Fail(Stage::Arguments);r.allResumed=true;return false;}
 size_t checkedBytes=0;
 for(size_t i=0;i<checkCount;++i){
  const auto& check=checks[i];
  if(!check.address||!check.expected||!check.size||check.size>maxCheckSize||check.size>maxCheckedBytes-checkedBytes){r.Fail(Stage::Arguments);r.allResumed=true;return false;}
  checkedBytes+=check.size;
 }
 Threads threads(ops,r);if(!threads.Prepare()){threads.Thaw();return false;}
 for(unsigned attempt=0;attempt<8;++attempt){
  r.attempts=attempt+1;bool busy=false;
  if(!threads.Freeze(patches,count,busy)){
   threads.Thaw();if(!busy)return false;
   if(attempt+1<8){Sleep(1);if(!threads.Prepare()){threads.Thaw();return false;}}continue;
  }
  bool ok=true,written=false;size_t protectedCount=0;DWORD protections[32]{};
  for(size_t i=0;i<count;++i)if(!SameBytes(patches[i].address,patches[i].before,patches[i].size)){r.Fail(Stage::ExpectedBytes,0,0,static_cast<unsigned>(i));ok=false;break;}
  if(ok)for(size_t i=0;i<checkCount;++i)if(!SameBytes(checks[i].address,checks[i].expected,checks[i].size)){r.Fail(Stage::ContractBytes,0,0,static_cast<unsigned>(i));ok=false;break;}
  if(ok)for(size_t i=0;i<count;++i){
   if(!ops.protect(patches[i].address,patches[i].size,PAGE_EXECUTE_READWRITE,&protections[i])){r.Fail(Stage::Protect,GetLastError(),0,static_cast<unsigned>(i));ok=false;break;}++protectedCount;
  }
  if(ok){
   for(size_t i=0;i<count;++i)memcpy(patches[i].address,patches[i].after,patches[i].size);written=true;
   for(size_t i=0;i<count;++i)if(!ops.flush(GetCurrentProcess(),patches[i].address,patches[i].size)){const DWORD e=GetLastError();if(ok)r.Fail(Stage::Flush,e,0,static_cast<unsigned>(i));ok=false;}
  }
  if(!ok&&written){
   for(size_t i=0;i<count;++i)memcpy(patches[i].address,patches[i].before,patches[i].size);
   for(size_t i=0;i<count;++i)if(!ops.flush(GetCurrentProcess(),patches[i].address,patches[i].size)){r.rollbackOk=false;r.rollbackError=GetLastError();}
   written=false;
  }
  bool restored=true;
  for(size_t i=protectedCount;i>0;--i){DWORD ignored=0;if(!ops.protect(patches[i-1].address,patches[i-1].size,protections[i-1],&ignored)){const DWORD e=GetLastError();if(restored&&ok)r.Fail(Stage::Restore,e,0,static_cast<unsigned>(i-1));restored=false;}}
  if(!restored){
   if(written){
    bool writable=true;
    for(size_t i=0;i<count;++i){DWORD ignored=0;if(!ops.protect(patches[i].address,patches[i].size,PAGE_EXECUTE_READWRITE,&ignored)){writable=false;r.rollbackError=GetLastError();}}
    if(writable){
     for(size_t i=0;i<count;++i)memcpy(patches[i].address,patches[i].before,patches[i].size);
     for(size_t i=0;i<count;++i)if(!ops.flush(GetCurrentProcess(),patches[i].address,patches[i].size)){r.rollbackOk=false;r.rollbackError=GetLastError();}
     written=false;
    }else r.rollbackOk=false;
   }
   bool finalRestore=true;
   for(size_t i=protectedCount;i>0;--i){DWORD ignored=0;if(!ops.protect(patches[i-1].address,patches[i-1].size,protections[i-1],&ignored)){finalRestore=false;r.rollbackError=GetLastError();}}
   r.protectionsRestored=finalRestore;
  }
  r.unchanged=true;for(size_t i=0;i<count;++i)if(memcmp(patches[i].address,patches[i].before,patches[i].size))r.unchanged=false;
  threads.Thaw();r.success=ok&&restored&&r.allResumed;
  if(r.success){r.stage=Stage::None;r.error=0;r.status=0;r.patch=UINT_MAX;r.thread=0;}
  return r.success;
 }
 return false;
}
} // namespace quiesced_patch
