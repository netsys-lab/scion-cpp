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

#include "scitra/crypto.hpp"
#include "scitra/scitra-alveo/debug.hpp"
#include "scitra/scitra-alveo/scitra_tun.hpp"
#include "scitra/scitra-alveo/service.hpp"
#include "scitra/scitra-alveo/sys_net.hpp"
#include "scitra/scitra-alveo/dataplane/mock_dataplane.hpp"
#include "scitra/scitra-alveo/dataplane/alveo.hpp"

#include <spdlog/spdlog.h>

#include <ranges>
#include <signal.h>

using namespace std::chrono_literals;


// Maximum packet size including headers and headroom.
static constexpr std::size_t PACKET_BUFFER_SIZE = 9000;
// Minimum safe MTU for SCION with an IPv4 underlay.
static constexpr std::uint16_t SAFE_MTU_IPV4 = 576 - 28;
// Minimum safe MTU for SCION with an IPv6 underlay.
static constexpr std::uint16_t SAFE_MTU_IPV6 = 1280 - 48;
// Size of the UDP/IPv4 underlay in bytes.
static constexpr int IPv4_UNDERLAY_SIZE = 28;
// Size of the UDP/IPv6 underlay in bytes.
static constexpr int IPv6_UNDERLAY_SIZE = 48;

// Minimum time a path must be valid in order to be used by active and passive
// flows. Active flows should switch path sooner than passive ones so that when
// scitra is communicating with another instance of itself the active side can
// switch paths first.
static const auto ACTIVE_FLOW_MIN_PATH_LIFE = 60s;
static const auto PASSIVE_FLOW_MIN_PATH_LIFE = 10s;

static PathCacheOptions PATH_CACHE_OPTS = {
    .minAcceptedLifetime = 5min,
    .refreshAtRemaining = 10min,
    .refreshInterval = 30min,
};

/// \brief Returns the minimum overhead SCION adds over IPv6 with either a
/// UDP/IPv4 or UDP/IPv6 underlay.
static int minScionOverhead(bool underlayIsIPv6)
{
    constexpr int IPv6_HEADER = 40; // IPv6 header
    constexpr int SCION_IPv4 = 36;  // SCION header with IPv4 host addresses
    constexpr int SCION_IPv6 = 60;  // SCION header with IPv6 host addresses
    if (!underlayIsIPv6)
        return IPv4_UNDERLAY_SIZE + SCION_IPv4 - IPv6_HEADER;
    else
        return IPv6_UNDERLAY_SIZE + SCION_IPv6 - IPv6_HEADER;
};

///////////////
// ScitraTun //
///////////////

