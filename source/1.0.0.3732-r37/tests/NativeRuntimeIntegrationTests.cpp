#define SAF_NEAR_ALLOCATION_TESTING
#define SAF_APPLY_RECOVERY_TESTING
#ifdef SAF_R14_BASELINE
#include "../evidence-3460/r15-investigation/r14-original/SuperAxiomForce.cpp"
#else
#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"
#endif
#include <limits>
static unsigned checks=0,failures=0;
static bool failInstantProtection=false;
static BOOL WINAPI RecoveryProtect(LPVOID p,SIZE_T n,DWORD mode,PDWORD old){
 if(failInstantProtection&&!g_instantAxiomSites.empty()&&p==g_instantAxiomSites[0].address){failInstantProtection=false;SetLastError(1234);return FALSE;}
 return VirtualProtect(p,n,mode,old);
}

static void Check(bool yes,const char* n){++checks;if(!yes){++failures;printf("FAIL %s\n",n);}}
static bool QueryBelowOnly(void* context,uintptr_t address,near_allocation::Region& region){
 if(address>=g_gameBase){SYSTEM_INFO sys{};GetSystemInfo(&sys);const auto page=address&~uintptr_t(0xFFF);
  region={page,reinterpret_cast<uintptr_t>(sys.lpMaximumApplicationAddress)+1-page,false};return true;}
 return QueryNearSupport(context,address,region);
}
int main(int argc,char**argv){
 if(argc!=3&&argc!=4)return 2;
 const bool belowOnly=argc==4&&!strcmp(argv[3],"--below-only");
 const bool earlyPressure=argc==4&&!strcmp(argv[3],"--early-pressure");
 const bool earlyReject=argc==4&&!strcmp(argv[3],"--early-reject");
 std::ifstream input(argv[1],std::ios::binary|std::ios::ate);const auto length=input.tellg();if(length<=0)return 2;
 std::vector<uint8_t> file(static_cast<size_t>(length));input.seekg(0);input.read(reinterpret_cast<char*>(file.data()),length);
 const auto*dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());const auto*nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data()+dos->e_lfanew);
 Check(nt->Signature==IMAGE_NT_SIGNATURE&&nt->OptionalHeader.SizeOfImage==0x13CC4000&&nt->FileHeader.TimeDateStamp==0x6AED2E85,"exact3549 PE fixture identity");
 auto* image=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x140000000),nt->OptionalHeader.SizeOfImage,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!image)return 2;
 g_gameBase=reinterpret_cast<uintptr_t>(image);g_gameSize=nt->OptionalHeader.SizeOfImage;g_image={image,g_gameSize,{},{}};g_logPath="NativeRuntimeIntegrationTests.log";{std::ofstream clean(g_logPath,std::ios::trunc);}
 const auto*sections=IMAGE_FIRST_SECTION(nt);for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto&s=sections[i];
  memcpy(image+s.VirtualAddress,file.data()+s.PointerToRawData,(std::min)(s.SizeOfRawData,s.Misc.VirtualSize));
  if(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)g_image.code.push_back({s.VirtualAddress,s.Misc.VirtualSize});
  else if(s.Characteristics&IMAGE_SCN_MEM_WRITE)g_image.writable.push_back({s.VirtualAddress,s.Misc.VirtualSize});}
 // Model native initializers; no game instruction or engine call is executed.
 *reinterpret_cast<float*>(image+0x70E73E8)=256;*reinterpret_cast<float*>(image+0x70E7398)=20;*reinterpret_cast<float*>(image+0x70E7F28)=40;
 const auto bindings=native::Discover(g_image);const auto straight=straight_pull::Discover(image,g_gameSize,g_gameBase,g_image.code);
 std::vector<void*> pressure;
 if(earlyPressure||earlyReject){
  Check(g_gameBase==0x140000000,"pressure fixture maps immutable game image at observed base in own process only");
  auto original=image[0x65A0EC];bool acquired=false;
#ifdef SAF_EARLY_RESERVATION_AVAILABLE
  acquired=PreallocateSupportForImage(g_gameBase,g_gameSize);
  Check(g_earlySupportAllocation&&near_allocation::Fits(reinterpret_cast<uintptr_t>(g_earlySupportAllocation),std::array<near_allocation::Reference,2>{{{g_gameBase+g_gameSize,0},{g_gameBase,0x1FFF}}}.data(),2),"early owned block satisfies conservative whole-image envelope without fixed RVAs");
#endif
  Check(acquired,"startup acquires owned8KB support before later address-space pressure");
  Check(!g_supportAllocation&&!g_configuredRange&&!g_configuredPull&&!g_clockSupportReady&&image[0x65A0EC]==original,"early storage has no executable adapters, registered support or game-code writes");
  if(earlyReject){
#ifdef SAF_EARLY_RESERVATION_AVAILABLE
   auto* pending=g_earlySupportAllocation;BindFloat("Range",bindings.range,g_range);BindFloat("Limit",bindings.limit,g_limit);BindFloat("Pull",bindings.pull,g_pull);Check(!InitializeSupport(native::Result{}),"invalid native contract rejects support initialization");
   MEMORY_BASIC_INFORMATION released{};VirtualQuery(pending,&released,sizeof(released));
   Check(!g_earlySupportAllocation&&!g_supportAllocation&&released.State==MEM_FREE,"invalid native contract releases unused early reservation");
#endif
   printf("NativeRuntimeEarlyReject checks=%u failures=%u\n",checks,failures);return failures?1:0;
  }
  // Reserve every remaining aligned slot in the exact observed rel32 window.
  // These are our private MEM_RESERVE/PAGE_NOACCESS mappings, never game memory.
  SYSTEM_INFO sys{};GetSystemInfo(&sys);const uintptr_t limit=0x1C0650000;
  for(uintptr_t at=0xC1380000;at<limit;){
   MEMORY_BASIC_INFORMATION region{};if(!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)))return 2;
   const uintptr_t end=(std::min)(limit,reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize);if(end<=at)return 2;
   uintptr_t first=0;near_allocation::AlignUp(at,sys.dwAllocationGranularity,first);
   if(region.State==MEM_FREE&&first<end){const auto reservationLength=near_allocation::AlignDown(end-first,sys.dwAllocationGranularity);
    if(reservationLength){auto* reservation=VirtualAlloc(reinterpret_cast<void*>(first),reservationLength,MEM_RESERVE,PAGE_NOACCESS);if(!reservation)return 2;pressure.push_back(reservation);}}
   at=end;
  }
  const near_allocation::Reference exact[]={
   {g_gameBase+0x64BB22,0x1000},{g_gameBase+0x64D0F1,0x1000},{g_gameBase+0x134ED31,0x1000},
   {g_gameBase+0x64A891,0x1000},{g_gameBase+0x64BB29,0x1000},{g_gameBase+0x134ED38,0x1000},
   {g_gameBase+0x65DC73,0x1008},{g_gameBase+0x65ACBF,0},{g_gameBase+0x65CB15,0x200},{g_gameBase+0x137CE04,0x400}};
  NearSupportErrors lateErrors{};const auto late=near_allocation::Allocate({&lateErrors,QueryNearSupport,AllocateNearSupport,ReleaseNearSupport},exact,10,0x2000,sys.dwAllocationGranularity,
   reinterpret_cast<uintptr_t>(sys.lpMinimumApplicationAddress),reinterpret_cast<uintptr_t>(sys.lpMaximumApplicationAddress),g_gameBase,g_gameSize);
  printf("Pressure late_queries=%u attempts=%u owned_early=%d\n",late.queries,late.attempts,acquired);
  Check(!late.address&&late.queries==(acquired?32458u:32457u)&&!late.attempts,"delayed real allocation has zero attempts under full pressure; one owned reservation adds one upper query");
