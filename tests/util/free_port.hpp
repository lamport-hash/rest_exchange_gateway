#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <stdexcept>

namespace gateway::testing {

/// Reserve an ephemeral TCP port on the loopback interface and release it.
/// Note: a small race window exists between release and reuse by the caller.
[[nodiscard]] inline auto pick_free_port() -> std::uint16_t
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("pick_free_port: socket() failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("pick_free_port: bind() failed");
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("pick_free_port: getsockname() failed");
    }

    const std::uint16_t port = ntohs(bound.sin_port);
    ::close(fd);
    return port;
}

} // namespace gateway::testing