ScitraTun::ScitraTun(const Arguments& args)
    : ioCtx(1)
    , signals(ioCtx)
    , eventTimer(ioCtx)
    , grpcIoCtx()
    , grpcWorkGuard(grpcIoCtx.get_executor())
    , daemon(grpcIoCtx, args.sciond)
    , staticPorts(args.ports)
    , cpuPort(args.cpuPort)
    , policyFile(args.policy)
    , pathCache(std::make_unique<SharedPathCache>(PATH_CACHE_OPTS))
{
    // Signals handled by signalHandler()
    signals.add(SIGINT);
    signals.add(SIGTERM);
    signals.add(SIGHUP);
    signals.add(SIGUSR1);
    signals.add(SIGUSR2);

    // Get local AS info from daemon
    if (auto maybe = daemon.rpcAsInfo(IsdAsn()); maybe.has_value()) {
        localAS = *maybe;
    } else {
        throw std::runtime_error(std::format(
            "Error connection to SCION daemon at '{}': {}",
            args.sciond, fmtError(maybe.error())));
    }

    // Parse public interface address
    if (auto maybe = generic::IPAddress::Parse(args.publicAddress); maybe.has_value()) {
        publicIP = std::move(*maybe);
    } else {
        throw std::runtime_error("Public IP address is invalid");
    }
    if (!publicIP.is4() && !publicIP.isScion()) {
        throw std::runtime_error(
            "Public IP address must either be an IPv4 or SCION-mapped IPv6 address");
    }
    if (auto maybe = mapToIPv6(ScIPAddress(localAS.isdAsn, publicIP)); maybe.has_value()) {
        mappedIP = std::move(*maybe);
    } else {
        throw std::runtime_error(std::format("Can't encode {} as IPv6",
            ScIPAddress(localAS.isdAsn, publicIP)));
    }

    // Determine Alveo card interface address
    if (!args.alveoAddress.empty()) {
        if (auto maybe = generic::IPAddress::Parse(args.alveoAddress); maybe.has_value()) {
            alveoIP = std::move(*maybe);
        } else {
            throw std::runtime_error("Tunnel IP address is invalid");
        }
    } else {
        alveoIP = mappedIP;
    }

    // Load path policy
    if (!args.policy.empty()) {
        if (auto ec = loadPathPolicy(policyFile); ec) {
            throw std::runtime_error(std::format("Loading policy from '{}' failed: {}",
                policyFile.string(), fmtError(ec)));
        }
    }

    // Connect to fast path
    if (args.mock) {
        dataplane = std::make_unique<MockDp>();
    } else {
        dataplane = std::make_unique<Alveo>();
    }
    if (auto ec = dataplane->initialize(args.sysfile); ec) {
        throw std::runtime_error(std::format("Initializing driver driver failed: {}",
            fmtError(ec)));
    }

    // Link SCMP handlers
    pmtu = std::make_unique<PathMtuDiscoverer<>>(localAS.mtu);
    pathCache->setNextScmpHandler(pmtu.get());
}

void ScitraTun::run()
{
    shouldExit = false;

    // Open CPU socket
    cpuSocket = std::make_unique<Socket>(ioCtx, cpuPort, true);
    auto ec = cpuSocket->open(generic::toUnderlay<boost::asio::ip::address>(alveoIP).value());
    if (ec) {
        throw std::runtime_error(std::format(
            "Can't open CPU socket {}", generic::IPEndpoint(alveoIP, cpuPort)));
    }

    ec = cpuRawSocket.open(publicIP.is4() ? AF_INET : AF_INET6);
    if (ec) {
        throw std::runtime_error("Can't open CPU raw socket");
    }

    // Program P4 tables
    spdlog::info("Program static tables");
    std::vector<std::uint64_t> keys = {0, 0};
    std::vector<std::uint64_t> params = {};
    ec = dataplane->tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_source_translation_46",
        keys, "translateSource46BGP", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));
    ec = dataplane->tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_dest_translation_46",
        keys, "translateDest46BGP", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));

    keys = {0, 2};
    ec = dataplane->tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_source_translation_46",
        keys, "translateSource46SCION", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));
    ec = dataplane->tableInsert(
        P4_PROG_IG_TRANSLATOR, "tab_dest_translation_46",
        keys, "translateDest46SCION", params);
    if (ec) spdlog::error("{}", scion::fmtError(ec));

    // Program forwarded ports
    // TODO
    // for (std::uint16_t port : staticPorts) {
    // }

    // Start signal handler
    asio::co_spawn(ioCtx, signalHandler(), asio::detached);

    // Start timer
    asio::co_spawn(ioCtx, tick(), asio::detached);

    // Start slow path packet handler
    asio::co_spawn(ioCtx, slowPath(), asio::detached);

    // Start worker threads
    threads.reserve(2);
    threads.emplace_back([this] {
        sigset_t sigset;
        sigfillset(&sigset);
        if (pthread_sigmask(SIG_UNBLOCK, &sigset, nullptr))
            throw std::system_error(errno, std::generic_category());
        ioCtx.run();
    });
    pthread_setname_np(threads.back().native_handle(), "worker");

    // Run gRPC context on its own thread
    threads.emplace_back([this] {
        grpcIoCtx.run();
    });
    pthread_setname_np(threads.back().native_handle(), "grpcIoCtx");
}

ScitraTun::~ScitraTun()
{
    stop();
    join();
}

