#include "elf_classes.hpp"
#include <cxxabi.h>
#include <elf.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace x4vr::elf {
Image Image::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open "+path);
    Image image;
    image.file_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const auto& f = image.file_;
    if (f.size() < sizeof(Elf64_Ehdr) || std::memcmp(f.data(), ELFMAG, SELFMAG) != 0)
        throw std::runtime_error(path+" is not an ELF file");
    Elf64_Ehdr header;
    std::memcpy(&header, f.data(), sizeof(header));
    if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_X86_64)
        throw std::runtime_error(path+" is not a little-endian x86-64 ELF64 file");
    if (header.e_type != ET_EXEC)
        throw std::runtime_error(path+" is position-independent; its data pointers live in relocations, which this scan doesn't apply");
    if (header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phoff+uint64_t(header.e_phnum)*sizeof(Elf64_Phdr) > f.size())
        throw std::runtime_error(path+" has a damaged program header table");
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr ph;
        std::memcpy(&ph, f.data()+header.e_phoff+i*sizeof(Elf64_Phdr), sizeof(ph));
        if (ph.p_type != PT_LOAD || !ph.p_filesz) continue;
        if (ph.p_offset+ph.p_filesz > f.size()) throw std::runtime_error(path+" has a truncated segment");
        image.segments_.push_back({ph.p_vaddr, {f.data()+ph.p_offset, ph.p_filesz}, (ph.p_flags & PF_X) != 0, (ph.p_flags & PF_W) != 0});
    }
    return image;
}
Image Image::from_segments(std::vector<Segment> segments) {
    Image image;
    image.segments_ = std::move(segments);
    return image;
}
const unsigned char* Image::at(uint64_t vaddr, size_t size) const {
    for (const auto& s : segments_)
        if (vaddr >= s.vaddr && vaddr-s.vaddr <= s.bytes.size() && s.bytes.size()-(vaddr-s.vaddr) >= size)
            return s.bytes.data()+(vaddr-s.vaddr);
    return nullptr;
}
std::optional<uint64_t> Image::qword(uint64_t vaddr) const {
    const auto* p = at(vaddr, 8);
    if (!p) return std::nullopt;
    uint64_t value;
    std::memcpy(&value, p, 8);
    return value;
}
std::optional<std::string_view> Image::string(uint64_t vaddr) const {
    for (const auto& s : segments_) {
        if (vaddr < s.vaddr || vaddr-s.vaddr >= s.bytes.size()) continue;
        const auto* begin = reinterpret_cast<const char*>(s.bytes.data()+(vaddr-s.vaddr));
        const size_t room = std::min<size_t>(s.bytes.size()-(vaddr-s.vaddr), 4096);
        const auto* end = static_cast<const char*>(std::memchr(begin, '\0', room));
        if (!end) return std::nullopt;
        return std::string_view(begin, size_t(end-begin));
    }
    return std::nullopt;
}
bool Image::is_code(uint64_t vaddr) const {
    for (const auto& s : segments_)
        if (s.exec && vaddr >= s.vaddr && vaddr-s.vaddr < s.bytes.size()) return true;
    return false;
}

std::string demangle_type(std::string_view mangled) {
    if (mangled.empty() || mangled.size() > 1024) return {};
    const std::string name(mangled);
    // Type names are either nested (N...E) or start with their length.
    if (name[0] != 'N' && !(name[0] >= '1' && name[0] <= '9')) return {};
    int status = 0;
    std::unique_ptr<char, void (*)(void*)> text(abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status), std::free);
    return status == 0 && text ? std::string(text.get()) : std::string();
}

