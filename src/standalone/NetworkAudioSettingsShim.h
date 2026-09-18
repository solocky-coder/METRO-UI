#pragma once

#include "NetworkAudioSettingsComponent.h"
#include "AppleUsbShareStatusComponent.h"
#include "network/AppleUsbNetworkTransport.h"
#include <memory>
#include <tuple>
#include <vector>

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
        // Snapshot every child the base class constructor already created
        // (server/user/group fields, gain/pan, sources list, etc.) so the
        // whole SonoBus/AOO page can be shown or hidden as a unit when the
        // tab switcher below flips to the Apple USB page. Nothing added
        // after this point (createTrackButton, the tab buttons, the USB
        // page) is part of this snapshot.
        for (auto* child : getChildren())
            basePageChildren.push_back (child);

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
        for (auto* child : basePageChildren)
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

        // Tab switcher between the SonoBus/AOO page (the base class's own
        // content, captured above) and the Apple USB Internet Share page.
        // Previously usbStatusComponent was just added as an extra child
        // laid out over the right-hand side of the SonoBus content, so the
        // two pages visually overlapped instead of being separate views.
        tabSonoBusButton.setButtonText ("SonoBus / AOO");
        tabSonoBusButton.setClickingTogglesState (false);
        tabSonoBusButton.onClick = [this] { setUsbTabActive (false); };
        addAndMakeVisible (tabSonoBusButton);

        tabUsbButton.setButtonText ("Apple USB Share");
        tabUsbButton.setClickingTogglesState (false);
        tabUsbButton.onClick = [this] { setUsbTabActive (true); };
        addAndMakeVisible (tabUsbButton);

        // The complete iPhoneUsbShare surface is embedded here as a child of
        // Network Audio. It is not a second process and does not create a
        // separate top-level window. Owned via unique_ptr (not a leaked raw
        // `new`) so its juce::Timer-driven refresh() actually stops when this
        // settings view is destroyed - previously it leaked on every dialog
        // close, leaving an orphaned timer ticking in the background for the
        // rest of the process's life. Enough of those, still firing during
        // final app shutdown after JUCE's own teardown has begun, produced
        // the access-violation-at-null crash seen on exit.
        usbStatusComponent = std::make_unique<::AppleUsbShareStatusComponent> (appleUsbService());
        addAndMakeVisible (usbStatusComponent.get());

        setUsbTabActive (false);
    }

    ~MetroNetworkAudioSettingsSelector() override = default;

    void resized() override
    {
        ::NetworkAudioSettingsComponent::resized();
        createTrackButton.setBounds (getWidth() - 222, 12, 206, 30);

        // Keep DIRECT USB in its own header slot. The integrated + Create Audio Track
        // button occupies the far-right slot, so DIRECT USB must never share that
        // rectangle or be painted underneath it.
        getDirectUsbButton().setBounds (getWidth() - 372, 12, 140, 30);

        // Tab switcher sits in the header row, in the space between the
        // "NETWORK AUDIO" title (left-aligned, ~300px) and createTrackButton
        // (right-aligned) — neither of which the base layout uses further in.
        constexpr int kTabW = 150, kTabH = 28, kTabGap = 8, kTabX = 316;
        tabSonoBusButton.setBounds (kTabX, 12, kTabW, kTabH);
        tabUsbButton.setBounds (kTabX + kTabW + kTabGap, 12, kTabW, kTabH);

        if (showingUsbTab)
        {
            // Fill the exact same content region the base class's own cards
            // occupy (device/channel/connection/sources), rather than a
            // narrow strip squeezed to one side of it.
            auto full = channelPanelBounds;
            if (! devicePanelBounds.isEmpty())
                full = full.getUnion (devicePanelBounds);
            full = full.getUnion (connectionPanelBounds).getUnion (sourcesPanelBounds);
            usbStatusComponent->setBounds (full);
        }
    }

private:
    void setUsbTabActive (bool active)
    {
        showingUsbTab = active;

        for (auto* child : basePageChildren)
            child->setVisible (! active);
        usbStatusComponent->setVisible (active);

        auto style = [] (juce::TextButton& b, bool isActive)
        {
            b.setColour (juce::TextButton::buttonColourId,
                         isActive ? juce::Colour (0xff2d8fd6) : juce::Colour (0xff2a2a30));
            b.setColour (juce::TextButton::textColourOffId,
                         isActive ? juce::Colours::white : juce::Colours::lightgrey);
        };
        style (tabSonoBusButton, ! active);
        style (tabUsbButton, active);

        resized();
    }

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
                message += "

AOO discovery found " + juce::String (discoveredSources)
                         + " source(s), but none has a negotiated audio format yet."
                           "

This is the network-audio handshake stage; the source must report its channel count and sample rate before a track can be created.";

                for (const auto& source : sources)
                    message += "

" + (source.user.isNotEmpty() ? source.user : "Unknown source")
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
    juce::TextButton tabSonoBusButton, tabUsbButton;
    std::vector<juce::Component*> basePageChildren;
    bool showingUsbTab = false;
    ::MetroNetworkAudio* networkAudio = nullptr;
    std::unique_ptr<::AppleUsbShareStatusComponent> usbStatusComponent;
};
}
