#pragma once

#include "NetworkAudioSettingsComponent.h"
#include "AppleUsbShareStatusComponent.h"
#include "NetworkMidiPanel.h"
#include "../network/NetworkMidiManager.h"
#include "network/AppleUsbNetworkTransport.h"
#include "NetworkAudioAutoTrack.h"
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
                                                 bool showDeviceSelector = false,
                                                 NetworkMidiManager* networkMidiManagerToUse = nullptr)
        : ::NetworkAudioSettingsComponent (deviceManager, networkAudioToUse, showDeviceSelector),
          networkAudio (networkAudioToUse),
          networkMidiManager (networkMidiManagerToUse)
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

        autoTrackToggle.setButtonText ("Auto-create");
        autoTrackToggle.setTooltip ("When a USB audio source finishes connecting, automatically create a METRO audio track for it "
                                    "(stereo if the source has two or more channels). Applies while USB AUDIO is on.");
        autoTrackToggle.setToggleState (NetworkAudioAutoTrack::isEnabled(), juce::dontSendNotification);
        autoTrackToggle.onClick = [this] { NetworkAudioAutoTrack::setEnabled (autoTrackToggle.getToggleState()); };
        addAndMakeVisible (autoTrackToggle);

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

                // Do not reset the master switch when this settings view is
                // reopened. The base component now reflects the live AOO backend
                // state, so an already-running stream remains visibly ON.
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
        tabSonoBusButton.onClick = [this] { setActivePage (0); };
        addAndMakeVisible (tabSonoBusButton);

        tabUsbButton.setButtonText ("Apple USB Share");
        tabUsbButton.setClickingTogglesState (false);
        tabUsbButton.onClick = [this] { setActivePage (1); };
        addAndMakeVisible (tabUsbButton);

        // Third page: send DYSEKT's live incoming MIDI to the iPad over the same
        // direct USB link (RTP-MIDI). The panel only controls/reflects the shared
        // RtpMidiSession, so reopening this dialog never interrupts a running session.
        tabMidiButton.setButtonText ("USB MIDI");
        tabMidiButton.setClickingTogglesState (false);
        tabMidiButton.onClick = [this] { setActivePage (2); };
        addAndMakeVisible (tabMidiButton);

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

        if (networkMidiManager != nullptr)
            midiPanel = std::make_unique<::NetworkMidiPanel> (*networkMidiManager, 1);
        addAndMakeVisible (midiPanel.get());

        setActivePage (0);
    }

    ~MetroNetworkAudioSettingsSelector() override = default;

    void paint (juce::Graphics& g) override
    {
        if (! showingUsbTab && ! showingMidiTab)
        {
            ::NetworkAudioSettingsComponent::paint (g);
            return;
        }

        g.fillAll (juce::Colour (0xff0d0d14));
        juce::Component* activePage = showingMidiTab ? static_cast<juce::Component*> (midiPanel.get())
                                                      : static_cast<juce::Component*> (usbStatusComponent.get());
        if (activePage != nullptr)
        {
            const auto panel = activePage->getBounds().toFloat();
            g.setColour (juce::Colour (0xff15151c));
            g.fillRoundedRectangle (panel, 8.0f);
            g.setColour (juce::Colour (0xff2a2a34));
            g.drawRoundedRectangle (panel, 8.0f, 1.0f);
        }
    }

    void resized() override
    {
        ::NetworkAudioSettingsComponent::resized();
        createTrackButton.setBounds (getWidth() - 222, 12, 206, 30);
        autoTrackToggle.setBounds (getWidth() - 222 - 8 - 120, 12, 120, 30);

        // The USB AUDIO button is laid out by the base class, inside the
        // Network Audio Channel card next to the "Network audio" switch. It is
        // intentionally not repositioned here.

        // Tab switcher sits in the header row, between the "NETWORK AUDIO" title
        // (left-aligned) and the Auto-create toggle / createTrackButton (right-aligned).
        // The three tabs share whatever width is left, so they never overlap those.
        constexpr int kTabH = 28, kTabGap = 6, kTabX = 230;
        const int tabAreaRight = autoTrackToggle.getX() - 8;
        const int tabW = juce::jlimit (84, 140, (tabAreaRight - kTabX - 2 * kTabGap) / 3);
        tabSonoBusButton.setBounds (kTabX, 12, tabW, kTabH);
        tabUsbButton.setBounds (kTabX + tabW + kTabGap, 12, tabW, kTabH);
        tabMidiButton.setBounds (kTabX + 2 * (tabW + kTabGap), 12, tabW, kTabH);

        if (showingUsbTab || showingMidiTab)
        {
            // Fill the exact same content region the base class's own cards
            // occupy (device/channel/connection/sources), rather than a
            // narrow strip squeezed to one side of it.
            auto full = channelPanelBounds;
            if (! devicePanelBounds.isEmpty())
                full = full.getUnion (devicePanelBounds);
            full = full.getUnion (connectionPanelBounds).getUnion (sourcesPanelBounds);
            usbStatusComponent->setBounds (full);
            midiPanel->setBounds (full);
        }
    }

