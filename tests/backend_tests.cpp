#include <x4vr/session.hpp>
#include "openxr_runtime.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void require(bool value, const char* description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
}
void near(float actual, float expected, const char* description, float epsilon = 0.00002f) {
    require(std::isfinite(actual) && std::abs(actual-expected) < epsilon, description);
}
template<class Operation> void rejects(Operation op, const char* description) {
    bool caught = false;
    try { op(); } catch (const std::exception&) { caught = true; }
    require(caught, description);
}
float ndc(const x4vr::Matrix& p, int axis, float x, float y, float z) {
    return (p.m[axis][0]*x + p.m[axis][1]*y + p.m[axis][2]*z + p.m[axis][3]) /
           (p.m[3][0]*x + p.m[3][1]*y + p.m[3][2]*z + p.m[3][3]);
}
x4vr::Matrix yaw(float angle) {
    auto r = x4vr::Matrix::identity();
    r.m[0][0] = r.m[2][2] = std::cos(angle);
    r.m[0][2] = std::sin(angle); r.m[2][0] = -std::sin(angle);
    return r;
}
void camera_math() {
    auto head = yaw(0.7f);
    head.m[0][3] = 1.2f; head.m[1][3] = 1.7f; head.m[2][3] = -2.f;
    const auto inv = x4vr::inverse_rigid(head);
    const auto product = x4vr::multiply(inv, head);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) near(product.m[i][j], i == j ? 1.f : 0.f, "Rigid inverse identity");
    const auto origin_inv = x4vr::inverse_rigid(x4vr::seated_origin(head));
    const auto centered = x4vr::multiply(origin_inv, head);
    for (int i = 0; i < 3; ++i) near(centered.m[i][3], 0, "Recenter removes translation");
    near(centered.m[0][0], 1, "Recenter removes yaw");
    auto pitch = x4vr::Matrix::identity();
    pitch.m[1][1] = pitch.m[2][2] = std::cos(0.3f);
    pitch.m[1][2] = -std::sin(0.3f); pitch.m[2][1] = std::sin(0.3f);
    const auto tilted = x4vr::multiply(head, pitch);
    const auto recentered = x4vr::multiply(x4vr::inverse_rigid(x4vr::seated_origin(tilted)), tilted);
    near(recentered.m[1][2], pitch.m[1][2], "Recenter preserves pitch");
    // Canted eyes have independent rotations, not just a constant IPD shift.
    auto left = yaw(0.1f), right = yaw(-0.1f);
    left.m[0][3] = -0.032f; right.m[0][3] = 0.032f;
    const auto world_left = x4vr::multiply(head, left);
    const auto world_right = x4vr::multiply(head, right);
    float distance2 = 0;
    for (int i = 0; i < 3; ++i) distance2 += std::pow(world_right.m[i][3] - world_left.m[i][3], 2.f);
    near(std::sqrt(distance2), 0.064f, "Head rotation preserves physical IPD");
    const auto relative = x4vr::multiply(x4vr::inverse_rigid(world_left), world_right);
    near(relative.m[0][0], std::cos(-0.2f), "Canted-eye relative rotation survives composition");
    auto invalid = head; invalid.m[0][0] = 3;
    rejects([&] { x4vr::inverse_rigid(invalid); }, "Reject scaling in tracking pose");
    invalid = head; invalid.m[0][3] = std::numeric_limits<float>::quiet_NaN();
    require(!x4vr::is_rigid(invalid), "Reject NaN pose");
    invalid = x4vr::Matrix::identity(); invalid.m[0][0] = -1;
    require(!x4vr::is_rigid(invalid), "Reject reflected tracking basis");
}
void projections() {
    for (const bool reverse : {false, true}) {
        const auto p = x4vr::vulkan_projection(-1.2f, 0.8f, -0.9f, 1.1f, 0.05f, 10000, reverse);
        near(ndc(p, 0, -1.2f, 0, -1), -1, "Asymmetric left frustum boundary");
        near(ndc(p, 0, 0.8f, 0, -1), 1, "Asymmetric right frustum boundary");
        near(ndc(p, 1, 0, -0.9f, -1), 1, "Vulkan lower boundary");
        near(ndc(p, 1, 0, 1.1f, -1), -1, "Vulkan upper boundary");
        near(ndc(p, 2, 0, 0, -0.05f), reverse ? 1.f : 0.f, "Vulkan near depth");
        near(ndc(p, 2, 0, 0, -10000), reverse ? 0.f : 1.f, "Vulkan far depth");
    }
    rejects([] { x4vr::vulkan_projection(-1, 1, -1, 1, 0, 100); }, "Reject zero near plane");
    rejects([] { x4vr::vulkan_projection(-1, 1, -1, 1, 10, 1); }, "Reject inverted depth range");
    rejects([] { x4vr::vulkan_projection(1, -1, -1, 1, 1, 10); }, "Reject inverted frustum");
    rejects([] { x4vr::vulkan_projection(-1, 1, -1, 1, 1, std::numeric_limits<float>::infinity()); }, "Reject non-finite far plane");
}
void image_contract() {
    // Synthetic metadata only: never pass these handles to OpenVR/Vulkan.
    vr::VRVulkanTextureData_t image{};
    image.m_nImage = 1;
    image.m_pDevice = reinterpret_cast<VkDevice_T*>(uintptr_t{1});
    image.m_pPhysicalDevice = reinterpret_cast<VkPhysicalDevice_T*>(uintptr_t{2});
    image.m_pInstance = reinterpret_cast<VkInstance_T*>(uintptr_t{3});
    image.m_pQueue = reinterpret_cast<VkQueue_T*>(uintptr_t{4});
    image.m_nWidth = 2400; image.m_nHeight = 2600;
    image.m_nFormat = 43; image.m_nSampleCount = 1;
    std::array images{image, image}; images[1].m_nImage = 2;
    require(x4vr::validate_images(images, 2400, 2600).empty(), "Accept coherent stereo metadata");
    auto bad = images; bad[1].m_nImage = 1;
    require(!x4vr::validate_images(bad, 2400, 2600).empty(), "Reject same image submitted as both eyes");
    bad = images; bad[1].m_pDevice = nullptr;
    require(!x4vr::validate_images(bad, 2400, 2600).empty(), "Reject missing context");
    bad = images; bad[1].m_pQueue = reinterpret_cast<VkQueue_T*>(uintptr_t{5});
    require(!x4vr::validate_images(bad, 2400, 2600).empty(), "Reject unsynchronized separate queues");
    bad = images; bad[1].m_nSampleCount = 4;
    require(!x4vr::validate_images(bad, 2400, 2600).empty(), "Reject unresolved MSAA");
    require(!x4vr::validate_images(images, 2401, 2600).empty(), "Reject stale extent after resolution change");
    x4vr::Session session; x4vr::Frame frame;
    x4vr::Matrix head;
    rejects([&] { session.sample_tracking(head); }, "Reject tracking before initialization");
    rejects([&] { session.begin_frame(frame, 0.05f, 100); }, "Reject frame before initialization");
    rejects([&] { session.submit(1, images); }, "Reject submission before initialization");
    session.shutdown(); session.shutdown();
}
// OpenXR backend: the same poses, frusta and texture crops as the OpenVR path, converted.
void openxr_conversions() {
    auto pose = yaw(0.7f);
    auto pitch = x4vr::Matrix::identity();
    pitch.m[1][1] = pitch.m[2][2] = std::cos(-1.2f); pitch.m[1][2] = -std::sin(-1.2f); pitch.m[2][1] = std::sin(-1.2f);
    pose = x4vr::multiply(pose, pitch);
    pose.m[0][3] = 0.03f; pose.m[1][3] = 1.2f; pose.m[2][3] = -0.4f;
    const auto xr = x4vr::to_xr(pose);
    const auto& q = xr.orientation;
    near(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w, 1, "Unit quaternion");
    const auto back = x4vr::from_xr(xr);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) near(back.m[i][j], pose.m[i][j], "Pose survives the OpenXR round trip", 1e-5f);
    auto half_turn = yaw(3.14159265f); // trace -1: the non-trace quaternion branches
    const auto turned = x4vr::from_xr(x4vr::to_xr(half_turn));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) near(turned.m[i][j], half_turn.m[i][j], "Half turn survives the round trip", 1e-5f);
    require(!x4vr::is_rigid(x4vr::Matrix{}), "An unset (all-zero) eye pose is not rigid: OpenXR uses the runtime pose");
    const std::array<float, 4> aero{-1.5697f, 0.7105f, -0.8036f, 1.1149f}; // Varjo Aero left eye, OpenVR convention
    const auto fov = x4vr::fov_from_tangents(aero);
    require(fov.angleLeft < 0 && fov.angleRight > 0 && fov.angleUp > 0 && fov.angleDown < 0, "OpenXR angle signs");
    near(fov.angleUp, 0.6770f, "Up angle from the negative OpenVR top", 1e-3f);
    const auto tangents = x4vr::tangents_from_fov(fov);
    for (int i = 0; i < 4; ++i) near(tangents[i], aero[i], "Tangents survive the round trip", 1e-5f);
    const auto rect = x4vr::bounds_rect({0.25f, 0.1f, 0.75f, 0.9f}, 1000, 2000);
    require(rect.offset.x == 250 && rect.offset.y == 200 && rect.extent.width == 500 && rect.extent.height == 1600, "Bounds to pixels");
    const auto flipped = x4vr::bounds_rect({0.25f, 0.9f, 0.75f, 0.1f}, 1000, 2000);
    require(flipped.offset.y == 200 && flipped.extent.height == 1600, "Bounds order does not matter");
    const std::vector<int64_t> varjo{43, 50, 37, 126, 129, 130};
    require(x4vr::swapchain_format(44, varjo) == 50, "B8G8R8A8_UNORM goes to its SRGB twin");
    require(x4vr::swapchain_format(37, varjo) == 43, "R8G8B8A8_UNORM goes to its SRGB twin");
    require(x4vr::swapchain_format(50, varjo) == 50, "SRGB stays");
    require(x4vr::swapchain_format(44, {37}) == 0, "No channel-swapping copy");
}
void shared_pose_pairs() {
    const auto a = x4vr::Matrix::identity();
    auto b = a; b.m[0][3] = 1;
    // Left eye: pose a (copy 1), then b (copy 3). Right eye: pose a (copy 2). Slot 2 is empty.
    std::array<std::array<x4vr::Matrix, 3>, 2> pose{{{a, b, b}, {a, b, b}}};
    const std::array<std::array<uint64_t, 3>, 2> seq{{{1, 3, 0}, {2, 0, 0}}};
    const std::array<std::array<bool, 3>, 2> filled{{{true, true, false}, {true, false, false}}};
    std::array<uint32_t, 2> pick{1, 0};
    require(x4vr::match_pair(pick, pose, seq, filled) && pick[0] == 0 && pick[1] == 0, "The eye that is ahead steps back to the other eye's pose");
    require(!x4vr::match_pair(pick, pose, seq, filled) && pick[0] == 0, "Equal poses stay");
    pose[0][0].m[1][3] = 1; // no left image with the right eye's pose
    pick = {1, 0};
    require(!x4vr::match_pair(pick, pose, seq, filled) && pick[0] == 1 && pick[1] == 0, "No match: the newest images go out");
}
}
int main() {
    try {
        camera_math(); projections(); image_contract(); openxr_conversions(); shared_pose_pairs();
        std::cout << checks << " checks passed (math/metadata/pre-init only; no GPU or HMD exercised).\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
