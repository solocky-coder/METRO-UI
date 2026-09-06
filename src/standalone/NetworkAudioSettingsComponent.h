#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../network/MetroNetworkAudio.h"

class NetworkAudioSettingsComponent : public juce::Component,
                                       private juce::Timer
{
public:
    NetworkAudioSettingsComponent (juce::AudioDeviceManager& deviceManager,
                                    MetroNetworkAudio& networkAudio);
    ~NetworkAudioSettingsComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void connectClicked();
    void disconnectClicked();
    void refreshSources();
    void updateStatus (const juce::String& text);

    juce::AudioDeviceSelectorComponent audioSelector;
    MetroNetworkAudio& networkAudio;

    juce::Label networkTitle;
    juce::Label transportLabel;
    juce::ToggleButton enableButton;
    juce::Label serverLabel;
    juce::TextEditor serverEditor;
    juce::Label portLabel;
    juce::TextEditor portEditor;
    juce::Label userLabel;
    juce::TextEditor userEditor;
    juce::Label groupLabel;
    juce::TextEditor groupEditor;
    juce::Label passwordLabel;
    juce::TextEditor passwordEditor;
    juce::ToggleButton publicGroupButton;
    juce::TextButton connectButton { "Connect / Join" };
    juce::TextButton disconnectButton { "Disconnect" };
    juce::Label statusLabel;
    juce::Label sourcesLabel;
    juce::ListBox sourceList;

    class SourceListModel : public juce::ListBoxModel
    {
    public:
        explicit SourceListModel (MetroNetworkAudio& owner) : owner (owner) {}
        int getNumRows() override;
        void paintListBoxItem (int rowNumber, juce::Graphics&, int width, int height, bool rowIsSelected) override;
    private:
        MetroNetworkAudio& owner;
    };

    SourceListModel sourceModel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioSettingsComponent)
};
