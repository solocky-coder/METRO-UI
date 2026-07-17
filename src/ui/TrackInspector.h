#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  TrackInspector — compact, selected-track control surface docked to the left
//  of ArrangeView's TrackHeaderStrip.
//
//  Routing stays intentionally simple: one source, one instrument destination.
//  Only SoundFont tracks expose a MIDI part/channel selector, since they alone
//  are multi-timbral in the current instrument model — Slice and Chromatic
//  tracks are always pinned to their fixed engine channel.
//==============================================================================
class TrackInspector : public juce::Component,
                       private juce::Timer
{
public:
    explicit TrackInspector (SequencerEngine& sequencer) : engine (sequencer)
    {
        configureButton (muteButton,    "M", juce::Colour (0xffc99140));
        configureButton (soloButton,    "S", juce::Colour (0xffd1b34c));
        configureButton (recordButton,  "R", juce::Colour (0xffd95454));
        configureButton (monitorButton, "I", juce::Colour (0xff41b8a2));

        inputBox.addItem ("All MIDI Inputs", 1);
        inputBox.setSelectedId (1, juce::dontSendNotification);
        inputBox.setEnabled (false);

        outputBox.setEnabled (false);

        channelBox.setVisible (false);
        for (int channel = 1; channel <= 16; ++channel)
            channelBox.addItem ("Part " + juce::String (channel), channel);

        volumeSlider.setSliderStyle (juce::Slider::LinearBar);
        volumeSlider.setRange (-60.0, 6.0, 0.1);
        volumeSlider.setValue (0.0, juce::dontSendNotification);
        volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 18);
        volumeSlider.setTextValueSuffix (" dB");

        panSlider.setSliderStyle (juce::Slider::LinearBar);
        panSlider.setRange (-100.0, 100.0, 1.0);
        panSlider.setValue (0.0, juce::dontSendNotification);
        panSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 18);
        panSlider.textFromValueFunction = [] (double value)
        {
            if (std::abs (value) < 0.5) return juce::String ("C");
            return value < 0.0 ? "L" + juce::String ((int) -value) : "R" + juce::String ((int) value);
        };

        automationBox.addItem ("Read",  1);
        automationBox.addItem ("Write", 2);
        automationBox.addItem ("Touch", 3);
        automationBox.addItem ("Latch", 4);
        automationBox.setSelectedId (1, juce::dontSendNotification);

        for (auto* control : { static_cast<juce::Component*> (&inputBox),
                                static_cast<juce::Component*> (&outputBox),
                                static_cast<juce::Component*> (&channelBox),
                                static_cast<juce::Component*> (&volumeSlider),
                                static_cast<juce::Component*> (&panSlider),
                                static_cast<juce::Component*> (&automationBox) })
            addAndMakeVisible (*control);

        muteButton.onClick = [this]
        {
            if (hasTrack())
                engine.setTrackEnabled (selectedTrack, ! engine.getTrackInfo (selectedTrack).enabled);
            refresh();
        };
        recordButton.onClick  = [this] { engine.setRecording (recordButton.getToggleState()); };
        monitorButton.onClick = [this] { monitorEnabled = monitorButton.getToggleState(); repaint(); };
        channelBox.onChange   = [this]
        {
            if (! hasTrack()) return;
            const auto info = engine.getTrackInfo (selectedTrack);
            if (info.type == TrackType::SfPlayer)
                engine.addOrUpdateSfTrackOnChannel (info.preset, channelBox.getSelectedId() - 1, info.colour);
        };

        setControlsVisible (false);
        startTimerHz (12);
    }

    ~TrackInspector() override { stopTimer(); }

    void setSelectedTrack (int trackIndex)
    {
        selectedTrack = trackIndex;
        refresh();
    }

    void refresh()
    {
        const bool valid = hasTrack();
        setControlsVisible (valid);
        if (! valid) { repaint(); return; }

        const auto info = engine.getTrackInfo (selectedTrack);
        muteButton.setToggleState    (! info.enabled, juce::dontSendNotification);
        recordButton.setToggleState  (engine.isRecording(), juce::dontSendNotification);
        monitorButton.setToggleState (monitorEnabled, juce::dontSendNotification);

        outputBox.clear (juce::dontSendNotification);
        outputBox.addItem (destinationName (info), 1);
        outputBox.setSelectedId (1, juce::dontSendNotification);

        const bool isMultiTimbral = info.type == TrackType::SfPlayer;
        channelBox.setVisible (isMultiTimbral);
        if (isMultiTimbral)
            channelBox.setSelectedId (info.midiChannel + 1, juce::dontSendNotification);

        resized();
        repaint();
    }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        area.removeFromTop (58); // painted track identity header + mute/solo/rec/monitor row
        area.removeFromTop (18); // "ROUTING" section label
        layoutRow (area, inputBox);
        layoutRow (area, outputBox);
        if (channelBox.isVisible()) layoutRow (area, channelBox);
        area.removeFromTop (10);
        area.removeFromTop (18); // "CHANNEL" section label
        layoutRow (area, volumeSlider);
        layoutRow (area, panSlider);
        area.removeFromTop (10);
        area.removeFromTop (18); // "AUTOMATION" section label
        layoutRow (area, automationBox);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = getTheme();
        g.setColour (theme.header.darker (0.12f));
        g.fillRect (getLocalBounds());
        g.setColour (theme.separator);
        g.fillRect (getWidth() - 1, 0, 1, getHeight());

        auto area = getLocalBounds().reduced (10);
        if (! hasTrack())
        {
            g.setColour (theme.foreground.withAlpha (0.42f));
            g.setFont (juce::Font (11.5f));
            g.drawFittedText ("SELECT A TRACK", area, juce::Justification::centred, 1);
            return;
        }

        const auto info = engine.getTrackInfo (selectedTrack);

        g.setColour (info.colour);
        g.fillRect (area.getX(), area.getY(), 4, 44);

        auto title = area.withTrimmedLeft (10).removeFromTop (22);
        g.setColour (theme.foreground);
        g.setFont (juce::Font (13.5f, juce::Font::bold));
        g.drawFittedText (info.name.toUpperCase(), title, juce::Justification::centredLeft, 1);

        auto subtitle = area.withTrimmedLeft (10).withTrimmedTop (22).removeFromTop (16);
        g.setColour (info.colour.withAlpha (0.85f));
        g.setFont (juce::Font (9.5f, juce::Font::bold));
        g.drawText (trackTypeName (info.type), subtitle, juce::Justification::centredLeft, false);

        // Mute / Solo / Record / Monitor row
        const int buttonY   = 58;
        const int buttonW   = juce::jmax (24, (getWidth() - 20 - 3 * 5) / 4);
        const int buttonGap = 5;
        muteButton   .setBounds (10 + 0 * (buttonW + buttonGap), buttonY, buttonW, 22);
        soloButton   .setBounds (10 + 1 * (buttonW + buttonGap), buttonY, buttonW, 22);
        recordButton .setBounds (10 + 2 * (buttonW + buttonGap), buttonY, buttonW, 22);
        monitorButton.setBounds (10 + 3 * (buttonW + buttonGap), buttonY, buttonW, 22);

        auto sections = getLocalBounds().reduced (10);
        sections.removeFromTop (86);
        sectionLabel (g, "ROUTING", sections.removeFromTop (18));
        sections.removeFromTop (64 + (channelBox.isVisible() ? 32 : 0) + 10);
        sectionLabel (g, "CHANNEL", sections.removeFromTop (18));
        sections.removeFromTop (64 + 10);
        sectionLabel (g, "AUTOMATION", sections.removeFromTop (18));
    }

