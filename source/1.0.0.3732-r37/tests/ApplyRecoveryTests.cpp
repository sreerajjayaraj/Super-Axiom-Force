#define SAF_APPLY_RECOVERY_TESTING
#ifdef SAF_R18_BASELINE
#include "E:/CD_Mods_Master/work/update-1.0.0.3527-20261104/SAF/src/SuperAxiomForce/SuperAxiomForce.cpp"
#else
#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"
#endif
#include <cstdio>
#include <filesystem>
static unsigned checks=0,failures=0;
static void Check(bool yes,const char* label){++checks;if(!yes){++failures;printf("FAIL %s\n",label);}}
static BytePatchSite PatchAt(uint8_t* p,size_t n,uint8_t before,uint8_t after){
 BytePatchSite s{};s.address=p;s.size=n;s.canRestore=true;
 memset(s.original,before,n);memset(s.patch,after,n);memcpy(p,s.original,n);return s;
}
#ifdef SAF_APPLY_RECOVERY_AVAILABLE
static void CaptureRetryContractsForTesting(){
 g_retryContracts.clear();g_retryContractReady=kAllFeatureBits;
 const std::vector<BytePatchSite>* groups[]={&g_scopedRangeSites,&g_scopedPullSites,&g_angularSites,&g_instantAxiomSites};
 for(unsigned i=0;i<4;++i)for(const auto&s:*groups[i])AddRetryContract(1u<<i,s.address,s.size);
}
#endif
static void Ini(bool enabled=true,bool instant=true){
 std::ofstream f(g_iniPath,std::ios::trunc);f<<"[AxiomForce]\nEnabled="<<enabled<<"\nRange=500\nPullSpeed=1000\nStraightPull=true\nInstantAxiom="<<instant<<"\n";
}
static void Tick(ULONGLONG now){
#ifdef SAF_APPLY_RECOVERY_AVAILABLE
 ProcessPendingApply(now);
#else
 (void)now;
 // Exact r18 watcher predicate: after its one LoadAndApply it only responds to
 // an INI timestamp change. There is no pending application service in r18.
 ULONGLONG current=0;if(GetFileModTime(&current)&&current!=g_lastIniModTime){g_lastIniModTime=current;LoadAndApply();}
#endif
}

