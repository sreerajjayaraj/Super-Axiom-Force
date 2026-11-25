#define NOMINMAX
#include <Windows.h>
#include "../src/SuperAxiomForce/ClockAcceleration.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <immintrin.h>
using clock_acceleration::Kind;
static unsigned checks=0,failures=0;
static void Check(bool yes,const char* name){++checks;if(!yes){++failures;printf("FAIL %s\n",name);}}
struct Fixture {
 unsigned char* m=nullptr;RUNTIME_FUNCTION row{};size_t stubSize=0;Kind kind;
 using Fn=float(*)(void*,float);Fn fn=nullptr;
 Fixture(Kind k,bool overflow):kind(k){
  m=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x4000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
  if(!m)return;
  std::vector<unsigned char>b;
  auto put=[&](std::initializer_list<unsigned char>x){b.insert(b.end(),x);};
  auto n32=[&](unsigned x){auto*p=reinterpret_cast<unsigned char*>(&x);b.insert(b.end(),p,p+4);};
  auto save=[&](unsigned at){
   for(unsigned reg=0;reg<16;++reg){put({static_cast<unsigned char>(reg<8?0x48:0x4C),0x89,static_cast<unsigned char>(0x81+((reg&7)<<3))});n32(at+reg*8);}
   put({0x9C,0x8F,0x81});n32(at+0x80);
   for(unsigned reg=0;reg<16;++reg){put({0xF3});if(reg>=8)put({0x44});put({0x0F,0x7F,static_cast<unsigned char>(0x81+((reg&7)<<3))});n32(at+0x200+reg*16);}
  };
  if(k==Kind::Reel)put({0x41,0x56,0x45,0x33,0xF6});
  else put({0x57,0x33,0xFF});
  if(overflow)put({0xB8,0xFF,0xFF,0xFF,0x7F,0x83,0xC0,1});else put({0x39,0xC0});
  save(0xC00);
  const size_t hook=b.size();
  const std::vector<unsigned char>orig=k==Kind::Reel?std::vector<unsigned char>{0xC4,0xC1,0x72,0x58,0x84,0x0E,0x20,0x0B,0,0}:std::vector<unsigned char>{0xC5,0xF2,0x58,0x84,0x0F,0x20,0x0B,0,0};
  b.insert(b.end(),orig.begin(),orig.end());
  if(k==Kind::Reel)put({0xC4,0xC1,0x7A,0x11,0x84,0x0E,0x20,0x0B,0,0});
  else put({0xC5,0xFA,0x11,0x84,0x0F,0x20,0x0B,0,0});
  save(0xD00);
  if(k==Kind::Reel)put({0x41,0x5E,0xC3});else put({0x5F,0xC3});
  const auto code=clock_acceleration::Emit(k,orig.data(),orig.size(),reinterpret_cast<uintptr_t>(m+hook+orig.size()),
                                           reinterpret_cast<uintptr_t>(m+0xA20));
  stubSize=code.size();Check(stubSize<0x200,"stub fits own allocated slot");
  memcpy(m,b.data(),b.size());memcpy(m+0x800,code.data(),code.size());memcpy(m+0xA00,clock_acceleration::kUnwind,sizeof(clock_acceleration::kUnwind));
  row={0x800,static_cast<DWORD>(0x800+code.size()),0xA00};
  Check(RtlAddFunctionTable(&row,1,reinterpret_cast<DWORD64>(m))!=FALSE,"stub unwind metadata registers");
  memset(m+hook,0x90,orig.size());const size_t call=hook+orig.size()-5;
  m[call]=0xE8;const int32_t d=static_cast<int32_t>(0x800-call-5);memcpy(m+call+1,&d,4);
  FlushInstructionCache(GetCurrentProcess(),m,0x2000);fn=reinterpret_cast<Fn>(m);
 }
 ~Fixture(){if(m){RtlDeleteFunctionTable(&row);VirtualFree(m,0,MEM_RELEASE);}}
 float Run(float old,float dt,float duration,unsigned char state=5,bool ready=true,bool alternate=false){
  void* object=m+0x2000;memcpy(m+0x2B20,&old,4);memcpy(m+0x2B24,&duration,4);
  uintptr_t first=ready?reinterpret_cast<uintptr_t>(m+0x3100):0,second=reinterpret_cast<uintptr_t>(m+0x3140);
  uintptr_t childA=reinterpret_cast<uintptr_t>(m+0x3180),childB=reinterpret_cast<uintptr_t>(m+0x31C0);
  memcpy(m+0x29C0,&first,8);memcpy(m+0x29B8,&second,8);memcpy(m+0x3108,&childA,8);memcpy(m+0x3148,&childB,8);
  m[0x3195]=0;m[0x31D5]=0;m[0x2B28]=alternate?1:0;
  // R8B is the third ABI argument. A separate leaf shim sets the native state.
  // Inject it in the wrapper's state slot without changing the clock stub.
  // The actual call uses the three-argument ABI so R8B has the selected state.
  using Call=float(*)(void*,float,unsigned char);
  float value=reinterpret_cast<Call>(fn)(object,dt,state);
  Check(!memcmp(m+0x2C00,m+0x2D00,0x88),"all16 integer registers and arithmetic flags preserved");
  for(unsigned i=0;i<16;++i)if(i!=0U)Check(!memcmp(m+0x2E00+i*16,m+0x2F00+i*16,16),"other XMM registers preserved");
  return value;
 }
 void Unwind(){
  DWORD64 base=0;auto* found=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(m+0x808),&base,nullptr);
  Check(found && base==reinterpret_cast<DWORD64>(m),"dynamic lookup identifies actual stub unwind record");
  std::array<DWORD64,8> stack{15,14,13,12,0x12345678,0,0,0};CONTEXT context{};
  context.Rsp=reinterpret_cast<DWORD64>(stack.data());context.Rip=reinterpret_cast<DWORD64>(m+0x808);
  void* handler=nullptr;DWORD64 frame=0;
  RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,context.Rip,found,&context,&handler,&frame,nullptr);
  Check(context.R12==12&&context.R13==13&&context.R14==14&&context.R15==15&&context.Rip==0x12345678&&context.Rsp==reinterpret_cast<DWORD64>(stack.data()+5),"native unwinder restores all pushed registers and caller");
 }
};
static float Expected(float old,float dt,float dur,Kind kind,unsigned char state){
 (void)kind;(void)state;
 float pred=std::nextafterf(dur,0);
 return std::isfinite(dur)&&dur>(std::numeric_limits<float>::min)()&&
  std::isfinite(dt)&&dt>=(std::numeric_limits<float>::min)()&&dt>=dur-pred&&std::isfinite(old)&&old>=0&&old<pred?pred:old+dt;
}
int main(){
 {
  Fixture flight(Kind::Propell,false);
  Fixture firing(Kind::Reel,false);
  flight.m[0xA20]=1;
  firing.m[0xA20]=1;
  float elapsed=0;
  for(unsigned frame=0;frame<20;++frame)
   elapsed=flight.Run(elapsed,.016f,2.f);
  Check(std::abs(elapsed-.32f)<0.00001f,"straight travel keeps 20 native animation steps");
  Check(firing.Run(0,.016f,2.f)==std::nextafterf(2.f,0.f),"straight travel keeps the instant tether launch");
  flight.m[0xA20]=0;
  Check(flight.Run(0,.016f,2.f)==std::nextafterf(2.f,0.f),"straight travel guard releases when the option is off");
 }
 const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
 for(auto kind:{Kind::Reel,Kind::Propell})for(bool overflow:{false,true}){
  Fixture f(kind,overflow);if(!f.m)return 2;f.Unwind();
  for(float dur:{0.0001f,0.005f,0.25f,1000.f,(std::numeric_limits<float>::max)(),(std::numeric_limits<float>::min)(),std::numeric_limits<float>::denorm_min(),0.f,-1.f,nan,inf})
   for(float old:{0.f,0.1f,-1.f,nan,inf})for(float dt:{0.016f,0.f,-1.f,nan,inf}){
    float got=f.Run(old,dt,dur),want=Expected(old,dt,dur,kind,5);
    if(!((std::isnan(got)&&std::isnan(want))||got==want))printf("MISMATCH kind%d old%g dt%g dur%g got%g want%g\n",static_cast<int>(kind),old,dt,dur,got,want);
    Check((std::isnan(got)&&std::isnan(want))||got==want,"emitted native clock matches guarded acceleration policy");
   }
  for(float dur:{0.0001f,0.005f,0.25f,1000.f}){
   float first=f.Run(0,0.016f,dur);Check(first/dur<1&&first/dur>0.999999f,"first frame moves near destination without native completion");
   float second=f.Run(first,0.016f,dur);Check(second/dur>=1,"next frame reaches original native completion test");
  }
  if(kind==Kind::Propell)Check(f.Run(0,.016f,1,3)==std::nextafterf(1,0),"propell slot input is not confused with a state5 tag");
  const float waiting=f.Run(0,.016f,.5f,5,false);
  Check(waiting==.016f,"invalid acquisition stays on native time without mod fast-forward");
  const float firstValid=f.Run(waiting,.016f,.5f);
  Check(firstValid/.5f<1&&firstValid/.5f>.999999f,"late valid acquisition still gets native movement before completion");
  Check(f.Run(firstValid,.016f,.5f)/.5f>=1,"late valid acquisition can complete on following native update");
  Check(f.Run(2,.016f,.5f)==2+.016f,"already expired native clock never rewinds or resets");
  if(kind==Kind::Propell)Check(f.Run(0,.016f,.5f,5,true,true)==.016f,"alternate native propell mode stays on original clock");
  float cyclicClock=.1f,position=0;unsigned motion=0,eligibility=0,blocked=0,stateChanges=0;
  for(unsigned tick=0;tick<6;++tick){
   cyclicClock=f.Run(cyclicClock,.016f,.5f);const float progress=(std::min)(cyclicClock/.5f,1.f);
   if(progress>=1){++eligibility;if(eligibility<3)++blocked;else++stateChanges;}
   else {++motion;position=500.f*progress;}
   if(tick<3)Check(stateChanges==0,"blocked native completion cannot cause an early state transition");
   if(stateChanges)break;
  }
  Check(motion==1&&eligibility==3&&blocked==2&&stateChanges==1,"blocked completion retries native eligibility without extra calls/state changes or clock rewind");
  Check(position>499.99f&&position<=500.f,"Range500 PullSpeed1000 performs endpoint interpolation rather than a16unit speed step");
  const unsigned saved=_mm_getcsr();_mm_setcsr(saved|0x8040);
  const float low=(std::numeric_limits<float>::min)();
  Check(f.Run(0,.016f,low)==.016f,"minimum normal duration stays native under DAZ/FTZ");
  Check(f.Run(0,.016f,.005f)/.005f<1,"normal first movement remains below completion under DAZ/FTZ");
  _mm_setcsr(saved);
 }
 std::mt19937 random(3446);for(unsigned i=0;i<100000;++i){unsigned bits=0x800000+(random()%0x7F000000);float d=0;memcpy(&d,&bits,4);Check(std::nextafterf(d,0)/d<1,"normal finite predecessor divides strictly below1");}
 printf("ClockAcceleration checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
