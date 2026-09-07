#include "MetroNetworkAudio.h"

#include <juce_events/juce_events.h>

#include <aoo/aoo.hpp>
#include <aoo/aoo_net.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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
 #include <cerrno>
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

std::mutex aooLifetimeMutex;
int aooLifetimeUsers = 0;
std::atomic<MetroSocket*> activeAooSocket { nullptr };

void closeSocket (MetroSocket socket)
{
    if (socket == metroInvalidSocket)
        return;
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
    if (aooLifetimeUsers++ == 0)
        aoo_initialize();
}

void stopAooLifetime()
{
    std::lock_guard<std::mutex> lock (aooLifetimeMutex);
    if (aooLifetimeUsers > 0 && --aooLifetimeUsers == 0)
        aoo_terminate();
}

int32_t sendUdp (void* user, const char* data, int32_t numBytes, void* address)
{
    auto* socket = static_cast<MetroSocket*> (user);
    if (socket == nullptr || *socket == metroInvalidSocket || address == nullptr)
        return 0;
    const auto result = sendto (*socket, data, numBytes, 0,
                                static_cast<const sockaddr*> (address), sizeof (sockaddr_in));
    return result == numBytes ? 1 : 0;
}

int32_t sendAooReply (void* endpoint, const char* data, int32_t numBytes)
{
    auto* socket = activeAooSocket.load();
    if (socket == nullptr || *socket == metroInvalidSocket || endpoint == nullptr)
        return 0;
    const auto result = sendto (*socket, data, numBytes, 0,
                                static_cast<const sockaddr*> (endpoint), sizeof (sockaddr_in));
    return result == numBytes ? 1 : 0;
}
}

class MetroNetworkAudio::Impl
{
public:
    MetroNetworkAudio& owner;
    std::atomic<bool> running { false };
    MetroSocket socket = metroInvalidSocket;
    aoo::net::iclient::pointer client;
    aoo::isink::pointer sink;
    std::thread ioThread;
    std::thread clientThread;

    mutable std::mutex stateMutex;
    std::vector<SourceInfo> sources;
    MetroNetworkAudio::SourceListener listener;
    std::vector<std::shared_ptr<sockaddr_in>> peerEndpoints;

    std::array<float, kAooChannels * kAooBlockSize> audioScratch {};
    std::array<aoo_sample*, kAooChannels> audioPointers {};

    std::atomic<bool> connected { false };
    std::atomic<bool> joined { false };
    juce::String group;

    explicit Impl (MetroNetworkAudio& ownerIn) : owner (ownerIn) {}
    ~Impl() { stop(); }

    bool start()
    {
        if (running.load()) return true;
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
        if (::bind (socket, reinterpret_cast<const sockaddr*> (&local), sizeof (local)) != 0
            || ! setNonBlocking (socket))
            return cleanupFailedStart();

        socklen_t localLength = sizeof (local);
        if (getsockname (socket, reinterpret_cast<sockaddr*> (&local), &localLength) != 0)
            return cleanupFailedStart();

        client.reset (aoo::net::iclient::create (&socket, sendUdp, ntohs (local.sin_port)));
        sink.reset (aoo::isink::create (0));
        if (client == nullptr || sink == nullptr) return cleanupFailedStart();
        if (sink->setup (kAooSampleRate, kAooBlockSize, kAooChannels) <= 0)
            return cleanupFailedStart();

        sink->set_buffersize (kAooBufferMs);
        sink->set_dynamic_resampling (1);
        sink->set_resend_limit (5);
        sink->set_resend_interval (10);
        sink->set_resend_maxnumframes (16);

        activeAooSocket.store (&socket);
        running.store (true);
        clientThread = std::thread ([this] { client->run(); });
        ioThread = std::thread ([this] { ioLoop(); });
        return true;
    }