void ScitraTun::stop()
{
    service::setServiceStatus(service::Status::StopPending);
    shouldExit = true;
    grpcWorkGuard.reset();
    grpcIoCtx.stop();
    std::unique_lock lock(socketMutex);
    cpuSocket->close();
    eventTimer.cancel();
    signals.cancel();
    dataplane->close();
}

void ScitraTun::join()
{
    for (auto& thread : threads)
        thread.join();
    threads.clear();
    cpuSocket.reset();
}

std::vector<PathPtr> ScitraTun::getPaths(const FlowID& flowid, std::uint8_t tc) const
{
    auto paths = pathCache->lookupCached(flowid.src.isdAsn(), flowid.dst.isdAsn());
    if (auto policy = pathPolicy.load()) {
        auto filtered = policy->apply(flowid.src, flowid.dst, flowid.proto, tc, paths);
        paths.resize(filtered.size());
    }
    return paths;
}

void ScitraTun::overrideFlowPath(const FlowID& flowid, PathPtr path)
{
    std::lock_guard lock(flowMutex);
    if (auto i = flows.find(flowid); i != flows.end()) {
        if (i->second->getType() == FlowType::Active && !i->second->isMultipath())
            i->second->lock().setPath(path);
    }
}

void ScitraTun::removeFlow(const FlowID& flowid)
{
    std::lock_guard lock(flowMutex);
    spdlog::debug("Remove flow {}", flowid);
    flows.erase(flowid);
}

void ScitraTun::refreshPaths(IsdAsn dst)
{
    pathCache->prefetch(localAS.isdAsn, dst,
    [this] (SharedPathCache& cache, IsdAsn src, IsdAsn dst) {
        return beginPathQuery(cache, src, dst);
    }, true);
}

std::vector<FlowInfo> ScitraTun::exportFlows(bool resetCounters) const
{
    std::vector<FlowInfo> out;
    out.reserve(flows.size());

    std::scoped_lock lock{socketMutex, flowMutex};
    FlowInfo::FlagSet flags = {};
    FlowState state;
    std::uint8_t tc;
    std::uint32_t token = 0;
    FlowCounters counters;
    std::chrono::steady_clock::time_point lastUsed;
    PathPtr path;

    for (auto& [id, flow] : flows) {
        if (flow->isMultipath()) flags |= FlowInfo::Flags::Multipath;
        auto fl = flow->lock();
        fl.getState(state);
        if (state > FlowState::CLOSED_WAIT) {
            fl.getTrafficClass(tc).getMptcpToken(token).getLastUpdate(lastUsed)
                .getCounters(counters).getPath(path);
            if (resetCounters) fl.resetCounters();
            out.emplace_back(id, flags, flow->getType(), state, tc, flow->getLocalEp(), token,
                counters, lastUsed, path, path ? pmtu->getMtu(id.dst.host(), *path) : 0);
        }

        if (flags[FlowInfo::Flags::Multipath]) {
            fl.applyToSubflows([&] (auto& sf) {
                auto sfl = sf.lock();
                sfl.getState(state).getTrafficClass(tc).getMptcpToken(token).getLastUpdate(lastUsed)
                   .getCounters(counters).getPath(path);
                if (resetCounters) sfl.resetCounters();
                out.emplace_back(id, flags, flow->getType(), state, tc, sf.getLocalEp(), token,
                counters, lastUsed, path, path ? pmtu->getMtu(id.dst.host(), *path) : 0);
            });
        }
    }
    return out;
}

std::error_code ScitraTun::loadPathPolicy(const std::filesystem::path& path)
{
    if (path.empty()) return ErrorCode::FileNotFound;
    auto policy = std::make_shared<path_policy::PolicySet>();
    auto [ec, msg] = policy->loadJsonFile(path);
    if (ec) {
        spdlog::error("Error loading policy from '{}': {}", path.string(), msg);
        return ec;
    }
    pathPolicy.store(policy);
    policyFile = path;
    return ErrorCode::Ok;
}

std::error_code ScitraTun::reloadPathPolicy()
{
    auto ec = loadPathPolicy(policyFile);
    if (!ec) spdlog::info("Path policy reloaded");
    return ec;
}

