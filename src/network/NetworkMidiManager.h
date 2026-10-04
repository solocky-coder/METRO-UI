#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include "../network/RtpMidiSession.h"
#include <array>
#include <atomic>
#include <memory>

class NetworkMidiManager final
{
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
        if (slot == nullptr)
            slot = std::make_unique<Slot>();

        const auto newPeer = peer.trim();
        const bool peerChanged = slot->peer != newPeer;
        slot->peer = newPeer;
        slot->name = name.trim().isNotEmpty() ? name.trim() : ("Network MIDI " + juce::String (id));
        slot->configured = RtpMidiSession::isValidIPv4 (slot->peer);
        if (peerChanged && slot->session.isRunning())
            slot->session.stop();
        if (! slot->configured)
            slot->session.stop();
        return id;
    }

    void clearDevice (int id)
    {
        if (! juce::isPositiveAndBelow (id, kMaxDevices + 1)) return;
        if (auto* s = devices[(size_t) (id - 1)].get())
            s->session.stop();
        devices[(size_t) (id - 1)].reset();
        if (selectedDevice.load() == id)
            selectedDevice.store (0);
    }

    RtpMidiSession* getSession (int id) noexcept { return getSlot (id) != nullptr ? &getSlot (id)->session : nullptr; }
    const RtpMidiSession* getSession (int id) const noexcept { return getSlotConst (id) != nullptr ? &getSlotConst (id)->session : nullptr; }

    void setChannelMask (int id, uint32_t mask) noexcept { if (auto* x = getSlot (id)) x->session.setChannelMask (mask); }
    uint32_t getChannelMask (int id) const noexcept { if (const auto* x = getSlotConst (id)) return x->session.getChannelMask(); return 0xFFFFu; }
    void setForwardRealtime (int id, bool on) noexcept { if (auto* x = getSlot (id)) x->session.setForwardRealtime (on); }
    bool getForwardRealtime (int id) const noexcept { if (const auto* x = getSlotConst (id)) return x->session.getForwardRealtime(); return false; }

    void startDevice (int id, int port = 5004)
    {
        auto* s = getSlot (id);
        if (s == nullptr || ! s->configured) return;
        s->port = juce::jlimit (1, 65534, port);
        s->session.start (s->peer, s->port);
    }

    void stopDevice (int id)
    {
        if (auto* s = getSlot (id))
            s->session.stop();
    }

    void stopAll()
    {
        for (auto& slot : devices)
            if (slot != nullptr)
                slot->session.stop();
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
        if (auto* s = getSlot (deviceId))
            s->session.sendMessage (message);
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

        if (const auto* s = devices[(size_t) (id - 1)].get())
        {
            out.configured = s->configured;
            out.name = s->name;
            out.peer = s->peer;
            out.state = s->session.getState();
            out.status = s->session.getStatusText();
            out.messagesSent = s->session.getMessagesSent();
        }
        return out;
    }

    int findDeviceByPeer (const juce::String& peer) const
    {
        for (int i = 1; i <= kMaxDevices; ++i)
            if (const auto* s = devices[(size_t) (i - 1)].get())
                if (s->peer == peer.trim())
                    return i;
        return 0;
    }

    int ensureDevice (const juce::String& peer, const juce::String& name = {})
    {
        const auto normalized = peer.trim();
        const int existing = findDeviceByPeer (normalized);
        if (existing != 0) return existing;

        for (int i = 1; i <= kMaxDevices; ++i)
            if (devices[(size_t) (i - 1)] == nullptr)
                return configureDevice (i, normalized, name);

        return 0;
    }

private:
    struct Slot
    {
        RtpMidiSession session;
        juce::String peer;
        juce::String name;
        int port = 5004;
        bool configured = false;
    };

    const Slot* getSlotConst (int id) const noexcept
    {
        if (! juce::isPositiveAndBelow (id, kMaxDevices + 1)) return nullptr;
        return devices[(size_t) (id - 1)].get();
    }

    Slot* getSlot (int id) noexcept
    {
        if (! juce::isPositiveAndBelow (id, kMaxDevices + 1)) return nullptr;
        return devices[(size_t) (id - 1)].get();
    }

    std::array<std::unique_ptr<Slot>, kMaxDevices> devices;
    std::atomic<int> selectedDevice { 0 };
};
