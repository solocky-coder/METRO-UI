#include "MetroNetworkAudio.h"
#include "NetworkAudioChannelState.h"

#include <juce_events/juce_events.h>
#include <aoo/aoo.hpp>
#include <aoo/aoo_net.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 using MetroSocket = SOCKET;
 static constexpr MetroSocket metroInvalidSocket = INVALID_SOCKET;
#else
 #include <arpa/inet.h>
 #include <fcntl.h>
 #include <netinet/in.h>
 #include <sys/select.h>
 #include <sys/socket.h>
 #include <unistd.h>
 using MetroSocket = int;
 static constexpr MetroSocket metroInvalidSocket = -1;
#endif

namespace
{
constexpr int kAooSampleRate = 48000;
constexpr int kAooBlockSize = 512;
constexpr int kAooChannels = 64;
constexpr int kAooBufferMs = 120;
constexpr size_t kMaxNetworkSources = 64;

// -------------------------------------------------------------------------
// AOO diagnostic logging
//
// Diagnostic-only instrumentation for tracing:
//
//   PEER_JOIN
//      -> wildcard invite
//      -> SOURCE_ADD
//      -> specific invite
//      -> incoming packet routing
//      -> SOURCE_FORMAT event
//      -> get_source_format()
//
// This does not intentionally alter AOO routing or source state.
//
// Written directly to disk rather than relying on DBG()/OutputDebugString:
// JUCE's DBG() macro compiles to a no-op unless JUCE_DEBUG is set, so a
// Release-configuration executable (e.g. a GitHub Actions Windows build)
// silently produces no output at all. All call sites here run on the AOO
// io/client threads or the message thread, never the realtime audio
// callback, so the blocking file I/O below is safe.
// -------------------------------------------------------------------------
constexpr bool kAooDiagnostics = true;

std::mutex aooLogMutex;
std::unique_ptr<juce::FileOutputStream> aooLogStream;

juce::File getAooLogFile()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
               .getChildFile ("metro-aoo-log.txt");
}

void aooDiag (const juce::String& message)
{
    if (! kAooDiagnostics)
        return;

    // Still visible in a debugger/console when JUCE_DEBUG is on.
    DBG ("[METRO-AOO] " + message);

    std::lock_guard<std::mutex> lock (aooLogMutex);

    if (aooLogStream == nullptr)
    {
        const auto file = getAooLogFile();
        file.deleteFile();   // fresh log per process run
        auto stream = std::make_unique<juce::FileOutputStream> (file);

        if (! stream->openedOk())
            return; // leave aooLogStream null; we'll retry the open next call

        aooLogStream = std::move (stream);
    }

    const auto line = juce::Time::getCurrentTime().toString (true, true, true, true)
                     + "  [METRO-AOO] " + message + "\n";

    aooLogStream->writeText (line, false, false, nullptr);
    aooLogStream->flush();
}

juce::String endpointString (const sockaddr_in* endpoint)
{
    if (endpoint == nullptr)
        return "<null>";

    char address[INET_ADDRSTRLEN] {};
    const char* result = inet_ntop (AF_INET, &endpoint->sin_addr,
                                    address, sizeof (address));

    return juce::String (result != nullptr ? result : "?")
         + ":" + juce::String (ntohs (endpoint->sin_port));
}

juce::String endpointDebug (const sockaddr_in* endpoint)
{
    if (endpoint == nullptr)
        return "ptr=null";

    return "ptr=" + juce::String::toHexString (
               static_cast<juce::int64> (reinterpret_cast<uintptr_t> (endpoint)))
         + " addr=" + endpointString (endpoint);
}

std::mutex aooLifetimeMutex;
int aooLifetimeUsers = 0;
std::atomic<MetroSocket*> activeAooSocket { nullptr };

void closeSocket (MetroSocket socket)
{
    if (socket == metroInvalidSocket) return;
#if defined(_WIN32)
    closesocket (socket);
#else
    ::close (socket);
#endif
}