std::shared_ptr<Flow> ScitraTun::getFlowEgress(
    const PacketBuffer& pkt, FlowID& id, const generic::IPEndpoint& localEp)
{
    std::shared_ptr<Flow> flow;
    std::lock_guard lock(flowMutex);

    // Map MPTCP subflows to SCION flow
    bool subflowSyn = false;
    uint32_t receiverToken = 0;
    if (!extraIPs.empty() && pkt.l4Valid == PacketBuffer::L4Type::TCP) {
        if (pkt.tcp.optMask.MpJoin && pkt.tcp.options.mpJoin.content.index() == 0) {
            // New subflow SYN, demultiplex by receiver token
            subflowSyn = true;
            receiverToken = std::get<0>(pkt.tcp.options.mpJoin.content).receiverToken;
            if (auto i = mpTokenMap.find(receiverToken); i != mpTokenMap.end()) {
                flow = i->second;
                auto sport = flow->getLocalEp().port(); // remap port to existing flow
                mpPortRemap[localEp] = sport;
                id.src = ScIPEndpoint(id.src.address(), sport);
            }
        } else if (localEp.host() != tunIP) {
            // Packet comes from one of the extra IPs, might need source port remapping
            if (auto i = mpPortRemap.find(localEp); i != mpPortRemap.end()) {
                id.src = ScIPEndpoint(id.src.address(), i->second);
            }
        }
    }

    if (!flow) {
        flow = flows[id];
        if (!flow) {
            spdlog::debug("New {} flow {}", toString(FlowType::Active), id);
            flow = Flow::Create(localEp, FlowType::Active);
            flows[id] = flow;
        }
    }

    // Learn the receiver token for additional subflows
    if (subflowSyn) {
        spdlog::debug("MPTCP connection {} has receiver token {:08x}", id, receiverToken);
        mpTokenMap[receiverToken] = flow;
        flow->lock().setMptcpToken(receiverToken);
    } else if (pkt.l4Valid == PacketBuffer::L4Type::TCP && pkt.tcp.optMask.MpCapable) {
        auto& cap = pkt.tcp.options.mpCap;
        if (cap.fieldMask.receiverKey) {
            receiverToken = scion::scitra::sha256_trunc(cap.receiverKey);
            spdlog::debug("MPTCP connection {} has receiver token {:08x} (key {})",
                id, receiverToken, cap.receiverKey);
            mpTokenMap[receiverToken] = flow;
            flow->lock().setMptcpToken(receiverToken);
        }
    }

    return flow;
}

// Get an existing flow or create a new one. If a new flow is created
// it will be of type `type` with local IP endpoint `localIP`.
std::shared_ptr<Flow> ScitraTun::getFlowIngress(
    const FlowID& id, const generic::IPEndpoint& localEp)
{
    std::lock_guard lock(flowMutex);
    auto flow = flows[id];
    if (!flow) {
        spdlog::debug("New {} flow {}", toString(FlowType::Passive), id);
        flow = Flow::Create(localEp, FlowType::Passive);
        flows[id] = flow;
    }
    return flow;
}

// Find an existing flow. Returns nullptr, if the flow is not found.
std::shared_ptr<Flow> ScitraTun::findFlow(const FlowID& id)
{
    std::lock_guard lock(flowMutex);
    auto flow = flows.find(id);
    if (flow == flows.end()) return nullptr;
    return flow->second;
}