namespace {
// Calls `visit(address, value)` for every 8-byte-aligned qword in the non-executable segments.
template<class Visit> void each_data_qword(const Image& image, Visit visit) {
    for (const auto& s : image.segments()) {
        if (s.exec) continue;
        for (size_t off = (8-s.vaddr%8)%8; off+8 <= s.bytes.size(); off += 8) {
            uint64_t value;
            std::memcpy(&value, s.bytes.data()+off, 8);
            visit(s.vaddr+off, value);
        }
    }
}
// The type name a type_info at `typeinfo` points to, demangled; empty if it isn't a type_info.
std::string type_name_at(const Image& image, uint64_t typeinfo, std::string* mangled = nullptr) {
    const auto name_ptr = image.qword(typeinfo+8);
    if (!name_ptr) return {};
    auto text = image.string(*name_ptr);
    if (!text) return {};
    if (!text->empty() && text->front() == '*') text->remove_prefix(1); // local types carry a '*'
    auto name = demangle_type(*text);
    if (!name.empty() && mangled) *mangled = std::string(*text);
    return name;
}
std::vector<std::string> bases_of(const Image& image, uint64_t typeinfo) {
    if (const auto single = image.qword(typeinfo+16)) { // __si_class_type_info
        if (auto name = type_name_at(image, *single); !name.empty()) return {name};
    }
    const auto* counts = image.at(typeinfo+16, 8); // __vmi_class_type_info
    if (!counts) return {};
    uint32_t count;
    std::memcpy(&count, counts+4, 4);
    if (count == 0 || count > 32) return {};
    std::vector<std::string> bases;
    for (uint32_t i = 0; i < count; ++i) {
        const auto base = image.qword(typeinfo+24+16*i);
        auto name = base ? type_name_at(image, *base) : std::string();
        if (name.empty()) return {};
        bases.push_back(std::move(name));
    }
    return bases;
}
}

std::vector<ClassInfo> find_classes(const Image& image, std::string_view filter) {
    if (filter.size() < 3) throw std::invalid_argument("class filter needs at least 3 characters");
    // 1. Mangled type names containing the filter, by address (also the '*'-prefixed form).
    std::unordered_map<uint64_t, std::string> names;
    for (const auto& s : image.segments()) {
        if (s.exec) continue;
        const auto* data = reinterpret_cast<const char*>(s.bytes.data());
        const std::string_view all(data, s.bytes.size());
        for (size_t hit = all.find(filter); hit != std::string_view::npos; hit = all.find(filter, hit+1)) {
            size_t begin = hit;
            while (begin > 0 && all[begin-1] != '\0' && hit-begin < 1024) --begin;
            const size_t end = all.find('\0', hit);
            if (end == std::string_view::npos) break;
            auto text = all.substr(begin, end-begin);
            const bool local = !text.empty() && text.front() == '*'; // a type_info may point at either form
            if (local) text.remove_prefix(1);
            if (demangle_type(text).empty()) continue;
            names[s.vaddr+begin] = std::string(text);
            if (local) names[s.vaddr+begin+1] = std::string(text);
        }
    }
    // 2. type_infos: a qword at typeinfo+8 points to the name.
    std::vector<ClassInfo> classes;
    std::unordered_map<uint64_t, size_t> by_typeinfo;
    each_data_qword(image, [&](uint64_t address, uint64_t value) {
        const auto name = names.find(value);
        if (name == names.end() || address < 8) return;
        const uint64_t typeinfo = address-8;
        if (by_typeinfo.count(typeinfo)) return;
        by_typeinfo.emplace(typeinfo, classes.size());
        classes.push_back({name->second, demangle_type(name->second), typeinfo, bases_of(image, typeinfo), {}});
    });
    // 3. vtables: a qword pointing to the type_info, preceded by a small offset to top and
    // followed by code addresses. Base-class references inside other type_infos fail this.
    each_data_qword(image, [&](uint64_t address, uint64_t value) {
        const auto owner = by_typeinfo.find(value);
        if (owner == by_typeinfo.end()) return;
        const auto top = image.qword(address-8);
        if (!top || int64_t(*top) > 0 || int64_t(*top) < -(int64_t(1) << 24) || int64_t(*top) % 8) return;
        VTable vtable{address, int64_t(*top), {}};
        for (uint64_t slot = address+8; vtable.slots.size() < 1024; slot += 8) {
            const auto target = image.qword(slot);
            if (!target || !image.is_code(*target)) break;
            vtable.slots.push_back(*target);
        }
        if (!vtable.slots.empty()) classes[owner->second].vtables.push_back(std::move(vtable));
    });
    std::sort(classes.begin(), classes.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    for (auto& c : classes)
        std::sort(c.vtables.begin(), c.vtables.end(), [](const auto& a, const auto& b) { return a.offset_to_top > b.offset_to_top; });
    return classes;
}
}
