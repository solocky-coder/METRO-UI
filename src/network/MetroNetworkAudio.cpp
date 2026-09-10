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
}

class MetroNetworkAudio::Impl
{
public:
    struct SourceRuntime
    {
        int64_t sourceKey = 0;
        int32_t sourceId = 0;
        std::shared_ptr<sockaddr_in> endpoint;
        aoo::isink::pointer sink;
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
    MetroSocket socket = metroInvalidSocket;
    aoo::net::iclient::pointer client;
    aoo::isink::pointer discoverySink;
    std::thread ioThread;
    std::thread clientThread;

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

    std::shared_ptr<sockaddr_in> findPeerEndpoint (const sockaddr_in* endpoint) const
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        for (const auto& peer : peers)
            if (sameEndpoint (peer.endpoint.get(), endpoint)) return peer.endpoint;
        return {};
    }

    SourceRuntime* createRuntime (int32_t sourceId, const sockaddr_in* endpoint)
    {
        if (sourceId == 0 || sourceId == AOO_ID_WILDCARD || endpoint == nullptr)
        {
            aooDiag ("createRuntime REJECT sourceId="
                     + juce::String (sourceId)
                     + " endpoint=" + endpointDebug (endpoint));
            return nullptr;
        }

        const auto sourceKey = makeSourceKey (endpoint, sourceId);

        aooDiag ("createRuntime sourceId=" + juce::String (sourceId)
                 + " sourceKey=" + juce::String (sourceKey)
                 + " endpoint=" + endpointDebug (endpoint));

        if (auto* existing = findRuntime (sourceKey))
        {
            // Keep the runtime attached to the newest endpoint tuple while
            // retaining the same logical sourceKey.
            *existing->endpoint = *endpoint;

            aooDiag ("createRuntime EXISTING runtime="
                     + juce::String::toHexString (
                         static_cast<juce::int64> (
                             reinterpret_cast<uintptr_t> (existing)))
                     + " sink="
                     + juce::String::toHexString (
                         static_cast<juce::int64> (
                             reinterpret_cast<uintptr_t> (existing->sink.get())))
                     + " endpoint=" + endpointDebug (existing->endpoint.get())
                     + " -> invite_source(" + juce::String (sourceId) + ")");

            const auto result = existing->sink->invite_source (
                existing->endpoint.get(), sourceId, sendAooReply);

            aooDiag ("createRuntime EXISTING invite_source result="
                     + juce::String (result));

            return existing;
        }

        auto runtime = std::make_unique<SourceRuntime>();
        runtime->sourceKey = sourceKey;
        runtime->sourceId = sourceId;
        runtime->endpoint = std::make_shared<sockaddr_in> (*endpoint);
        runtime->sink.reset (aoo::isink::create (0));
        if (runtime->sink == nullptr || runtime->sink->setup (kAooSampleRate, kAooBlockSize, kAooChannels) <= 0)
            return nullptr;

        runtime->sink->set_buffersize (kAooBufferMs);
        runtime->sink->set_dynamic_resampling (1);
        runtime->sink->set_resend_limit (5);
        runtime->sink->set_resend_interval (10);
        runtime->sink->set_resend_maxnumframes (16);

        auto* raw = runtime.get();

        aooDiag ("createRuntime NEW runtime="
                 + juce::String::toHexString (
                     static_cast<juce::int64> (
                         reinterpret_cast<uintptr_t> (raw)))
                 + " sink="
                 + juce::String::toHexString (
                     static_cast<juce::int64> (
                         reinterpret_cast<uintptr_t> (raw->sink.get())))
                 + " endpoint=" + endpointDebug (raw->endpoint.get())
                 + " sourceId=" + juce::String (sourceId));

        for (auto& slot : runtimeSlots)
        {
            SourceRuntime* expected = nullptr;
            if (slot.compare_exchange_strong (expected, raw, std::memory_order_release, std::memory_order_relaxed))
            {
                runtimes.push_back (std::move (runtime));

                aooDiag ("createRuntime NEW invite_source sink="
                         + juce::String::toHexString (
                             static_cast<juce::int64> (
                                 reinterpret_cast<uintptr_t> (raw->sink.get())))
                         + " endpoint=" + endpointDebug (raw->endpoint.get())
                         + " sourceId=" + juce::String (sourceId));

                const auto result = raw->sink->invite_source (
                    raw->endpoint.get(), sourceId, sendAooReply);

                aooDiag ("createRuntime NEW invite_source result="
                         + juce::String (result));

                return raw;
            }
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
        local.sin_port = htons (0);
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
        running.store (true, std::memory_order_release);
        clientThread = std::thread ([this] { client->run(); });
        ioThread = std::thread ([this] { ioLoop(); });
        return true;
    }

    bool cleanupFailedStart()
    {
        activeAooSocket.store (nullptr, std::memory_order_release);
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
        if (client != nullptr) client->quit();
        if (ioThread.joinable()) ioThread.join();
        if (clientThread.joinable()) clientThread.join();
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
                             + " from=" + endpointDebug (&from));

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

                    if (discoverySink != nullptr)
                    {
                        aooDiag ("RX -> discoverySink="
                                 + juce::String::toHexString (
                                     static_cast<juce::int64> (
                                         reinterpret_cast<uintptr_t> (
                                             discoverySink.get())))
                                 + " endpoint="
                                 + endpointDebug (discoveryEndpoint != nullptr
                                                       ? discoveryEndpoint.get()
                                                       : &from));

                        discoverySink->handle_message (packet.data(), n,
                                                       discoveryEndpoint != nullptr ? discoveryEndpoint.get() : &from,
                                                       sendAooReply);
                    }

                    for (auto& runtime : runtimes)
                    {
                        if (runtime == nullptr || runtime->sink == nullptr || runtime->endpoint == nullptr)
                            continue;
                        if (! sameEndpoint (runtime->endpoint.get(), &from))
                            continue;

                        runtime->sink->handle_message (packet.data(), n,
                                                       runtime->endpoint.get(), sendAooReply);
                    }
                }
            }

            client->send();
            if (discoverySink != nullptr) discoverySink->send();
            for (auto& runtime : runtimes)
                if (runtime != nullptr && runtime->sink != nullptr) runtime->sink->send();

            if (client->events_available() > 0) client->handle_events (clientEventHandler, this);
            if (discoverySink != nullptr && discoverySink->events_available() > 0) discoverySink->handle_events (discoveryEventHandler, this);
            for (auto& runtime : runtimes)
                if (runtime != nullptr && runtime->sink != nullptr && runtime->sink->events_available() > 0)
                    runtime->sink->handle_events (sourceEventHandler, this);
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
                        aooDiag ("PEER_JOIN wildcard invite sink="
                                 + juce::String::toHexString (
                                     static_cast<juce::int64> (
                                         reinterpret_cast<uintptr_t> (
                                             self->discoverySink.get())))
                                 + " endpoint=" + endpointDebug (endpoint.get()));

                        const auto result = self->discoverySink->invite_source (
                            endpoint.get(), AOO_ID_WILDCARD, sendAooReply);

                        aooDiag ("PEER_JOIN wildcard invite result="
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

    static bool markFormat (Impl* self, int64_t key, aoo::isink* sink, void* endpoint, int32_t sourceId)
    {
        aooDiag ("get_source_format BEGIN"
                 " key=" + juce::String (key)
                 + " sourceId=" + juce::String (sourceId)
                 + " sink="
                 + juce::String::toHexString (
                     static_cast<juce::int64> (
                         reinterpret_cast<uintptr_t> (sink)))
                 + " endpoint="
                 + endpointDebug (static_cast<const sockaddr_in*> (endpoint)));

        if (sink == nullptr || endpoint == nullptr)
        {
            aooDiag ("get_source_format SKIP null sink/endpoint");
            return false;
        }

        aoo_format_storage format {};
        const auto result = sink->get_source_format (endpoint, sourceId, format);

        aooDiag ("get_source_format RESULT result="
                 + juce::String (result)
                 + " sourceId=" + juce::String (sourceId)
                 + " endpoint="
                 + endpointDebug (static_cast<const sockaddr_in*> (endpoint))
                 + " channels="
                 + juce::String (format.header.nchannels)
                 + " sampleRate="
                 + juce::String (format.header.samplerate));

        if (result <= 0
            || format.header.nchannels <= 0
            || format.header.samplerate <= 0.0)
        {
            aooDiag ("get_source_format INVALID/NOT_AVAILABLE"
                     " result=" + juce::String (result)
                     + " channels="
                     + juce::String (format.header.nchannels)
                     + " sampleRate="
                     + juce::String (format.header.samplerate));
            return false;
        }

        bool changed = false;
        std::lock_guard<std::mutex> lock (self->stateMutex);
        for (auto& source : self->sources)
        {
            if (source.sourceKey != key)
                continue;

            changed = source.channels != format.header.nchannels
                   || source.sampleRate != format.header.samplerate
                   || ! source.online;

            source.channels = format.header.nchannels;
            source.sampleRate = format.header.samplerate;
            source.online = true;
            break;
        }

        aooDiag ("get_source_format ACCEPTED key="
                 + juce::String (key)
                 + " sourceId=" + juce::String (sourceId)
                 + " channels="
                 + juce::String (format.header.nchannels)
                 + " sampleRate="
                 + juce::String (format.header.samplerate));

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

                    // A wildcard invite (see AOONET_CLIENT_PEER_JOIN_EVENT) can be echoed
                    // back as an ADD event carrying AOO_ID_WILDCARD itself rather than a
                    // concrete per-source id. That id can never resolve a real format via
                    // get_source_format(), so tracking it just leaves a permanent
                    // "0 ch / 0 Hz" ghost source in the list. Wait for the real numbered
                    // source instead.
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
                    if (self->discoverySink != nullptr)
                    {
                        aooDiag ("DISCOVERY SOURCE_ADD markFormat via discovery"
                                 " sink="
                                 + juce::String::toHexString (
                                     static_cast<juce::int64> (
                                         reinterpret_cast<uintptr_t> (
                                             self->discoverySink.get())))
                                 + " endpoint="
                                 + endpointDebug (
                                     static_cast<const sockaddr_in*> (
                                         event->endpoint)));

                        markFormat (self, info.sourceKey, self->discoverySink.get(),
                                    event->endpoint, event->id);
                    }

                    changed = true;
                    break;
                }
                case AOO_SOURCE_FORMAT_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_source_event*> (events[i]);
                    if (event->endpoint == nullptr || event->id == AOO_ID_WILDCARD) break;

                    aooDiag ("DISCOVERY SOURCE_FORMAT"
                             " id=" + juce::String (event->id)
                             + " endpoint="
                             + endpointDebug (
                                 static_cast<const sockaddr_in*> (
                                     event->endpoint)));

                    const auto key = makeSourceKey (static_cast<const sockaddr_in*> (event->endpoint), event->id);
                    auto* runtime = self->createRuntime (event->id, static_cast<const sockaddr_in*> (event->endpoint));

                    // Prefer the dedicated sink, then fall back to discovery.
                    if (runtime != nullptr && runtime->sink != nullptr)
                    {
                        aooDiag ("DISCOVERY SOURCE_FORMAT markFormat via runtime");
                        markFormat (self, key, runtime->sink.get(), event->endpoint, event->id);
                    }
                    if (self->discoverySink != nullptr)
                    {
                        aooDiag ("DISCOVERY SOURCE_FORMAT markFormat via discovery");
                        markFormat (self, key, self->discoverySink.get(), event->endpoint, event->id);
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
                    const auto key = makeSourceKey (static_cast<const sockaddr_in*> (event->endpoint), event->id);
                    auto* runtime = self->findRuntime (key);
                    if (runtime != nullptr && runtime->sink != nullptr)
                        markFormat (self, key, runtime->sink.get(), event->endpoint, event->id);
                    if (self->discoverySink != nullptr)
                        markFormat (self, key, self->discoverySink.get(), event->endpoint, event->id);
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
void MetroNetworkAudio::stop() { if (impl != nullptr) impl->stop(); }
bool MetroNetworkAudio::isRunning() const noexcept { return impl != nullptr && impl->running.load (std::memory_order_acquire); }

bool MetroNetworkAudio::connectToServer (const juce::String& host, int port, const juce::String& username, const juce::String& password)
{
    return impl != nullptr && impl->connectToServer (host, port, username, password);
}

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