#ifdef SAF_APPLY_RECOVERY_AVAILABLE
static uint8_t*fixture=nullptr;
enum class Fault{None,Snapshot,Enumeration,Open,Suspend,Context,Query,ThreadSet,Protect,Flush,Restore,Resume,RollbackCache,ContractRace};
static Fault fault=Fault::None;
static unsigned remaining=0,held=0,protectCalls=0,suspendCalls=0,queryCalls=0,flushCalls=0;
static quiesced_patch::QuerySystem nativeQuery=nullptr;
static bool Fail(Fault what){if(fault!=what||!remaining)return false;--remaining;SetLastError(1234);return true;}
static HANDLE WINAPI Snapshot(DWORD flags,DWORD pid){if(Fail(Fault::Snapshot))return INVALID_HANDLE_VALUE;return CreateToolhelp32Snapshot(flags,pid);}
static BOOL WINAPI First(HANDLE h,LPTHREADENTRY32 e){if(Fail(Fault::Enumeration))return FALSE;return Thread32First(h,e);}
static HANDLE WINAPI Open(DWORD access,BOOL inherit,DWORD id){if(Fail(Fault::Open))return nullptr;return OpenThread(access,inherit,id);}
static DWORD WINAPI Suspend(HANDLE h){++suspendCalls;if(Fail(Fault::Suspend))return DWORD(-1);auto n=SuspendThread(h);if(n!=DWORD(-1))++held;return n;}
static DWORD WINAPI Resume(HANDLE h){if(Fail(Fault::Resume))return DWORD(-1);auto n=ResumeThread(h);if(n!=DWORD(-1))--held;return n;}
static BOOL WINAPI Context(HANDLE h,LPCONTEXT c){if(Fail(Fault::Context))return FALSE;return GetThreadContext(h,c);}
static BOOL WINAPI Protect(LPVOID p,SIZE_T n,DWORD protection,PDWORD old){
 ++protectCalls;if(Fail(Fault::Protect)||(fault==Fault::Restore&&protectCalls==2)){SetLastError(1234);return FALSE;}return VirtualProtect(p,n,protection,old);
}
static BOOL WINAPI Flush(HANDLE h,LPCVOID p,SIZE_T n){++flushCalls;if(Fail(Fault::Flush)||fault==Fault::RollbackCache){SetLastError(1234);return FALSE;}return FlushInstructionCache(h,p,n);}
static NTSTATUS NTAPI Query(SYSTEM_INFORMATION_CLASS c,PVOID b,ULONG n,PULONG size){
 ++queryCalls;
 if(Fail(Fault::Query)){*size=0;return static_cast<NTSTATUS>(0xC0000001);}
 auto status=nativeQuery(c,b,n,size);if(status<0)return status;
 if(Fail(Fault::ThreadSet)){auto*p=static_cast<uint8_t*>(b);for(ULONG at=0;at<*size;){auto*r=reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(p+at);
  if(reinterpret_cast<uintptr_t>(r->UniqueProcessId)==GetCurrentProcessId()&&r->NumberOfThreads){reinterpret_cast<SYSTEM_THREAD_INFORMATION*>(r+1)[0].ClientId.UniqueThread=reinterpret_cast<HANDLE>(uintptr_t{0x7FFFFFFF});break;}
  if(!r->NextEntryOffset)break;at+=r->NextEntryOffset;}}
 if(fault==Fault::ContractRace)fixture[32]^=1;return status;
}
static quiesced_patch::Ops FaultOps(){
 quiesced_patch::Ops o{Protect,Flush,Suspend,Resume,Context};o.query=Query;o.snapshot=Snapshot;o.first=First;o.open=Open;return o;
}
static std::string LogText(){std::ifstream f(g_logPath);return {(std::istreambuf_iterator<char>(f)),{}};}
static void ResetFixture(){
 g_testPatchOps=nullptr;g_recovery={};fault=Fault::None;remaining=held=protectCalls=suspendCalls=queryCalls=flushCalls=0;
 const std::vector<BytePatchSite>*groups[]={&g_scopedRangeSites,&g_scopedPullSites,&g_angularSites,&g_instantAxiomSites};
 for(const auto*group:groups)for(const auto&s:*group)memcpy(s.address,s.original,s.size);
 for(auto*group:{&g_scopedRangeSites,&g_scopedPullSites,&g_angularSites,&g_instantAxiomSites})for(auto&s:*group)s.owned=false;
 *g_range.address=g_range.original;g_range.owned=false;*g_pull.address=g_pull.original;g_pull.owned=false;
 memset(fixture+32,0xA5,8);CaptureRetryContractsForTesting();AddRetryContract(kInstantBit,fixture+32,8);
 Ini(true,false);{std::ofstream clear(g_logPath,std::ios::trunc);}LoadAndApply();Ini(true,true);
}
static void ExtendedRecovery(uint8_t*m){
 fixture=m;nativeQuery=reinterpret_cast<quiesced_patch::QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation"));
 std::atomic<bool>stop=false;std::thread parked([&](){while(!stop.load())Sleep(1);});
 const auto ops=FaultOps();
 for(auto scenario:{Fault::Snapshot,Fault::Enumeration,Fault::Open,Fault::Suspend,Fault::Context,Fault::Query,Fault::ThreadSet,Fault::Protect,Fault::Flush,Fault::Restore}){
  ResetFixture();fault=scenario;remaining=(scenario==Fault::Query||scenario==Fault::ThreadSet)?1000:1;g_testPatchOps=&ops;LoadAndApply();
  Check((g_recovery.pending&kInstantBit)!=0&&!g_instantAxiomSites[0].owned&&!memcmp(m,g_instantAxiomSites[0].original,2),"recoverable unchanged transaction retains bounded pending request");
  Check(held==0,"diagnostic failure path releases all actual owned thread suspensions before logging");
  const char*stage=scenario==Fault::Snapshot?"thread-snapshot":scenario==Fault::Enumeration?"thread-enumeration":scenario==Fault::Open?"thread-open":scenario==Fault::Suspend?"thread-suspend":scenario==Fault::Context?"thread-context":scenario==Fault::Query?"thread-query":scenario==Fault::ThreadSet?"thread-set-changed":scenario==Fault::Protect?"write-protect":scenario==Fault::Flush?"instruction-cache":"restore-protect";
  Check(LogText().find(std::string("stage=")+stage)!=std::string::npos,"diagnostic identifies exact failed transaction stage");
  if(scenario!=Fault::Query&&scenario!=Fault::ThreadSet)Check(LogText().find("error=1234")!=std::string::npos,"diagnostic preserves immediate injected Win32 error");
  if(scenario==Fault::Query)Check(LogText().find("ntstatus=0xC0000001")!=std::string::npos&&queryCalls==8,"native-query status and eight internal attempts are precise");
  {std::ofstream out("ApplyRecovery.fault-"+std::to_string(static_cast<unsigned>(scenario))+".log");out<<LogText();}
  fault=Fault::None;Tick(g_recovery.due[3]);
  Check(g_instantAxiomSites[0].owned&&!(g_recovery.pending&kInstantBit)&&g_recovery.attempts[3]==1&&held==0,"pending retry uses fresh transaction and succeeds after transient failure");
  const auto previous=protectCalls;Tick(GetTickCount64()+1000000);
  Check(protectCalls==previous,"successful recovery leaves no background repatch loop");
 }
 ResetFixture();fault=Fault::Resume;remaining=17;g_testPatchOps=&ops;LoadAndApply();
 Check(g_instantAxiomSites[0].owned&&!g_recovery.pending&&held==0,"seventeen resume failures eventually release every owned suspend before successful publication");
 Check(LogText().find("resume_failures=17")!=std::string::npos&&LogText().find("resume_error=1234")!=std::string::npos,"bounded diagnostics retain resume failure count and original error after eventual release");
 ResetFixture();fault=Fault::Flush;remaining=1;g_testPatchOps=&ops;LoadAndApply();fault=Fault::ContractRace;
 Tick(g_recovery.due[3]);
 Check(!g_instantAxiomSites[0].owned&&!memcmp(m,g_instantAxiomSites[0].original,2)&&!g_recovery.pending&&held==0,"native context race after freeze rejects retry without any instruction write");
 Check(LogText().find("stage=native-contract")!=std::string::npos,"frozen context failure is distinct and nonrecoverable");
 ResetFixture();fault=Fault::RollbackCache;remaining=1000;g_testPatchOps=&ops;LoadAndApply();
 Check(!g_recovery.pending&&!g_instantAxiomSites[0].owned&&!memcmp(m,g_instantAxiomSites[0].original,2)&&held==0,"unconfirmed rollback-cache restoration never schedules a retry");
 Check(LogText().find("rollback_ok=0")!=std::string::npos&&LogText().find("failed-indeterminate")!=std::string::npos,"uncertain rollback is reported without a false unchanged guarantee");
 ResetFixture();fault=Fault::Suspend;remaining=1000;g_testPatchOps=&ops;LoadAndApply();
 const auto generation=g_recovery.generation;
 for(unsigned retry=0;retry<4;++retry){const auto due=g_recovery.due[3];if(retry)Check(due>0,"bounded exponential retry has a deadline");Tick(due);}
 Check(g_recovery.attempts[3]==4&&g_recovery.pending==0&&!g_instantAxiomSites[0].owned&&held==0,"four additional attempts exhaust safely with original bytes and no suspension leak");
 const auto calls=suspendCalls;Tick(GetTickCount64()+1000000);Check(suspendCalls==calls,"retry exhaustion stops all transaction attempts");
 fault=Fault::None;Ini(true,false);LoadAndApply();Check(g_recovery.generation!=generation&&!g_recovery.pending,"actual configuration change resets exhausted budget");
 Ini(true,true);fault=Fault::Flush;remaining=1;LoadAndApply();Check(g_recovery.pending==kInstantBit&&g_recovery.attempts[3]==0,"new configuration receives a fresh bounded retry budget");
 // Same timestamp cannot conceal a disabled request: typed values are re-read.
 WIN32_FILE_ATTRIBUTE_DATA info{};GetFileAttributesExA(g_iniPath.c_str(),GetFileExInfoStandard,&info);
 Ini(false,false);HANDLE file=CreateFileA(g_iniPath.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
 Check(file!=INVALID_HANDLE_VALUE&&SetFileTime(file,nullptr,nullptr,&info.ftLastWriteTime),"private disabled configuration retains prior timestamp for content revalidation regression");if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
 fault=Fault::None;Tick(g_recovery.due[3]);
 Check(!g_recovery.pending&&!g_instantAxiomSites[0].owned&&!g_scopedRangeSites[0].owned&&!g_scopedPullSites[0].owned&&!g_angularSites[0].owned,"fresh disabled config cancels pending activation and restores earlier owned groups");
 Check(*g_range.address==g_range.original&&*g_pull.address==g_pull.original,"configuration cancellation restores owned private numeric values");
 ResetFixture();fault=Fault::Flush;remaining=1;g_testPatchOps=&ops;LoadAndApply();fault=Fault::None;fixture[32]^=1;Tick(g_recovery.due[3]);
 Check(!g_recovery.pending&&!g_instantAxiomSites[0].owned&&fixture[32]!=0xA5,"changed native contract outside patch span is preserved and stops retry");
 g_testPatchOps=nullptr;stop=true;parked.join();
}
#endif

int main(){
 auto* m=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x4000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));if(!m)return 2;
 g_gameBase=reinterpret_cast<uintptr_t>(m);g_gameSize=0x4000;g_iniPath="ApplyRecovery.private.ini";g_logPath="ApplyRecovery.private.log";
 {std::ofstream clean(g_logPath,std::ios::trunc);}
 auto* values=reinterpret_cast<float*>(m+0x1000);values[0]=256;values[1]=20;values[2]=40;
 g_range={values,256,0,false};g_limit={values+1,20,0,false};g_pull={values+2,40,0,false};
 g_scopedRangeSites.clear();for(size_t i=0;i<6;++i)g_scopedRangeSites.push_back(PatchAt(m+0x1100+i*0x20,4,0x11,0x22));
 g_scopedPullSites={PatchAt(m+0x1300,4,0x33,0x44)};
 g_angularSites={PatchAt(m+0x1400,15,0x55,0x66)};
 auto instant=PatchAt(m,2,0xEB,0x90);instant.original[1]=0xFE;memcpy(m,instant.original,2);m[16]=0xC3;
 g_instantAxiomSites={instant};
