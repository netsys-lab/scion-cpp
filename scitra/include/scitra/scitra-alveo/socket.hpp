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

#pragma once

#include "scitra/packet.hpp"
#include "scitra/scitra-alveo/debug.hpp"
#include "scitra/scitra-alveo/error_codes.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace asio = boost::asio;
using namespace scion;
using namespace scion::scitra;


// Encapsulates a UDP underlay socket. Contains logic for parsing and emitting
// packets.
class Socket
{
private:
    // The local TCP/UDP port number of the socket. Identifies the socket.
    const std::uint16_t m_localPort;

    // Whether the socket is persistent or temporary.
    const bool m_persistent;

    // Underlay UDP socket for comunicating with SCION routers and hosts.
    asio::ip::udp::socket m_underlay;

    // Local IP address the socket is bound to.
    generic::IPAddress m_localAddress;

    // External IP address and port after NAT.
    std::atomic<std::shared_ptr<generic::IPEndpoint>> m_mapped;

    // Last time something was sent from the socket.
    std::atomic<std::chrono::steady_clock::time_point> m_lastUsed;

public:
    Socket(asio::io_context& ioCtx, std::uint16_t port, bool persistent)
        : m_localPort(port)
        , m_persistent(persistent)
        , m_underlay(ioCtx)
    {}

    std::uint16_t port() const { return m_localPort; }

    bool persistent() const { return m_persistent; }

    // Returns the last time something was sent on the socket.
    std::chrono::steady_clock::time_point lastUsed() const { return m_lastUsed.load(); }

    // Check if the underlay socket is open.
    bool isOpen() const { return m_underlay.is_open(); }

    // Open the underlay socket and bind it to `bindAddress`.
    std::error_code open(const asio::ip::address& bindAddress);

    // Close the underlay socket and cancel all asynchronous operations on it.
    void close()
    {
        m_underlay.close();
    }

#if PERF_DEBUG == 1
    DebugTimestamp lastRx;
#endif

    // Receive and parse a packet.
    asio::awaitable<std::error_code>
    recvPacket(PacketBuffer& pkt, asio::ip::udp::endpoint& from);
};
