#pragma once
// Finds C++ classes and their vtables in a non-PIE x86-64 ELF executable through the RTTI that
// a stripped binary keeps (Itanium C++ ABI): type name string → type_info → vtable.
//
//   type_info:  [+0] vptr of the type_info class   [+8] → mangled name ("N2VR9OpenTrackE")
//               __si_class_type_info: [+16] → base type_info
//               __vmi_class_type_info: [+16] flags (u32), base count (u32), then per base
//               [+24+16*i] → base type_info, [+32+16*i] offset and flags
//   vtable:     [-8] offset to top   [+0] → type_info   [+8...] virtual functions; an object's
//               vptr holds the address of the first function slot (the "address point")
//
// Linux X4 is a non-PIE executable, so these pointers are plain
// absolute addresses in the file. Position-independent binaries keep them in relocations
// instead and are refused.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x4vr::elf {
struct Segment {
    uint64_t vaddr = 0;                   // load address of the first byte
    std::span<const unsigned char> bytes; // the part backed by the file (no .bss)
    bool exec = false, write = false;
};

class Image {
public:
    // Reads an ELF64 x86-64 file and its PT_LOAD segments. Throws std::runtime_error.
    static Image load(const std::string& path);
    // Segments that already sit in memory (the running game); the caller keeps them alive.
    static Image from_segments(std::vector<Segment> segments);

    const std::vector<Segment>& segments() const { return segments_; }
    const unsigned char* at(uint64_t vaddr, size_t size) const; // nullptr if not file-backed
    std::optional<uint64_t> qword(uint64_t vaddr) const;
    std::optional<std::string_view> string(uint64_t vaddr) const; // NUL-terminated, max 4096
    bool is_code(uint64_t vaddr) const;

private:
    std::vector<unsigned char> file_;
    std::vector<Segment> segments_;
};

struct VTable {
    uint64_t typeinfo_ref = 0; // address of the vtable's type_info pointer
    int64_t offset_to_top = 0; // 0 for the primary vtable, negative for secondary ones
    std::vector<uint64_t> slots; // virtual function addresses, in slot order
    uint64_t address_point() const { return typeinfo_ref+8; } // what an object's vptr holds
};

struct ClassInfo {
    std::string mangled;   // e.g. N2VR9OpenTrackE
    std::string name;      // demangled, e.g. VR::OpenTrack
    uint64_t typeinfo = 0;
    std::vector<std::string> bases; // demangled
    std::vector<VTable> vtables;
};

// Demangles a type name ("N2VR9OpenTrackE" → "VR::OpenTrack"); empty if it isn't one.
std::string demangle_type(std::string_view mangled);

// Classes whose mangled type name contains `filter` (at least 3 characters), with their bases
// and vtables. A name that is present without a type_info is not reported.
std::vector<ClassInfo> find_classes(const Image& image, std::string_view filter);
}
