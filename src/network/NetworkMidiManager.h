#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "../network/RtpMidiSession.h"
#include <array>
#include <atomic>

class NetworkMidiManager final
{
private:
    struct Slot
    {
        RtpMidiSession session;
        juce::String peer;
        juce::String name;
        int port = 5004;
        bool configured = false;
    };

public:
    static constexpr int kMaxDevices = 16;

    struct DeviceInfo
    {
        int id = 0;
        bool configured = false;
        juce::String name;
        juce::String peer;
        RtpMidiSession::State state = RtpMidiSession::State::Off;
        juce::String status;
        uint32_t messagesSent = 0;
    };

    NetworkMidiManager() = default;
    ~NetworkMidiManager() { stopAll(); }

    NetworkMidiManager (const NetworkMidiManager&) = delete;
    NetworkMidiManager& operator= (const NetworkMidiManager&) = delete;

    int configureDevice (int id, const juce::String& peer, const juce::String& name = {})
    {
        if (! juce::isPositiveAndBelow (id, kMaxDevices + 1))
            return 0;

        auto& slot = devices[(size_t) (id - 1)];
        const auto newPeer = peer.trim();
        const bool peerChanged = slot.peer != newPeer;
        slot.peer = newPeer;
        slot.name = name.trim().isNotEmpty() ? name.trim() : ("Network MIDI " + juce::String (id));
        slot.configured = RtpMidiSession::isValidIPv4 (slot.peer);

        if (peerChanged && slot.session.isRunning())
            slot.session.stop();
        if (! slot.configured)
            slot.session.stop();

        return id;
    }

    void clearDevice (int id)
    {
        if (! juce::isPositiveAndBelow (id, kMaxDevices + 1))
            return;

        auto& slot = devices[(size_t) (id - 1)];
        slot.session.stop();
        slot.configured = false;
        slot.peer.clear();
        slot.name.clear();

        if (selectedDevice.load (std::memory_order_acquire) == id)
            selectedDevice.store (0, std::memory_order_release);
    }

    RtpMidiSession* getSession (int id) noexcept
    {
        return juce::isPositiveAndBelow (id, kMaxDevices + 1)
             ? &devices[(size_t) (id - 1)].session : nullptr;
    }

    const RtpMidiSession* getSession (int id) const noexcept
    {
        return juce::isPositiveAndBelow (id, kMaxDevices + 1)
             ? &devices[(size_t) (id - 1)].session : nullptr;
    }

    void setChannelMask (int id, uint32_t mask) noexcept
    {
        if (auto* s = getSession (id))
            s->setChannelMask (mask);
    }

    uint32_t getChannelMask (int id) const noexcept
    {
        if (const auto* s = getSession (id))
            return s->getChannelMask();
        return 0xFFFFu;
    }

    void setForwardRealtime (int id, bool on) noexcept
    {
        if (auto* s = getSession (id))
            s->setForwardRealtime (on);
    }

    bool getForwardRealtime (int id) const noexcept
    {
        if (const auto* s = getSession (id))
            return s->getForwardRealtime();
        return false;
    }

    void startDevice (int id, int port = 5004)
    {
        if (! juce::isPositiveAndBelow (id, kMaxDevices + 1))
            return;

        auto& slot = devices[(size_t) (id - 1)];
        if (! slot.configured)
            return;

        slot.port = juce::jlimit (1, 65534, port);
        slot.session.start (slot.peer, slot.port);
    }

    void stopDevice (int id)
    {
        if (auto* s = getSession (id))
            s->stop();
    }

    void stopAll()
    {
        for (auto& slot : devices)
            slot.session.stop();
    }

    void setSelectedDevice (int id) noexcept
    {
        selectedDevice.store (juce::isPositiveAndBelow (id, kMaxDevices + 1) ? id : 0,
                              std::memory_order_release);
    }

    int getSelectedDevice() const noexcept
    {
        return selectedDevice.load (std::memory_order_acquire);
    }

    static void engineSink (void* context, int deviceId, const juce::MidiMessage& message) noexcept
    {
        if (auto* self = static_cast<NetworkMidiManager*> (context))
            self->send (deviceId, message);
    }

    void send (int deviceId, const juce::MidiMessage& message) noexcept
    {
        if (auto* session = getSession (deviceId))
            session->sendMessage (message);
    }

    void sendSelected (const juce::MidiMessage& message) noexcept
    {
        send (getSelectedDevice(), message);
    }

    DeviceInfo getDeviceInfo (int id) const
    {
        DeviceInfo out;
        out.id = id;
        if (! juce::isPositiveAndBelow (id, kMaxDevices + 1))
            return out;

        const auto& slot = devices[(size_t) (id - 1)];
        out.configured = slot.configured;
        out.name = slot.name;
        out.peer = slot.peer;
        out.state = slot.session.getState();
        out.status = slot.session.getStatusText();
        out.messagesSent = slot.session.getMessagesSent();
        return out;
    }

    int findDeviceByPeer (const juce::String& peer) const
    {
        for (int i = 1; i <= kMaxDevices; ++i)
            if (devices[(size_t) (i - 1)].configured
                && devices[(size_t) (i - 1)].peer == peer.trim())
                return i;
        return 0;
    }

    int ensureDevice (const juce::String& peer, const juce::String& name = {})
    {
        const auto normalized = peer.trim();
        if (const int existing = findDeviceByPeer (normalized))
            return existing;

        for (int i = 1; i <= kMaxDevices; ++i)
            if (! devices[(size_t) (i - 1)].configured)
                return configureDevice (i, normalized, name);

        return 0;
    }

private:
    std::array<Slot, kMaxDevices> devices;
    std::atomic<int> selectedDevice { 0 };
};
