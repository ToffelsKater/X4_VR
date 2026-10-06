#pragma once
// OpenTrack's "UDP over network" packet, which Linux X4 reads when OpenTrack Support is on
// (Controls → Head Tracking Support; default 127.0.0.1:4242). Six little-endian doubles:
// x, y, z, yaw, pitch, roll, in centimetres and degrees.
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

namespace x4vr::opentrack {
inline constexpr uint16_t default_port = 4242;

struct Pose {
    double x = 0, y = 0, z = 0;          // OpenTrack convention: centimetres
    double yaw = 0, pitch = 0, roll = 0; // OpenTrack convention: degrees
    double& axis(int i) { return (&x)[i]; }
    double axis(int i) const { return (&x)[i]; }
};
static_assert(sizeof(Pose) == 6*sizeof(double));

using Packet = std::array<unsigned char, 48>;
static_assert(std::endian::native == std::endian::little, "OpenTrack packets are little-endian");

inline Packet encode(const Pose& pose) {
    Packet packet;
    for (int i = 0; i < 6; ++i) { const double v = pose.axis(i); std::memcpy(packet.data()+8*i, &v, 8); }
    return packet;
}
inline std::optional<Pose> decode(const void* data, size_t size) {
    if (size != sizeof(Packet)) return std::nullopt;
    Pose pose;
    for (int i = 0; i < 6; ++i) std::memcpy(&pose.axis(i), static_cast<const unsigned char*>(data)+8*i, 8);
    return pose;
}

inline constexpr std::array<std::string_view, 6> axis_names{"x", "y", "z", "yaw", "pitch", "roll"};
inline int axis_index(std::string_view name) {
    for (int i = 0; i < 6; ++i) if (axis_names[i] == name) return i;
    return -1;
}
}
