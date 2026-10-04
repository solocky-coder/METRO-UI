#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "../network/RtpMidiSession.h"

// MidiInputCallback registered on every enabled MIDI input (next to the audio
// processor player and the MidiRouter). It copies each live incoming message into
// the shared RTP-MIDI session, which filters it (channel mask, realtime flag) and
// sends it to the connected iPad over the USB link.
//
// RtpMidiSession::sendMessage() is allocation-free and never touches the network
// on the calling thread, and it ignores everything unless a session is Connected,
// so registering this callback costs nothing while USB MIDI is off.
class RtpMidiInputForwarder final : public juce::MidiInputCallback
{
public:
    static RtpMidiInputForwarder& instance()
    {
        static RtpMidiInputForwarder forwarder;
        return forwarder;
    }

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& message) override
    {
        RtpMidiSession::shared().sendMessage (message);
    }

private:
    RtpMidiInputForwarder() = default;
};