void ScitraTun::maintainFlowsAndSockets()
{
    using namespace std::chrono;
    using scion::hdr::ScionProto;
    static const auto SOCKET_TIMEOUT = seconds(60);
    static const auto PMTU_TIMEOUT = hours(1);

    auto mySockets = getSocketInodes(32);
    std::ranges::sort(mySockets);
    auto udpSockets = getSocketsUdp6(32);
    auto tcpSockets = getSocketsTcp6(32);

    std::scoped_lock lock{socketMutex, flowMutex};
    const auto now = std::chrono::steady_clock::now();

    // Clear old PMTU cache entries
    pmtu->clear(steady_clock::now() - PMTU_TIMEOUT);

    // Advance flow states
    FlowState state = FlowState::CLOSED;
    for (auto i = flows.begin(); i != flows.end();) {
        if (i->second->isMultipath()) {
            i->second->lock().removeSubflows([&] (Flow& sf) {
                sf.lock().getState(state).tick(now);
                if (state == FlowState::CLOSED) {
                    spdlog::debug("Remove subflowflow {} (bound to {})", i->first, sf.getLocalEp());
                    mpPortRemap.erase(sf.getLocalEp());
                    return true;
                }
                return false;
            });
        }
        std::uint32_t token = 0;
        i->second->lock().getMptcpToken(token).getState(state).tick(now);
        if (state == FlowState::CLOSED) {
            spdlog::debug("Remove flow {}", i->first);
            mpTokenMap.erase(token);
            i = flows.erase(i);
        } else {
            ++i;
        }
    }

    // Maintain up-to-date paths
    for (auto&& [id, flow] : flows) {
        if (flow->getType() == FlowType::Active) {
            pathCache->prefetch(id.src.isdAsn(), id.dst.isdAsn(),
            [this] (SharedPathCache& cache, IsdAsn src, IsdAsn dst) {
                return beginPathQuery(cache, src, dst);
            });
        }
    }

    // Close all sockets that aren't used anymore.
    for (auto i = sockets.begin(); i != sockets.end();) {
        auto& socket = i->second;
        auto localPort = socket->port();
        if (socket->persistent()) {
            ++i;
            continue;
        }
        // Keep socket if there is a TCP socket using the same port.
        // Ignores listening TCP sockets as server should use persistent
        // port forwarding.
        auto tcp = std::ranges::find_if(tcpSockets, [&] (const SocketInfo& s) {
            if (std::ranges::binary_search(mySockets, s.inode))
                return false;
            if (s.localPort == localPort && s.state != TCP_LISTEN)
                return s.localAddr.isUnspecified() || s.localAddr == tunIP
                    || std::ranges::find(extraIPs, s.localAddr) != extraIPs.end();
            return false;
        });
        if (tcp != tcpSockets.end()) {
            ++i;
            continue;
        }
        // Keep socket if there is a corresponding UDP socket that is connected
        // to a SCION-mapped IP address or an unconnected socket that recently
        // had outgoing traffic.
        auto udp = std::ranges::find_if(udpSockets, [&] (const SocketInfo& s) {
            if (std::ranges::binary_search(mySockets, s.inode))
                return false;
            if (s.localPort != localPort)
                return false;
            if (s.remoteAddr.isScion()) {
                return s.localAddr.isUnspecified() || s.localAddr == tunIP
                    || std::ranges::find(extraIPs, s.localAddr) != extraIPs.end();
            } else if (s.remoteAddr.isUnspecified()) {
                return now - socket->lastUsed() < SOCKET_TIMEOUT;
            }
            return false;
        });
        if (udp != udpSockets.end()) {
            ++i;
            continue;
        }
        // Remove socket and all flows connected to it
        socket->close();
        i = sockets.erase(i);
        for (auto j = flows.begin(); j != flows.end(); ++j) {
            if (j->first.src.port() == localPort) {
                if (j->second->isMultipath()) {
                    j->second->lock().applyToSubflows([] (Flow& sf) {
                        sf.lock().close();
                    }).close();
                } else {
                    j->second->lock().close();
                }
            }
        }
    }
}

asio::awaitable<void> ScitraTun::signalHandler()
{
    constexpr auto token = boost::asio::as_tuple(boost::asio::use_awaitable);
    while (!shouldExit) {
        auto [ec, signal] = co_await signals.async_wait(token);
        if (ec) {
            if (ec == std::errc::operation_canceled) {
                co_return;
            } else {
                spdlog::critical("Signal handler error: {}\n", fmtError(ec));
                std::exit(EXIT_FAILURE);
            }
        }
        if (signal == SIGINT || signal == SIGTERM) {
            if (signal == SIGINT)
                spdlog::critical("Got SIGINT, stopping...");
            else
                spdlog::critical("Got SIGTERM, stopping...");
            stop();
            co_return;
        } else if (signal == SIGHUP) {
            // Reload configuration
            if (!policyFile.empty())
                reloadPathPolicy();
        } else if (signal == SIGUSR1) {
            printStatus(); // Print status to stdout
        }
    }
}

