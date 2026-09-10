#pragma once

#include "NetworkAudioSettingsComponent.h"
#include <tuple>

namespace juce
{
class MetroNetworkAudioSettingsSelector : public ::NetworkAudioSettingsComponent
{
public:
    explicit MetroNetworkAudioSettingsSelector (juce::AudioDeviceManager& deviceManager,
                                                 ::MetroNetworkAudio* networkAudioToUse)
        : ::NetworkAudioSettingsComponent (deviceManager, networkAudioToUse),
          networkAudio (networkAudioToUse)
    {
        createTrackButton.setButtonText ("+ Create Audio Track");
        createTrackButton.setTooltip ("Create a METRO audio track from a SonoBus/AOO source and choose its channel");
        createTrackButton.onClick = [this] { showCreateNetworkTrackMenu(); };
        addAndMakeVisible (createTrackButton);
#if ! DYSEKT_HAS_AOO
        createTrackButton.setEnabled (false);
#endif
    }

    void resized() override
    {
        ::NetworkAudioSettingsComponent::resized();
        createTrackButton.setBounds (getWidth() - 222, 12, 206, 30);
    }

private:
    void showCreateNetworkTrackMenu()
    {
#if DYSEKT_HAS_AOO
        if (networkAudio == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::WarningIcon,
                "Create Audio Track",
                "Network audio is not available.",
                "OK",
                this);
            return;
        }

        const auto sources = networkAudio->getSources();
        juce::PopupMenu menu;
        int nextItemId = 1000;
        std::vector<std::tuple<int64_t, int32_t, int, juce::String, juce::String>> choices;
        int discoveredSources = 0;

        for (const auto& source : sources)
        {
            ++discoveredSources;
            if (! source.online || source.channels <= 0 || source.sampleRate <= 0.0)
                continue;

            const auto user = source.user.isNotEmpty() ? source.user : "Unknown source";
            const auto sourceLabel = user + " | source #" + juce::String (source.sourceId)
                                   + " | " + juce::String (source.channels) + " ch";

            juce::PopupMenu channels;
            const int sourceBaseId = nextItemId;
            for (int channel = 0; channel < source.channels; ++channel)
            {
                const int itemId = sourceBaseId + channel;
                channels.addItem (itemId, "Channel " + juce::String (channel + 1));
                choices.emplace_back (source.sourceKey, source.sourceId, channel,
                                      "source #" + juce::String (source.sourceId), user);
            }
            nextItemId += juce::jmax (source.channels, 1);
            menu.addSubMenu (sourceLabel, channels, true);
        }

        if (! menu.containsAnyActiveItems())
        {
            juce::String message = "No online network sources with available channels were found.";
            if (discoveredSources > 0)
            {
                message += "\n\nAOO discovery found " + juce::String (discoveredSources)
                         + " source(s), but none has a negotiated audio format yet."
                           "\n\nThis is the network-audio handshake stage; the source must report its channel count and sample rate before a track can be created.";

                for (const auto& source : sources)
                {
                    message += "\n\n" + (source.user.isNotEmpty() ? source.user : "Unknown source")
                             + " | source #" + juce::String (source.sourceId)
                             + " | " + juce::String (source.channels) + " ch"
                             + " | " + juce::String (source.sampleRate, 0) + " Hz"
                             + " | online=" + (source.online ? "yes" : "no");
                }
            }

            juce::AlertWindow::showMessageBoxAsync (
                juce::AlertWindow::InfoIcon,
                "Create Audio Track",
                message,
                "OK",
                this);
            return;
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&createTrackButton),
                            [choices = std::move (choices)] (int result)
        {
            if (result == 0)
                return;

            const int choiceIndex = result - 1000;
            if (! juce::isPositiveAndBelow (choiceIndex, (int) choices.size()))
                return;

            const auto& choice = choices[(size_t) choiceIndex];
            const int64_t sourceKey = std::get<0> (choice);
            const int32_t sourceId = std::get<1> (choice);
            const int sourceChannel = std::get<2> (choice);
            const auto sourceName = std::get<3> (choice);
            const auto userName = std::get<4> (choice);

            const int trackIndex = NetworkAudioProcessor::createActiveNetworkAudioTrack (
                sourceKey, sourceId, sourceChannel, sourceName, userName);

            if (trackIndex >= 0)
            {
                juce::AlertWindow::showMessageBoxAsync (
                    juce::AlertWindow::InfoIcon,
                    "Audio Track Created",
                    userName + " | " + sourceName + " — Channel " + juce::String (sourceChannel + 1)
                        + " is now a METRO audio track.",
                    "OK");
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync (
                    juce::AlertWindow::WarningIcon,
                    "Audio Track Not Created",
                    "The standalone audio processor is not available.",
                    "OK");
            }
        });
#else
        juce::ignoreUnused (this);
#endif
    }

    juce::TextButton createTrackButton;
    ::MetroNetworkAudio* networkAudio = nullptr;
};
}
