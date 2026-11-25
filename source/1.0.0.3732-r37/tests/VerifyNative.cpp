#define NOMINMAX
#include <windows.h>
#include "../src/SuperAxiomForce/NativeDiscovery.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

static int failures = 0;
static void Check(bool value, const char* name) {
    std::printf("%s: %s\n", value ? "PASS" : "FAIL", name);
    if (!value) ++failures;
}

static void Rel32(std::vector<uint8_t>& bytes, size_t field, size_t target) {
    const int32_t displacement = static_cast<int32_t>(target) - static_cast<int32_t>(field + 4);
    std::memcpy(bytes.data() + field, &displacement, 4);
}

static void Report(const char* name, const native::Binding& binding, size_t instruction, size_t target) {
    std::printf("%s matches=%zu valid=%d instruction=0x%zX target=0x%zX\n",
        name, binding.matches, binding.valid, binding.instruction, binding.target);
    Check(binding.valid && binding.matches == 1 && binding.instruction == instruction &&
          binding.target == target, name);
}

static void Mutations(const std::vector<uint8_t>& actual) {
    std::vector<uint8_t> bytes(0x1000, 0xCC);
    const auto copy = [&](size_t to, size_t from, size_t size) {
        std::memcpy(bytes.data() + to, actual.data() + from, size);
    };
    copy(0x100, 0x64A889, 21);
    copy(0x180, 0x64BB1B, 29);
    copy(0x200, 0x65DC6B, 29);
    copy(0x280, 0x65A0E9, 33);
    copy(0x300, 0x65ACE0, 27);
    copy(0x380, 0x65CC96, 63);
    Rel32(bytes, 0x104, 0x800);
    Rel32(bytes, 0x183, 0x810);
    Rel32(bytes, 0x18A, 0x800);
    Rel32(bytes, 0x204, 0x820);
    Rel32(bytes, 0x285, 0x240);
    Rel32(bytes, 0x29C, 0x500);
    const auto original = bytes;
    native::Image image{bytes.data(), bytes.size(), {{0, 0x800}}, {{0x800, 0x800}}};
    auto result = native::Discover(image);
    Check(result.range.valid && result.limit.valid && result.pull.valid &&
          result.activation.binding.valid && result.reel.binding.valid && result.propell.binding.valid,
          "relocated real-code fixture resolves all six independent contexts");

    bytes[0x200] = 0xCC;
    result = native::Discover(image);
    Check(result.range.valid && result.limit.valid && !result.pull.valid,
          "missing PullSpeed does not discard proven Range/Limit bindings");
    bytes = original;
    std::memcpy(bytes.data() + 0x600, bytes.data() + 0x100, 21);
    Rel32(bytes, 0x604, 0x800);
    result = native::Discover(image);
    Check(!result.range.valid && result.range.matches == 2 && result.pull.valid,
          "duplicate Range context fails closed while PullSpeed remains independent");
    bytes = original;
    Rel32(bytes, 0x104, 0x801);
    Check(!native::Discover(image).range.valid, "unaligned float target rejected");
    Rel32(bytes, 0x104, 0x500);
    Check(!native::Discover(image).range.valid, "code-target float rejected");
    Rel32(bytes, 0x104, 0x2000);
    Check(!native::Discover(image).range.valid, "out-of-image float rejected");
    bytes = original;
    image.writable.clear();
    result = native::Discover(image);
    Check(!result.range.valid && !result.limit.valid && !result.pull.valid,
          "read-only data is not patchable");
    image.writable = {{0x800, 0x800}};
    Rel32(bytes, 0x18A, 0x824);
    Check(!native::Discover(image).limit.valid, "limit selector must corroborate Range target");
    bytes = original;
    Rel32(bytes, 0x204, 0x800);
    Check(!native::Discover(image).pull.valid, "aliased PullSpeed/Range rejected");
    bytes = original;
    std::memset(bytes.data() + 0x283, 0x90, 6);
    result = native::Discover(image);
    Check(result.activation.binding.valid && !result.activation.canRestore,
          "external six-byte activation NOP recognized without claiming restoration");
    bytes[0x288] = 0xFF;
    Check(!native::Discover(image).activation.binding.valid, "partial activation NOP rejected");
    bytes = original;
    Rel32(bytes, 0x285, 0x500);
    Check(!native::Discover(image).activation.binding.valid, "wrong activation branch direction rejected");
    bytes = original;
    Rel32(bytes, 0x29C, 0x810);
    Check(!native::Discover(image).activation.binding.valid, "activation call into data rejected");
    bytes = original;
    bytes[0x305] = 0xEB;
    bytes[0x399] = 0xEB;
    result = native::Discover(image);
    Check(result.reel.binding.valid && !result.reel.canRestore &&
          result.propell.binding.valid && !result.propell.canRestore,
          "external duration jumps recognized without ownership");
    bytes[0x306] = 0x10;
    Check(!native::Discover(image).reel.binding.valid, "changed duration branch endpoint rejected");
    bytes = original;
    std::memcpy(bytes.data() + 0x600, bytes.data() + 0x300, 27);
    Check(!native::Discover(image).reel.binding.valid, "duplicate duration gate rejected");
    bytes = original;
    bytes[0x180 + 16] = 0x28;
    Check(!native::Discover(image).limit.valid, "unproven selector stack-flag offset rejected");
    bytes = original;
    bytes[0x380 + 5] = 0x94;
    Check(!native::Discover(image).propell.binding.valid, "mixed old/new propell duration member rejected");
    bytes = original;
    bytes[0x380 + 33] = 0x90;
    Check(!native::Discover(image).propell.binding.valid, "mixed old/new propell elapsed member rejected");
    bytes = original;
    image.code = {{0x100, 8}};
    Check(!native::Discover(image).range.valid, "truncated executable span cannot match");
    image.code.clear();
    Check(!native::Discover(image).activation.binding.valid, "no readable code means no patch sites");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: VerifyNative.exe path-to-CrimsonDesert.exe\n");
        return 2;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("cannot open input");
        const auto length = input.tellg();
        if (length < 0x100 || length > 0x80000000LL) throw std::runtime_error("invalid file length");
        std::vector<uint8_t> file(static_cast<size_t>(length));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(file.data()), length)) throw std::runtime_error("short read");
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
            throw std::runtime_error("invalid DOS header");
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->OptionalHeader.SizeOfImage > 0x80000000ULL) throw std::runtime_error("invalid PE64 image");
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        if (reinterpret_cast<const uint8_t*>(sections + nt->FileHeader.NumberOfSections) > file.data() + file.size())
            throw std::runtime_error("invalid section table");
        std::vector<uint8_t> bytes(nt->OptionalHeader.SizeOfImage, 0);
        native::Image image{bytes.data(), bytes.size(), {}, {}};
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& sec = sections[i];
            const size_t rva = sec.VirtualAddress, raw = sec.PointerToRawData;
            const size_t size = sec.Misc.VirtualSize, count = (std::min)(size, static_cast<size_t>(sec.SizeOfRawData));
            if (rva > bytes.size() || size > bytes.size() - rva || raw > file.size() || count > file.size() - raw)
                throw std::runtime_error("section outside file/image");
            std::memcpy(bytes.data() + rva, file.data() + raw, count);
            if (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) image.code.push_back({rva, size});
            else if (sec.Characteristics & IMAGE_SCN_MEM_WRITE) image.writable.push_back({rva, size});
        }
        std::printf("Read-only offline PE image: %s size=0x%zX; no game code executed\n", argv[1], bytes.size());
        const auto result = native::Discover(image);
        Report("Range", result.range, 0x64A889, 0x70E73E8);
        Report("Limit", result.limit, 0x64BB1B, 0x70E7398);
        Report("PullSpeed", result.pull, 0x65DC6B, 0x70E7F28);
        const size_t rangeFields[] = {0x64BB1E, 0x64D0ED, 0x134ED2D, 0x64A88D, 0x64BB25, 0x134ED34};
        for (size_t i = 0; i < result.scopedRange.size(); ++i) {
            const auto& patch = result.scopedRange[i];
            Check(patch.binding.valid && patch.binding.matches == 1 && patch.offset == rangeFields[i] &&
                  patch.size == 4 && patch.canRestore, "native scoped geometric range reader");
            int32_t displacement = 0; std::memcpy(&displacement, patch.patch.data(), 4);
            Check(static_cast<int64_t>(patch.offset + 4) + displacement == static_cast<int64_t>(result.range.target),
                  "scoped geometric reader retargets corroborated Range");
        }
        auto corrupt = bytes;
        corrupt[0x64D0ED] ^= 1;
        auto changed = image; changed.bytes = corrupt.data();
        Check(!native::Discover(changed).scopedRange[1].binding.valid,
              "wrong candidate displacement rejected");
        corrupt = bytes; corrupt[0x134ED34] ^= 1;
        Check(!native::Discover(changed).scopedRange[2].binding.valid,
              "wrong leash companion Range displacement rejected");

        Check(result.clock[0].valid && result.clock[0].instruction == 0x65ACB5 &&
              result.clock[1].valid && result.clock[1].instruction == 0x65CB0C,
              "native elapsed-add adapters resolve exact current contracts");
        const native::CodePatch patches[] = {result.activation, result.reel, result.propell};
        const size_t expected[] = {0x65A0EC, 0x65ACE5, 0x65CCAF};
        for (size_t i = 0; i < 3; ++i) {
            const auto& p = patches[i];
            std::printf("Instant[%zu] matches=%zu valid=%d RVA=0x%zX size=%zu original=", i,
                        p.binding.matches, p.binding.valid, p.offset, p.size);
            for (size_t j = 0; j < p.size; ++j) std::printf("%02X ", p.original[j]);
            std::puts("");
            Check(p.binding.valid && p.binding.matches == 1 && p.offset == expected[i] && p.canRestore &&
                  p.size == (i == 0 ? 6U : 2U), "native InstantAxiom gate and patch width");
        }
        const auto straight = straight_pull::Discover(image.bytes, image.size, nt->OptionalHeader.ImageBase, image.code);
        std::printf("StraightPull contexts=%zu selected=%zu ignored=%zu target=0x%llX reason=%s\n",
            straight.contextMatches, straight.sites.size(), straight.ignoredContextCount,
            static_cast<unsigned long long>(straight.sharedTarget), straight.reason.c_str());
        Check(straight.valid && straight.contextMatches == 3 && straight.sites.size() == 2 &&
              straight.ignoredContextCount == 1 && straight.sharedTarget == 0x14137BA70ULL &&
              straight.sites[0].callOffset == 0x137EAFE && straight.sites[1].callOffset == 0x137EBDE,
              "exact native StraightPull pair excludes distant lookalike");
        Mutations(bytes);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ERROR: %s\n", error.what());
        return 2;
    }
    std::printf("Native verification failures=%d\n", failures);
    return failures ? 1 : 0;
}
