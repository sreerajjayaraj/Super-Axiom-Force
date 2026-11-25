#define SAF_OWNERSHIP_TESTING
#include "../src/SuperAxiomForce/SuperAxiomForce.cpp"
static unsigned checks=0,failures=0;
static BytePatchSite* raceSite=nullptr;
static void Check(bool yes,const char* n){++checks;printf("%s %s\n",yes?"PASS":"FAIL",n);if(!yes)++failures;}
static void BeforeClock(size_t i){auto* p=reinterpret_cast<uint8_t*>(g_gameBase+(i==0?0x65ACB5:0x65CB0C));p[0]=0xCC;}
static void BeforeAngular(){reinterpret_cast<uint8_t*>(g_gameBase)[straight_pull_angular::kSiteRva]=0xCC;}
static void BeforeExpected(BytePatchSite* s){if(s==raceSite){s->address[0]^=1;g_testBeforeExpectedSnapshot=nullptr;}}
static void BeforeRedirect(const native::Image& im,size_t field){const_cast<uint8_t*>(im.bytes)[field]^=1;native::g_testBeforeRedirectSnapshot=nullptr;}
static void AfterActivation(const native::Image& im,size_t field){const_cast<uint8_t*>(im.bytes)[field]=0x0F;native::g_testAfterActivationSnapshot=nullptr;}
int main(int argc,char**argv){
 if(argc!=2)return 2;
 std::ifstream input(argv[1],std::ios::binary|std::ios::ate);const auto length=input.tellg();if(length<=0)return 2;
 std::vector<uint8_t> file(static_cast<size_t>(length));input.seekg(0);input.read(reinterpret_cast<char*>(file.data()),length);
 const auto*dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());const auto*nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data()+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->OptionalHeader.SizeOfImage!=0x13CC4000||nt->FileHeader.TimeDateStamp!=0x6AED2E85)return 2;
 auto* image=static_cast<uint8_t*>(VirtualAlloc(nullptr,nt->OptionalHeader.SizeOfImage,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!image)return 2;
 g_gameBase=reinterpret_cast<uintptr_t>(image);g_gameSize=nt->OptionalHeader.SizeOfImage;g_image={image,g_gameSize,{},{}};g_logPath="OwnershipRaceTests.log";
 const auto*sections=IMAGE_FIRST_SECTION(nt);for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i){const auto&s=sections[i];
  memcpy(image+s.VirtualAddress,file.data()+s.PointerToRawData,(std::min)(s.SizeOfRawData,s.Misc.VirtualSize));
  if(s.Characteristics&IMAGE_SCN_MEM_EXECUTE)g_image.code.push_back({s.VirtualAddress,s.Misc.VirtualSize});
  else if(s.Characteristics&IMAGE_SCN_MEM_WRITE)g_image.writable.push_back({s.VirtualAddress,s.Misc.VirtualSize});}
 *reinterpret_cast<float*>(image+0x70E73E8)=256;*reinterpret_cast<float*>(image+0x70E7398)=20;*reinterpret_cast<float*>(image+0x70E7F28)=40;
 const auto bindings=native::Discover(g_image);const auto straight=straight_pull::Discover(image,g_gameSize,g_gameBase,g_image.code);
 const auto field=bindings.scopedRange[0].offset;uint8_t expectedField[4]{};memcpy(expectedField,image+field,4);
 native::g_testBeforeRedirectSnapshot=BeforeRedirect;
 const auto racedRedirect=native::RedirectRange(g_image,native::Find(g_image,native::kLimit),3,bindings.limit,bindings.range);
 Check(!memcmp(racedRedirect.original.data(),expectedField,4)&&memcmp(image+field,expectedField,4)!=0,"redirect original derives from validated target and rejects later live recapture");
 memcpy(image+field,expectedField,4);
 image[bindings.activation.offset]=0xCC;native::g_testAfterActivationSnapshot=AfterActivation;
 const auto racedActivation=native::Discover(g_image);
 Check(!racedActivation.activation.binding.valid&&racedActivation.activation.original[0]==0xCC&&image[bindings.activation.offset]==0x0F,"activation validation uses captured bytes rather than later repaired live opcode");
 // Mutate only after resolver validation, exactly before the vulnerable captures.
 g_testBeforeClockSnapshot=BeforeClock;g_testBeforeAngularSnapshot=BeforeAngular;BindFeatureSites(bindings,straight);
 Check(g_clockSupportReady&&g_angularReady&&g_instantAxiomSites.size()==3&&g_angularSites.size()==1,"actual production binder reaches injected snapshot boundaries");
 const uint8_t* originals[]={clock_acceleration::kReelAdd,clock_acceleration::kPropellAdd};const size_t widths[]={10,9};
 for(size_t i=0;i<2;++i){auto&s=g_instantAxiomSites[i+1];
  Check(!memcmp(s.original,originals[i],widths[i]),"clock binder pins validated original despite intervening mutation");
  auto* begin=g_supportAllocation+i*0x200;
  Check(std::search(begin,begin+0x200,originals[i],originals[i]+widths[i])!=begin+0x200,"clock fallback embeds validated ADD rather than intervening live bytes");}
 Check(!memcmp(g_angularSites[0].original,straight_pull_angular::kOriginal,15),"angular binder pins validated original despite intervening mutation");
 Check(!ApplyInstantAxiom(true)&&image[0x65A0EC]==0x0F&&image[0x65ACB5]==0xCC&&image[0x65CB0C]==0xCC,"intervening clock edits reject entire enable group without overwrite");
 Check(!ApplyStraightPull(true)&&image[straight_pull_angular::kSiteRva]==0xCC,"intervening angular edit is preserved");
 // Restore only private fixture bytes so classification-to-capture races can
 // be exercised independently of the binder/fallback capture races above.
 for(size_t i=0;i<2;++i){auto&s=g_instantAxiomSites[i+1];memcpy(s.address,originals[i],widths[i]);memcpy(s.original,originals[i],widths[i]);s.owned=false;}
 memcpy(g_instantAxiomSites[0].address,g_instantAxiomSites[0].original,6);g_instantAxiomSites[0].owned=false;
 memcpy(g_angularSites[0].address,straight_pull_angular::kOriginal,15);memcpy(g_angularSites[0].original,straight_pull_angular::kOriginal,15);g_angularSites[0].owned=false;
 raceSite=&g_instantAxiomSites.back();g_testBeforeExpectedSnapshot=BeforeExpected;
 Check(!ApplyInstantAxiom(true),"enable rejects edit between classification and transaction snapshot");
 Check(image[0x65A0EC]==0x0F&&!memcmp(image+0x65ACB5,clock_acceleration::kReelAdd,10),"enable race makes no partial activation or earlier-clock writes");
 Check(image[0x65CB0C]==(clock_acceleration::kPropellAdd[0]^1),"enable race preserves intervening external byte");
 Check(!g_instantAxiomSites[0].owned&&!g_instantAxiomSites[1].owned&&!g_instantAxiomSites[2].owned,"failed enable publishes no ownership");
 memcpy(image+0x65CB0C,clock_acceleration::kPropellAdd,9);
 Check(ApplyInstantAxiom(true),"clean enable succeeds after race fixture reset");
 raceSite=&g_instantAxiomSites.back();g_testBeforeExpectedSnapshot=BeforeExpected;
 Check(!ApplyInstantAxiom(false),"disable rejects edit between classification and transaction snapshot");
 Check(!memcmp(g_instantAxiomSites[0].address,g_instantAxiomSites[0].patch,6)&&!memcmp(g_instantAxiomSites[1].address,g_instantAxiomSites[1].patch,10),"disable race does not partially restore earlier sites");
 Check(g_instantAxiomSites[2].address[0]==(g_instantAxiomSites[2].patch[0]^1),"disable race preserves intervening external byte");
 Check(g_instantAxiomSites[0].owned&&g_instantAxiomSites[1].owned&&g_instantAxiomSites[2].owned,"failed disable retains previous ownership state");
 memcpy(g_instantAxiomSites[2].address,g_instantAxiomSites[2].patch,9);
 Check(ApplyInstantAxiom(false),"clean disable restores owned spans after race fixture reset");
 raceSite=&g_angularSites[0];g_testBeforeExpectedSnapshot=BeforeExpected;
 Check(!ApplyStraightPull(true)&&image[straight_pull_angular::kSiteRva]==(straight_pull_angular::kOriginal[0]^1),"angular classification-to-capture race also rejects without overwrite");
 printf("OwnershipRace checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
