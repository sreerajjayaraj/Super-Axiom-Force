#pragma once

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <span>
#include <vector>

namespace axiom::native {

enum class SectionKind { Headers, Code, Writable, ReadOnly, Unknown };
struct RelativeRole { uintptr_t key; uint32_t offset; };
struct OwnerShape {
    uint32_t length, offset;
    const uint8_t* bytes;
    const uint8_t* mask;
    size_t size;
    std::span<const RelativeRole> roles;
};
struct RelativeEdge {
    unsigned field, next, width;
    SectionKind kind;
    uintptr_t role;
};
struct SiteShape {
    uintptr_t key;
    const uint8_t* bytes;
    const uint8_t* mask;
    size_t size;
    std::span<const RelativeEdge> edges;
};
#include "AxiomOwnerShapes.generated.h"
#include "AxiomSiteShapes.generated.h"

class Roles {
    struct Section { uint32_t begin, end; SectionKind kind; bool discardable; };
    struct RuntimeRow { uint32_t begin, end, unwind; };
    uintptr_t base_=0;
    size_t imageSize_=0;
    std::vector<Section> sections_;
    std::vector<RuntimeRow> owners_;
    std::map<uintptr_t,uint32_t> addresses_;
    std::map<uintptr_t,std::vector<uint8_t>> witnesses_;
    uintptr_t failedRole_=0;
    bool ready_=false;

    bool Range(uint32_t rva, size_t size) const {
        return size && rva<imageSize_ && size<=imageSize_-rva;
    }

    bool Read(uint32_t rva, void* output, size_t size) const {
        SIZE_T copied=0;
        return Range(rva,size) && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(base_+rva),output,size,&copied) && copied==size;
    }

    template<class T> bool Read(uint32_t rva,T& output) const {
        return Read(rva,&output,sizeof(output));
    }

    SectionKind Kind(uint32_t rva, size_t size=1) const {
        if (rva==0 && size==1) return SectionKind::Headers;
        if (!Range(rva,size)) return SectionKind::Unknown;
        for (const auto& section:sections_) {
            if (rva>=section.begin && rva<section.end && size<=section.end-rva)
                return section.kind;
        }
        return SectionKind::Unknown;
    }

