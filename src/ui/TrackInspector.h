#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  TrackInspector — compact, selected-track control surface docked to the left
//  of ArrangeView's TrackHeaderStrip.
//
//  No MIDI Out / Monitor controls: output routing is fixed (engine owns the
//  destination per track type) and there is no separate audio monitor path
//  to gate — this project records MIDI only. Only SoundFont tracks expose a
//  MIDI part/channel selector, since they alone are multi-timbral in the
//  current instrument model — Slice and Chromatic tracks are always pinned
//  to their fixed engine channel.
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

        inputBox.addItem ("All MIDI Inputs", 1);
        inputBox.setSelectedId (1, juce::dontSendNotification);
        inputBox.setEnabled (false);

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

        for (auto* control : { static_cast<juce::Component*> (&inputBox),
                                static_cast<juce::Component*> (&channelBox),
                                static_cast<juce::Component*> (&volumeSlider),
                                static_cast<juce::Component*> (&panSlider) })
            addAndMakeVisible (*control);

        muteButton.onClick = [this]
        {
            if (hasTrack())
                engine.setTrackEnabled (selectedTrack, ! engine.getTrackInfo (selectedTrack).enabled);
            refresh();
        };
        recordButton.onClick  = [this] { engine.setRecording (recordButton.getToggleState()); };
        soloButton.onClick    = [this]
        {
            if (hasTrack())
                engine.setTrackSolo (selectedTrack, soloButton.getToggleState());
        };
        volumeSlider.onValueChange = [this]
        {
            if (hasTrack())
                engine.setTrackVolumeDb (selectedTrack, (float) volumeSlider.getValue());
        };
        panSlider.onValueChange = [this]
        {
            if (hasTrack())
                engine.setTrackPan (selectedTrack, (float) (panSlider.getValue() / 100.0));
        };
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
        soloButton.setToggleState    (info.solo, juce::dontSendNotification);
        recordButton.setToggleState  (engine.isRecording(), juce::dontSendNotification);
        volumeSlider.setValue (info.volumeDb, juce::dontSendNotification);
        panSlider.setValue (info.pan * 100.0, juce::dontSendNotification);

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
        auto area = getLocalBounds().reduced (12);
        area.removeFromTop (52); // selected-track identity card
        area.removeFromTop (31); // performance control row + breathing room

        area.removeFromTop (20); // ROUTING heading
        layoutField (area, inputBox);
        if (channelBox.isVisible()) layoutField (area, channelBox);

        area.removeFromTop (10);
        area.removeFromTop (20); // CHANNEL heading
        layoutField (area, volumeSlider);
        layoutField (area, panSlider);

        const int buttonW = juce::jmax (25, (getWidth() - 24 - 2 * 5) / 3);
        const int buttonY = 67;
        constexpr int buttonGap = 5;
        muteButton  .setBounds (12 + 0 * (buttonW + buttonGap), buttonY, buttonW, 25);
        soloButton  .setBounds (12 + 1 * (buttonW + buttonGap), buttonY, buttonW, 25);
        recordButton.setBounds (12 + 2 * (buttonW + buttonGap), buttonY, buttonW, 25);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = getTheme();
        g.setColour (theme.header.darker (0.12f));
        g.fillRect (getLocalBounds());
        g.setColour (theme.separator);
        g.fillRect (getWidth() - 1, 0, 1, getHeight());

        auto content = getLocalBounds().reduced (12);
        if (! hasTrack())
        {
            g.setColour (theme.foreground.withAlpha (0.42f));
            g.setFont (DysektLookAndFeel::makeFont (12.0f, true));
            g.drawFittedText ("SELECT A TRACK", content, juce::Justification::centred, 1);
            return;
        }

        const auto info = engine.getTrackInfo (selectedTrack);
        auto card = content.removeFromTop (52).toFloat();
        g.setColour (theme.button.brighter (0.06f));
        g.fillRoundedRectangle (card, 6.0f);
        g.setColour (theme.separator.brighter (0.12f));
        g.drawRoundedRectangle (card.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (info.colour);
        g.fillRoundedRectangle ({ card.getX(), card.getY(), 4.0f, card.getHeight() }, 2.0f);

        auto title = card.toNearestInt().withTrimmedLeft (13).reduced (0, 7).removeFromTop (19);
        g.setColour (theme.foreground);
        g.setFont (DysektLookAndFeel::makeFont (15.0f, true));
        g.drawFittedText (info.name.toUpperCase(), title, juce::Justification::centredLeft, 1);

        auto subtitle = card.toNearestInt().withTrimmedLeft (13).withTrimmedTop (29).removeFromTop (13);
        g.setColour (info.colour.withAlpha (0.90f));
        g.setFont (DysektLookAndFeel::makeFont (10.0f, true));
        g.drawText (trackTypeName (info.type), subtitle, juce::Justification::centredLeft, false);

        content.removeFromTop (31);
        sectionLabel (g, "ROUTING", content.removeFromTop (20));
        drawFieldLabel (g, "INPUT", inputBox);
        if (channelBox.isVisible()) drawFieldLabel (g, "PART", channelBox);

        content.removeFromTop ((channelBox.isVisible() ? 2 : 1) * 42 + 10);
        sectionLabel (g, "CHANNEL", content.removeFromTop (20));
        drawFieldLabel (g, "VOLUME", volumeSlider);
        drawFieldLabel (g, "PAN", panSlider);
    }

private:
    SequencerEngine& engine;
    int  selectedTrack   = -1;

    juce::TextButton muteButton, soloButton, recordButton;
    juce::ComboBox   inputBox, channelBox;
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
                                static_cast<juce::Component*> (&inputBox),
                                static_cast<juce::Component*> (&volumeSlider),
                                static_cast<juce::Component*> (&panSlider) })
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

    static void layoutField (juce::Rectangle<int>& area, juce::Component& control)
    {
        auto field = area.removeFromTop (42);
        control.setBounds (field.withTrimmedTop (14).withHeight (28));
    }

    static void drawFieldLabel (juce::Graphics& g, const juce::String& text, const juce::Component& control)
    {
        auto label = control.getBounds().withHeight (11).translated (0, -13);
        g.setColour (getTheme().foreground.withAlpha (0.48f));
        g.setFont (DysektLookAndFeel::makeFont (9.0f, true));
        g.drawText (text, label, juce::Justification::centredLeft, false);
    }

    static void sectionLabel (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> bounds)
    {
        g.setColour (getTheme().foreground.withAlpha (0.48f));
        g.setFont (DysektLookAndFeel::makeFont (9.0f, true));
        g.drawText (text, bounds, juce::Justification::centredLeft);
        const float ruleY = (float) bounds.getCentreY();
        g.setColour (getTheme().separator.withAlpha (0.85f));
        g.drawLine ((float) bounds.getX() + 56.0f, ruleY, (float) bounds.getRight(), ruleY, 1.0f);
    }

    void timerCallback() override
    {
        if (hasTrack())
        {
            const auto info = engine.getTrackInfo (selectedTrack);
            const bool shouldBeMuted = ! info.enabled;
            if (muteButton.getToggleState() != shouldBeMuted)
                muteButton.setToggleState (shouldBeMuted, juce::dontSendNotification);
            if (soloButton.getToggleState() != info.solo)
                soloButton.setToggleState (info.solo, juce::dontSendNotification);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackInspector)
};
