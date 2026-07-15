#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../MidiLearnManager.h"
#include "DysektLookAndFeel.h"

class DysektProcessor;

class MidiLearnDialog : public juce::Component, private juce::ListBoxModel
{
public:
    MidiLearnDialog (MidiLearnManager& ml,
                     DysektProcessor&  proc,
                     std::function<void()> onClose = nullptr);

    void paint   (juce::Graphics&) override;
    void resized () override;

    // ListBoxModel
    int             getNumRows () override;
    void            paintListBoxItem (int, juce::Graphics&, int, int, bool) override;
    juce::Component* refreshComponentForRow (int, bool, juce::Component*) override;

private:
    MidiLearnManager& midiLearn;
    DysektProcessor&  processor;
    std::function<void()> onCloseCallback;

    juce::ListBox    mappingList { "Midi Mapping List", this };
    juce::TextButton saveButton  { "Save .dlm" };
    juce::TextButton loadButton  { "Load .dlm" };
    juce::TextButton closeButton { "Close" };

    void saveToFile();
    void loadFromFile();
    void close();

    std::unique_ptr<juce::FileChooser> fileChooser;

    // ── Per-row component ─────────────────────────────────────────────────────
    struct MappingRowComponent : public juce::Component, private juce::Timer
    {
        juce::Label      paramLabel;
        juce::ComboBox   channelCombo, ccCombo, modeCombo;
        juce::TextButton armButton   { "LEARN" };
        juce::TextButton clearButton { "X" };
        juce::TextButton flipButton  { "FLIP" };
        int              fieldId  { -1 };
        MidiLearnManager* midiLearn { nullptr };

        MappingRowComponent();

        void update (int field, MidiLearnManager& ml, const juce::String& name);
        void resized() override;
        void timerCallback() override;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MappingRowComponent)
    };

    void encoderModeChanged (int fieldId, MidiLearnManager::EncoderMode mode);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLearnDialog)
};