bool setNonBlocking (MetroSocket socket)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket (socket, FIONBIO, &mode) == 0;
#else
    const auto flags = fcntl (socket, F_GETFL, 0);
    return flags >= 0 && fcntl (socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void startAooLifetime()
{
    std::lock_guard<std::mutex> lock (aooLifetimeMutex);
    if (aooLifetimeUsers++ == 0) aoo_initialize();
}

void stopAooLifetime()
{
    std::lock_guard<std::mutex> lock (aooLifetimeMutex);
    if (aooLifetimeUsers > 0 && --aooLifetimeUsers == 0) aoo_terminate();
}

int32_t sendUdp (void* user, const char* data, int32_t numBytes, void* address)
{
    auto* socket = static_cast<MetroSocket*> (user);
    if (socket == nullptr || *socket == metroInvalidSocket || address == nullptr) return 0;
    const auto result = sendto (*socket, data, numBytes, 0,
                                static_cast<const sockaddr*> (address), sizeof (sockaddr_in));
    return result == numBytes ? 1 : 0;
}

int32_t sendAooReply (void* endpoint, const char* data, int32_t numBytes)
{
    auto* socket = activeAooSocket.load (std::memory_order_acquire);
    if (socket == nullptr || *socket == metroInvalidSocket || endpoint == nullptr)
    {
        aooDiag ("sendAooReply FAILED endpoint="
                 + endpointDebug (static_cast<const sockaddr_in*> (endpoint))
                 + " bytes=" + juce::String (numBytes));
        return 0;
    }

    aooDiag ("sendAooReply endpoint="
             + endpointDebug (static_cast<const sockaddr_in*> (endpoint))
             + " bytes=" + juce::String (numBytes));

    const auto result = sendto (*socket, data, numBytes, 0,
                                static_cast<const sockaddr*> (endpoint), sizeof (sockaddr_in));

    if (result != numBytes)
        aooDiag ("sendAooReply sendto FAILED result="
                 + juce::String ((int) result)
                 + " expected=" + juce::String (numBytes));

    return result == numBytes ? 1 : 0;
}

int64_t makeSourceKey (const sockaddr_in* endpoint, int32_t sourceId) noexcept
{
    // AOO source IDs are scoped to a peer. Keep the METRO identity stable
    // when the peer's UDP port changes during reconnect/NAT remapping.
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash] (uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            hash ^= (value >> (i * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    if (endpoint != nullptr)
        mix (static_cast<uint64_t> (endpoint->sin_addr.s_addr));
    mix (static_cast<uint32_t> (sourceId));
    hash &= 0x7fffffffffffffffull;
    return hash == 0 ? 1 : static_cast<int64_t> (hash);
}

bool sameEndpoint (const sockaddr_in* a, const sockaddr_in* b) noexcept
{
    return a != nullptr && b != nullptr
        && a->sin_family == b->sin_family
        && a->sin_addr.s_addr == b->sin_addr.s_addr
        && a->sin_port == b->sin_port;
}

bool samePeerAddress (const sockaddr_in* a, const sockaddr_in* b) noexcept
{
    // AOO peer identity is the remote IP + source id. The UDP source port can
    // change between the invited endpoint and the actual packets (for example
    // iOS may send from an ephemeral port). Do not require the port to match
    // when deciding which runtime owns an incoming packet.
    return a != nullptr && b != nullptr
        && a->sin_family == b->sin_family
        && a->sin_addr.s_addr == b->sin_addr.s_addr;
}
}

class MetroNetworkAudio::Impl
{
public:
    struct SourceRuntime
    {
        enum class HandshakeState
        {
            New,
            Invited,
            Formatted
        };

        int64_t sourceKey = 0;
        int32_t sourceId = 0;
        std::shared_ptr<sockaddr_in> endpoint;
        aoo::isink::pointer sink;

        // One-way source handshake:
        // New -> Invited -> Formatted.
        // Repeated discovery/format events must never re-invite a source.
        HandshakeState handshake = HandshakeState::New;
        bool directSourcePublished = false;

        std::array<float, kAooChannels * kAooBlockSize> scratch {};
        std::array<aoo_sample*, kAooChannels> pointers {};
    };

    struct PeerInfo
    {
        std::shared_ptr<sockaddr_in> endpoint;
        juce::String group;
        juce::String user;
    };

    MetroNetworkAudio& owner;
    std::atomic<bool> running { false };
    std::atomic<bool> directMode { false };
    MetroSocket socket = metroInvalidSocket;
    aoo::net::iclient::pointer client;
    aoo::isink::pointer discoverySink;
    std::thread ioThread;

    mutable std::mutex stateMutex;
    std::vector<MetroNetworkAudio::SourceInfo> sources;
    MetroNetworkAudio::SourceListener listener;
    std::vector<PeerInfo> peers;
    std::vector<std::unique_ptr<SourceRuntime>> runtimes;
    std::array<std::atomic<SourceRuntime*>, kMaxNetworkSources> runtimeSlots {};
    std::array<float, kAooChannels * kAooBlockSize> mixScratch {};
    std::array<aoo_sample*, kAooChannels> mixPointers {};

    std::atomic<bool> connected { false };
    std::atomic<bool> joined { false };
    juce::String group;
    juce::String pendingGroupPassword;
    bool pendingGroupPublic = false;
    std::atomic<bool> groupJoinPending { false };

    explicit Impl (MetroNetworkAudio& ownerIn) : owner (ownerIn)
    {
        for (auto& slot : runtimeSlots) slot.store (nullptr, std::memory_order_relaxed);
    }

    ~Impl() { stop(); }

    SourceRuntime* findRuntime (int64_t sourceKey) const noexcept
    {
        for (const auto& slot : runtimeSlots)
        {
            auto* runtime = slot.load (std::memory_order_acquire);
            if (runtime != nullptr && runtime->sourceKey == sourceKey) return runtime;
        }
        return nullptr;
    }

    const PeerInfo* findPeer (const sockaddr_in* endpoint) const noexcept
    {
        for (const auto& peer : peers)
            if (sameEndpoint (peer.endpoint.get(), endpoint)) return &peer;
        return nullptr;
    }

    bool isDirectPeerAddress (const sockaddr_in* endpoint) const
    {
        if (endpoint == nullptr)
            return false;

        // In Direct USB mode we no longer pre-create runtimes for possible
        // Apple addresses. The isolated USB network uses 192.168.99.0/24,
        // with the Apple peer normally at .2. Accept packets from that
        // isolated subnet and let the first real packet create the runtime.
        const auto hostOrder = ntohl (endpoint->sin_addr.s_addr);
        if ((hostOrder & 0xFFFFFF00u) == 0xC0A86300u)
            return true;

        std::lock_guard<std::mutex> lock (stateMutex);
        for (const auto& peer : peers)
            if (peer.endpoint != nullptr
                && peer.endpoint->sin_addr.s_addr == endpoint->sin_addr.s_addr)
                return true;
        return false;
    }

    std::shared_ptr<sockaddr_in> findPeerEndpoint (const sockaddr_in* endpoint) const
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        for (const auto& peer : peers)
            if (sameEndpoint (peer.endpoint.get(), endpoint)) return peer.endpoint;
        return {};
    }

    void publishDirectRuntimeSource (SourceRuntime* runtime)
    {
        if (runtime == nullptr || ! directMode.load (std::memory_order_acquire)
            || runtime->directSourcePublished)
            return;

        SourceInfo info;
        info.sourceKey = runtime->sourceKey;
        info.sourceId = runtime->sourceId;
        info.user = "USB";
        info.group = "Direct USB";
        info.online = true;
        upsertSource (info);
        runtime->directSourcePublished = true;
        notifySourceChange (this);
        aooDiag ("DIRECT SOURCE published sourceId=" + juce::String (runtime->sourceId)
                 + " endpoint=" + endpointDebug (runtime->endpoint.get()));
    }

    SourceRuntime* createRuntime (int32_t sourceId, const sockaddr_in* endpoint)
    {
        if (sourceId == AOO_ID_WILDCARD
            || sourceId == AOO_ID_NONE
            || endpoint == nullptr)
        {
            aooDiag ("createRuntime REJECT sourceId="
                     + juce::String (sourceId)
                     + " endpoint=" + endpointDebug (endpoint));
            return nullptr;
        }

        const auto identitySourceId = directMode.load (std::memory_order_acquire) ? 0 : sourceId;
        const auto sourceKey = makeSourceKey (endpoint, identitySourceId);

        aooDiag ("createRuntime sourceId=" + juce::String (sourceId)
                 + " sourceKey=" + juce::String (sourceKey)
                 + " endpoint=" + endpointDebug (endpoint));

        // SOURCE_ADD and SOURCE_FORMAT may both reach this function. Once a
        // runtime exists, simply reuse it. Never invite an existing source.
        // Direct USB is special: the Apple source may change its UDP source
        // port. Do not use the endpoint port as the runtime identity or a
        // second runtime/source will be created for the same iPad.
        SourceRuntime* existing = findRuntime (sourceKey);

        if (directMode.load (std::memory_order_acquire))
        {
            for (auto& slot : runtimeSlots)
            {
                auto* candidate = slot.load (std::memory_order_acquire);
                if (candidate != nullptr
                    && candidate->sourceId == sourceId
                    && candidate->endpoint != nullptr
                    && samePeerAddress (candidate->endpoint.get(), endpoint))
                {
                    existing = candidate;
                    break;
                }
            }
        }

        if (existing != nullptr)
        {
            if (existing->endpoint != nullptr
                && ! sameEndpoint (existing->endpoint.get(), endpoint))
            {
                *existing->endpoint = *endpoint;
                aooDiag ("createRuntime EXISTING endpoint updated"
                         " runtime="
                         + juce::String::toHexString (
                             static_cast<juce::int64> (
                                 reinterpret_cast<uintptr_t> (existing)))
                         + " endpoint="
                         + endpointDebug (existing->endpoint.get()));
            }
            else
            {
                aooDiag ("createRuntime EXISTING no-op"
                         " sourceId=" + juce::String (sourceId)
                         + " handshake="
                         + juce::String (
                             existing->handshake == SourceRuntime::HandshakeState::New
                                 ? "New"
                                 : existing->handshake == SourceRuntime::HandshakeState::Invited
                                     ? "Invited"
                                     : "Formatted"));
            }

            return existing;
        }

        auto runtime = std::make_unique<SourceRuntime>();
        runtime->sourceKey = sourceKey;
        runtime->sourceId = sourceId;
        runtime->endpoint = std::make_shared<sockaddr_in> (*endpoint);
        runtime->sink.reset (aoo::isink::create (0));

        if (runtime->sink == nullptr
            || runtime->sink->setup (
                   kAooSampleRate,
                   kAooBlockSize,
                   kAooChannels) <= 0)
        {
            aooDiag ("createRuntime FAILED sink setup"
                     " sourceId=" + juce::String (sourceId)
                     + " endpoint=" + endpointDebug (endpoint));
            return nullptr;
        }

        runtime->sink->set_buffersize (kAooBufferMs);
        runtime->sink->set_dynamic_resampling (1);
        runtime->sink->set_resend_limit (5);
        runtime->sink->set_resend_interval (10);
        runtime->sink->set_resend_maxnumframes (16);

        auto* raw = runtime.get();

        for (auto& slot : runtimeSlots)
        {
            SourceRuntime* expected = nullptr;

            if (! slot.compare_exchange_strong (
                    expected,
                    raw,
                    std::memory_order_release,
                    std::memory_order_relaxed))
                continue;

            runtimes.push_back (std::move (runtime));

            // Direct USB runtimes may be proactively created for an Apple peer
            // so an AUv3-hosted SonoBus instance can receive the AOO invite before
            // it emits its first packet. Keep that bootstrap runtime invisible;
            // publish it only after the peer actually responds.

            // The only place a dedicated runtime is invited. This is the
            // New -> Invited transition and happens at most once per runtime.
            const auto result = raw->sink->invite_source (
                raw->endpoint.get(),
                sourceId,
                sendAooReply);

            if (result > 0)
            {
                raw->handshake = SourceRuntime::HandshakeState::Invited;
                aooDiag ("SOURCE_HANDSHAKE New -> Invited"
                         " sourceId=" + juce::String (sourceId)
                         + " endpoint=" + endpointDebug (raw->endpoint.get())
                         + " result=" + juce::String (result));
            }
            else
            {
                aooDiag ("SOURCE_HANDSHAKE invite FAILED"
                         " sourceId=" + juce::String (sourceId)
                         + " endpoint=" + endpointDebug (raw->endpoint.get())
                         + " result=" + juce::String (result));
            }

            return raw;
        }

        aooDiag ("createRuntime FAILED: no free runtime slot");
        return nullptr;
    }

    void destroyRuntimeSlots()
    {
        for (auto& slot : runtimeSlots) slot.store (nullptr, std::memory_order_release);
        runtimes.clear();
    }

    bool start()
    {
        return startWithBinding (0);
    }

    bool startDirect (int localPort)
    {
        return startWithBinding (localPort);
    }

    bool startWithBinding (int requestedPort)
    {
        if (running.load (std::memory_order_acquire)) return true;
#if defined(_WIN32)
        WSADATA wsa {};
        if (WSAStartup (MAKEWORD (2, 2), &wsa) != 0) return false;
#endif
        startAooLifetime();
        socket = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == metroInvalidSocket) return cleanupFailedStart();

        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl (INADDR_ANY);
        local.sin_port = htons (static_cast<uint16_t> (juce::jlimit (0, 65535, requestedPort)));
        if (::bind (socket, reinterpret_cast<const sockaddr*> (&local), sizeof (local)) != 0 || ! setNonBlocking (socket))
            return cleanupFailedStart();

        socklen_t localLength = sizeof (local);
        if (getsockname (socket, reinterpret_cast<sockaddr*> (&local), &localLength) != 0)
            return cleanupFailedStart();

        client.reset (aoo::net::iclient::create (&socket, sendUdp, ntohs (local.sin_port)));
        discoverySink.reset (aoo::isink::create (0));
        if (client == nullptr || discoverySink == nullptr) return cleanupFailedStart();
        if (discoverySink->setup (kAooSampleRate, kAooBlockSize, kAooChannels) <= 0) return cleanupFailedStart();
        discoverySink->set_buffersize (kAooBufferMs);
        discoverySink->set_dynamic_resampling (1);
        discoverySink->set_resend_limit (5);
        discoverySink->set_resend_interval (10);
        discoverySink->set_resend_maxnumframes (16);

        activeAooSocket.store (&socket, std::memory_order_release);
        directMode.store (requestedPort != 0, std::memory_order_release);
        running.store (true, std::memory_order_release);

        // ioLoop() is the sole driver of the AOO client. It owns
        // handle_message(), send(), and handle_events().
        ioThread = std::thread ([this] { ioLoop(); });
        return true;
    }

    bool cleanupFailedStart()
    {
        activeAooSocket.store (nullptr, std::memory_order_release);
        directMode.store (false, std::memory_order_release);
        client.reset();
        discoverySink.reset();
        destroyRuntimeSlots();
        closeSocket (socket);
        socket = metroInvalidSocket;
        stopAooLifetime();
#if defined(_WIN32)
        WSACleanup();
#endif
        return false;
    }

    void stop()
    {
        if (! running.exchange (false, std::memory_order_acq_rel)) return;
        connected.store (false, std::memory_order_release);
        joined.store (false, std::memory_order_release);
        groupJoinPending.store (false, std::memory_order_release);
        activeAooSocket.store (nullptr, std::memory_order_release);
        directMode.store (false, std::memory_order_release);
        if (client != nullptr) client->quit();
        if (ioThread.joinable()) ioThread.join();
        client.reset();
        discoverySink.reset();
        destroyRuntimeSlots();
        closeSocket (socket);
        socket = metroInvalidSocket;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            sources.clear();
            peers.clear();
        }
        stopAooLifetime();
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    bool connectDirectPeer (const juce::String& peerHost, int peerPort, int sourceId)
    {
        if (! running.load (std::memory_order_acquire) || discoverySink == nullptr || peerHost.isEmpty() || peerPort <= 0 || peerPort > 65535)
            return false;

        sockaddr_in endpoint {};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons (static_cast<uint16_t> (peerPort));
        if (inet_pton (AF_INET, peerHost.toRawUTF8(), &endpoint.sin_addr) != 1)
            return false;

        if (sourceId == AOO_ID_WILDCARD || sourceId == AOO_ID_NONE) return false;
        auto* runtime = createRuntime (sourceId, &endpoint);
        if (runtime == nullptr) return false;

        {
            std::lock_guard<std::mutex> lock (stateMutex);
            auto existing = std::find_if (peers.begin(), peers.end(), [&endpoint] (const auto& peer) { return sameEndpoint (peer.endpoint.get(), &endpoint); });
            if (existing == peers.end()) peers.push_back ({ std::make_shared<sockaddr_in> (endpoint), "USB", peerHost });
        }
        return true;
    }

    void disconnectDirectPeers ()
    {
        stop();
    }

    bool connectToServer (const juce::String& host, int port, const juce::String& username, const juce::String& password)
    {
        if (! running.load (std::memory_order_acquire) || client == nullptr) return false;
        groupJoinPending.store (false, std::memory_order_release);
        joined.store (false, std::memory_order_release);
        connected.store (false, std::memory_order_release);
        return client->connect (host.toRawUTF8(), port, username.toRawUTF8(), password.toRawUTF8()) > 0;
    }

    bool joinGroup (const juce::String& groupName, const juce::String& password, bool isPublic)
    {
        if (! running.load (std::memory_order_acquire) || client == nullptr) return false;
        group = groupName;
        pendingGroupPassword = password;
        pendingGroupPublic = isPublic;
        groupJoinPending.store (true, std::memory_order_release);
        if (! connected.load (std::memory_order_acquire)) return true;
        return queueGroupJoin();
    }

    bool queueGroupJoin()
    {
        if (! running.load (std::memory_order_acquire) || client == nullptr || ! connected.load (std::memory_order_acquire) || group.isEmpty()) return false;
        const bool queued = client->group_join (group.toRawUTF8(), pendingGroupPassword.toRawUTF8(), pendingGroupPublic) > 0;
        if (queued) groupJoinPending.store (false, std::memory_order_release);
        return queued;
    }

    void leaveGroup (const juce::String& groupName)
    {
        groupJoinPending.store (false, std::memory_order_release);
        if (client != nullptr) client->group_leave (groupName.toRawUTF8());
        joined.store (false, std::memory_order_release);
    }

    void disconnect()
    {
        groupJoinPending.store (false, std::memory_order_release);
        if (client != nullptr) client->disconnect();
        connected.store (false, std::memory_order_release);
        joined.store (false, std::memory_order_release);
    }

    void ioLoop()
    {
        std::array<char, AOO_MAXPACKETSIZE> packet {};
        while (running.load (std::memory_order_acquire))
        {
            fd_set readSet;
            FD_ZERO (&readSet);
            FD_SET (socket, &readSet);
            timeval timeout {};
            timeout.tv_usec = 20000;
            const auto ready = select (static_cast<int> (socket + 1), &readSet, nullptr, nullptr, &timeout);

            if (ready > 0 && FD_ISSET (socket, &readSet))
            {
                sockaddr_in from {};
                socklen_t fromLength = sizeof (from);
                const auto n = recvfrom (socket, packet.data(), static_cast<int> (packet.size()), 0,
                                         reinterpret_cast<sockaddr*> (&from), &fromLength);
                if (n > 0)
                {
                    aooDiag ("RX packet bytes=" + juce::String (n)
                             + " from=" + endpointDebug (&from)
                             + " head="
                             + juce::String::toHexString (reinterpret_cast<const uint8_t*> (packet.data()),
                                                          static_cast<int> (std::min (n, 24))));

                    if (directMode.load (std::memory_order_acquire) && ! isDirectPeerAddress (&from))
                    {
                        aooDiag ("RX direct USB filter dropped packet from=" + endpointDebug (&from));
                        continue;
                    }

                    // In direct USB mode there is no AOO server/client
                    // control session. Feeding peer packets through the network
                    // client can consume them as server traffic before the
                    // dedicated source sink sees them. The source sink is the
                    // sole owner of direct-peer packets.
                    if (! directMode.load (std::memory_order_acquire))
                        client->handle_message (packet.data(), n, &from);

                    // AOO identifies a sink source by the endpoint pointer, not
                    // by the sockaddr value. recvfrom() gives us a new stack
                    // address on every packet, while invites use persistent
                    // endpoint objects. Route packets to those same persistent
                    // endpoint objects so format/data replies match the invited
                    // source descriptor instead of creating a second descriptor.
                    auto discoveryEndpoint = findPeerEndpoint (&from);

                    aooDiag ("RX discovery routing from="
                             + endpointDebug (&from)
                             + " persistentPeerEndpoint="
                             + endpointDebug (discoveryEndpoint.get()));

                    // Route each UDP packet to exactly one sink.
                    // In Direct USB mode runtimes are created lazily from the
                    // first real packet. This prevents a disconnected/no-USB
                    // state from manufacturing a visible source in the UI.
                    SourceRuntime* targetRuntime = nullptr;

                    if (directMode.load (std::memory_order_acquire))
                    {
                        for (auto& slot : runtimeSlots)
                        {
                            auto* runtime = slot.load (std::memory_order_acquire);
                            if (runtime != nullptr && runtime->endpoint != nullptr
                                && samePeerAddress (runtime->endpoint.get(), &from))
                            {
                                targetRuntime = runtime;
                                break;
                            }
                        }

                        if (targetRuntime == nullptr)
                        {
                            targetRuntime = createRuntime (0, &from);
                            if (targetRuntime != nullptr)
                            {
                                publishDirectRuntimeSource (targetRuntime);
                                aooDiag ("RX direct USB created runtime from real peer packet"
                                         " sourceId=0 endpoint=" + endpointDebug (&from));

                                std::lock_guard<std::mutex> lock (stateMutex);
                                peers.push_back ({ std::make_shared<sockaddr_in> (from), "USB",
                                                   juce::String (inet_ntoa (from.sin_addr)) });
                            }
                        }
                    }

                    // runtimeSlots is the cross-thread publication mechanism.
                    // Direct USB has exactly one AOO source identity per Apple
                    // peer. Never send the same packet through discovery as well
                    // as the dedicated runtime; discovery can manufacture a
                    // second source (for example source id 1) for the same iPad.
                    if (directMode.load (std::memory_order_acquire))
                    {
                        for (auto& slot : runtimeSlots)
                        {
                            auto* runtime = slot.load (std::memory_order_acquire);
                            if (runtime == nullptr || runtime->sink == nullptr
                                || runtime->endpoint == nullptr
                                || ! samePeerAddress (runtime->endpoint.get(), &from))
                                continue;

                            if (! sameEndpoint (runtime->endpoint.get(), &from))
                            {
                                aooDiag ("RX runtime endpoint updated from="
                                         + endpointDebug (runtime->endpoint.get())
                                         + " to=" + endpointDebug (&from));
                                *runtime->endpoint = from;
                            }

                            targetRuntime = runtime;
                            if (! runtime->directSourcePublished)
                                publishDirectRuntimeSource (runtime);
                            break;
                        }
                    }
                    else
                    {
                        for (auto& slot : runtimeSlots)
                        {
                            auto* runtime = slot.load (std::memory_order_acquire);
                            if (runtime == nullptr || runtime->sink == nullptr
                                || runtime->endpoint == nullptr
                                || ! samePeerAddress (runtime->endpoint.get(), &from))
                                continue;

                            targetRuntime = runtime;
                            break;
                        }
                    }

                    if (targetRuntime != nullptr)
                    {
                        aooDiag ("RX -> runtime"
                                 " sourceId=" + juce::String (targetRuntime->sourceId)
                                 + " endpoint=" + endpointDebug (targetRuntime->endpoint.get()));

                        const auto handled = targetRuntime->sink->handle_message (
                            packet.data(),
                            n,
                            targetRuntime->endpoint.get(),
                            sendAooReply);
                        aooDiag ("RX runtime handle_message result="
                                 + juce::String (handled)
                                 + " sourceId=" + juce::String (targetRuntime->sourceId)
                                 + " bytes=" + juce::String (n));
                    }
                    else if (discoverySink != nullptr)
                    {
                        aooDiag ("RX -> discoverySink endpoint="
                                 + endpointDebug (
                                     discoveryEndpoint != nullptr
                                         ? discoveryEndpoint.get()
                                         : &from));

                        discoverySink->handle_message (
                            packet.data(),
                            n,
                            discoveryEndpoint != nullptr
                                ? discoveryEndpoint.get()
                                : &from,
                            sendAooReply);
                    }
                }
            }

            if (! directMode.load (std::memory_order_acquire))
                client->send();

            if (discoverySink != nullptr) discoverySink->send();
            for (auto& slot : runtimeSlots)
            {
                auto* runtime = slot.load (std::memory_order_acquire);
                if (runtime != nullptr && runtime->sink != nullptr)
                {
                    const auto sent = runtime->sink->send();
                    if (sent > 0)
                        aooDiag ("TX runtime send result=" + juce::String (sent)
                                 + " sourceId=" + juce::String (runtime->sourceId)
                                 + " endpoint=" + endpointDebug (runtime->endpoint.get()));
                }
            }

            if (! directMode.load (std::memory_order_acquire)
                && client->events_available() > 0)
                client->handle_events (clientEventHandler, this);

            if (! directMode.load (std::memory_order_acquire)
                && discoverySink != nullptr && discoverySink->events_available() > 0)
                discoverySink->handle_events (discoveryEventHandler, this);

            for (auto& slot : runtimeSlots)
            {
                auto* runtime = slot.load (std::memory_order_acquire);
                if (runtime != nullptr && runtime->sink != nullptr
                    && runtime->sink->events_available() > 0)
                    runtime->sink->handle_events (sourceEventHandler, this);
            }
        }
    }

    static int32_t clientEventHandler (void* user, const aoo_event** events, int32_t count)
    {
        auto* self = static_cast<Impl*> (user);
        for (int32_t i = 0; i < count; ++i)
        {
            if (events[i] == nullptr) continue;
            switch (events[i]->type)
            {
                case AOONET_CLIENT_CONNECT_EVENT:
                    self->connected.store (true, std::memory_order_release);
                    if (self->groupJoinPending.load (std::memory_order_acquire)) self->queueGroupJoin();
                    break;
                case AOONET_CLIENT_GROUP_JOIN_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoonet_client_group_event*> (events[i]);
                    if (event->result > 0) self->joined.store (true, std::memory_order_release);
                    break;
                }
                case AOONET_CLIENT_PEER_JOIN_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoonet_client_peer_event*> (events[i]);
                    if (event->result <= 0 || event->address == nullptr || event->length != sizeof (sockaddr_in)) break;
                    auto endpoint = std::make_shared<sockaddr_in>();
                    std::memcpy (endpoint.get(), event->address, sizeof (sockaddr_in));

                    aooDiag ("PEER_JOIN endpoint=" + endpointDebug (endpoint.get())
                             + " user="
                             + juce::String (event->user != nullptr
                                                  ? event->user : "<null>")
                             + " group="
                             + juce::String (event->group != nullptr
                                                  ? event->group : "<null>"));

                    {
                        std::lock_guard<std::mutex> lock (self->stateMutex);
                        auto it = std::find_if (self->peers.begin(), self->peers.end(), [&endpoint] (const auto& peer)
                        {
                            return sameEndpoint (peer.endpoint.get(), endpoint.get());
                        });
                        if (it == self->peers.end())
                        {
                            PeerInfo peer;
                            peer.endpoint = endpoint;
                            peer.group = event->group != nullptr ? juce::String::fromUTF8 (event->group) : juce::String();
                            peer.user = event->user != nullptr ? juce::String::fromUTF8 (event->user) : juce::String();
                            self->peers.push_back (std::move (peer));
                        }
                        else
                        {
                            if (event->group != nullptr) it->group = juce::String::fromUTF8 (event->group);
                            if (event->user != nullptr) it->user = juce::String::fromUTF8 (event->user);
                        }

                        if (auto peer = self->findPeer (endpoint.get()))
                        {
                            for (auto& source : self->sources)
                            {
                                auto* runtime = self->findRuntime (source.sourceKey);
                                if (runtime == nullptr || runtime->endpoint == nullptr)
                                    continue;

                                // A peer can expose multiple AOO sources. Only update
                                // sources belonging to the peer that just joined; do not
                                // overwrite every source with the last peer's name.
                                if (runtime->endpoint->sin_addr.s_addr != endpoint->sin_addr.s_addr)
                                    continue;

                                source.user = peer->user;
                                if (peer->group.isNotEmpty()) source.group = peer->group;
                            }
                        }
                    }
                    if (self->discoverySink != nullptr)
                    {
                        // SonoBus exposes each peer's main audio stream as AOO source 0.
                        // Its bundled AOO fork explicitly rejects wildcard source messages,
                        // so inviting AOO_ID_WILDCARD only creates a local -1 descriptor and
                        // can never complete the remote format handshake.
                        constexpr int32_t sonoBusMainSourceId = 0;

                        aooDiag ("PEER_JOIN source invite sink="
                                 + juce::String::toHexString (
                                     static_cast<juce::int64> (
                                         reinterpret_cast<uintptr_t> (
                                             self->discoverySink.get())))
                                 + " endpoint=" + endpointDebug (endpoint.get())
                                 + " sourceId=" + juce::String (sonoBusMainSourceId));

                        const auto result = self->discoverySink->invite_source (
                            endpoint.get(), sonoBusMainSourceId, sendAooReply);

                        aooDiag ("PEER_JOIN source invite result="
                                 + juce::String (result));
                    }
                    notifySourceChange (self);
                    break;
                }
                case AOONET_CLIENT_PEER_LEAVE_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoonet_client_peer_event*> (events[i]);
                    if (event->result <= 0 || event->address == nullptr || event->length != sizeof (sockaddr_in)) break;
                    const auto* endpoint = static_cast<const sockaddr_in*> (event->address);
                    const juce::String peerGroup = event->group != nullptr ? juce::String::fromUTF8 (event->group) : juce::String();
                    const juce::String peerUser = event->user != nullptr ? juce::String::fromUTF8 (event->user) : juce::String();
                    bool sourceChanged = false;
                    {
                        std::lock_guard<std::mutex> lock (self->stateMutex);
                        self->peers.erase (std::remove_if (self->peers.begin(), self->peers.end(), [&] (const auto& peer)
                        {
                            return sameEndpoint (peer.endpoint.get(), endpoint);
                        }), self->peers.end());

                        for (auto& source : self->sources)
                        {
                            auto* runtime = self->findRuntime (source.sourceKey);
                            if (runtime == nullptr || runtime->endpoint == nullptr) continue;
                            const bool sameIp = runtime->endpoint->sin_addr.s_addr == endpoint->sin_addr.s_addr;
                            const bool sameGroup = peerGroup.isEmpty() || source.group.isEmpty() || source.group == peerGroup;
                            const bool sameUser = peerUser.isEmpty() || source.user.isEmpty() || source.user == peerUser;
                            if (sameIp && sameGroup && sameUser && source.online)
                            {
                                source.online = false;
                                sourceChanged = true;
                            }
                        }
                    }
                    if (sourceChanged) notifySourceChange (self);
                    break;
                }
                case AOONET_CLIENT_DISCONNECT_EVENT:
                {
                    self->connected.store (false, std::memory_order_release);
                    self->joined.store (false, std::memory_order_release);
                    bool sourceChanged = false;
                    {
                        std::lock_guard<std::mutex> lock (self->stateMutex);
                        for (auto& source : self->sources)
                            if (source.online)
                            {
                                source.online = false;
                                sourceChanged = true;
                            }
                    }
                    if (sourceChanged) notifySourceChange (self);
                    break;
                }
                default: break;
            }
        }
        return 1;
    }

    static void notifySourceChange (Impl* self)
    {
        MetroNetworkAudio::SourceListener callback;
        {
            std::lock_guard<std::mutex> lock (self->stateMutex);
            callback = self->listener;
        }
        if (callback) juce::MessageManager::callAsync (std::move (callback));
    }

    static bool markFormat (Impl* self,
                            int64_t key,
                            aoo::isink* sink,
                            void* endpoint,
                            int32_t sourceId)
    {
        if (sink == nullptr || endpoint == nullptr)
            return false;

        aooDiag ("get_source_format BEGIN"
                 " key=" + juce::String (key)
                 + " sourceId=" + juce::String (sourceId)
                 + " endpoint="
                 + endpointDebug (
                     static_cast<const sockaddr_in*> (endpoint)));

        aoo_format_storage format {};
        const auto result =
            sink->get_source_format (endpoint, sourceId, format);

        aooDiag ("get_source_format RESULT result="
                 + juce::String (result)
                 + " sourceId=" + juce::String (sourceId)
                 + " endpoint="
                 + endpointDebug (
                     static_cast<const sockaddr_in*> (endpoint))
                 + " channels="
                 + juce::String (format.header.nchannels)
                 + " sampleRate="
                 + juce::String (format.header.samplerate));

        if (result <= 0
            || format.header.nchannels <= 0
            || format.header.samplerate <= 0.0)
            return false;

        bool changed = false;

        {
            std::lock_guard<std::mutex> lock (self->stateMutex);

            for (auto& source : self->sources)
            {
                if (source.sourceKey != key)
                    continue;

                changed =
                    source.channels != format.header.nchannels
                    || source.sampleRate != format.header.samplerate
                    || ! source.online;

                source.channels = format.header.nchannels;
                source.sampleRate = format.header.samplerate;
                source.online = true;
                break;
            }
        }

        // Valid format completes the source handshake. This transition is
        // terminal until the runtime is destroyed; it never calls
        // invite_source() and therefore cannot create an invite/format loop.
        if (auto* runtime = self->findRuntime (key))
        {
            if (runtime->handshake != SourceRuntime::HandshakeState::Formatted)
            {
                runtime->handshake =
                    SourceRuntime::HandshakeState::Formatted;

                aooDiag ("SOURCE_HANDSHAKE Invited -> Formatted"
                         " key=" + juce::String (key)
                         + " sourceId=" + juce::String (sourceId));
            }
        }

        return changed;
    }

    static int32_t discoveryEventHandler (void* user, const aoo_event** events, int32_t count)
    {
        auto* self = static_cast<Impl*> (user);
        bool changed = false;
        for (int32_t i = 0; i < count; ++i)
        {
            if (events[i] == nullptr) continue;
            switch (events[i]->type)
            {
                case AOO_SOURCE_ADD_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_source_event*> (events[i]);
                    if (event->endpoint == nullptr) break;

                    aooDiag ("DISCOVERY SOURCE_ADD"
                             " id=" + juce::String (event->id)
                             + " endpoint="
                             + endpointDebug (
                                 static_cast<const sockaddr_in*> (
                                     event->endpoint)));

                    // Never expose a wildcard descriptor as a routable source. SonoBus
                    // peers are invited explicitly as source 0, so a -1 event can only be
                    // stale state from an older wildcard invitation.
                    if (event->id == AOO_ID_WILDCARD) break;
                    const auto* endpoint = static_cast<const sockaddr_in*> (event->endpoint);
                    SourceInfo info;
                    info.sourceKey = makeSourceKey (endpoint, event->id);
                    info.sourceId = event->id;
                    info.group = self->group;
                    info.online = true;
                    {
                        std::lock_guard<std::mutex> lock (self->stateMutex);
                        if (auto peer = self->findPeer (endpoint))
                        {
                            info.user = peer->user;
                            if (peer->group.isNotEmpty()) info.group = peer->group;
                        }
                    }
                    self->upsertSource (info);
                    auto* runtime = self->createRuntime (event->id, endpoint);

                    // SOURCE_ADD can arrive before the corresponding FORMAT event.
                    // Populate metadata immediately when AOO already has it.
                    if (runtime != nullptr && runtime->sink != nullptr)
                    {
                        aooDiag ("DISCOVERY SOURCE_ADD markFormat via runtime"
                                 " runtime="
                                 + juce::String::toHexString (
                                     static_cast<juce::int64> (
                                         reinterpret_cast<uintptr_t> (runtime)))
                                 + " sink="
                                 + juce::String::toHexString (
                                     static_cast<juce::int64> (
                                         reinterpret_cast<uintptr_t> (
                                             runtime->sink.get())))
                                 + " endpoint="
                                 + endpointDebug (runtime->endpoint.get()));

                        markFormat (self, info.sourceKey, runtime->sink.get(),
                                    runtime->endpoint.get(), event->id);
                    }

                    changed = true;
                    break;
                }
                case AOO_SOURCE_FORMAT_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_source_event*> (events[i]);
                    if (event->endpoint == nullptr || event->id == AOO_ID_WILDCARD) break;

                    const auto* endpoint =
                        static_cast<const sockaddr_in*> (event->endpoint);
                    const auto key = makeSourceKey (endpoint, event->id);

                    aooDiag ("DISCOVERY SOURCE_FORMAT"
                             " id=" + juce::String (event->id)
                             + " endpoint=" + endpointDebug (endpoint));

                    // SOURCE_FORMAT is a state transition, not a reason to
                    // create/invite another sink.
                    auto* runtime = self->findRuntime (key);

                    if (runtime != nullptr && runtime->sink != nullptr)
                    {
                        self->publishDirectRuntimeSource (runtime);
                        markFormat (
                            self,
                            key,
                            runtime->sink.get(),
                            runtime->endpoint.get(),
                            event->id);
                    }
                    else if (self->discoverySink != nullptr)
                    {
                        // Fallback only when no dedicated runtime exists.
                        markFormat (
                            self,
                            key,
                            self->discoverySink.get(),
                            event->endpoint,
                            event->id);
                    }

                    changed = true;
                    break;
                }
                case AOO_BLOCK_LOST_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_block_lost_event*> (events[i]);
                    if (event->endpoint == nullptr) break;
                    const auto key = makeSourceKey (static_cast<const sockaddr_in*> (event->endpoint), event->id);
                    std::lock_guard<std::mutex> lock (self->stateMutex);
                    for (auto& source : self->sources)
                        if (source.sourceKey == key)
                            source.packetLoss = std::min (1.0f, source.packetLoss + 0.001f * (float) event->count);
                    changed = true;
                    break;
                }
                default: break;
            }
        }
        if (changed) notifySourceChange (self);
        return 1;
    }

    static int32_t sourceEventHandler (void* user, const aoo_event** events, int32_t count)
    {
        auto* self = static_cast<Impl*> (user);
        aooDiag ("RUNTIME events count=" + juce::String (count));
        bool changed = false;
        for (int32_t i = 0; i < count; ++i)
        {
            if (events[i] == nullptr) continue;
            switch (events[i]->type)
            {
                case AOO_SOURCE_FORMAT_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_source_event*> (events[i]);
                    if (event->endpoint == nullptr || event->id == AOO_ID_WILDCARD) break;

                    const auto* endpoint =
                        static_cast<const sockaddr_in*> (event->endpoint);

                    // Direct USB source identity is the Apple peer address,
                    // not the transient UDP source port and not an AOO
                    // descriptor that may be reported differently by the
                    // remote SonoBus build. Resolve the event back to the
                    // single runtime for that peer, then use that runtime's
                    // stable source key when publishing the format.
                    SourceRuntime* runtime = nullptr;
                    int64_t key = makeSourceKey (endpoint, event->id);

                    if (self->directMode.load (std::memory_order_acquire))
                    {
                        for (auto& slot : self->runtimeSlots)
                        {
                            auto* candidate = slot.load (std::memory_order_acquire);
                            if (candidate != nullptr
                                && candidate->sink != nullptr
                                && candidate->endpoint != nullptr
                                && samePeerAddress (candidate->endpoint.get(), endpoint))
                            {
                                runtime = candidate;
                                key = candidate->sourceKey;
                                break;
                            }
                        }
                    }
                    else
                    {
                        runtime = self->findRuntime (key);
                    }

                    aooDiag ("RUNTIME SOURCE_FORMAT"
                             " eventId=" + juce::String (event->id)
                             + " endpoint=" + endpointDebug (endpoint)
                             + " resolvedKey=" + juce::String (key)
                             + " runtime=" + juce::String (runtime != nullptr ? 1 : 0));

                    if (runtime != nullptr && runtime->sink != nullptr)
                    {
                        markFormat (
                            self,
                            key,
                            runtime->sink.get(),
                            runtime->endpoint.get(),
                            event->id);
                    }

                    changed = true;
                    break;
                }
                case AOO_BLOCK_LOST_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_block_lost_event*> (events[i]);
                    if (event->endpoint == nullptr) break;
                    const auto key = makeSourceKey (static_cast<const sockaddr_in*> (event->endpoint), event->id);
                    std::lock_guard<std::mutex> lock (self->stateMutex);
                    for (auto& source : self->sources)
                        if (source.sourceKey == key)
                            source.packetLoss = std::min (1.0f, source.packetLoss + 0.001f * (float) event->count);
                    changed = true;
                    break;
                }
                case AOO_SOURCE_REMOVE_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_source_event*> (events[i]);
                    if (event->endpoint == nullptr) break;
                    const auto key = makeSourceKey (static_cast<const sockaddr_in*> (event->endpoint), event->id);
                    std::lock_guard<std::mutex> lock (self->stateMutex);
                    for (auto& source : self->sources)
                        if (source.sourceKey == key) source.online = false;
                    changed = true;
                    break;
                }
                default: break;
            }
        }
        if (changed) notifySourceChange (self);
        return 1;
    }

    void upsertSource (const MetroNetworkAudio::SourceInfo& info)
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        auto it = std::find_if (sources.begin(), sources.end(), [&info] (const auto& source) { return source.sourceKey == info.sourceKey; });
        if (it == sources.end())
            sources.push_back (info);
        else
        {
            it->sourceId = info.sourceId;
            if (info.user.isNotEmpty()) it->user = info.user;
            if (info.group.isNotEmpty()) it->group = info.group;
            it->online = true;
        }
    }

    void refreshSourceFormats()
    {
        // Snapshot the AOO runtimes while holding the state lock, then query AOO
        // without that lock. This keeps format polling independent from callbacks.
        struct PendingQuery
        {
            int64_t sourceKey = 0;
            int32_t sourceId = 0;
            std::shared_ptr<sockaddr_in> endpoint;
            aoo::isink* sink = nullptr;
        };

        std::vector<PendingQuery> queries;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            for (const auto& source : sources)
            {
                if (source.channels > 0 && source.sampleRate > 0.0)
                    continue;

                auto* runtime = findRuntime (source.sourceKey);
                if (runtime == nullptr || runtime->sink == nullptr || runtime->endpoint == nullptr)
                    continue;

                queries.push_back ({ source.sourceKey, runtime->sourceId, runtime->endpoint, runtime->sink.get() });
            }
        }

        struct PendingFormat
        {
            int64_t sourceKey = 0;
            int channels = 0;
            double sampleRate = 0.0;
        };

        std::vector<PendingFormat> formats;
        for (const auto& query : queries)
        {
            aoo_format_storage format {};
            if (query.sink->get_source_format (query.endpoint.get(), query.sourceId, format) > 0
                && format.header.nchannels > 0 && format.header.samplerate > 0.0)
                formats.push_back ({ query.sourceKey, format.header.nchannels, static_cast<double> (format.header.samplerate) });
        }

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            for (const auto& update : formats)
            {
                for (auto& source : sources)
                {
                    if (source.sourceKey != update.sourceKey)
                        continue;

                    const bool sourceChanged = source.channels != update.channels
                                             || source.sampleRate != update.sampleRate
                                             || ! source.online;
                    source.channels = update.channels;
                    source.sampleRate = update.sampleRate;
                    source.online = true;
                    changed = changed || sourceChanged;
                    break;
                }
            }
        }

        if (changed) notifySourceChange (this);
    }

    bool processSourceChannel (juce::AudioBuffer<float>& destination, int numSamples, int64_t sourceKey, int sourceChannel)
    {
        destination.clear();
        if (! running.load (std::memory_order_acquire) || numSamples <= 0 || sourceChannel < 0) return false;
        auto* runtime = findRuntime (sourceKey);
        if (runtime == nullptr || runtime->sink == nullptr || sourceChannel >= kAooChannels) return false;

        bool produced = false;
        int offset = 0;
        while (offset < numSamples)
        {
            const int block = std::min (kAooBlockSize, numSamples - offset);
            for (int c = 0; c < kAooChannels; ++c)
            {
                runtime->pointers[(size_t) c] = runtime->scratch.data() + (size_t) c * kAooBlockSize;
                std::fill (runtime->pointers[(size_t) c], runtime->pointers[(size_t) c] + block, 0.0f);
            }

            if (runtime->sink->process (runtime->pointers.data(), block, aoo_osctime_get()) > 0)
            {
                const auto* src = runtime->pointers[(size_t) sourceChannel];
                if (destination.getNumChannels() > 0) destination.copyFrom (0, offset, src, block);
                if (destination.getNumChannels() > 1) destination.copyFrom (1, offset, src, block);
                produced = true;
            }
            offset += block;
        }
        return produced;
    }

    void processLegacyMix (juce::AudioBuffer<float>& destination, int numSamples)
    {
        destination.clear();
        if (! running.load (std::memory_order_acquire) || discoverySink == nullptr || numSamples <= 0) return;
        auto& channel = getNetworkAudioChannelState();
        if (! channel.enabled.load (std::memory_order_relaxed) || channel.muted.load (std::memory_order_relaxed) || ! channel.monitor.load (std::memory_order_relaxed))
        {
            channel.publishPeak (0.0f, 0.0f);
            return;
        }

        const float gain = channel.linearGain();
        const float pan = channel.pan.load (std::memory_order_relaxed);
        const float panAngle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
        const float leftGain = gain * std::cos (panAngle);
        const float rightGain = gain * std::sin (panAngle);
        float peakL = 0.0f, peakR = 0.0f;
        int offset = 0;

        while (offset < numSamples)
        {
            const int block = std::min (kAooBlockSize, numSamples - offset);
            for (int c = 0; c < kAooChannels; ++c)
            {
                mixPointers[(size_t) c] = mixScratch.data() + (size_t) c * kAooBlockSize;
                std::fill (mixPointers[(size_t) c], mixPointers[(size_t) c] + block, 0.0f);
            }

            if (discoverySink->process (mixPointers.data(), block, aoo_osctime_get()) > 0)
            {
                const int sourceChannels = std::min (destination.getNumChannels(), kAooChannels);
                for (int c = 0; c < sourceChannels; ++c)
                    for (int i = 0; i < block; ++i)
                    {
                        const float sample = mixPointers[(size_t) c][i];
                        if (c == 0) peakL = std::max (peakL, std::abs (sample));
                        if (c == 1) peakR = std::max (peakR, std::abs (sample));
                    }
                if (sourceChannels > 0) destination.addFrom (0, offset, mixPointers[0], block, leftGain);
                if (destination.getNumChannels() > 1) destination.addFrom (1, offset, sourceChannels > 1 ? mixPointers[1] : mixPointers[0], block, rightGain);
                for (int c = 2; c < sourceChannels; ++c) destination.addFrom (c, offset, mixPointers[(size_t) c], block, gain);
            }
            offset += block;
        }
        channel.publishPeak (peakL * std::abs (leftGain), peakR * std::abs (rightGain));
    }
};

