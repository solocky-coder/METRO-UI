#pragma once
#include <set>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"
#include "../PluginEditor.h"
#include "../sequencer/MidiClip.h"
#include "MidiRouter.h"
#include "NetworkAudioProcessor.h"
#include "NetworkAudioAutoTrack.h"
#include "NetworkAudioLabels.h"
#include "NetworkAudioSettingsShim.h"
#include "AudioDeviceSettingsStore.h"
#include "RtpMidiInputForwarder.h"
#include "../network/NetworkMidiManager.h"
#include "NetworkMidiSettings.h"

class MainWindow : public juce::DocumentWindow,
                   public juce::MenuBarModel,
                   private juce::ChangeListener,
                   private juce::Timer
{
public:
    static constexpr int kMenuH = 24;

    explicit MainWindow (const juce::String& appName)
        : DocumentWindow (appName, juce::Colour (0xFF000000), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);

        // Restores the configuration saved from Audio Settings (if any); falls back to the
        // default device when nothing is saved or the saved state is missing/corrupt.
        AudioDeviceSettingsStore::initialiseDeviceManager (deviceManager, 0, 2);
        deviceManager.addChangeListener (this);

        juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Standalone);
        processor = std::make_unique<NetworkAudioProcessor>();
        juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Undefined);

        auto* device = deviceManager.getCurrentAudioDevice();
        const double deviceSampleRate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
        const int deviceBlockSize = device != nullptr ? juce::jmax (1, device->getCurrentBufferSizeSamples()) : 512;
        processor->prepareToPlay (deviceSampleRate > 0.0 ? deviceSampleRate : 44100.0, deviceBlockSize);
        editor = std::make_unique<DysektEditor> (*processor);
        editor->setWindowMenuBar (this, kMenuH);

        player.setProcessor (processor.get());
        deviceManager.addAudioCallback (&player);
        midiRouter = std::make_unique<MidiRouter> (deviceManager);
        networkMidiManager = std::make_unique<NetworkMidiManager>();
        processor->sequencer.setNetworkMidiSink (&NetworkMidiManager::engineSink, networkMidiManager.get());
        // Network MIDI live input is routed by PluginProcessor::processBlock()
        // after the selected arranger track is resolved. Do not also install
        // the legacy standalone MidiInput callback forwarder here: JUCE feeds
        // the same input into processBlock(), so doing both sends every live
        // message twice to the selected RTP-MIDI device.
        RtpMidiInputForwarder::instance().setManager (nullptr);
        RtpMidiInputForwarder::instance().setEngine (nullptr);

        for (const auto& input : juce::MidiInput::getAvailableDevices())
        {
            deviceManager.setMidiInputDeviceEnabled (input.identifier, true);
            deviceManager.addMidiInputDeviceCallback (input.identifier, &player);
            deviceManager.addMidiInputDeviceCallback (input.identifier, midiRouter.get());
            deviceManager.addMidiInputDeviceCallback (input.identifier, &RtpMidiInputForwarder::instance());
            registeredMidiInputIds.add (input.identifier);
        }

        // USB MIDI (live MIDI -> iPad over the direct USB link): restore filters and, if it
        // was left on, start the session. It waits quietly until the USB link exists.
        NetworkMidiSettings::startFromSavedSettings (*networkMidiManager);

        networkAudio = std::make_unique<MetroNetworkAudio>();
        networkAudio->setSourceLabels (NetworkAudioLabels::load());
        networkAudio->start();
        processor->setNetworkAudio (networkAudio.get());
        // MIDI children of a network audio track start at that audio device's own IP.
        processor->sequencer.setNetworkAudioPeerResolver (
            [] (void* ctx, int32_t sourceId) -> juce::String
            {
                if (auto* na = static_cast<MetroNetworkAudio*> (ctx))
                    for (const auto& src : na->getSources())
                        if (src.sourceId == sourceId && src.peerAddress.isNotEmpty())
                            return src.peerAddress;
                return {};
            },
            networkAudio.get());
        autoTrack = std::make_unique<NetworkAudioAutoTrack> (networkAudio.get());

        setContentNonOwned (editor.get(), true);
        menuBar = std::make_unique<juce::MenuBarComponent> (this);
        setMenuBar (this, kMenuH);
        setSize (editor->getWidth(), editor->getHeight() + kMenuH);
        setVisible (true);
        centreWithSize (getWidth(), getHeight());
        setFullScreen (true);
        startTimerHz (5);
    }

    void resized() override
    {
        DocumentWindow::resized();
        if (editor != nullptr)
        {
            const auto contentArea = getLocalBounds().withTrimmedTop (
                (menuBar != nullptr) ? kMenuH : 0);
            editor->setBounds (contentArea);
        }
    }

    ~MainWindow() override
    {
        stopTimer();
        if (editor != nullptr)
            editor->setWindowMenuBar (nullptr, 0);
        setMenuBar (nullptr);
        deviceManager.removeAudioCallback (&player);

        for (const auto& id : registeredMidiInputIds)
        {
            deviceManager.removeMidiInputDeviceCallback (id, &player);
            deviceManager.removeMidiInputDeviceCallback (id, &RtpMidiInputForwarder::instance());
            if (midiRouter != nullptr)
                deviceManager.removeMidiInputDeviceCallback (id, midiRouter.get());
        }

        processor->sequencer.setNetworkMidiSink (nullptr, nullptr);
        processor->sequencer.setNetworkAudioPeerResolver (nullptr, nullptr);
        RtpMidiInputForwarder::instance().setEngine (nullptr);
        RtpMidiInputForwarder::instance().setManager (nullptr);
        networkMidiManager.reset();

        autoTrack.reset();

        if (networkAudio != nullptr)
            networkAudio->stop();

        deviceManager.removeChangeListener (this);
        player.setProcessor (nullptr);
    }

    juce::StringArray getMenuBarNames() override
    {
        return { "File", "Audio / MIDI", "Help" };
    }

    juce::PopupMenu getMenuForIndex (int menuIndex, const juce::String&) override
    {
        juce::PopupMenu menu;
        if (menuIndex == 0)
        {
            menu.addItem (1, "New Project");
            menu.addItem (2, "Open Project...");
            menu.addItem (3, "Save Project");
            menu.addItem (4, "Save Project As...");
            menu.addSeparator();
            menu.addItem (5, "Export MIDI Clip...");
            menu.addSeparator();
            menu.addItem (6, "Quit");
        }
        else if (menuIndex == 1)
        {
            menu.addItem (10, "Audio Settings...");
            menu.addItem (13, "Network Audio...");
            menu.addItem (14, "Network MIDI...");
            menu.addItem (11, "MIDI Settings...");
            menu.addSeparator();
            menu.addItem (12, "MIDI Routing...");
        }
        else
        {
            menu.addItem (20, "About DYSEKT-SF");
        }
        return menu;
    }

    void menuItemSelected (int itemId, int) override
    {
        switch (itemId)
        {
            case 1:  newProject(); break;
            case 2:  openProject(); break;
            case 3:  saveProject(); break;
            case 4:  saveProjectAs(); break;
            case 5:  exportMidiClip(); break;
            case 6:  juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            case 10: showAudioSettings(); break;
            case 13: showNetworkAudioSettings(); break;
            case 14: showNetworkMidiSettings(); break;
            case 11: showMidiSettings(); break;
            case 12: showMidiRouting(); break;
            case 20: showAbout(); break;
            default: break;
        }
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    void newProject()
    {
        juce::AlertWindow::showOkCancelBox (
            juce::AlertWindow::QuestionIcon, "New Project",
            "Discard current project and start fresh?", "New", "Cancel", nullptr,
            juce::ModalCallbackFunction::create ([this] (int result)
            {
                if (result == 1)
                {
                    juce::MemoryBlock blank;
                    processor->setStateInformation (blank.getData(), (int) blank.getSize());
                    currentProjectFile = juce::File();
                    setName ("DYSEKT-SF");
                }
            }));
    }

    void openProject()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Open DYSEKT-SF Project",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.dysekt");
        fileChooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result == juce::File()) return;
                juce::FileInputStream fis (result);
                if (! fis.openedOk()) return;
                const int64_t size = fis.getTotalLength();
                juce::MemoryBlock block ((size_t) size);
                fis.read (block.getData(), (int) size);
                processor->setStateInformation (block.getData(), (int) block.getSize());
                currentProjectFile = result;
                setName ("DYSEKT-SF  —  " + result.getFileNameWithoutExtension());
            });
    }

    void saveProject()
    {
        if (currentProjectFile == juce::File()) { saveProjectAs(); return; }
        juce::MemoryBlock state;
        processor->getStateInformation (state);
        juce::FileOutputStream fos (currentProjectFile);
        if (fos.openedOk())
        {
            fos.setPosition (0);
            fos.truncate();
            fos.write (state.getData(), state.getSize());
        }
    }

    void saveProjectAs()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Save DYSEKT-SF Project",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile ("Untitled.dysekt"), "*.dysekt");
        fileChooser->launchAsync (
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result == juce::File()) return;
                currentProjectFile = result.withFileExtension ("dysekt");
                saveProject();
                setName ("DYSEKT-SF  —  " + currentProjectFile.getFileNameWithoutExtension());
            });
    }

    void exportMidiClip()
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Export MIDI Clip",
            juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                .getChildFile ("DYSEKT_clip.mid"), "*.mid");
        fileChooser->launchAsync (
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result == juce::File()) return;
                juce::MidiFile midiFile;
                midiFile.setTicksPerQuarterNote ((int) MidiClip::kPPQ);
                juce::MidiMessageSequence track;
                const MidiClip& clip = processor->sequencer.getClip();
                const juce::ScopedReadLock sl (clip.getLock());
                for (const auto& n : clip.getNotes())
                {
                    track.addEvent (juce::MidiMessage::noteOn (1, n.note, (juce::uint8) n.velocity), (double) n.startTick);
                    track.addEvent (juce::MidiMessage::noteOff (1, n.note), (double) n.endTick());
                }
                track.sort();
                midiFile.addTrack (track);
                auto dest = result.withFileExtension ("mid");
                juce::FileOutputStream fos (dest);
                if (fos.openedOk()) midiFile.writeTo (fos);
            });
    }

    // Plain output/input device picker: just JUCE's built-in AudioDeviceSelectorComponent,
    // Metro-styled. Network Audio used to be folded into this same dialog; it's now its own
    // menu item/dialog (see showNetworkAudioSettings()) so this one only handles the device
    // itself.
    class AudioOnlySettingsComponent : public juce::Component
    {
    public:
        explicit AudioOnlySettingsComponent (juce::AudioDeviceManager& dm)
            : deviceManager (dm),
              audioSelector (dm, 0, 0, 1, 2, false, false, false, false)
        {
            // Same reasoning as NetworkAudioSettingsComponent: this is launched as its own
            // top-level DialogWindow, so it never inherits METRO's app-wide LookAndFeel.
            setLookAndFeel (&settingsLookAndFeel);
            addAndMakeVisible (audioSelector);

            // Device changes apply live as soon as they are made, but are only written to
            // disk when Save is pressed, so unsaved changes never replace the last saved
            // configuration (they simply don't survive a restart).
            saveButton.setButtonText ("SAVE");
            saveButton.setTooltip ("Save these audio settings and restore them the next time the app starts");
            saveButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            saveButton.onClick = [this] { saveSettings(); };
            addAndMakeVisible (saveButton);

            statusLabel.setJustificationType (juce::Justification::centredLeft);
            statusLabel.setFont (juce::Font (13.0f));
            statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xFFB0B0BC));
            statusLabel.setText ("Changes apply immediately. Press Save to keep them for the next launch.",
                                 juce::dontSendNotification);
            addAndMakeVisible (statusLabel);

            setSize (700, 320 + kFooterH);
        }

        ~AudioOnlySettingsComponent() override { setLookAndFeel (nullptr); }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xFF0D0D14)); }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16);
            auto footer = area.removeFromBottom (kFooterH - 16);
            area.removeFromBottom (8);

            audioSelector.setBounds (area);

            saveButton.setBounds (footer.removeFromRight (110).withSizeKeepingCentre (110, 32));
            footer.removeFromRight (12);
            statusLabel.setBounds (footer);
        }

    private:
        static constexpr int kFooterH = 56;

        void saveSettings()
        {
            const auto result = AudioDeviceSettingsStore::save (deviceManager);
            const bool ok = result.wasOk();

            statusLabel.setColour (juce::Label::textColourId,
                                   ok ? juce::Colour (0xFF7FE0A0) : juce::Colour (0xFFE0A07F));
            statusLabel.setText (ok ? "Saved. These settings will be restored on the next launch."
                                    : result.getErrorMessage(),
                                 juce::dontSendNotification);
        }

        juce::AudioDeviceManager& deviceManager;
        MetroLookAndFeel settingsLookAndFeel;
        juce::AudioDeviceSelectorComponent audioSelector;
        juce::TextButton saveButton;
        juce::Label statusLabel;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioOnlySettingsComponent)
    };

    // Shared by showAudioSettings() and showNetworkAudioSettings(): comp sizes itself to a
    // fixed ideal size in its own constructor, which can be taller than some laptop screens,
    // and these dialogs are otherwise non-resizable — so rather than risk the window being
    // silently clipped with no scrollbar or resize handle to reach the rest of it, put comp
    // inside a Viewport and clamp the *window's* size to the actual screen. If the full size
    // fits, this is invisible (no visible scrollbars); if it doesn't, the dialog scrolls
    // instead of clipping. Takes ownership of comp via the viewport.
    void launchScrollableSettingsDialog (juce::Component* comp, const juce::String& title, bool allowResize = true)
    {
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent (comp, true); // viewport now owns and deletes comp

        const auto idealSize = comp->getBounds();
        const auto displayArea = juce::Desktop::getInstance()
                                      .getDisplays()
                                      .getDisplayForRect (getScreenBounds())
                                      ->userArea;
        constexpr int kScreenMargin = 80; // room for the OS title bar, taskbar/dock, etc.
        const int maxW = displayArea.getWidth()  - kScreenMargin;
        const int maxH = displayArea.getHeight() - kScreenMargin;

        // Only show a scrollbar on an axis that genuinely doesn't fit on screen. When a vertical
        // bar is needed, widen the viewport by its thickness so it doesn't squeeze the content
        // and spuriously trigger a horizontal bar (and vice versa).
        const int sb = viewport->getScrollBarThickness();
        const bool needsV = idealSize.getHeight() > maxH;
        const bool needsH = idealSize.getWidth() + (needsV ? sb : 0) > maxW;
        const int viewportWidth  = juce::jmin (idealSize.getWidth()  + (needsV ? sb : 0), maxW);
        const int viewportHeight = juce::jmin (idealSize.getHeight() + (needsH ? sb : 0), maxH);
        viewport->setScrollBarsShown (needsV, needsH);
        viewport->setSize (viewportWidth, viewportHeight);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (viewport);
        opts.dialogTitle = title;
        opts.dialogBackgroundColour = juce::Colour (0xFF0D0D14);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = allowResize; // Network Audio passes false: fixed-size window
        opts.launchAsync();
    }

    void showAudioSettings()
    {
        launchScrollableSettingsDialog (new AudioOnlySettingsComponent (deviceManager), "Audio Settings");
    }

    void showNetworkAudioSettings()
    {
        auto* comp = new juce::MetroNetworkAudioSettingsSelector (deviceManager, networkAudio.get(), false, networkMidiManager.get());
        launchScrollableSettingsDialog (comp, "Network Audio", false);
    }

    void showNetworkMidiSettings()
    {
        if (networkMidiManager == nullptr)
            return;

        auto* panel = new NetworkMidiPanel (*networkMidiManager, 1);
        panel->setSize (860, 700);
        launchScrollableSettingsDialog (panel, "Network MIDI", true);
    }

    class MidiOnlySettingsComponent : public juce::Component
    {
    public:
        explicit MidiOnlySettingsComponent (juce::AudioDeviceManager& dm,
                                             juce::AudioProcessorPlayer& pl,
                                             juce::StringArray& registeredIds)
            : deviceManager (dm), player (pl), registeredMidiInputIds (registeredIds)
        {
            setSize (480, 320);
            titleLabel.setText ("MIDI Input Devices", juce::dontSendNotification);
            titleLabel.setFont (juce::Font (16.0f, juce::Font::bold));
            titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible (titleLabel);
            refreshDeviceList();
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xFF0D0D14)); }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16);
            titleLabel.setBounds (area.removeFromTop (28));
            area.removeFromTop (8);
            for (auto* row : rows) row->setBounds (area.removeFromTop (28));
        }

    private:
        void refreshDeviceList()
        {
            rows.clear();
            const auto devices = juce::MidiInput::getAvailableDevices();
            if (devices.isEmpty())
            {
                auto* lbl = new juce::Label();
                lbl->setText ("No MIDI input devices found.", juce::dontSendNotification);
                lbl->setColour (juce::Label::textColourId, juce::Colours::grey);
                addAndMakeVisible (lbl);
                rows.add (lbl);
            }
            else
            {
                for (const auto& dev : devices)
                {
                    auto* btn = new juce::ToggleButton (dev.name);
                    btn->setToggleState (deviceManager.isMidiInputDeviceEnabled (dev.identifier), juce::dontSendNotification);
                    btn->setColour (juce::ToggleButton::textColourId, juce::Colours::white);
                    const juce::String id = dev.identifier;
                    btn->onClick = [this, id, btn]
                    {
                        const bool enable = btn->getToggleState();
                        if (enable)
                        {
                            deviceManager.setMidiInputDeviceEnabled (id, true);
                            if (! registeredMidiInputIds.contains (id))
                            {
                                deviceManager.addMidiInputDeviceCallback (id, &player);
                                deviceManager.addMidiInputDeviceCallback (id, &RtpMidiInputForwarder::instance());
                                registeredMidiInputIds.add (id);
                            }
                        }
                        else
                        {
                            deviceManager.removeMidiInputDeviceCallback (id, &player);
                            deviceManager.removeMidiInputDeviceCallback (id, &RtpMidiInputForwarder::instance());
                            registeredMidiInputIds.removeString (id);
                            deviceManager.setMidiInputDeviceEnabled (id, false);
                        }
                    };
                    addAndMakeVisible (btn);
                    rows.add (btn);
                }
            }
            setSize (480, juce::jmax (120, 28 + 8 + (int) rows.size() * 28 + 16));
        }

        juce::AudioDeviceManager& deviceManager;
        juce::AudioProcessorPlayer& player;
        juce::StringArray& registeredMidiInputIds;
        juce::Label titleLabel;
        juce::OwnedArray<juce::Component> rows;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiOnlySettingsComponent)
    };

    void showMidiSettings()
    {
        auto* comp = new MidiOnlySettingsComponent (deviceManager, player, registeredMidiInputIds);
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (comp);
        opts.dialogTitle = "MIDI Settings";
        opts.dialogBackgroundColour = juce::Colour (0xFF0D0D14);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = false;
        opts.launchAsync();
    }

    void showMidiRouting()
    {
        if (midiRouter == nullptr) return;
        juce::StringArray trackNames;
        for (int i = 0; i < processor->sequencer.getNumTracks(); ++i)
            trackNames.add (processor->sequencer.getTrackInfo(i).name);
        auto* dlg = new MidiRoutingDialog (*midiRouter, trackNames);
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (dlg);
        opts.dialogTitle = "MIDI Routing";
        opts.dialogBackgroundColour = juce::Colour (0xFF0D0D14);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = true;
        opts.launchAsync();
    }

    void showAbout()
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::AlertWindow::InfoIcon, "DYSEKT-SF Standalone",
            "DYSEKT-SF Sampler + Sequencer\nVersion 1.0\n\nPowered by JUCE.");
    }

    void timerCallback() override
    {
        if (processor == nullptr || networkMidiManager == nullptr)
            return;

        const auto& seq = processor->sequencer;
        networkMidiManager->setSelectedDevice (seq.getSelectedNetworkMidiDevice());

        // Children of a network audio track carry their own device id and peer
        // (there is no separate device track), so configure each device id once
        // from whichever of its tracks comes first.
        std::set<int> configuredDevices;
        for (int i = 0; i < seq.getNumTracks(); ++i)
        {
            const auto info = seq.getTrackInfo (i);
            if (info.type != TrackType::NetworkMidi || info.networkMidiPeer.isEmpty()
                || ! configuredDevices.insert (info.networkMidiDeviceId).second)
                continue;

            networkMidiManager->configureDevice (info.networkMidiDeviceId, info.networkMidiPeer, info.name);
            if (networkMidiManager->getDeviceInfo (info.networkMidiDeviceId).state == RtpMidiSession::State::Off)
                networkMidiManager->startDevice (info.networkMidiDeviceId, 5004);
        }

        for (int id = 1; id <= NetworkMidiManager::kMaxDevices; ++id)
        {
            const auto state = networkMidiManager->getDeviceInfo (id).state;
            seq.setNetworkMidiLinkState (id, (int) state);
        }
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (midiRouter != nullptr) midiRouter->refresh();
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorPlayer player;
    juce::StringArray registeredMidiInputIds;
    std::unique_ptr<MidiRouter> midiRouter;
    std::unique_ptr<MetroNetworkAudio> networkAudio;
    std::unique_ptr<NetworkMidiManager> networkMidiManager;
    std::unique_ptr<NetworkAudioAutoTrack> autoTrack;
    std::unique_ptr<NetworkAudioProcessor> processor;
    std::unique_ptr<DysektEditor> editor;
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File currentProjectFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
