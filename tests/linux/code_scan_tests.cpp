// X4 scan (src/linux/code_scan.hpp) against this test's own executable: its .text holds the
// patterns at X4 9.00's layout (the camera offset's target block 0x281 past its jump), a frame
// half reader names a global, and a VR::OpenTrack class has `movss 0xd0(%rdi),%xmm0` in slot 34.
// Built non-PIE like X4.
#include "code_scan.hpp"
#include <cstdio>
#include <cstdlib>

extern "C" int x4vr_test_half;
int x4vr_test_half = 0;
extern "C" const unsigned char x4vr_test_clamp[], x4vr_test_zeroing[], x4vr_test_offset[], x4vr_test_half_reader[];
asm(R"(
    .pushsection .text
    .globl x4vr_test_clamp, x4vr_test_zeroing, x4vr_test_offset, x4vr_test_half_reader
x4vr_test_clamp:
    cmp $7,%eax
    .byte 0x74, 0x19
    movss -0xb0(%rbp),%xmm1
    pxor %xmm0,%xmm0
    comiss %xmm0,%xmm1
    .byte 0x76, 0x08
    movss %xmm0,-0xb0(%rbp)
    ud2
x4vr_test_zeroing:
    call x4vr_test_clamp
    test %al,%al
    .byte 0x0f, 0x84
    .long 0x100
    test %r13,%r13
    .byte 0x74, 0x2a
    mov 0x148(%r13),%rdi
    test %rdi,%rdi
    .byte 0x74, 0x1e
    mov (%rdi),%rax
    call *0x40(%rax)
    ud2
x4vr_test_offset:
    mov 0x18(%rbx),%r12
    test %r12,%r12
    .byte 0x0f, 0x84
    .long 0x213
    mov x4vr_test_half(%rip),%rax
    mov 0x780(%rbx),%rdx
    test %rax,%rax
    .byte 0x0f, 0x84
    .long 0x2b6
    cmp %rdx,0x238(%rax)
    .skip 0x281-(. - (x4vr_test_offset+13)), 0xcc
    movaps 0x10(%rbp),%xmm6
    movaps 0x20(%rbp),%xmm7
    movaps 0x30(%rbp),%xmm5
    movaps 0x0(%rbp),%xmm8
    .byte 0xe9
    .long 0
    ud2
x4vr_test_half_reader:
    mov x4vr_test_half(%rip),%eax
    xor $1,%eax
    ret
    .popsection
)");

namespace VR {
#define X4VR_SLOT(n) virtual int slot##n() const { return n; }
#define X4VR_SLOTS10(a) X4VR_SLOT(a##0) X4VR_SLOT(a##1) X4VR_SLOT(a##2) X4VR_SLOT(a##3) X4VR_SLOT(a##4) \
                        X4VR_SLOT(a##5) X4VR_SLOT(a##6) X4VR_SLOT(a##7) X4VR_SLOT(a##8) X4VR_SLOT(a##9)
struct OpenTrack { // slots 0 and 1: the destructors; then 32 more, so position() is slot 34
    virtual ~OpenTrack() = default;
    X4VR_SLOTS10(1) X4VR_SLOTS10(2) X4VR_SLOTS10(3) X4VR_SLOT(40) X4VR_SLOT(41)
    virtual float position() const; // out of line: the key function, so the vtable is emitted here
    unsigned char padding[0xd0-8]{};
    float value = 1;
};
__attribute__((noinline)) float OpenTrack::position() const { return value; }
}

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } };
    VR::OpenTrack* volatile tracker = new VR::OpenTrack; // keeps the class and its vtable
    check(tracker->position() == 1, "class in use");

    const auto image = x4vr::elf::Image::load("/proc/self/exe");
    const auto sites = x4vr::linux_port::code::find_x4_sites(image);
    for (const auto& note : sites.notes) std::printf("%s\n", note.c_str());
    namespace x4 = x4vr::linux_port::code::x4;
    check(sites.backward_clamp == uint64_t(reinterpret_cast<uintptr_t>(x4vr_test_clamp)), "backward clamp site");
    check(sites.onfoot_zeroing == uint64_t(reinterpret_cast<uintptr_t>(x4vr_test_zeroing)), "on-foot zeroing site");
    check(sites.camera_offset == uint64_t(reinterpret_cast<uintptr_t>(x4vr_test_offset)), "camera offset site");
    check(sites.player_global == uint64_t(reinterpret_cast<uintptr_t>(&x4vr_test_half)), "player global from the camera offset site");
    check(sites.frame_half_global == uint64_t(reinterpret_cast<uintptr_t>(&x4vr_test_half)), "frame half global");
    check(sites.opentrack_vtable != 0, "VR::OpenTrack vtable");
    check(image.at(sites.backward_clamp+x4::backward_clamp_at, 1) && *image.at(sites.backward_clamp+x4::backward_clamp_at, 1) == 0x76, "clamp's jbe");
    check(image.at(sites.onfoot_zeroing+x4::onfoot_zeroing_at, 1) && *image.at(sites.onfoot_zeroing+x4::onfoot_zeroing_at, 1) == 0x84, "zeroing's je");
    check(image.at(sites.camera_offset+x4::camera_offset_at, 1) && *image.at(sites.camera_offset+x4::camera_offset_at, 1) == 0x13, "offset's je displacement");
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
