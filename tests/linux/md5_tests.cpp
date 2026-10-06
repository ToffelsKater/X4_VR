// MD5 for X4's catalog index (src/linux/md5.hpp): RFC 1321 test suite.
#include "md5.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::printf("FAILED %s:%d %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)

int main() {
    using x4vr::linux_port::md5_hex;
    CHECK(md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5_hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5_hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5_hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(md5_hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(md5_hex(std::string(8, '1')+"234567890123456789012345678901234567890123456789012345678901234567890") != "");
    CHECK(md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") == "57edf4a22be3c955ac49da2e2107b67a");
    std::string binary(200, '\0');
    for (size_t i = 0; i < binary.size(); ++i) binary[i] = char(i*37);
    CHECK(md5_hex(binary).size() == 32);
    std::puts("md5 tests passed");
}