asio::awaitable<std::error_code> ScitraTun::tick()
{
    constexpr auto token = boost::asio::as_tuple(boost::asio::use_awaitable);
    while (!shouldExit) {
        eventTimer.expires_after(std::chrono::seconds(1));
        auto [ec] = co_await eventTimer.async_wait(token);
        if (ec) co_return ec;
        maintainFlowsAndSockets();
    }
    co_return ErrorCode::Ok;
}

asio::awaitable<std::error_code> ScitraTun::slowPath()
{
    using std::uint8_t;
    using std::uint16_t;
    using L4Type = PacketBuffer::L4Type;
    PacketBuffer pkt{std::pmr::vector<std::byte>(PACKET_BUFFER_SIZE)};

    while (!shouldExit) {
        asio::ip::udp::endpoint from;
        auto ec = co_await cpuSocket->recvPacket(pkt, from);
        if (ec) {
            if (ec == ErrorCondition::Cancelled) {
                break;
            } else if (ec != ErrorCondition::InvalidPacket) {
                spdlog::error("IP->SCION Error reading from TUN queue: {}", fmtError(ec));
            }
            continue;
        }

        const auto recvd = std::chrono::steady_clock::now();

        generic::IPEndpoint localEp(pkt.ipv6.src, pkt.l4SPort());
        std::uint16_t remappedSPort = 0;
        bool mpOutOfPaths = false;
        std::shared_ptr<Flow> flow;
        auto [verdict, port, nextHop] = translateEgress(pkt, publicIP, REPLACE_ADDRESS,
            [&] (const ScIPAddress& src, const ScIPAddress& dst,
                uint16_t sport, uint16_t dport, hdr::ScionProto proto, uint8_t tc) {
            PathPtr path;
            std::uint16_t mtu = 0;
            ScIPAddress localAddr(localAS.isdAsn, src.host());
            FlowID flowid(localAddr, dst, sport, dport, proto);

            flow = getFlowEgress(pkt, flowid, localEp);
            remappedSPort = flowid.src.port();
            if (flow->isMultipath()) {
                flow = flow->getSubflowByAddr(localEp);
            }
            flow->lock().getPath(path);

            bool expiresSoon = false;
            auto now = std::chrono::utc_clock::now();
            if (path) {
                auto ttl = path->expiry() - now;
                if (flow->getType() == FlowType::Active)
                    expiresSoon = ttl < ACTIVE_FLOW_MIN_PATH_LIFE;
                else
                    expiresSoon = ttl < PASSIVE_FLOW_MIN_PATH_LIFE;
            }

            if (!path || path->broken() || expiresSoon) {
                // Test for MP_JOIN as well because isMultipath() updates too late.
                if (flow->isMultipath() || (pkt.l4Valid == L4Type::TCP && pkt.tcp.optMask.MpJoin)) {
                    if (!path) {
                        // Ensure that MPTCP subflows have unique paths.
                        // Looking for the lead flow here is the inverse operation to
                        // flow = flow->getSubflowByAddr(localEp). We could keep a pointer to the
                        // lead flow from earlier, but this code path is invoked only once per
                        // subflow so probably not worth it.
                        if (auto leadFlow = findFlow(flowid); leadFlow) {
                            auto lock = leadFlow->lock();
                            std::vector<PathPtr> paths;
                            lock.getAllPaths(paths);
                            path = selectPathMulti(flowid, tc, paths);
                            if (!path) {
                                mpOutOfPaths = true;
                                spdlog::debug("IP->SCION Not enough paths to establish subflow {}",
                                    flowid);
                                return std::make_pair(Maybe<PathPtr>(
                                    Error(ScitraError::NotEnoughPaths)), (std::uint16_t)0);
                            }
                        } else {
                            spdlog::debug("IP->SCION Can't find lead flow for {}", flowid);
                            return std::make_pair(Maybe<PathPtr>(
                                Error(ScitraError::NoLeadFlow)), (std::uint16_t)0);
                        }
                    } else {
                        // MPTCP subflows can't reselect their paths
                        path = refreshPath(path);
                        if (path->broken() || (path->expiry() - now).count() < 0) {
                            spdlog::debug("IP->SCION Path of MPTCP subflow {} is broken", flowid);
                            return std::make_pair(Maybe<PathPtr>(
                                Error(ScitraError::SubflowBroken)), (std::uint16_t)0);
                        }
                    }
                    spdlog::debug("Selected path for {} (subflow bound to {}): {}",
                        flowid, localEp, *path, mtu);
                } else {
                    path = selectPath(flowid, tc);
                    if (!path) return std::make_pair(
                        Maybe<PathPtr>(Error(ErrorCode::Pending)), (std::uint16_t)0);

                    spdlog::debug("Selected path for {}: {}", flowid, *path);
                }
                mtu = pmtu->getMtu(dst.host(), *path, recvd);
                spdlog::debug("Path {} has MTU of {} octets", *path, mtu);
            } else {
                mtu = pmtu->getMtu(dst.host(), *path, recvd);
            }
            flow->lock().setPath(path);
            return std::make_pair(Maybe<PathPtr>(std::move(path)), mtu);
        });

        if (verdict == Verdict::Pass) {
            assert(flow);
            flow->lock()
                .updateStateEgress(pkt, recvd)
                .countEgress(1, (std::uint32_t)pkt.payload().size());
            auto nh = generic::toUnderlay<asio::ip::udp::endpoint>(nextHop);
            if (!nh.has_value()) continue; // this should never happen
            auto ec = cpuRawSocket.sendPacket(pkt); // blocking
            if (ec) {
                if (ec == std::errc::message_size) {
                    // MTU to next hop is lower than expected AS-internal MTU. Fall back to
                    // minimum safe MTU. The discovered MTU could be read from the socket's
                    // error queue, but it would be difficult to assign it to the right paths.
                    spdlog::warn("IP->SCION Translated packet too big to send to next hop '{}'."
                        " Falling back to minimum safe MTU. Consider setting --underlay-mtu",
                        " to a more conservative value.",
                        *nh);
                    pmtu->updateMtu(pkt.sci.dst.host(), pkt.path,
                        nextHop.host().is4() ? SAFE_MTU_IPV4 : SAFE_MTU_IPV6);
                } else {
                    spdlog::error("IP->SCION Error sending packet to next hop '{}': {}",
                        *nh, fmtError(ec));
                }
            }
        } else if (verdict == Verdict::Return) {
            spdlog::warn("(not implemented) IP->SCION Return packet to local sender");
        } else {
            spdlog::debug("IP->SCION Packet dropped");
        }
        DBG_TIME_END(tun.lastRx, egrTicks, egrSamples);
    }
    co_return ScitraError::Cancelled;
}

