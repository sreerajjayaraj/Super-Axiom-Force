#define NOMINMAX
#include <Windows.h>
#include <immintrin.h>
#include "../src/SuperAxiomForce/StraightPullAngular.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>
namespace angular=straight_pull_angular;
static unsigned checks=0,failures=0;
static void Check(bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL %s\n",name);}}
struct alignas(16) Context { std::array<uint8_t,0xC00> data{}; };
struct Fixture {
 uint8_t* memory=nullptr;using Run=void(*)(Context*,uintptr_t);Run run=nullptr;
 RUNTIME_FUNCTION row{};bool registered=false;
 Fixture(uint32_t flags,bool baseline=false) {
  memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x3000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));if(!memory)return;
  size_t size=0;
  if(baseline){memcpy(memory+0x1000,angular::kOriginal,sizeof(angular::kOriginal));memory[0x1000+sizeof(angular::kOriginal)]=0xC3;size=sizeof(angular::kOriginal)+1;}
  else if(!angular::Emit(memory+0x1000,0x200,0x140000000ULL,0,&size))return;
  memcpy(memory+0x1200,angular::kUnwind,sizeof(angular::kUnwind));row={0x1000,static_cast<DWORD>(0x1000+(baseline?size:angular::kCodeSize)),0x1200};
  registered=RtlAddFunctionTable(&row,1,reinterpret_cast<DWORD64>(memory))!=FALSE;
  std::vector<uint8_t>b;
  auto put=[&](std::initializer_list<uint8_t>x){b.insert(b.end(),x);};
  auto n32=[&](uint32_t x){auto*p=reinterpret_cast<uint8_t*>(&x);b.insert(b.end(),p,p+4);};
  auto n64=[&](uint64_t x){auto*p=reinterpret_cast<uint8_t*>(&x);b.insert(b.end(),p,p+8);};
  const unsigned nv[]={3,5,6,7,12,13,14,15};
  auto stackreg=[&](unsigned reg,unsigned at,bool load){put({static_cast<uint8_t>(reg<8?0x48:0x4C),static_cast<uint8_t>(load?0x8B:0x89),static_cast<uint8_t>(0x84+((reg&7)<<3)),0x24});n32(at);};
  auto stackxmm=[&](unsigned reg,unsigned at,bool load){put({0xF3});if(reg>=8)put({0x44});put({0x0F,static_cast<uint8_t>(load?0x6F:0x7F),static_cast<uint8_t>(0x84+((reg&7)<<3)),0x24});n32(at);};
  auto capture=[&](unsigned at){
   for(unsigned reg=0;reg<16;++reg){put({static_cast<uint8_t>(reg<8?0x48:0x4C),0x89,static_cast<uint8_t>(0x81+((reg&7)<<3))});n32(at+reg*8);}
   put({0x9C,0x8F,0x81});n32(at+0x80);
   for(unsigned reg=0;reg<16;++reg){put({0xC5,static_cast<uint8_t>(reg<8?0xFE:0x7E),0x7F,static_cast<uint8_t>(0x81+((reg&7)<<3))});n32(at+0x100+reg*32);}
  };
  put({0x48,0x81,0xEC});n32(0x1D8);
  for(unsigned i=0;i<8;++i)stackreg(nv[i],0x20+i*8,false);
  for(unsigned reg=6;reg<16;++reg)stackxmm(reg,0x60+(reg-6)*16,false);
  put({0x48,0x89,0x94,0x24});n32(0x1C8); // synthetic native helper caller return
  for(unsigned reg=0;reg<16;++reg)if(reg!=1 && reg!=4){put({static_cast<uint8_t>(reg<8?0x48:0x49),static_cast<uint8_t>(0xB8+(reg&7))});n64(0x1122334400000000ULL+reg);}
  for(unsigned reg=0;reg<16;++reg){put({0xC5,static_cast<uint8_t>(reg<8?0xFE:0x7E),0x6F,static_cast<uint8_t>(0x81+((reg&7)<<3))});n32(0x800+reg*32);}
  put({0x68});n32(flags|2);put({0x9D});capture(0);
  const size_t call=b.size();put({0xE8});n32(static_cast<uint32_t>(0x1000-call-5));capture(0x400);
  for(unsigned reg=6;reg<16;++reg)stackxmm(reg,0x60+(reg-6)*16,true);
  for(unsigned i=0;i<8;++i)stackreg(nv[i],0x20+i*8,true);
  put({0x48,0x81,0xC4});n32(0x1D8);put({0xC3});
  Check(b.size()<0x1000,"fixture does not overlap adapter");memcpy(memory,b.data(),b.size());FlushInstructionCache(GetCurrentProcess(),memory,0x3000);run=reinterpret_cast<Run>(memory);
 }
 ~Fixture(){if(registered)RtlDeleteFunctionTable(&row);if(memory)VirtualFree(memory,0,MEM_RELEASE);}
 void Unwind(){
  DWORD64 base=0;auto* found=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(memory+0x1000+10),&base,nullptr);
  Check(found==&row && base==reinterpret_cast<DWORD64>(memory),"actual adapter has leaf unwind row");
  std::array<DWORD64,4> stack{0x123456789,0,0,0};CONTEXT c{};c.Rsp=reinterpret_cast<DWORD64>(stack.data());c.Rip=reinterpret_cast<DWORD64>(memory+0x100A);
  c.Rbx=11;c.Rbp=12;c.Rsi=13;c.Rdi=14;c.R12=15;c.R13=16;c.R14=17;c.R15=18;
  void* handler=nullptr;DWORD64 frame=0;RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,c.Rip,found,&c,&handler,&frame,nullptr);
  Check(c.Rip==stack[0] && c.Rsp==reinterpret_cast<DWORD64>(stack.data()+1) && c.Rbx==11 && c.Rbp==12 && c.Rsi==13 && c.Rdi==14 && c.R12==15 && c.R13==16 && c.R14==17 && c.R15==18,"leaf unwind returns to native helper without changing nonvolatile registers");
 }
};
static void TestNative(const char* path){
 std::ifstream file(path,std::ios::binary|std::ios::ate);const auto length=file.tellg();if(length<0x1000||length>0x20000000){Check(false,"bounded native file length");return;}std::vector<uint8_t> raw(static_cast<size_t>(length));file.seekg(0);if(!file.read(reinterpret_cast<char*>(raw.data()),length)){Check(false,"bulk native file read");return;}if(raw.size()<0x1000){Check(false,"native file read");return;}
 auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data()+dos->e_lfanew);
 Check(nt->FileHeader.TimeDateStamp==0x6AED2E85 && nt->OptionalHeader.SizeOfImage==0x13CC4000 && nt->OptionalHeader.CheckSum==0x137C1424,"native PE identity pinned3576");
 auto bytes=[&](uint32_t rva,size_t n)->const uint8_t*{const auto*s=IMAGE_FIRST_SECTION(nt);for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)if(rva>=s[i].VirtualAddress && rva-s[i].VirtualAddress+n<=s[i].SizeOfRawData)return raw.data()+s[i].PointerToRawData+rva-s[i].VirtualAddress;return nullptr;};
 const auto* angular_bytes=bytes(angular::kSiteRva,sizeof(angular::kOriginal));Check(angular_bytes && !memcmp(angular_bytes,angular::kOriginal,sizeof(angular::kOriginal)),"all15 angular instruction bytes match current native game");
 std::vector<uint8_t> mapped(nt->OptionalHeader.SizeOfImage);memcpy(mapped.data(),raw.data(),nt->OptionalHeader.SizeOfHeaders);
 const auto* sections=IMAGE_FIRST_SECTION(nt);for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i)memcpy(mapped.data()+sections[i].VirtualAddress,raw.data()+sections[i].PointerToRawData,sections[i].SizeOfRawData);
 Check(angular::ValidateNative(mapped.data(),mapped.size()),"shared production validator proves native prologue,args,angularspan,quaternioncall and all3calltargets");
 for(size_t mutation:{size_t(angular::kSiteRva),size_t(angular::kHelperRva),size_t(0x137EADC),size_t(0x137EBBC),size_t(angular::kResumeRva),size_t(0x137EAFE),size_t(0x137EBDF),size_t(0x13801EF)}){mapped[mutation]^=1;Check(!angular::ValidateNative(mapped.data(),mapped.size()),"shared native validator rejects every changed proof boundary");mapped[mutation]^=1;}
 Check(!angular::ValidateNative(mapped.data(),0x13801F2),"shared native validator rejects truncated image");
 for(uint32_t ret:angular::kSelectedReturnRvas){auto* call=bytes(ret-5,5);int32_t disp=0;if(call)memcpy(&disp,call+1,4);Check(call&&call[0]==0xE8&&static_cast<int64_t>(ret)+disp==angular::kHelperRva,"selected call targets native helper");}
 auto* third=bytes(0x13801EE,5);int32_t disp=0;if(third)memcpy(&disp,third+1,4);Check(third&&third[0]==0xE8&&0x13801F3LL+disp==angular::kHelperRva,"third helper caller remains distinct and native");
 const uint8_t stack[]={0x48,0x81,0xEC,0x88,0x01,0,0};Check(!memcmp(bytes(0x137BA86,7),stack,7),"native helper allocates188 after eight nonvolatile pushes");
 const uint8_t callquat[]={0x48,0x8D,0x4C,0x24,0x40,0xE8,0x42,0x72,0x06,0xFF};Check(!memcmp(bytes(angular::kResumeRva,10),callquat,10),"original quaternion call immediately follows angular block");
 const uint8_t radial[]={0xC5,0x8A,0x59,0x8E,0xD4,0,0,0,0xC5,0xFA,0x10,0x86,0xB4,0,0,0,0xC5,0xFA,0x5C,0xD1};Check(!memcmp(bytes(0x137CB38,sizeof(radial)),radial,sizeof(radial)),"native radial distance subtract retained outside angular span");
 // Never execute copied game code. The native quaternion constructor and all
 // three calls to its polynomial helper are proved statically by Verify3527Native.py.
 // Keep the zero/nonzero semantic assertions using an independent C++ reference;
 // these do not claim execution equivalence with the game's polynomial routine.
 const auto reference=[](float pitch,float yaw,float roll){
  const float sp=std::sin(pitch*0.5f),cp=std::cos(pitch*0.5f);
  const float sy=std::sin(yaw*0.5f),cy=std::cos(yaw*0.5f);
  const float sr=std::sin(roll*0.5f),cr=std::cos(roll*0.5f);
  return std::array<float,4>{sp*cy*cr+cp*sy*sr,cp*sy*cr-sp*cy*sr,
                           cp*cy*sr-sp*sy*cr,cp*cy*cr+sp*sy*sr};
 };
 const auto zero=reference(0,0,0),turn=reference(0.25f,-0.5f,0.75f);
 Check(zero[0]==0&&zero[1]==0&&zero[2]==0&&zero[3]==1,"independent quaternion reference: zero increment yields identity");
 Check(turn[0]!=0||turn[1]!=0||turn[2]!=0,"independent quaternion reference: nonzero increment yields a turn");
}
int main(int argc,char** argv){
 std::array<uint8_t,angular::kBufferSize> emitted{};emitted.fill(0xCC);size_t written=99;
 Check(!angular::Emit(emitted.data(),angular::kBufferSize-1,0x140000000,0,&written)&&written==0&&emitted[0]==0xCC,"short buffer rejected without mutation");
 Check(angular::Emit(emitted.data(),emitted.size(),0x140000000,0,&written)&&written==emitted.size(),"production emitter writes complete bounded adapter");
 if(written!=emitted.size()){std::printf("StraightPullAngular red: %u checks %u failures\n",checks,failures);return 1;}
 Check(emitted[angular::kCodeSize-1]==0xC3,"CALL adapter ends in RET before constant data");
 unsigned copies=0;for(size_t i=0;i+sizeof(angular::kOriginal)<=angular::kCodeSize;++i)if(!memcmp(emitted.data()+i,angular::kOriginal,sizeof(angular::kOriginal)))++copies;
 Check(copies==1,"actual emitted adapter replays exactly one original15byte math block");
 for(size_t i=0;i<2;++i){uintptr_t value=0;memcpy(&value,emitted.data()+angular::kDataOffset+i*16,8);Check(value==(i?UINT64_MAX:0x140000000ULL+angular::kSelectedReturnRvas[0]),"only the normal substep can suppress rotation");}
 auto invalid=emitted;written=99;Check(!angular::Emit(invalid.data(),invalid.size(),0,0,&written)&&written==0&&invalid==emitted,"invalid base rejected without buffer mutation");
 auto relocated=emitted;Check(angular::Emit(relocated.data(),relocated.size(),0x7FF600000000ULL,0,&written),"emitter supports relocated game modulebase");
 uintptr_t relocated_return=0;memcpy(&relocated_return,relocated.data()+angular::kDataOffset,8);Check(relocated_return==0x7FF60137EB03ULL,"selected return constants relocate with module");
 const std::array<uint32_t,8> values={0x3F800000,0xBF800000,0,0x80000000,0x3C83126F,0x7F800000,0x7FC12345,1};
 for(unsigned flagCase=0;flagCase<64;++flagCase){
  const uint32_t flagBits=(flagCase&1)|((flagCase&2)<<1)|((flagCase&4)<<2)|((flagCase&8)<<3)|((flagCase&16)<<3)|((flagCase&32)<<6);
  Fixture f(flagBits);Check(f.run!=nullptr&&f.registered,"private emitted adapter and unwind fixture ready");if(!f.run)return 2;if(flagCase==0)f.Unwind();
  for(uintptr_t caller:{uintptr_t(0x14137EB03),uintptr_t(0x14137EBE3),uintptr_t(0x1413801F3),uintptr_t(0),uintptr_t(0x141379F24)})for(uint32_t scalar:values){
   Context c{};for(unsigned reg=0;reg<16;++reg)for(unsigned lane=0;lane<8;++lane){uint32_t x=0x3F000000+reg*0x10000+lane*0x100;memcpy(c.data.data()+0x800+reg*32+lane*4,&x,4);}
   for(unsigned reg:{9U,10U,11U,14U})memcpy(c.data.data()+0x800+reg*32,&scalar,4);
   f.run(&c,caller);const bool selected=caller==0x14137EB03;
   Check(!memcmp(c.data.data(),c.data.data()+0x400,0x88),"all16 GPRs includingRSP and complete RFLAGS preserved");
   for(unsigned reg=0;reg<16;++reg){
    std::array<uint8_t,32> want{};memcpy(want.data(),c.data.data()+0x100+reg*32,32);
    if(reg==0){want.fill(0);if(selected)memset(want.data(),0xFF,4);}
    else if(reg==4){want.fill(0);memset(want.data()+8,0xFF,8);}
    else if(reg>=1&&reg<=3){want.fill(0);const unsigned source=reg==1?10:reg==2?9:11;const __m128 a=_mm_loadu_ps(reinterpret_cast<const float*>(c.data.data()+0x800+source*32));const __m128 dt=_mm_loadu_ps(reinterpret_cast<const float*>(c.data.data()+0x800+14*32));const __m128 native=_mm_mul_ss(a,dt);_mm_storeu_ps(reinterpret_cast<float*>(want.data()),native);if(selected)memset(want.data(),0,4);}
    Check(!memcmp(want.data(),c.data.data()+0x500+reg*32,32),"each YMM lane matches scalar-only correction or native replay/scratch contract");
   }
  }
 }
 if(argc==2)TestNative(argv[1]);
 std::printf("StraightPullAngular: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
