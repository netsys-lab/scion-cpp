// Copyright (c) 2024-2025 Lars-Christian Schulz
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "scitra/scitra-alveo/socket.hpp"
#include "scion/asio/addresses.hpp"

#include <spdlog/spdlog.h>


std::error_code Socket::open(const asio::ip::address& bindAddress)
{
    boost::system::error_code ec;
    int res = 0;

    auto proto = bindAddress.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4();
    m_underlay.open(proto, ec);
    if (ec) return ec;

    const auto sockfd = m_underlay.native_handle();

    m_underlay.bind(asio::ip::udp::endpoint(bindAddress, m_localPort), ec);
    if (ec) return ec;
    m_localAddress = generic::toGenericAddr(bindAddress);

    // Disable automatic fragmentation of large UDP packets.
    int mtuDisc = IP_PMTUDISC_DO;
    if (proto.family() == AF_INET) {
        res = setsockopt(sockfd, IPPROTO_IP, IP_MTU_DISCOVER, &mtuDisc, sizeof(mtuDisc));
    } else {
        res = setsockopt(sockfd, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &mtuDisc, sizeof(mtuDisc));
    }
    if (res) return std::error_code(errno, std::system_category());

    return ScitraError::Ok;
}

asio::awaitable<std::error_code>
Socket::recvPacket(PacketBuffer& pkt, asio::ip::udp::endpoint& from)
{
    // 4 bytes headroom for the following edge case: A SCION header with an empty path and IPv4
    // host addresses is 4 bytes smaller than the IPv6 header created by the translator.
    constexpr std::size_t IP_HEADROOM = 4;
    auto buffer = pkt.clearAndGetBuffer(IP_HEADROOM);

    constexpr auto token = boost::asio::as_tuple(boost::asio::use_awaitable);
    auto [ec, n] = co_await m_underlay.async_receive_from(asio::buffer(buffer), from, token);
    if (ec) co_return ec;
    DBG_TIME_BEGIN(lastRx);
    ec = pkt.parsePacket(n, true);
    if (ec) co_return ec;

    co_return ScitraError::Ok;
}
