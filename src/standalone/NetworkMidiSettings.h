#pragma once

#include <juce_core/juce_core.h>
#include "../network/RtpMidiSession.h"

// Persists the "USB MIDI" (RTP-MIDI over the direct USB link) settings next to the
// other standalone settings (see NetworkAudioLabels.h): device address, port,
// forwarded channels, clock forwarding and whether the feature was switched on.
//
// The session itself is a process-wide object (RtpMidiSession::shared()), so the
// Network Audio panel can be opened and closed freely without touching a running
// session - the panel just reflects it.
namespace NetworkMidiSettings
{
    struct Values
    {
        juce::String peer = "192.168.99.2";   // first Apple USB slot's device address
        int          port = 5004;             // device's network-MIDI control port
        uint32_t     channelMask = 0xFFFFu;   // bit n = MIDI channel n+1 forwarded
        bool         forwardRealtime = false; // clock / start / continue / stop
        bool         enabled = false;
    };

    inline juce::File file()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("DYSEKT-SF")
                   .getChildFile ("usb-midi.json");
    }

    inline Values load()
    {
        Values v;
        const auto f = file();
        if (! f.existsAsFile())
            return v;

        const auto parsed = juce::JSON::parse (f);
        if (auto* object = parsed.getDynamicObject())
        {
            const auto peer = object->getProperty ("peer").toString().trim();
            if (RtpMidiSession::isValidIPv4 (peer))
                v.peer = peer;

            v.port            = juce::jlimit (1, 65534, (int) object->getProperty ("port"));
            v.channelMask     = (uint32_t) (int) object->getProperty ("channelMask") & 0xFFFFu;
            v.forwardRealtime = (bool) object->getProperty ("forwardRealtime");
            v.enabled         = (bool) object->getProperty ("enabled");

            if (! object->hasProperty ("port"))        v.port = 5004;
            if (! object->hasProperty ("channelMask")) v.channelMask = 0xFFFFu;
        }

        return v;
    }

    inline void save (const Values& v)
    {
        auto* object = new juce::DynamicObject();
        object->setProperty ("peer", v.peer);
        object->setProperty ("port", v.port);
        object->setProperty ("channelMask", (int) v.channelMask);
        object->setProperty ("forwardRealtime", v.forwardRealtime);
        object->setProperty ("enabled", v.enabled);

        const auto f = file();
        f.getParentDirectory().createDirectory();
        f.replaceWithText (juce::JSON::toString (juce::var (object)));
    }

    /** Pushes the filter settings into the shared session (does not start it). */
    inline void applyFilters (const Values& v)
    {
        auto& session = RtpMidiSession::shared();
        session.setChannelMask (v.channelMask);
        session.setForwardRealtime (v.forwardRealtime);
    }

    /** Called once at app start: restores filters and, if it was left on, restarts the
        session. A started session just waits quietly until the USB link exists. */
    inline void startFromSavedSettings()
    {
        const auto v = load();
        applyFilters (v);

        if (v.enabled && RtpMidiSession::isValidIPv4 (v.peer))
            RtpMidiSession::shared().start (v.peer, v.port);
    }
}