private:
    // page: 0 = SonoBus / AOO, 1 = Apple USB Share, 2 = USB MIDI
    void setActivePage (int page)
    {
        showingUsbTab  = (page == 1);
        showingMidiTab = (page == 2);
        const bool showBase = (page == 0);

        for (auto* child : basePageChildren)
            child->setVisible (showBase);

        // USB AUDIO (the AOO direct-audio toggle) belongs to the SonoBus / AOO
        // page: it requires "Network audio: ON" (an AOO-page control), it
        // starts the AOO backend on the direct port, and the "SonoBus connect
        // info" prompt it produces lives on that page too. Showing it on the
        // Apple USB Share tab made it read as that tab's link status. That
        // tab already has its own Start / Stop / Diagnostics for the link.
        getDirectUsbButton().setVisible (showBase);
        usbStatusComponent->setVisible (showingUsbTab);
        midiPanel->setVisible (showingMidiTab);

        auto style = [] (juce::TextButton& b, bool isActive)
        {
            b.setColour (juce::TextButton::buttonColourId,
                         isActive ? juce::Colour (0xff2d8fd6) : juce::Colour (0xff2a2a30));
            b.setColour (juce::TextButton::textColourOffId,
                         isActive ? juce::Colours::white : juce::Colours::lightgrey);
            // DysektLookAndFeel ignores buttonColourId for normal buttons; a
            // toggled button is what it fills with the theme accent, so use
            // the toggle state to make the active tab visibly highlighted.
            b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            b.setToggleState (isActive, juce::dontSendNotification);
        };
        style (tabSonoBusButton, page == 0);
        style (tabUsbButton, page == 1);
        style (tabMidiButton, page == 2);

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
            int itemId = nextItemId;

            // A source with two or more channels is offered first as one
            // stereo track; the single-channel entries below stay available
            // for splitting a feed or picking one side.
            if (source.channels >= 2)
            {
                channels.addItem (itemId++, "Stereo (Channel 1+2)");
                choices.emplace_back (source.sourceKey, source.sourceId, MetroNetworkAudio::kStereoPair,
                                      "source #" + juce::String (source.sourceId), user);
                channels.addSeparator();
            }

            for (int channel = 0; channel < source.channels; ++channel)
            {
                channels.addItem (itemId++, "Channel " + juce::String (channel + 1));
                choices.emplace_back (source.sourceKey, source.sourceId, channel,
                                      "source #" + juce::String (source.sourceId), user);
            }
            nextItemId = itemId;
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
                    ? userName + " | " + sourceName + " — " + (MetroNetworkAudio::isStereoRoute (sourceChannel) ? juce::String ("Stereo (Channel 1+2)") : "Channel " + juce::String (sourceChannel + 1)) + " is now a METRO audio track."
                    : "The standalone audio processor is not available.",
                "OK");
        });
#else
        juce::ignoreUnused (this);
#endif
    }

    juce::TextButton createTrackButton;
    juce::ToggleButton autoTrackToggle;
    juce::TextButton tabSonoBusButton, tabUsbButton, tabMidiButton;
    std::vector<juce::Component*> basePageChildren;
    bool showingUsbTab = false;
    bool showingMidiTab = false;
    ::MetroNetworkAudio* networkAudio = nullptr;
    std::unique_ptr<::AppleUsbShareStatusComponent> usbStatusComponent;
    std::unique_ptr<::NetworkMidiPanel> midiPanel;
};
}