// Spawns a co-routine that queries paths to the given destination.
std::error_code ScitraTun::beginPathQuery(SharedPathCache& cache, IsdAsn src, IsdAsn dst)
{
    using namespace scion::daemon;
    asio::co_spawn(grpcIoCtx, [this, src, dst] () -> asio::awaitable<void> {
        std::vector<PathPtr> paths;
        auto flags = PathReqFlags::Refresh | PathReqFlags::AllMetadata;
        co_await daemon.rpcPathsAsync(src, dst, flags, std::back_inserter(paths));
        pathCache->store(src, dst, paths);
    }, asio::detached);
    return ErrorCode::Pending;
}

// Returns possible paths for the given flow. May initiate an asynchronous path
// query if there are no cached paths.
Maybe<std::vector<PathPtr>> ScitraTun::getPathsForFlow(const FlowID& flowid, std::uint8_t tc)
{
    auto paths = pathCache->lookup(localAS.isdAsn, flowid.dst.isdAsn(),
        [this] (SharedPathCache& cache, IsdAsn src, IsdAsn dst) {
            return beginPathQuery(cache, src, dst);
        }
    );

    if (paths) {
        if (auto policy = pathPolicy.load()) {
            auto filtered = policy->apply(flowid.src, flowid.dst, flowid.proto, tc, *paths);
            paths->resize(filtered.size());
        }
    }
    return paths;
}