MetroNetworkAudio::MetroNetworkAudio() : impl (std::make_unique<Impl> (*this)) {}
MetroNetworkAudio::~MetroNetworkAudio() { stop(); }
bool MetroNetworkAudio::start() { return impl != nullptr && impl->start(); }
bool MetroNetworkAudio::startDirect (int localPort) { return impl != nullptr && impl->startDirect (localPort); }
void MetroNetworkAudio::stop() { if (impl != nullptr) impl->stop(); }
bool MetroNetworkAudio::isRunning() const noexcept { return impl != nullptr && impl->running.load (std::memory_order_acquire); }

bool MetroNetworkAudio::connectToServer (const juce::String& host, int port, const juce::String& username, const juce::String& password)
{
    return impl != nullptr && impl->connectToServer (host, port, username, password);
}

bool MetroNetworkAudio::connectDirectPeer (const juce::String& peerHost, int peerPort, int sourceId) { return impl != nullptr && impl->connectDirectPeer (peerHost, peerPort, sourceId); }
void MetroNetworkAudio::disconnectDirectPeers() { if (impl != nullptr) impl->disconnectDirectPeers(); }

bool MetroNetworkAudio::joinGroup (const juce::String& group, const juce::String& password, bool isPublic)
{
    return impl != nullptr && impl->joinGroup (group, password, isPublic);
}