    bool cleanupFailedStart()
    {
        activeAooSocket.store (nullptr);
        client.reset();
        sink.reset();
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
        if (! running.exchange (false)) return;
        connected.store (false);
        joined.store (false);
        activeAooSocket.store (nullptr);
        if (client != nullptr) client->quit();
        if (ioThread.joinable()) ioThread.join();
        if (clientThread.joinable()) clientThread.join();
        client.reset();
        sink.reset();
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

    bool connectToServer (const juce::String& host, int port,
                          const juce::String& username, const juce::String& password)
    {
        if (! running.load() || client == nullptr) return false;
        return client->connect (host.toRawUTF8(), port,
                                username.toRawUTF8(), password.toRawUTF8()) > 0;
    }

    bool joinGroup (const juce::String& groupName, const juce::String& password, bool isPublic)
    {
        if (! running.load() || client == nullptr) return false;
        group = groupName;
        return client->group_join (groupName.toRawUTF8(), password.toRawUTF8(), isPublic) > 0;
    }

    void leaveGroup (const juce::String& groupName)
    {
        if (client != nullptr) client->group_leave (groupName.toRawUTF8());
        joined.store (false);
    }

    void disconnect()
    {
        if (client != nullptr) client->disconnect();
        connected.store (false);
        joined.store (false);
    }

    void ioLoop()
    {
        std::array<char, AOO_MAXPACKETSIZE> packet {};
        while (running.load())
        {
            fd_set readSet;
            FD_ZERO (&readSet);
            FD_SET (socket, &readSet);
            timeval timeout {};
            timeout.tv_sec = 0;
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
                    sink->handle_message (packet.data(), n, &from, sendAooReply);
                }
            }

            client->send();
            sink->send();
            if (client->events_available() > 0)
                client->handle_events (clientEventHandler, this);
            if (sink->events_available() > 0)
                sink->handle_events (sinkEventHandler, this);
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
                    self->connected.store (true);
                    break;
                case AOONET_CLIENT_GROUP_JOIN_EVENT:
                    self->joined.store (true);
                    break;
                case AOONET_CLIENT_PEER_JOIN_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoonet_client_peer_event*> (events[i]);
                    if (event->result > 0 && event->address != nullptr && self->sink != nullptr)
                    {
                        auto endpoint = std::make_shared<sockaddr_in>();
                        std::memcpy (endpoint.get(), event->address, sizeof (sockaddr_in));
                        {
                            std::lock_guard<std::mutex> lock (self->stateMutex);
                            self->peerEndpoints.push_back (endpoint);
                        }
                        self->sink->invite_source (endpoint.get(), AOO_ID_WILDCARD, sendAooReply);
                    }
                    break;
                }
                case AOONET_CLIENT_DISCONNECT_EVENT:
                    self->connected.store (false);
                    self->joined.store (false);
                    break;
                default:
                    break;
            }
        }
        return 1;
    }

    static int32_t sinkEventHandler (void* user, const aoo_event** events, int32_t count)
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
                    SourceInfo info;
                    info.sourceId = event->id;
                    info.group = self->group;
                    info.online = true;
                    self->upsertSource (info);
                    changed = true;
                    break;
                }
                case AOO_SOURCE_FORMAT_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_source_event*> (events[i]);
                    aoo_format_storage format {};
                    if (self->sink->get_source_format (event->endpoint, event->id, format) > 0)
                    {
                        std::lock_guard<std::mutex> lock (self->stateMutex);
                        for (auto& source : self->sources)
                            if (source.sourceId == event->id)
                            {
                                source.channels = format.header.nchannels;
                                source.sampleRate = format.header.samplerate;
                                source.online = true;
                            }
                        changed = true;
                    }
                    break;
                }
                case AOO_BLOCK_LOST_EVENT:
                {
                    const auto* event = reinterpret_cast<const aoo_block_lost_event*> (events[i]);
                    std::lock_guard<std::mutex> lock (self->stateMutex);
                    for (auto& source : self->sources)
                        if (source.sourceId == event->id)
                            source.packetLoss = std::min (1.0f, source.packetLoss + 0.001f * (float) event->count);
                    changed = true;
                    break;
                }
                default:
                    break;
            }
        }
        if (changed)
        {
            MetroNetworkAudio::SourceListener listener;
            {
                std::lock_guard<std::mutex> lock (self->stateMutex);
                listener = self->listener;
            }
            if (listener)
                juce::MessageManager::callAsync (std::move (listener));
        }
        return 1;
    }

    void upsertSource (const SourceInfo& info)
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        auto it = std::find_if (sources.begin(), sources.end(),
                                [&info] (const auto& source) { return source.sourceId == info.sourceId; });
        if (it == sources.end()) sources.push_back (info);
        else it->online = true;
    }

    void process (juce::AudioBuffer<float>& destination, int numSamples)
    {
        destination.clear();
        if (! running.load() || sink == nullptr || numSamples <= 0)
            return;

        int offset = 0;
        while (offset < numSamples)
        {
            const int block = std::min (kAooBlockSize, numSamples - offset);
            for (int channel = 0; channel < kAooChannels; ++channel)
            {
                audioPointers[(size_t) channel] = audioScratch.data() + (size_t) channel * kAooBlockSize;
                std::fill (audioPointers[(size_t) channel], audioPointers[(size_t) channel] + block, 0.0f);
            }

            if (sink->process (audioPointers.data(), block, aoo_osctime_get()) > 0)
            {
                const int channels = std::min (destination.getNumChannels(), kAooChannels);
                for (int channel = 0; channel < channels; ++channel)
                    destination.addFrom (channel, offset, audioPointers[(size_t) channel], block);
            }
            offset += block;
        }
    }
};

MetroNetworkAudio::MetroNetworkAudio()
    : impl (std::make_unique<Impl> (*this)) {}

MetroNetworkAudio::~MetroNetworkAudio() { stop(); }

bool MetroNetworkAudio::start() { return impl != nullptr && impl->start(); }

void MetroNetworkAudio::stop() { if (impl != nullptr) impl->stop(); }

bool MetroNetworkAudio::isRunning() const noexcept
{
    return impl != nullptr && impl->running.load();
}

bool MetroNetworkAudio::connectToServer (const juce::String& host, int port,
                                         const juce::String& username,
                                         const juce::String& password)
{
    return impl != nullptr && impl->connectToServer (host, port, username, password);
}

bool MetroNetworkAudio::joinGroup (const juce::String& group,
                                   const juce::String& password, bool isPublic)
{
    return impl != nullptr && impl->joinGroup (group, password, isPublic);
}

void MetroNetworkAudio::leaveGroup (const juce::String& group)
{
    if (impl != nullptr) impl->leaveGroup (group);
}

void MetroNetworkAudio::disconnect()
{
    if (impl != nullptr) impl->disconnect();
}

void MetroNetworkAudio::process (juce::AudioBuffer<float>& destination,
                                 int numSamples, double sampleRate)
{
    juce::ignoreUnused (sampleRate);
    if (impl != nullptr)
        impl->process (destination, numSamples);
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
