#pragma once

#include "NetworkAudioSettingsComponent.h"
#include "AppleUsbShareStatusComponent.h"
#include "network/AppleUsbNetworkTransport.h"
#include <tuple>

namespace juce
{
class MetroNetworkAudioSettingsSelector : public ::NetworkAudioSettingsComponent
{
public:
    explicit MetroNetworkAudioSettingsSelector (juce::AudioDeviceManager& deviceManager,
                                                 ::MetroNetworkAudio* networkAudioToUse,
                                                 bool showDeviceSelector = false)
        : ::NetworkAudioSettingsComponent (deviceManager, networkAudioToUse, showDeviceSelector),
          networkAudio (networkAudioToUse)
    {
        createTrackButton.setButtonText ("+ Create Audio Track");
        createTrackButton.setTooltip ("Create a METRO audio track from a SonoBus/AOO source and choose its channel");
        createTrackButton.onClick = [this] { showCreateNetworkTrackMenu(); };
        addAndMakeVisible (createTrackButton);

#if ! DYSEKT_HAS_AOO
        createTrackButton.setEnabled (false);
#endif

        // The base component owns the visible service ON/OFF button. DYSEKT's
        // integrated Apple USB/NCM service is controlled by the same switch.
        // The switch is deliberately initialized OFF every time this settings
        // view is created; opening the page must never start USB setup.
        for (auto* child : getChildren())
        {
            if (auto* button = dynamic_cast<juce::TextButton*> (child))
            {
                if (! button->getButtonText().startsWithIgnoreCase ("Network audio:"))
                    continue;

                button->setToggleState (false, juce::dontSendNotification);
                button->setButtonText ("Network audio: OFF");
                getNetworkAudioChannelState().enabled.store (false, std::memory_order_relaxed);

                auto baseClick = button->onClick;
                button->onClick = [button, baseClick]
                {
                    auto& usbService = appleUsbService();
                    const bool requestedOn = button->getToggleState();

                    if (requestedOn)
                    {
                        const bool usbReady = usbService.start ({});
                        button->setTooltip (usbReady
                            ? "Network audio ON — Apple USB/NCM transport ready"
                            : "Network audio ON — Apple USB/NCM setup failed: " + usbService.status());
                    }
                    else
                    {
                        usbService.stop();
                        button->setTooltip ("Network audio OFF");
                    }

                    if (baseClick != nullptr)
                        baseClick();

                    if (! button->getToggleState())
                        usbService.stop();
                };
                break;
            }
        }

        // The complete iPhoneUsbShare surface is embedded here as a child of
        // Network Audio. It is not a second process and does not create a
        // separate top-level window.
        usbStatusComponent = new ::AppleUsbShareStatusComponent (appleUsbService());
        addAndMakeVisible (usbStatusComponent);
        resized();
    }

    ~MetroNetworkAudioSettingsSelector() override = default;

    void resized() override
    {
        ::NetworkAudioSettingsComponent::resized();
        createTrackButton.setBounds (getWidth() - 222, 12, 206, 30);

        // The embedded app surface is intentionally large enough to preserve
        // the reference application's title, device card, connection metrics,
        // activity area and Start/Stop/Diagnostics controls.
        const int x = juce::jmax (390, getWidth() - 570);
        usbStatusComponent->setBounds (x, 44, getWidth() - x - 16, 420);
    }

private:
    static ::AppleUsbNetworkTransport& appleUsbService()
    {
        static ::AppleUsbNetworkTransport service;
        return service;
    }

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
                    message += "\n\n" + (source.user.isNotEmpty() ? source.user : "Unknown source")
                             + " | source #" + juce::String (source.sourceId)
                             + " | " + juce::String (source.channels) + " ch"
                             + " | " + juce::String (source.sampleRate, 0) + " Hz"
                             + " | online=" + (source.online ? "yes" : "no");
            }

            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon, "Create Audio Track", message, "OK");
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

            juce::AlertWindow::showMessageBoxAsync (
                trackIndex >= 0 ? juce::AlertWindow::InfoIcon : juce::AlertWindow::WarningIcon,
                trackIndex >= 0 ? "Audio Track Created" : "Audio Track Not Created",
                trackIndex >= 0
                    ? userName + " | " + sourceName + " — Channel " + juce::String (sourceChannel + 1) + " is now a METRO audio track."
                    : "The standalone audio processor is not available.",
                "OK");
        });
#else
        juce::ignoreUnused (this);
#endif
    }

    juce::TextButton createTrackButton;
    ::MetroNetworkAudio* networkAudio = nullptr;
    ::AppleUsbShareStatusComponent* usbStatusComponent = nullptr;
};
}