void MetroNetworkAudio::leaveGroup (const juce::String& group) { if (impl != nullptr) impl->leaveGroup (group); }
void MetroNetworkAudio::disconnect() { if (impl != nullptr) impl->disconnect(); }

void MetroNetworkAudio::process (juce::AudioBuffer<float>& destination, int numSamples, double sampleRate)
{
    juce::ignoreUnused (sampleRate);
    if (impl != nullptr) impl->processLegacyMix (destination, numSamples);
}

bool MetroNetworkAudio::processSourceChannel (juce::AudioBuffer<float>& destination, int numSamples, double sampleRate,
                                              int64_t sourceKey, int sourceChannel)
{
    juce::ignoreUnused (sampleRate);
    return impl != nullptr && impl->processSourceChannel (destination, numSamples, sourceKey, sourceChannel);
}

std::vector<MetroNetworkAudio::SourceInfo> MetroNetworkAudio::getSources() const
{
    if (impl == nullptr) return {};
    impl->refreshSourceFormats();
    std::lock_guard<std::mutex> lock (impl->stateMutex);
    return impl->sources;
}

void MetroNetworkAudio::setSourceListener (SourceListener listener)
{
    if (impl == nullptr) return;
    std::lock_guard<std::mutex> lock (impl->stateMutex);
    impl->listener = std::move (listener);
}
