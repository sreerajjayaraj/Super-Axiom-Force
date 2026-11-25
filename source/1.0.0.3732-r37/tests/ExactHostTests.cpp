#include <Windows.h>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstring>
#include "../src/SuperAxiomForce/ExactHostPolicy.h"
int main(int argc,char**argv){if(argc!=2)return 2;std::ifstream f(argv[1],std::ios::binary);std::vector<unsigned char> raw(8192);f.read(reinterpret_cast<char*>(raw.data()),raw.size());if(f.gcount()<4096)return 2;
 constexpr uintptr_t base=0x140000000ULL;unsigned checks=0,failures=0;auto ck=[&](bool b,const char*s){++checks;if(!b){++failures;printf("FAIL %s\n",s);}};
 auto read=[&](uintptr_t at,void*out,size_t n){if(at<base||at-base>raw.size()||n>raw.size()-(at-base))return false;memcpy(out,raw.data()+at-base,n);return true;};
 ck(saf_host::CompatibleImage(base,read),"current 3576 header accepted as data");auto*d=reinterpret_cast<IMAGE_DOS_HEADER*>(raw.data());const auto pe=static_cast<size_t>(d->e_lfanew);
 for(size_t at:{size_t(0),pe,pe+4,pe+24,pe+48,pe+56,pe+80,pe+92,pe+164}){raw[at]^=1;ck(!saf_host::CompatibleImage(base,read),"invalid PE family shape refuses before startup");raw[at]^=1;}
 for(size_t at:{pe+8,pe+88}){raw[at]^=1;ck(saf_host::CompatibleImage(base,read),"mutable build metadata admits native discovery");raw[at]^=1;}
 ck(!saf_host::CompatibleImage(base,[](uintptr_t,void*,size_t){return false;}),"unreadable main headers refuse");
 uint32_t too_far=0x1001,old=0;memcpy(&old,raw.data()+0x3C,4);memcpy(raw.data()+0x3C,&too_far,4);ck(!saf_host::CompatibleImage(base,read),"unbounded PE offset refuses");memcpy(raw.data()+0x3C,&old,4);
 printf("SAF early family host checks=%u failures=%u (header data only)\n",checks,failures);return failures?1:0;}