#ifdef SAF_APPLY_RECOVERY_AVAILABLE
 CaptureRetryContractsForTesting();
#endif
 Ini();GetFileModTime(&g_lastIniModTime);
 HANDLE worker=CreateThread(nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(m),nullptr,0,nullptr);if(!worker)return 2;
 Sleep(20);LoadAndApply();
 Check(!memcmp(m,instant.original,2)&&!g_instantAxiomSites[0].owned,"first production apply fails unchanged while a private thread executes inside Instant span");
 Check(g_scopedRangeSites[0].owned&&g_scopedPullSites[0].owned&&g_angularSites[0].owned,"independent earlier production groups still succeed");
 // Redirect only our private synthetic worker past its spin; no game bytes,
 // engine calls, foreign process handles, or active settings are used.
 Check(SuspendThread(worker)!=DWORD(-1),"private busy worker suspends for controlled fixture stop");
 CONTEXT ctx{};ctx.ContextFlags=CONTEXT_CONTROL;Check(GetThreadContext(worker,&ctx)!=FALSE,"private worker context available");
 ctx.Rip=reinterpret_cast<DWORD64>(m+16);Check(SetThreadContext(worker,&ctx)!=FALSE,"private worker resumes at private RET");
 Check(ResumeThread(worker)!=DWORD(-1)&&WaitForSingleObject(worker,2000)==WAIT_OBJECT_0,"all fixture suspension released and worker exits");
 CloseHandle(worker);Tick(GetTickCount64()+1000);
 Check(!memcmp(m,instant.patch,2)&&g_instantAxiomSites[0].owned,"unchanged INI automatically recovers a transient unchanged production transaction failure");
 std::filesystem::copy_file(g_logPath,"ApplyRecovery.busy-first.log",std::filesystem::copy_options::overwrite_existing);
 ApplyInstantAxiom(false);
#ifdef SAF_APPLY_RECOVERY_AVAILABLE
 ExtendedRecovery(m);
#endif
 VirtualFree(m,0,MEM_RELEASE);
 printf("ApplyRecovery checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
