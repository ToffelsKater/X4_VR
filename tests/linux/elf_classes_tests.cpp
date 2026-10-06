// RTTI class scan, checked against this test's own classes. Linked without PIE (as Linux X4 is),
// so the file holds the absolute addresses the process uses.
#include "elf_classes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <typeinfo>

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

namespace VR {
struct ProbeTrackerBase {
    virtual ~ProbeTrackerBase();
    virtual int position();
};
struct ProbeTracker : ProbeTrackerBase {
    int position() override;
    virtual int still();
};
struct ProbeListener { virtual ~ProbeListener(); virtual void heard(); };
struct ProbeBoth : ProbeTracker, ProbeListener { int still() override; void heard() override; };
ProbeTrackerBase::~ProbeTrackerBase() = default;
int ProbeTrackerBase::position() { return 1; }
int ProbeTracker::position() { return 2; }
int ProbeTracker::still() { return 3; }
ProbeListener::~ProbeListener() = default;
void ProbeListener::heard() {}
int ProbeBoth::still() { return 4; }
void ProbeBoth::heard() {}
}

static const x4vr::elf::ClassInfo* find(const std::vector<x4vr::elf::ClassInfo>& classes, const char* name) {
    for (const auto& c : classes) if (c.name == name) return &c;
    return nullptr;
}
static uint64_t vptr(const void* object) { uint64_t v; std::memcpy(&v, object, 8); return v; }

int main() {
    CHECK(x4vr::elf::demangle_type("N2VR9OpenTrackE") == "VR::OpenTrack");
    CHECK(x4vr::elf::demangle_type("N1U23HeadTrackerCameraBridgeE") == "U::HeadTrackerCameraBridge");
    CHECK(x4vr::elf::demangle_type("hello").empty() && x4vr::elf::demangle_type("").empty());

    const auto image = x4vr::elf::Image::load("/proc/self/exe");
    const auto classes = x4vr::elf::find_classes(image, "Probe");
    for (const auto& c : classes) std::printf("found %s (%zu vtables)\n", c.name.c_str(), c.vtables.size());

    const VR::ProbeTracker tracker;
    const VR::ProbeBoth both;
    const auto* t = find(classes, "VR::ProbeTracker");
    CHECK(t && t->mangled == "N2VR12ProbeTrackerE");
    CHECK(t->typeinfo == reinterpret_cast<uint64_t>(&typeid(VR::ProbeTracker)));
    CHECK(t->bases.size() == 1 && t->bases[0] == "VR::ProbeTrackerBase");
    CHECK(t->vtables.size() == 1 && t->vtables[0].offset_to_top == 0);
    CHECK(t->vtables[0].address_point() == vptr(&tracker));
    CHECK(t->vtables[0].slots.size() == 4); // two destructor entries, position, still

    const auto* b = find(classes, "VR::ProbeBoth");
    CHECK(b && b->bases.size() == 2 && b->bases[0] == "VR::ProbeTracker" && b->bases[1] == "VR::ProbeListener");
    CHECK(b->vtables.size() == 2 && b->vtables[0].offset_to_top == 0 && b->vtables[1].offset_to_top < 0);
    CHECK(b->vtables[0].address_point() == vptr(&both));
    CHECK(b->vtables[1].address_point() == vptr(static_cast<const VR::ProbeListener*>(&both)));

    CHECK(find(classes, "VR::ProbeTrackerBase") && find(classes, "VR::ProbeListener"));
    bool refused = false;
    try { (void)x4vr::elf::find_classes(image, "ab"); } catch (const std::invalid_argument&) { refused = true; }
    CHECK(refused);
    std::puts("elf class tests passed");
}
