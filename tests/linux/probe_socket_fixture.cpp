// Stand-in for X4's OpenTrack reader in the probe_preload test: binds a UDP port, receives
// packets the way a game might (blocking recvfrom, poll, then non-blocking recv until EAGAIN).
#include "opentrack.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto port = uint16_t(std::atoi(argv[1]));
    const int reader = socket(AF_INET, SOCK_DGRAM, 0), writer = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (reader < 0 || writer < 0 || bind(reader, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { std::perror("socket/bind"); return 1; }
    for (int i = 1; i <= 4; ++i) {
        const auto packet = x4vr::opentrack::encode({double(i), 0, 0, 10.0*i, 0, 0});
        sendto(writer, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    unsigned char buffer[64];
    sockaddr_in from{};
    socklen_t from_length = sizeof(from);
    if (recvfrom(reader, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_length) != 48) return 1;
    pollfd watch{reader, POLLIN, 0};
    if (poll(&watch, 1, 1000) != 1) return 1;
    int packets = 1;
    while (recv(reader, buffer, sizeof(buffer), MSG_DONTWAIT) == 48) ++packets;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return 1;
    close(reader);
    close(writer);
    std::printf("received %d packets\n", packets);
    return packets == 4 ? 0 : 1;
}
