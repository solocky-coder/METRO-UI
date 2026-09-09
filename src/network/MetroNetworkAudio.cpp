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
    if (socket == nullptr || *socket == metroInvalidSocket || endpoint == nullptr) return 0;
    const auto result = sendto (*socket, data, numBytes, 0,
                                static_cast<const sockaddr*> (endpoint), sizeof (sockaddr_in));
    return result == numBytes ? 1 : 0;
}

int64_t makeSourceKey (const sockaddr_in* endpoint, int32_t sourceId) noexcept
{
    // FNV-1a over endpoint address/port and raw AOO source ID. This is an
    // adapter handle, not a cryptographic identifier; its purpose is to keep
    // equal AOO IDs from different peers distinct inside METRO.
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
    {
        mix (static_cast<uint64_t> (endpoint->sin_addr.s_addr));
        mix (static_cast<uint64_t> (endpoint->sin_port));
    }
    mix (static_cast<uint32_t> (sourceId));
    hash &= 0x7fffffffffffffffull;
    return hash == 0 ? 1 : static_cast<int64_t> (hash);
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
    std::vector<std::shared_ptr<sockaddr_in>> peerEndpoints;
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

    SourceRuntime* createRuntime (int32_t sourceId, const sockaddr_in* endpoint)
    {
        if (sourceId == 0 || endpoint == nullptr) return nullptr;
        const auto sourceKey = makeSourceKey (endpoint, sourceId);
        if (auto* existing = findRuntime (sourceKey)) return existing;

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
        for (auto& slot : runtimeSlots)
        {
            SourceRuntime* expected = nullptr;
            if (slot.compare_exchange_strong (expected, raw, std::memory_order_release, std::memory_order_relaxed))
            {
                runtimes.push_back (std::move (runtime));
                raw->sink->invite_source (raw->endpoint.get(), sourceId, sendAooReply);
                return raw;
            }
        }
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
            peerEndpoints.clear();
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
                    client->handle_message (packet.data(), n, &from);
                    if (discoverySink != nullptr) discoverySink->handle_message (packet.data(), n, &from, sendAooReply);
                    for (auto& runtime : runtimes)
                        if (runtime != nullptr && runtime->sink != nullptr)
                            runtime->sink->handle_message (packet.data(), n, &from, sendAooReply);
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
                    {
                        std::lock_guard<std::mutex> lock (self->stateMutex);
                        self->peerEndpoints.push_back (endpoint);
                    }
                    if (self->discoverySink != nullptr)
                        self->discoverySink->invite_source (endpoint.get(), AOO_ID_WILDCARD, sendAooReply);
                    break;
                }
                case AOONET_CLIENT_DISCONNECT_EVENT:
                    self->connected.store (false, std::memory_order_release);
                    self->joined.store (false, std::memory_order_release);
                    break;
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

    static void markFormat (Impl* self, int64_t key, aoo::isink* sink, const void* endpoint, int32_t sourceId)
    {
        if (sink == nullptr || endpoint == nullptr) return;
        aoo_format_storage format {};
        if (sink->get_source_format (endpoint, sourceId, format) <= 0) return;
        std::lock_guard<std::mutex> lock (self->stateMutex);
        for (auto& source : self->sources)
            if (source.sourceKey == key)
            {
                source.channels = format.header.nchannels;
                source.sampleRate = format.header.samplerate;
                source.online = true;
                break;
            }
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
                    const auto* endpoint = static_cast<const sockaddr_in*> (event->endpoint);
                    SourceInfo info;
                    info.sourceKey = makeSourceKey (endpoint, event->id);
                    info.sourceId = event->id;
                    info.group = self->group;
                    info.online = true;
                    self->upsertSource (info);
                    self->createRuntime (event->id, endpoint);
                    changed = true;
                    break;
                }
                case AOO_SOURCE_FORMAT_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_source_event*> (events[i]);
                    if (event->endpoint == nullptr) break;
                    const auto key = makeSourceKey (static_cast<const sockaddr_in*> (event->endpoint), event->id);
                    self->createRuntime (event->id, static_cast<const sockaddr_in*> (event->endpoint));
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
                    if (event->endpoint == nullptr) break;
                    const auto key = makeSourceKey (static_cast<const sockaddr_in*> (event->endpoint), event->id);
                    auto* runtime = self->findRuntime (key);
                    if (runtime == nullptr || runtime->sink == nullptr) break;
                    markFormat (self, key, runtime->sink.get(), event->endpoint, event->id);
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
            if (info.group.isNotEmpty()) it->group = info.group;
            it->online = true;
        }
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
    std::lock_guard<std::mutex> lock (impl->stateMutex);
    return impl->sources;
}

void MetroNetworkAudio::setSourceListener (SourceListener listener)
{
    if (impl == nullptr) return;
    std::lock_guard<std::mutex> lock (impl->stateMutex);
    impl->listener = std::move (listener);
}
