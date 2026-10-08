// What x4vr-run passes from `x4vr vr-vulkan` to the mod (src/linux/vr_query.hpp): extension lists and
// the headset GPU's UUID, written by one and read by the other.
#include "vr_query.hpp"
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }
}

int main() {
    using namespace x4vr::vr_query;
    check(words("VK_KHR_a  VK_KHR_b\tVK_KHR_c\n") == std::vector<std::string>{"VK_KHR_a", "VK_KHR_b", "VK_KHR_c"}, "words split on any space");
    check(words("").empty() && words("   ").empty(), "no words");
    check(join({"VK_KHR_a", "VK_KHR_b"}) == "VK_KHR_a VK_KHR_b" && join({}).empty(), "join");
    check(words(join({"VK_KHR_a", "VK_KHR_b"})) == std::vector<std::string>{"VK_KHR_a", "VK_KHR_b"}, "join and words round trip");

    const uint8_t uuid[uuid_size]{0x00, 0x01, 0x7f, 0x80, 0xab, 0xcd, 0xef, 0xff, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x90};
    const auto text = uuid_text(uuid);
    check(text == "00017f80abcdefff1020304050607090", "UUID as 32 lowercase hex digits");
    const auto parsed = parse_uuid(text);
    check(parsed && std::equal(parsed->begin(), parsed->end(), uuid), "UUID round trip");
    check(parse_uuid("00017F80ABCDEFFF1020304050607090") == parsed, "upper case accepted");
    check(!parse_uuid("") && !parse_uuid("00017f80abcdefff102030405060709") && !parse_uuid("00017f80abcdefff10203040506070900"), "wrong length refused");
    check(!parse_uuid("00017f80abcdefff10203040506070zz"), "not hex refused");

    if (failures) return 1;
    std::printf("vr_query: all passed\n");
}
