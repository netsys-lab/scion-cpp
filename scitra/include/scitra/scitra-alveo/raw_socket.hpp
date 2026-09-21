#pragma once

#include "scitra/packet.hpp"
#include "scitra/scitra-alveo/error_codes.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>

namespace scion {
namespace scitra {

// Raw socket for sending packets out from the slow path.
class RawSocket
{
private:
    int m_family = 0;
    int m_socket = -1;

public:
    RawSocket() = default;
    ~RawSocket()
    {
        close();
    }

    std::error_code open(int family)
    {
        if (m_socket >= 0) return std::make_error_code(std::errc::file_exists);

        m_socket = socket(family, SOCK_RAW, IPPROTO_RAW);
        if (m_socket < 0) return std::error_code(errno, std::system_category());

        const int enb = 1;
        if (family == AF_INET) {
            if (setsockopt(m_socket, IPPROTO_IP, IP_HDRINCL, &enb, sizeof(enb)) < 0) {
                ::close(m_socket);
                return std::error_code(errno, std::system_category());
            }
        } else {
            if (setsockopt(m_socket, IPPROTO_IPV6, IPV6_HDRINCL, &enb, sizeof(enb)) < 0) {
                ::close(m_socket);
                return std::error_code(errno, std::system_category());
            }
        }

        shutdown(m_socket, SHUT_RD);
        m_family = family;
        return ScitraError::Ok;
    }

    void close() noexcept
    {
        if (m_socket >= 0) {
            ::close(m_socket);
            m_socket = -1;
            m_family = 0;
        }
    }

    std::error_code sendPacket(PacketBuffer& pkt)
    {
        auto buffer = pkt.emitPacket(false);
        if (!buffer.has_value()) {
            return buffer.error();
        }

        if (m_family == AF_INET) {
            if (pkt.ipValid != PacketBuffer::IPValidity::IPv4)
                return ScitraError::InvalidArgument;
            auto dst = generic::toUnderlay<sockaddr_in>(pkt.ipv4.dst).value();
            auto n = sendto(m_socket, buffer->data(), buffer->size(), 0, (sockaddr*)&dst, sizeof(dst));
            if (n < 0) return std::error_code(errno, std::system_category());
        } else if (m_family == AF_INET6) {
            if (pkt.ipValid != PacketBuffer::IPValidity::IPv6)
                return ScitraError::InvalidArgument;
            auto dst = generic::toUnderlay<sockaddr_in6>(pkt.ipv6.dst).value();
            auto n = sendto(m_socket, buffer->data(), buffer->size(), 0, (sockaddr*)&dst, sizeof(dst));
            if (n < 0) return std::error_code(errno, std::system_category());
        }

        return ScitraError::Ok;
    }
};

} // namespace scitra
} // namespace scion
