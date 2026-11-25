#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"
#include <limits>
static int checks=0,failures=0;
static void Check(bool b,const char* n){++checks;printf("%s %s\n",b?"PASS":"FAIL",n);if(!b)++failures;}
int main() {
 auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x1000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
 if (!memory) return 2;
 g_gameBase=reinterpret_cast<uintptr_t>(memory);g_gameSize=0x1000;g_logPath="ScopedRangeTests.log";
 auto* values=reinterpret_cast<float*>(memory+0x800);values[0]=256;values[1]=20;values[2]=256;values[3]=40;values[4]=40;
 g_configuredRange=values+2;g_configuredPull=values+4;g_range={values+2,256,0,false};g_pull={values+4,40,0,false};g_limit={values+1,20,0,false};
 // Three native-form RIP displacement fields use private floats, never game code.
 for(size_t i=0;i<6;++i){BytePatchSite s{};s.address=memory+0x100+i*0x20;s.size=4;s.canRestore=true;
  const auto field=reinterpret_cast<uintptr_t>(s.address);
  const int32_t before=static_cast<int32_t>(reinterpret_cast<uintptr_t>(i<3?values+1:values)-(field+4));
  const int32_t after=static_cast<int32_t>(reinterpret_cast<uintptr_t>(values+2)-(field+4));
  memcpy(s.original,&before,4);memcpy(s.patch,&after,4);memcpy(s.address,s.original,4);g_scopedRangeSites.push_back(s);}
 const auto target=[&](size_t i){int32_t d=0;memcpy(&d,g_scopedRangeSites[i].address,4);return reinterpret_cast<uintptr_t>(g_scopedRangeSites[i].address)+4+d;};
 Check(ApplyAxiomRange(500,true),"scoped range applies");
 Check(values[2]==500 && values[0]==256 && values[1]==20,"configured reach extends without changing shared physics20");
 for(size_t i=0;i<6;++i)Check(target(i)==reinterpret_cast<uintptr_t>(values+2),"geometric consumer uses configuredRange");
 Check(ApplyAxiomRange(750,true),"reload applies updated reach");
 Check(values[2]==750&&values[0]==256&&values[1]==20,"reload never mutates common20");
 Check(ApplyAxiomRange(750,false),"disable restores owned range and displacements");
 Check(values[2]==256&&values[0]==256&&values[1]==20,"disable restores originalRange and common20 remains unchanged");
 for(size_t i=0;i<6;++i)Check(target(i)==reinterpret_cast<uintptr_t>(i<3?values+1:values),"disable restores common20 consumer");
 Check(!ApplyAxiomRange((std::numeric_limits<float>::quiet_NaN)(),true),"nonfinite configured reach rejected");
 Check(values[0]==256&&target(0)==reinterpret_cast<uintptr_t>(values+1),"invalid config leaves all code and values unchanged");
 Check(!ApplyAxiomRange((std::numeric_limits<float>::max)(),true),"finite squared-distance overflow request rejected");
 Check(!ApplyAxiomRange(0,true)&&!ApplyAxiomRange(-1,true)&&!ApplyAxiomRange(INFINITY,true),"zero negative infinity reach rejected");
 {BytePatchSite s{};s.address=memory+0x280;s.size=4;s.canRestore=true;
 int32_t original=static_cast<int32_t>(reinterpret_cast<uintptr_t>(values+3)-reinterpret_cast<uintptr_t>(s.address+4));
 int32_t redirected=static_cast<int32_t>(reinterpret_cast<uintptr_t>(values+4)-reinterpret_cast<uintptr_t>(s.address+4));
 memcpy(s.original,&original,4);memcpy(s.patch,&redirected,4);memcpy(s.address,s.original,4);g_scopedPullSites.push_back(s);}
 Check(ApplyAxiomPullSpeed(1000,true)&&values[3]==40&&values[4]==1000,"private Axiom pull speed preserves shared40");
 Check(ApplyAxiomPullSpeed(1000,false)&&values[3]==40&&values[4]==40,"pull disable restores owned operand/private scalar");
 // A single conflict must not redirect the other two consumers or writeRange.
 g_scopedRangeSites[1].address[0]^=1;
 Check(!ApplyAxiomRange(500,true),"one conflicting displacement rejects range group");
 Check(values[0]==256&&target(0)==reinterpret_cast<uintptr_t>(values+1)&&target(2)==reinterpret_cast<uintptr_t>(values+1),"conflict preflight prevents partial geometric edits");
 g_scopedRangeSites[1].address[0]^=1;
 // Execute the actual native common-limit load/clamp instruction forms in a
 // self-contained function, proving its20 cap is unaffected by extendedreach.
 auto* fn=memory+0x400;
 const unsigned char body[]={0xC5,0xF8,0x28,0xD0,0xC5,0xFA,0x10,0x05,0,0,0,0,0xC5,0xEA,0x5D,0xC0,0xC3};
 memcpy(fn,body,sizeof(body));int32_t disp=static_cast<int32_t>(reinterpret_cast<uintptr_t>(values+1)-reinterpret_cast<uintptr_t>(fn+12));memcpy(fn+8,&disp,4);
 FlushInstructionCache(GetCurrentProcess(),memory,0x1000);
 using Clamp=float(*)(float);const auto clamp=reinterpret_cast<Clamp>(fn);
 Check(clamp(100)==20,"native physics speed cap starts at20");
 Check(ApplyAxiomRange(500,true)&&clamp(100)==20,"native physics cap stays20 when reach becomes500");
 Check(clamp(5)==5,"native sublimit physics remains unchanged");
 ApplyAxiomRange(500,false);VirtualFree(memory,0,MEM_RELEASE);
 printf("ScopedRange checks=%d failures=%d\n",checks,failures);return failures?1:0;
}