    bool LoadLayout() {
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!Read(0,dos) || dos.e_magic!=IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew<static_cast<LONG>(sizeof(dos)) || dos.e_lfanew>0x1000 ||
            !Read(dos.e_lfanew,nt) || nt.Signature!=IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
            nt.FileHeader.SizeOfOptionalHeader!=sizeof(nt.OptionalHeader) ||
            nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.OptionalHeader.SizeOfImage!=imageSize_ ||
            !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96 ||
            nt.OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_EXCEPTION)
            return false;
        const auto first=static_cast<uint32_t>(dos.e_lfanew)+sizeof(nt);
        for (unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
            IMAGE_SECTION_HEADER section{};
            if (!Read(static_cast<uint32_t>(first+i*sizeof(section)),section)) return false;
            if (!section.Misc.VirtualSize) continue;
            if (!Range(section.VirtualAddress,section.Misc.VirtualSize)) return false;
            const auto flags=section.Characteristics;
            const auto kind=(flags&IMAGE_SCN_MEM_EXECUTE)?SectionKind::Code:
                            (flags&IMAGE_SCN_MEM_WRITE)?SectionKind::Writable:SectionKind::ReadOnly;
            const Section span{section.VirtualAddress,section.VirtualAddress+section.Misc.VirtualSize,kind,
                               (flags&IMAGE_SCN_MEM_DISCARDABLE)!=0};
            for (const auto& other:sections_)
                if (span.begin<other.end && other.begin<span.end) return false;
            sections_.push_back(span);
        }
        const auto directory=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!directory.Size || directory.Size%sizeof(RuntimeRow) ||
            Kind(directory.VirtualAddress,directory.Size)!=SectionKind::ReadOnly) return false;
        owners_.resize(directory.Size/sizeof(RuntimeRow));
        if (!Read(directory.VirtualAddress,owners_.data(),directory.Size)) return false;
        return std::is_sorted(owners_.begin(),owners_.end(),
                              [](const auto& a,const auto& b){ return a.begin<b.begin; });
    }

    uint32_t Resolve(uintptr_t key) const {
        const auto it=addresses_.find(key);
        return it==addresses_.end()?0:it->second;
    }

    bool Bind(uintptr_t key,uint32_t rva) {
        if (!rva || !Range(rva,1)) return false;
        auto [it,added]=addresses_.emplace(key,rva);
        return added || it->second==rva;
    }

    static bool Matches(const uint8_t* actual,const uint8_t* original,const uint8_t* mask,size_t size) {
        for (size_t i=0;i<size;++i)
            if ((actual[i]^original[i])&mask[i]) return false;
        return true;
    }

    bool BindOwners() {
        std::array<uint8_t,128> observed{};
        for (const auto& owner:kOwners) {
            failedRole_=owner.roles.front().key;
            uint32_t found=0;
            if (owner.size>observed.size() || owner.offset>owner.length ||
                owner.size>owner.length-owner.offset) return false;
            for (const auto& row:owners_) {
                if (row.end<=row.begin || row.end-row.begin!=owner.length ||
                    Kind(row.begin,owner.length)!=SectionKind::Code ||
                    Kind(row.unwind,4)!=SectionKind::ReadOnly ||
                    !Read(row.begin+owner.offset,observed.data(),owner.size) ||
                    !Matches(observed.data(),owner.bytes,owner.mask,owner.size)) continue;
                if (found) return false;
                found=row.begin;
            }
            if (!found) return false;
            for (const auto& role:owner.roles)
                if (role.offset>=owner.length || !Bind(role.key,found+role.offset)) return false;
        }
        return true;
    }

    const RuntimeRow* OwnerOf(uint32_t rva) const {
        auto row=std::upper_bound(owners_.begin(),owners_.end(),rva,
            [](uint32_t address,const auto& record) { return address<record.begin; });
        if (row==owners_.begin()) return nullptr;
        --row;
        return rva<row->end ? &*row : nullptr;
    }

    bool Follow(uint32_t site,unsigned field,unsigned next,SectionKind kind,uintptr_t key) {
        int32_t displacement=0;
        if (!site || !Read(site+field,displacement)) return false;
        const int64_t value=static_cast<int64_t>(site)+next+displacement;
        return value>0 && value<static_cast<int64_t>(imageSize_) &&
               Kind(static_cast<uint32_t>(value))==kind && Bind(key,static_cast<uint32_t>(value));
    }

    const SiteShape* Site(uintptr_t key) const {
        const auto after=std::lower_bound(std::begin(kSites),std::end(kSites),key,
            [](const SiteShape& shape,uintptr_t address){ return shape.key<address; });
        return after!=std::end(kSites) && after->key==key ? after : nullptr;
    }

    bool BindGrowContext(uintptr_t key,const RuntimeRow& row) {
        failedRole_=key;
        const auto* shape=Site(key);
        if (!shape || row.end<=row.begin || row.end-row.begin>2048 ||
            Kind(row.begin,row.end-row.begin)!=SectionKind::Code) return false;
        std::vector<uint8_t> code(row.end-row.begin);
        if (!Read(row.begin,code.data(),code.size()) || shape->size>code.size()) return false;
        uint32_t match=0;
        for (size_t i=0;i<=code.size()-shape->size;++i) {
            if (!Matches(code.data()+i,shape->bytes,shape->mask,shape->size)) continue;
            if (match) return false;
            match=row.begin+static_cast<uint32_t>(i);
        }
        return Bind(key,match);
    }

    // The type name, self-relative COL and vtable[-1] are the MSVC RTTI chain.
    // The fixture image is copied from disk and has its preferred-base pointers.
    bool BindComponentTable(uintptr_t pointerBase) {
        constexpr char name[]=".?AVClientRemoteCatchActorComponent@pa@@";
        uint32_t type=0,locator=0,table=0;
        std::vector<uint8_t> bytes;
        for (const auto& section:sections_) {
            if (section.kind!=SectionKind::Writable || section.discardable) continue;
            bytes.resize(section.end-section.begin);
            if (!Read(section.begin,bytes.data(),bytes.size())) return false;
            auto found=std::search(bytes.begin(),bytes.end(),std::begin(name),std::end(name));
            if (found==bytes.end()) continue;
            const size_t offset=found-bytes.begin();
            if (type || offset<16 || (offset&7) ||
                std::search(found+sizeof(name),bytes.end(),std::begin(name),std::end(name))!=bytes.end())
                return false;
            type=section.begin+static_cast<uint32_t>(offset)-16;
        }
        if (!type) return false;
        for (const auto& section:sections_) {
            if (section.kind!=SectionKind::ReadOnly || section.discardable) continue;
            bytes.resize(section.end-section.begin);
            if (!Read(section.begin,bytes.data(),bytes.size())) return false;
            for (size_t i=0;i+24<=bytes.size();i+=4) {
                std::array<uint32_t,6> col{};
                std::memcpy(col.data(),bytes.data()+i,sizeof(col));
                if (col[0]!=1 || col[1] || col[2] || col[3]!=type || col[5]!=section.begin+i ||
                    Kind(col[4],16)!=SectionKind::ReadOnly) continue;
                if (locator) return false;
                locator=col[5];
            }
        }
        if (!locator) return false;
        for (const auto& section:sections_) {
            if (section.kind!=SectionKind::ReadOnly || section.discardable) continue;
            bytes.resize(section.end-section.begin);
            if (!Read(section.begin,bytes.data(),bytes.size())) return false;
            for (size_t i=0;i+16<=bytes.size();i+=8) {
                uintptr_t value=0,first=0;
                std::memcpy(&value,bytes.data()+i,8);
                if (value!=pointerBase+locator) continue;
                std::memcpy(&first,bytes.data()+i+8,8);
                if (first<pointerBase || first-pointerBase>=imageSize_ ||
                    Kind(static_cast<uint32_t>(first-pointerBase))!=SectionKind::Code || table) return false;
                table=section.begin+static_cast<uint32_t>(i)+8;
            }
        }
        return Bind(0x5994CC8,table);
    }

    bool BindLinkedRoles(uintptr_t pointerBase) {
        failedRole_=0x3E3860;
        if (!Follow(Resolve(0x140F96B)+18,1,5,SectionKind::Code,0x3E3860)) return false;
        constexpr uintptr_t growOwner=0x8AB54E0;
        if (!Follow(Resolve(0x3E3860),1,5,SectionKind::Code,growOwner)) return false;
        const auto* row=OwnerOf(Resolve(growOwner));
        if (!row || row->begin!=Resolve(growOwner) || !BindGrowContext(0x8AB5509,*row) ||
            !BindGrowContext(0x8AB55F6,*row)) return false;
        failedRole_=0x1595740;
        if (!Follow(Resolve(0x676AAC),1,5,SectionKind::Code,0x1595740) ||
            !Follow(Resolve(0x8AB55F6)+15,1,5,SectionKind::Code,0x1595740)) return false;
        failedRole_=0x72451C0;
        if (!Follow(Resolve(0x6747BC),3,7,SectionKind::Writable,0x72451C0)) return false;
        failedRole_=0x716D858;
        if (!Follow(Resolve(0x13B8CB7),3,7,SectionKind::Writable,0x716D858)) return false;
        for (const auto gate:{0x13BD1B4,0x3C89BE,0x69683F})
            if (!Follow(Resolve(gate),2,7,SectionKind::Writable,0x716D858)) return false;
        failedRole_=0x5994CC8;
        return BindComponentTable(pointerBase);
    }

    bool VerifySites() {
        for (const auto& site:kSites) {
            failedRole_=site.key;
            const auto rva=Resolve(site.key);
            if (Kind(rva,site.size)!=SectionKind::Code) return false;
            std::vector<uint8_t> actual(site.size);
            if (!Read(rva,actual.data(),actual.size()) ||
                !Matches(actual.data(),site.bytes,site.mask,site.size)) return false;
            for (const auto& edge:site.edges) {
                int32_t displacement=0;
                if (edge.width==1) displacement=static_cast<int8_t>(actual[edge.field]);
                else std::memcpy(&displacement,actual.data()+edge.field,sizeof(displacement));
                const auto target=static_cast<int64_t>(rva)+edge.next+displacement;
                if (target<0 || target>=static_cast<int64_t>(imageSize_) ||
                    Kind(static_cast<uint32_t>(target))!=edge.kind ||
                    (edge.role && target!=Resolve(edge.role))) return false;
            }
            witnesses_.emplace(site.key,std::move(actual));
        }
        return true;
    }

public:
    bool Initialize(uintptr_t base,size_t size,uintptr_t pointerBase=0) {
        if (ready_) return base==base_ && size==imageSize_;
        *this=Roles{};
        base_=base;
        imageSize_=size;
        if (!LoadLayout() || !BindOwners() ||
            !BindLinkedRoles(pointerBase?pointerBase:base) || !VerifySites()) return false;
        ready_=true;
        return true;
    }

    uintptr_t FailedRole() const { return failedRole_; }
    bool Ready() const { return ready_; }

    uintptr_t Address(uintptr_t key) const {
        const auto rva=Resolve(key);
        return ready_ && rva ? base_+rva : 0;
    }

    std::span<const uint8_t> Witness(uintptr_t key,size_t length) const {
        const auto it=witnesses_.find(key);
        if (!ready_ || it==witnesses_.end() || length>it->second.size()) return {};
        return {it->second.data(),length};
    }
};
}

