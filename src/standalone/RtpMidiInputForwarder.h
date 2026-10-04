#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "../network/NetworkMidiManager.h"
#include "../sequencer/SequencerEngine.h"

// MidiInputCallback registered on every enabled MIDI input. It forwards live MIDI
// only to the Network MIDI arranger device currently selected by SequencerEngine.
class RtpMidiInputForwarder final : public juce::MidiInputCallback
{
public:
    static RtpMidiInputForwarder& instance()
    {
        static RtpMidiInputForwarder forwarder;
        return forwarder;
    }

    void setManager (NetworkMidiManager* value) noexcept
    {
        manager.store (value, std::memory_order_release);
    }

    void setEngine (SequencerEngine* value) noexcept
    {
        engine.store (value, std::memory_order_release);
    }

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override
    {
        auto* m = manager.load (std::memory_order_acquire);
        auto* e = engine.load (std::memory_order_acquire);
        if (m != nullptr && e != nullptr)
        {
            const int device = e->getSelectedNetworkMidiDevice();
            if (device != 0)
                m->send (device, message);
        }
    }

private:
    RtpMidiInputForwarder() = default;
    std::atomic<NetworkMidiManager*> manager { nullptr };
    std::atomic<SequencerEngine*> engine { nullptr };
};