#ifdef SAF_EARLY_RESERVATION_AVAILABLE
  Check(near_allocation::Fits(reinterpret_cast<uintptr_t>(g_earlySupportAllocation),exact,10),"pre-reserved support still reaches all ten independently listed current native references");
#endif
 }
 NearSupportErrors testErrors{};const near_allocation::Api belowApi{&testErrors,QueryBelowOnly,AllocateNearSupport,ReleaseNearSupport};
 if(belowOnly)g_testNearApiOverride=&belowApi;
 BindFeatureSites(bindings,straight);g_testNearApiOverride=nullptr;CaptureRetryContracts(bindings);
 Check(g_retryContractReady==kAllFeatureBits&&g_retryContracts.size()==18,"all18 actual current native retry contexts captured before any patch write");
 {std::ifstream log(g_logPath);const std::string contents((std::istreambuf_iterator<char>(log)),std::istreambuf_iterator<char>());
  Check(contents.find("NearSupport map query_counts=")!=std::string::npos,"production allocation reports raw queried state counts before feature enable");
  if(!belowOnly)Check(contents.find("NearSupport map sample=")!=std::string::npos,"production allocation retains bounded raw VirtualQuery samples");}
 if(belowOnly)Check(reinterpret_cast<uintptr_t>(g_supportAllocation)<g_gameBase,"forced busy upper arena uses real below-image support allocation");
 Check(g_clockSupportReady&&g_angularReady&&g_scopedRangeSites.size()==6&&g_scopedPullSites.size()==1&&g_angularSites.size()==1&&g_instantAxiomSites.size()==3,"actual production binder assembles all scoped contracts");
 if(!g_clockSupportReady){printf("NativeRuntimeIntegration checks=%u failures=%u (no support, safely stopping fixture)\n",checks,failures);return 1;}
 Check(g_range.address==g_configuredRange&&g_pull.address==g_configuredPull,"production numeric writers address private scalars");
 MEMORY_BASIC_INFORMATION code{},data{};VirtualQuery(g_supportAllocation,&code,sizeof(code));VirtualQuery(g_configuredRange,&data,sizeof(data));
 Check(code.Protect==PAGE_EXECUTE_READ&&data.Protect==PAGE_READWRITE,"adaptercodeRX and privatescalarsRW are separatepages");
 g_iniPath=argv[2];LoadAndApply();LoadAndApply();
 Check(*g_configuredRange==500&&*g_configuredPull==1000,"unchanged actual INI applies private configured values");
 Check(*reinterpret_cast<float*>(image+0x70E73E8)==256&&*reinterpret_cast<float*>(image+0x70E7398)==20&&*reinterpret_cast<float*>(image+0x70E7F28)==40,"allthree shared native globals remain unchanged after enable/reload");
 for(const auto&s:g_scopedRangeSites){int32_t d=0;memcpy(&d,s.address,4);Check(reinterpret_cast<uintptr_t>(s.address)+4+d==reinterpret_cast<uintptr_t>(g_configuredRange),"every exact geometric operand addresses private reach");}
 for(const auto&s:g_scopedPullSites){int32_t d=0;memcpy(&d,s.address,4);Check(reinterpret_cast<uintptr_t>(s.address)+4+d==reinterpret_cast<uintptr_t>(g_configuredPull),"native Axiom duration load addresses private pullspeed");}
 for(unsigned i=1;i<3;++i){const auto&s=g_instantAxiomSites[i];Check(s.address[s.size-5]==0xE8&&IsAllNops(s.address,s.size-5),"clockCALL occupies final5bytes and returns alignednativeend");}
 for(size_t i=0;i<2;++i){const auto&site=g_instantAxiomSites[i+1];int32_t displacement=0;memcpy(&displacement,site.address+site.size-4,4);
  Check(reinterpret_cast<uintptr_t>(site.address)+site.size+displacement==reinterpret_cast<uintptr_t>(g_supportAllocation+i*0x200),"actual clock CALL reaches selected support slot");
  if(belowOnly)Check(displacement<0,"below-image clock CALL uses valid negative signed rel32");}
 {const auto&site=g_angularSites[0];int32_t displacement=0;memcpy(&displacement,site.address+11,4);
  Check(reinterpret_cast<uintptr_t>(site.address)+15+displacement==reinterpret_cast<uintptr_t>(g_supportAllocation+0x400),"actual angular CALL reaches selected support slot");
  if(belowOnly)Check(displacement<0,"below-image angular CALL uses valid negative signed rel32");}
 Check(image[0x65ACE5]==0x76&&image[0x65CCAF]==0x76,"both original native duration/completion branches remain intact");
 Check(image[0x137EAFE]==0xE8&&image[0x137EBDE]==0xE8&&image[0x13801EE]==0xE8,"all original integration calls including ignoredthird remain intact");
 Check(image[0x137CDF5+10]==0xE8,"StraightPull uses angular-only native boundary");
 for(size_t off:{size_t(0),size_t(0x200),size_t(0x400)}){DWORD64 base=0;Check(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(g_supportAllocation+off),&base,nullptr)!=nullptr,"allthree adapters have registeredcallframe unwind records");}
 // A transient protection failure on the actual three current Instant spans
 // recovers through production pending service; all mapped game bytes are data.
 Check(ApplyInstantAxiom(false),"current native fixture restores Instant before recovery fault");
 quiesced_patch::Ops retryOps{};retryOps.protect=RecoveryProtect;g_testPatchOps=&retryOps;failInstantProtection=true;g_recovery={};
 LoadAndApply();
 Check(g_recovery.pending==kInstantBit&&!g_instantAxiomSites[0].owned,"actual current Instant transaction queues unchanged protection failure");
 ProcessPendingApply(g_recovery.due[3]);g_testPatchOps=nullptr;
 Check(!g_recovery.pending&&g_instantAxiomSites[0].owned&&g_instantAxiomSites[1].owned&&g_instantAxiomSites[2].owned,"actual current18-context revalidation allows recovery of all three Instant spans");
 Check(RetryContractsValid(kAllFeatureBits),"normalized owned operands and adapters retain every native retry contract");
 // A conflict in the final Instant site must prevent all earlier enable writes.
 Check(ApplyInstantAxiom(false),"instant disable restores every owned native span");
 auto& last=g_instantAxiomSites.back();last.address[0]^=1;
 Check(!ApplyInstantAxiom(true),"last Instant site conflict rejects entiregroup");
 Check(!memcmp(g_instantAxiomSites[0].address,g_instantAxiomSites[0].original,6)&&!memcmp(g_instantAxiomSites[1].address,g_instantAxiomSites[1].original,10),"Instant conflict leaves earlier activation/clockspan unchanged");last.address[0]^=1;
 Check(ApplyStraightPull(false)&&ApplyAxiomRange(500,false)&&ApplyAxiomPullSpeed(1000,false),"all scoped mechanics disable and restore exact original spans");
 for(const auto&s:g_scopedRangeSites)Check(!memcmp(s.address,s.original,s.size),"range disable restores native operand");
 Check(!memcmp(image+straight_pull_angular::kSiteRva,straight_pull_angular::kOriginal,15),"StraightPull disable restores angular block only");
 Check(g_supportAllocation&&g_clockSupportReady,"retired support allocation/functiontables retained after disable");
 Check(*reinterpret_cast<float*>(image+0x70E73E8)==256&&*reinterpret_cast<float*>(image+0x70E7398)==20&&*reinterpret_cast<float*>(image+0x70E7F28)==40,"shared constants remain native after disable");
 printf("NativeRuntimeIntegration checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
