#include "../src/SuperAxiomForce/QuiescedPatch.h"
#include <atomic>
#include <cstdio>
#include <thread>
namespace {
int checks=0,failures=0,held=0,suspends=0,resumes=0,contexts=0,protects=0,flushes=0;
int failSuspend=0,failContext=0,failProtect=0,failFlush=0,failResume=0,busyAttempts=0;
int failResumeCount=0,queryCalls=0,queryMode=0;
quiesced_patch::QuerySystem realQuery=nullptr;
bool closureOrderOk=true;
uintptr_t busyIP=0;
void Check(bool yes,const char*n){++checks;if(!yes){++failures;printf("FAIL %s\n",n);}}
DWORD WINAPI Suspend(HANDLE){++suspends;if(suspends==failSuspend)return DWORD(-1);++held;return 0;}
DWORD WINAPI Resume(HANDLE){++resumes;if(resumes==failResume||resumes<=failResumeCount)return DWORD(-1);--held;return 1;}
BOOL WINAPI Context(HANDLE,LPCONTEXT c){++contexts;if(contexts==failContext)return FALSE;c->Rip=contexts<=busyAttempts?busyIP:1;return TRUE;}
BOOL WINAPI Protect(LPVOID p,SIZE_T n,DWORD x,PDWORD old){++protects;if(protects==failProtect)return FALSE;return VirtualProtect(p,n,x,old);}
BOOL WINAPI Flush(HANDLE h,LPCVOID p,SIZE_T n){++flushes;if(flushes==failFlush)return FALSE;return FlushInstructionCache(h,p,n);}
NTSTATUS NTAPI Query(SYSTEM_INFORMATION_CLASS c,PVOID b,ULONG n,PULONG size){
 closureOrderOk=closureOrderOk&&contexts>=held;
 ++queryCalls;if(queryCalls==1&&queryMode==2){*size=0x2000000;return static_cast<NTSTATUS>(0xC0000004);}
 const auto status=realQuery(c,b,n,size);if(status<0)return status;
 if(queryCalls==1&&queryMode==1){auto*p=static_cast<unsigned char*>(b);for(ULONG at=0;at<*size;){auto*r=reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(p+at);
  if(reinterpret_cast<uintptr_t>(r->UniqueProcessId)==GetCurrentProcessId()&&r->NumberOfThreads){auto*t=reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(r+1);t[0].ClientId.UniqueThread=reinterpret_cast<HANDLE>(uintptr_t{0x7FFFFFFF});break;}
  if(!r->NextEntryOffset)break;at+=r->NextEntryOffset;}}
 return status;
}
void Reset(){held=suspends=resumes=contexts=protects=flushes=0;failSuspend=failContext=failProtect=failFlush=failResume=busyAttempts=failResumeCount=queryCalls=queryMode=0;}
}
int main(){
 std::atomic<bool> stop=false;std::thread thread([&](){while(!stop.load())Sleep(1);});
 auto*m=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x2000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!m)return 2;
 quiesced_patch::Patch p[2]{};for(unsigned i=0;i<2;++i){p[i].address=m+i*0x1000;p[i].size=10;memset(p[i].before,0x90,10);memset(p[i].after,0xCC,10);memcpy(p[i].address,p[i].before,10);}
 quiesced_patch::Ops ops{Protect,Flush,Suspend,Resume,Context};busyIP=reinterpret_cast<uintptr_t>(m+3);
 realQuery=reinterpret_cast<quiesced_patch::QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));ops.query=Query;
 const auto original=[&](){return !memcmp(m,p[0].before,10)&&!memcmp(m+0x1000,p[1].before,10);};
 for(int scenario=0;scenario<6;++scenario){Reset();if(scenario==0)failSuspend=1;if(scenario==1)failContext=1;if(scenario==2)failProtect=2;if(scenario==3)failFlush=2;if(scenario==4)failProtect=3;if(scenario==5)busyAttempts=9999;
  Check(!quiesced_patch::Write(p,2,ops),"injected freeze/protection/cache/busy failure rejects group");
  Check(original(),"every early or late failure preserves all original code bytes");Check(held==0,"failure path resumes every owned suspend increment");
 }
 Reset();busyAttempts=1;Check(quiesced_patch::Write(p,2,ops),"busy instruction retries only after thaw then succeeds");
 Check(!memcmp(m,p[0].after,10)&&!memcmp(m+0x1000,p[1].after,10),"success applies complete group");Check(held==0&&suspends>1,"successful retry leaves no suspended threads");
 for(auto&x:p){auto tmp=x;memcpy(x.before,tmp.after,10);memcpy(x.after,tmp.before,10);}Reset();failResume=1;
 Check(quiesced_patch::Write(p,2,ops),"transient resume failure is retried");Check(held==0,"resume retry restores every suspend increment");
 Check(!memcmp(m,p[0].after,10)&&!memcmp(m+0x1000,p[1].after,10),"restoration transaction applies exact originals");
 for(int mode:{1,2}){for(auto&x:p){auto tmp=x;memcpy(x.before,tmp.after,10);memcpy(x.after,tmp.before,10);}Reset();queryMode=mode;failResumeCount=3;
  Check(quiesced_patch::Write(p,2,ops),"unknown thread or too-small native snapshot re-prepares outside freeze");
  Check(queryCalls>=2&&held==0&&resumes>3,"closure retry and three failed resumes leave no suspended leak");}
 for(auto&x:p){auto tmp=x;memcpy(x.before,tmp.after,10);memcpy(x.after,tmp.before,10);}Reset();failResumeCount=17;
 Check(quiesced_patch::Write(p,2,ops)&&held==0&&resumes>=18,"cleanup cannot return during seventeen failed resumes before eventual success");
 // Exercise the real Windows suspend/context/resume path as well as injected failures.
 for(auto&x:p){auto tmp=x;memcpy(x.before,tmp.after,10);memcpy(x.after,tmp.before,10);}
 Check(quiesced_patch::Write(p,2),"real own-process Windows quiescence transaction");
 Check(closureOrderOk,"all suspend context acknowledgements precede final thread-set query");
 stop=true;thread.join();VirtualFree(m,0,MEM_RELEASE);
 printf("QuiescedPatch checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
