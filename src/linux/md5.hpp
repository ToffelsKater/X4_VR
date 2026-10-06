#pragma once
// MD5 (RFC 1321), only for X4's catalog index: each .cat line lists a file's MD5. The Windows
// launcher uses BCrypt for this.
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace x4vr::linux_port {
inline std::string md5_hex(std::string_view data) {
    static constexpr uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr int shift[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    std::string message(data);
    const uint64_t bits = uint64_t(data.size())*8;
    message += char(0x80);
    while (message.size() % 64 != 56) message += char(0);
    for (int i = 0; i < 8; ++i) message += char((bits >> (8*i)) & 0xff);
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t block = 0; block < message.size(); block += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            w[i] = 0;
            for (int b = 0; b < 4; ++b) w[i] |= uint32_t(uint8_t(message[block+4*i+b])) << (8*b);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5*i+1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3*i+5) % 16; }
            else { f = c ^ (b | ~d); g = (7*i) % 16; }
            const uint32_t sum = a+f+k[i]+w[g];
            const int s = shift[(i/16)*4+i%4];
            a = d; d = c; c = b;
            b += (sum << s) | (sum >> (32-s));
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::string hex;
    for (const auto word : h)
        for (int i = 0; i < 4; ++i) { char pair[3]; std::snprintf(pair, sizeof(pair), "%02x", (word >> (8*i)) & 0xff); hex += pair; }
    return hex;
}
}
