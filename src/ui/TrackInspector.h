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

        channelBox.setVisible (false);
        for (int channel = 1; channel <= 16; ++channel)
            channelBox.addItem ("Part " + juce::String (channel), channel);

        // Mockup styling: VOLUME is a bordered, gradient-filled bar with a
        // glowing round thumb and a plain "0.0 dB" readout to its right.
        // PAN is a thin unboxed line with the same glowing thumb, but the
        // L/C/R readout is drawn *inside* the thumb instead of in a text box.
        volumeSlider.setLookAndFeel (&mockupSliderLnF);
        volumeSlider.getProperties().set ("mockupPan", false);
        volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        volumeSlider.setRange (-60.0, 6.0, 0.1);
        volumeSlider.setValue (0.0, juce::dontSendNotification);
        volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 18);
        volumeSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        volumeSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        volumeSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        volumeSlider.setTextValueSuffix (" dB");

        panSlider.setLookAndFeel (&mockupSliderLnF);
        panSlider.getProperties().set ("mockupPan", true);
        panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        panSlider.setRange (-100.0, 100.0, 1.0);
        panSlider.setValue (0.0, juce::dontSendNotification);
        panSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        panSlider.textFromValueFunction = [] (double value)
        {
            if (std::abs (value) < 0.5) return juce::String ("C");
            return value < 0.0 ? "L" + juce::String ((int) -value) : "R" + juce::String ((int) value);
        };

        for (auto* control : { static_cast<juce::Component*> (&channelBox),
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

            // SF2-preset tracks are reassigned exclusively through the
            // SF2-PLAYER panel's right-click "Assign MIDI channel" menu now
            // (Sf2InstrumentWorkspace::handleChannelAssigned) — that's the
            // only path that keeps processor.sfPlayerChannelMask in sync,
            // which is what actually gates whether processMidi() routes any
            // MIDI to the SF2 engine on a given channel at all. Routing an
            // SF2 track's channel change through here (as before) updated
            // the track's own midiChannel/FluidSynth program but left that
            // mask stale, so a track moved to a not-yet-enabled channel
            // would go silent despite showing the "correct" channel — see
            // channelBox's visibility below, which now only shows this
            // control for real .sfz-instrument tracks in the first place.
            if (info.type == TrackType::SfPlayer && info.isSfzInstrument)
                engine.addSfzTrack (info.name, channelBox.getSelectedId() - 1, info.colour);
        };

        setControlsVisible (false);
        startTimerHz (12);
    }

    ~TrackInspector() override
    {
        stopTimer();
        volumeSlider.setLookAndFeel (nullptr);
        panSlider.setLookAndFeel (nullptr);
    }

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

        // Only real .sfz-instrument tracks use this control now. Genuine SF2
        // preset tracks are reassigned exclusively via the SF2-PLAYER
        // panel's right-click menu — see channelBox.onChange above for why.
        const bool showChannelBox = info.type == TrackType::SfPlayer && info.isSfzInstrument;
        channelBox.setVisible (showChannelBox);
        if (showChannelBox)
            channelBox.setSelectedId (info.midiChannel + 1, juce::dontSendNotification);

        resized();
        repaint();
    }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        area.removeFromTop (18); // "SELECTED TRACK" caption
        area.removeFromTop (52); // selected-track identity card

        // M/S/R button row sits a few px below the identity card. Derived
        // from the live cursor (rather than a hardcoded literal) so it can
        // never drift out of sync with the caption/card heights above again —
        // that mismatch is exactly what caused the buttons to overlap the
        // name card after the "SELECTED TRACK" caption was added.
        const int buttonY = area.getY() + 3;
        area.removeFromTop (31); // performance control row + breathing room

        if (channelBox.isVisible())
        {
            area.removeFromTop (10);
            layoutField (area, channelBox);
        }

        area.removeFromTop (10);
        area.removeFromTop (20); // CHANNEL heading
        layoutField (area, volumeSlider);
        layoutField (area, panSlider);

        const int buttonW = juce::jmax (25, (getWidth() - 24 - 2 * 5) / 3);
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

        // "SELECTED TRACK" caption — same muted-caps treatment as the
        // "CHANNEL" section label below, for visual consistency.
        auto caption = content.removeFromTop (18);
        g.setColour (theme.foreground.withAlpha (0.48f));
        g.setFont (DysektLookAndFeel::makeFont (9.0f, true));
        g.drawText ("SELECTED TRACK", caption, juce::Justification::centredLeft, false);

        const auto info = engine.getTrackInfo (selectedTrack);
        auto card = content.removeFromTop (52).toFloat();
        g.setColour (theme.button.brighter (0.06f));
        g.fillRoundedRectangle (card, 5.0f);
        g.setColour (theme.accent.withAlpha (0.55f));
        g.drawRoundedRectangle (card.reduced (0.5f), 5.0f, 1.0f);
        g.setColour (theme.accent);
        g.fillRoundedRectangle ({ card.getX(), card.getY(), 4.0f, card.getHeight() }, 2.0f);

        auto title = card.toNearestInt().withTrimmedLeft (16).reduced (0, 7).removeFromTop (19);
        g.setColour (theme.foreground);
        g.setFont (DysektLookAndFeel::makeFont (15.0f, true));
        g.drawFittedText (info.name.toUpperCase(), title, juce::Justification::centredLeft, 1);

        auto subtitle = card.toNearestInt().withTrimmedLeft (16).withTrimmedTop (29).removeFromTop (13);
        g.setColour (theme.foreground.withAlpha (0.55f));
        g.setFont (DysektLookAndFeel::makeFont (10.0f, true));
        g.drawText (trackTypeName (info.type), subtitle, juce::Justification::centredLeft, false);

        content.removeFromTop (31);
        if (channelBox.isVisible())
        {
            content.removeFromTop (10);
            drawFieldLabel (g, "PART", channelBox);
            content.removeFromTop (42);
        }

        content.removeFromTop (10);
        sectionLabel (g, "CHANNEL", content.removeFromTop (20));
        drawFieldLabel (g, "VOLUME", volumeSlider);
        drawFieldLabel (g, "PAN", panSlider);
    }

