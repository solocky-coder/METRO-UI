#pragma once

#include "NetworkAudioProcessor.h"
#include "../network/MetroNetworkAudio.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <set>

// Creates a METRO audio track for each Direct-USB AOO source as soon as its
// audio format has been negotiated, so the user does not have to open the
// Network Audio dialog and pick "+ Create Audio Track" by hand.
//
// Lives for the whole app session (owned by MainWindow), not the settings
// dialog, so it works with the dialog closed. Runs on the message thread.
//
//  - Two or more channels -> one stereo track (channels 1+2); one channel ->
//    mono. Other channels remain available from the manual menu.
//  - Only in Direct USB mode; group/server sessions are never auto-populated.
//  - A source that already has a track (for example from a loaded project)
//    is left alone, and a track the user deletes is not recreated until the
//    source disconnects and reconnects.
class NetworkAudioAutoTrack final : private juce::Timer
{
public:
    explicit NetworkAudioAutoTrack (MetroNetworkAudio* audioToWatch) : audio (audioToWatch)
    {
        startTimer (250);
    }

    ~NetworkAudioAutoTrack() override { stopTimer(); }

    // Session-wide switch (default on). Toggled from the Network Audio dialog.
    static bool isEnabled() noexcept { return enabledFlag().load (std::memory_order_relaxed); }
    static void setEnabled (bool shouldBeEnabled) noexcept { enabledFlag().store (shouldBeEnabled, std::memory_order_relaxed); }

private:
    static std::atomic<bool>& enabledFlag()
    {
        static std::atomic<bool> flag { true };
        return flag;
    }

    void timerCallback() override
    {
#if DYSEKT_HAS_AOO
        if (audio == nullptr || ! audio->isRunning() || ! audio->isDirectMode())
        {
            handled.clear();
            return;
        }

        std::set<int64_t> live;
        for (const auto& source : audio->getSources())
        {
            if (! source.online || source.channels <= 0 || source.sampleRate <= 0.0)
                continue;

            live.insert (source.sourceKey);

            if (! isEnabled() || handled.count (source.sourceKey) != 0)
                continue;

            if (NetworkAudioProcessor::hasActiveNetworkAudioTrack (source.sourceKey))
            {
                handled.insert (source.sourceKey);
                continue;
            }

            const int channel = source.channels >= 2 ? MetroNetworkAudio::kStereoPair : 0;
            const auto user = source.user.isNotEmpty() ? source.user : juce::String ("Unknown source");
            const int trackIndex = NetworkAudioProcessor::createActiveNetworkAudioTrack (
                source.sourceKey, source.sourceId, channel,
                "source #" + juce::String (source.sourceId), user);

            // Retry on the next tick if the processor was not ready yet.
            if (trackIndex >= 0)
                handled.insert (source.sourceKey);
        }

        // Forget sources that went away so a reconnect can create a track again.
        for (auto it = handled.begin(); it != handled.end();)
            it = live.count (*it) != 0 ? std::next (it) : handled.erase (it);
#endif
    }

    MetroNetworkAudio* audio = nullptr;
    std::set<int64_t> handled;
};