private:
    SequencerEngine& engine;
    int  selectedTrack   = -1;
    bool monitorEnabled  = true;

    juce::TextButton muteButton, soloButton, recordButton, monitorButton;
    juce::ComboBox   inputBox, outputBox, channelBox, automationBox;
    juce::Slider     volumeSlider, panSlider;

    bool hasTrack() const { return juce::isPositiveAndBelow (selectedTrack, engine.getNumTracks()); }

    void configureButton (juce::TextButton& button, const juce::String& text, juce::Colour colour)
    {
        button.setButtonText (text);
        button.setClickingTogglesState (true);
        button.setColour (juce::TextButton::buttonColourId,   colour.withAlpha (0.16f));
        button.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.62f));
        button.setColour (juce::TextButton::textColourOffId,  colour.brighter (0.25f));
        button.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
        addAndMakeVisible (button);
    }

    void setControlsVisible (bool visible)
    {
        for (auto* control : { static_cast<juce::Component*> (&muteButton),
                                static_cast<juce::Component*> (&soloButton),
                                static_cast<juce::Component*> (&recordButton),
                                static_cast<juce::Component*> (&monitorButton),
                                static_cast<juce::Component*> (&inputBox),
                                static_cast<juce::Component*> (&outputBox),
                                static_cast<juce::Component*> (&volumeSlider),
                                static_cast<juce::Component*> (&panSlider),
                                static_cast<juce::Component*> (&automationBox) })
            control->setVisible (visible);
        if (! visible) channelBox.setVisible (false);
    }

    static juce::String trackTypeName (TrackType type)
    {
        switch (type)
        {
            case TrackType::MainSlice:      return "SLICE TRACK";
            case TrackType::ChromaticSlice: return "CHROMATIC SLICE";
            case TrackType::SfPlayer:       return "SOUNDFONT PROGRAM";
        }
        return {};
    }

    static juce::String destinationName (const SequencerTrackInfo& info)
    {
        switch (info.type)
        {
            case TrackType::MainSlice:      return "Slices — Pad Map";
            case TrackType::ChromaticSlice: return "Slices — Chromatic";
            case TrackType::SfPlayer:       return info.preset.name.isNotEmpty()
                                                    ? "SoundFont — " + info.preset.name
                                                    : "SoundFont Program";
        }
        return "Not Routed";
    }

    static void layoutRow (juce::Rectangle<int>& area, juce::Component& control)
    {
        auto row = area.removeFromTop (26);
        control.setBounds (row.withTrimmedTop (2));
        area.removeFromTop (4);
    }

    static void sectionLabel (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> bounds)
    {
        g.setColour (getTheme().foreground.withAlpha (0.44f));
        g.setFont (juce::Font (9.0f, juce::Font::bold));
        g.drawText (text, bounds, juce::Justification::centredLeft);
    }

    void timerCallback() override
    {
        if (hasTrack())
        {
            const bool shouldBeMuted = ! engine.getTrackInfo (selectedTrack).enabled;
            if (muteButton.getToggleState() != shouldBeMuted)
                muteButton.setToggleState (shouldBeMuted, juce::dontSendNotification);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackInspector)
};
