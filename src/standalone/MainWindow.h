#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "../PluginProcessor.h"
#include "../PluginEditor.h"
#include "../sequencer/MidiClip.h"
#include "MidiRouter.h"
#include "NetworkAudioProcessor.h"
#include "NetworkAudioSettingsShim.h"

class MainWindow : public juce::DocumentWindow,
                   public juce::MenuBarModel,
                   private juce::ChangeListener
{
public:
    static constexpr int kMenuH = 24;

    explicit MainWindow (const juce::String& appName)
        : DocumentWindow (appName, juce::Colour (0xFF000000), DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);

        deviceManager.initialise (0, 2, nullptr, true, {}, nullptr);
        deviceManager.addChangeListener (this);

        juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Standalone);
        processor = std::make_unique<NetworkAudioProcessor>();
        juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Undefined);

        auto* device = deviceManager.getCurrentAudioDevice();
        const double deviceSampleRate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
        const int deviceBlockSize = device != nullptr ? juce::jmax (1, device->getCurrentBufferSizeSamples()) : 512;
        processor->prepareToPlay (deviceSampleRate > 0.0 ? deviceSampleRate : 44100.0, deviceBlockSize);
        editor = std::make_unique<DysektEditor> (*processor);

        player.setProcessor (processor.get());
        deviceManager.addAudioCallback (&player);
        midiRouter = std::make_unique<MidiRouter> (deviceManager);

        for (const auto& input : juce::MidiInput::getAvailableDevices())
        {
            deviceManager.setMidiInputDeviceEnabled (input.identifier, true);
            deviceManager.addMidiInputDeviceCallback (input.identifier, &player);
            deviceManager.addMidiInputDeviceCallback (input.identifier, midiRouter.get());
            registeredMidiInputIds.add (input.identifier);
        }

        networkAudio = std::make_unique<MetroNetworkAudio>();
        networkAudio->start();
        processor->setNetworkAudio (networkAudio.get());

        setContentNonOwned (editor.get(), true);
        menuBar = std::make_unique<juce::MenuBarComponent> (this);
        setMenuBar (this, kMenuH);
        setSize (editor->getWidth(), editor->getHeight() + kMenuH);
        setVisible (true);
        centreWithSize (getWidth(), getHeight());
        setFullScreen (true);
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
        setMenuBar (nullptr);
        deviceManager.removeAudioCallback (&player);

        for (const auto& id : registeredMidiInputIds)
        {
            deviceManager.removeMidiInputDeviceCallback (id, &player);
            if (midiRouter != nullptr)
                deviceManager.removeMidiInputDeviceCallback (id, midiRouter.get());
        }

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

    void showAudioSettings()
    {
        auto* comp = new juce::MetroNetworkAudioSettingsSelector (deviceManager, networkAudio.get());

        // comp sizes itself to a fixed ideal size in its own constructor (currently 980x1092
        // to fit every card comfortably). That can be taller than some laptop screens, and
        // this dialog is otherwise non-resizable — so rather than risk the window being
        // silently clipped with no scrollbar or resize handle to reach the rest of it, put
        // comp inside a Viewport and clamp the *window's* size to the actual screen. If the
        // full size fits, this is invisible (no visible scrollbars); if it doesn't, the
        // dialog scrolls instead of clipping.
        auto* viewport = new juce::Viewport();
        viewport->setViewedComponent (comp, true); // viewport now owns and deletes comp

        const auto idealSize = comp->getBounds();
        const auto displayArea = juce::Desktop::getInstance()
                                      .getDisplays()
                                      .getDisplayForRect (getScreenBounds())
                                      ->userArea;
        constexpr int kScreenMargin = 80; // room for the OS title bar, taskbar/dock, etc.
        const int viewportWidth = juce::jmin (idealSize.getWidth(), displayArea.getWidth() - kScreenMargin);
        const int viewportHeight = juce::jmin (idealSize.getHeight(), displayArea.getHeight() - kScreenMargin);
        viewport->setSize (viewportWidth, viewportHeight);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (viewport);
        opts.dialogTitle = "Audio Settings";
        opts.dialogBackgroundColour = juce::Colour (0xFF0D0D14);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = true; // belt-and-braces, in case someone still wants more room
        opts.launchAsync();
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
                                registeredMidiInputIds.add (id);
                            }
                        }
                        else
                        {
                            deviceManager.removeMidiInputDeviceCallback (id, &player);
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

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (midiRouter != nullptr) midiRouter->refresh();
    }

    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorPlayer player;
    juce::StringArray registeredMidiInputIds;
    std::unique_ptr<MidiRouter> midiRouter;
    std::unique_ptr<MetroNetworkAudio> networkAudio;
    std::unique_ptr<NetworkAudioProcessor> processor;
    std::unique_ptr<DysektEditor> editor;
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::File currentProjectFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};
