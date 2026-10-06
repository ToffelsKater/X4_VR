// OpenTrack UDP packet: six little-endian doubles x, y, z, yaw, pitch, roll, 48 bytes.
#include "opentrack.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

int main() {
    using namespace x4vr::opentrack;
    const Pose pose{1.5, -2, 3, 30, -45, 90};
    const auto packet = encode(pose);
    CHECK(packet.size() == 48);
    // 1.5 as an IEEE double: 0x3FF8000000000000, low byte first.
    const unsigned char one_and_half[8]{0, 0, 0, 0, 0, 0, 0xf8, 0x3f};
    for (int i = 0; i < 8; ++i) CHECK(packet[size_t(i)] == one_and_half[i]);
    const auto back = decode(packet.data(), packet.size());
    CHECK(back && back->x == 1.5 && back->y == -2 && back->z == 3 && back->yaw == 30 && back->pitch == -45 && back->roll == 90);
    CHECK(!decode(packet.data(), 47) && !decode(packet.data(), 49));
    CHECK(axis_index("x") == 0 && axis_index("yaw") == 3 && axis_index("roll") == 5 && axis_index("YAW") == -1);
    Pose p;
    p.axis(axis_index("pitch")) = 7;
    CHECK(p.pitch == 7);
    std::puts("opentrack packet tests passed");
}