// Find a newer version of the same path, i.e. a path with the same hop sequence
// but more up-to-date timestamps and MACs. May return `path` if no newer path
// is available.
PathPtr ScitraTun::refreshPath(const PathPtr& path)
{
    auto paths = pathCache->lookup(path->firstAS(), path->lastAS(),
        [this] (SharedPathCache& cache, IsdAsn src, IsdAsn dst) {
            return beginPathQuery(cache, src, dst);
        }
    );

    if (paths) {
        for (const auto& candidate : *paths) {
            if (*candidate == *path) return candidate;
        }
    }
    return path;
}

// Select a new path from scratch. May return nullptr.
PathPtr ScitraTun::selectPath(const FlowID& flowid, std::uint8_t tc)
{
    auto paths = getPathsForFlow(flowid, tc);
    if (paths) {
        for (auto& path : *paths) {
            if (!path->broken()) return path;
        }
        return nullptr; // all paths have failed
    } else {
        if (paths.error() == ErrorCondition::Pending)
            return nullptr; // paths not ready yet
        else
            return nullptr; // no path
    }
}

// Select a new path that is not contained in `others`. Prefers paths that have
// the least overlap with the paths in `others`. May return nullptr.
PathPtr ScitraTun::selectPathMulti(
    const FlowID& flowid, std::uint8_t tc, const std::span<PathPtr>& others)
{
    auto paths = getPathsForFlow(flowid, tc);
    if (!paths) {
        if (paths.error() == ErrorCondition::Pending)
            return nullptr; // paths not ready yet
        else
            return nullptr; // no path
    }

    // Find path with the least overlap to other subflows
    float minOverlap = std::numeric_limits<float>::infinity();
    PathPtr best = nullptr;
    for (auto& path : *paths) {
        bool reject = false;
        float overlap = 0.0f;
        for (auto& other : others) {
            if (path->digest() == other->digest() && equalHops(*path, *other)) {
                reject = true; // path already in use by a different subflow
                break;
            } else {
                if (auto over = path->overlap(*other); over)
                    overlap += (float)over->first / (float)over->second;
                else
                    overlap += 1.0f; // assume full overlap if no other data is available
            }
        }
        if (!reject) {
            if (overlap == 0.0f) {
                return path;
            } else if (overlap < minOverlap) {
                minOverlap = overlap;
                best = path;
            }
        }
    }
    return best;
}

void ScitraTun::printStatus()
{
    const auto now = std::chrono::steady_clock::now();
    const auto date = std::chrono::system_clock::now();
    std::cout << std::format("### Scitra-TUN {:%F %T} ###\n", date);

    for (auto& flow : exportFlows(false)) {
        if (!flow.flags[FlowInfo::Flags::Multipath]) {
            auto proto = protoToString((int)flow.tuple.proto);
            std::cout << std::format(
                "{} -> {} [{}] bound_to {} type {} state {} time {:%M:%S}",
                flow.tuple.src,
                flow.tuple.dst,
                flow.boundTo,
                proto,
                toString(flow.type),
                toString(flow.state),
                now - flow.lastUsed
            );
        } else if (flow.tuple.proto == scion::hdr::ScionProto::TCP) {
            std::cout << std::format(
                "{} -> {} [{}] bound_to {} type {} state {} token {:08x} time {:%M:%S}",
                flow.tuple.src,
                flow.tuple.dst,
                flow.boundTo,
                "MPTCP",
                toString(flow.type),
                toString(flow.state),
                flow.mptcpRemoteToken,
                now - flow.lastUsed
            );
        }
        if (flow.path)
            std::cout << std::format(" path {} mtu {}", *flow.path, flow.mtu);
        std::cout << std::format(" tx_pkts {} tx_bytes {} rx_pkts {} rx_bytes {}",
            flow.counters.pktsEgress, flow.counters.bytesEgress,
            flow.counters.pktsIngress, flow.counters.bytesIngress);
        std::cout << '\n';
    }
    std::cout << std::flush;
}