private:
    //==========================================================================
    //  MockupSliderLnF — draws VOLUME/PAN per the mockup: VOLUME is a bordered
    //  track with a blue gradient fill; PAN is a bare line. Both get a glowing
    //  round thumb; PAN's thumb carries its L/C/R readout instead of a text box.
    //  Selected per-slider via the "mockupPan" component property.
    //==========================================================================
    struct MockupSliderLnF : public juce::LookAndFeel_V4
    {
        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                                const juce::Slider::SliderStyle, juce::Slider& slider) override
        {
            const bool isPan = slider.getProperties().getWithDefault ("mockupPan", false);
            const float centreY = (float) y + (float) height * 0.5f;
            const juce::Colour accent (0xFF3AA8FF);

            if (isPan)
            {
                g.setColour (juce::Colours::white.withAlpha (0.16f));
                g.drawLine ((float) x, centreY, (float) (x + width), centreY, 1.5f);
            }
            else
            {
                const juce::Rectangle<float> box ((float) x, (float) y + 2.0f,
                                                    (float) width, (float) height - 4.0f);
                g.setColour (juce::Colour (0xFF10191F));
                g.fillRoundedRectangle (box, 3.0f);

                const float fillW = juce::jlimit (0.0f, box.getWidth(), sliderPos - box.getX());
                if (fillW > 0.5f)
                {
                    auto fill = box.withWidth (fillW);
                    juce::ColourGradient grad (juce::Colour (0xFF1C6FA8), fill.getX(), fill.getCentreY(),
                                                accent, fill.getRight(), fill.getCentreY(), false);
                    g.setGradientFill (grad);
                    g.fillRoundedRectangle (fill, 3.0f);
                }

                g.setColour (juce::Colour (0xFF2B4552));
                g.drawRoundedRectangle (box, 3.0f, 1.0f);
            }

            const float radius = isPan ? 9.0f : 8.0f;
            const juce::Point<float> centre (sliderPos, centreY);

            for (int i = 3; i >= 1; --i)
            {
                g.setColour (accent.withAlpha (0.09f * (float) i));
                g.fillEllipse (juce::Rectangle<float> (radius * 2.0f + i * 3.0f, radius * 2.0f + i * 3.0f)
                                   .withCentre (centre));
            }
            g.setColour (accent);
            g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (centre));

            if (isPan)
            {
                g.setColour (juce::Colours::white);
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText (slider.getTextFromValue (slider.getValue()),
                            juce::Rectangle<float> (radius * 2.0f, radius * 2.0f)
                                .withCentre (centre).toNearestInt(),
                            juce::Justification::centred, false);
            }
        }
    };

    SequencerEngine& engine;
    int  selectedTrack   = -1;

    MockupSliderLnF  mockupSliderLnF;
    juce::TextButton muteButton, soloButton, recordButton;
    juce::ComboBox   channelBox;
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
