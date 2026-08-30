#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"
#include <cmath>

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
        // DYSEKT-METRO pass: M/S/R go back to filled swatches, but flat —
        // solid tile per button, sharp corners, no cyan-outline chrome.
        // Each keeps its own accent (amber/gold/red) so the row still reads
        // as three distinct controls rather than three cyan copies.
        configureButton (muteButton,    "M", juce::Colour (0xffc99140));
        configureButton (soloButton,    "S", juce::Colour (0xffd1b34c));
        configureButton (recordButton,  "R", juce::Colour (0xffd95454));
        for (auto* b : { &muteButton, &soloButton, &recordButton })
            b->getProperties().set ("flatFill", true);

        channelBox.setVisible (false);
        for (int channel = 1; channel <= 16; ++channel)
            channelBox.addItem ("Part " + juce::String (channel), channel);

        // Mockup styling: both VOLUME and PAN share one thin-groove/flat-thumb
        // vocabulary instead of two unrelated widgets. VOLUME fills from the
        // left edge with a unity-gain tick at 0 dB; PAN fills outward from a
        // centre tick so the pan amount reads from the bar length, not just
        // the number. Both get a plain right-aligned readout — PAN's L/C/R
        // used to be crammed inside the thumb itself, which is illegible at
        // that size, so it now matches VOLUME's text box instead.
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
        panSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 40, 18);
        panSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
        panSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        panSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
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
        // DYSEKT-METRO pass: solid flat accent tile — no hairline, no
        // rounding, no alpha wash. Title/subtitle switch to on-accent text
        // (near-black over the bright accent fill) instead of the theme's
        // usual off-white foreground, since foreground-on-accent doesn't
        // have enough contrast once the card is a full-strength solid.
        g.setColour (theme.accent);
        g.fillRect (card);

        auto title = card.toNearestInt().withTrimmedLeft (16).reduced (0, 7).removeFromTop (19);
        g.setColour (theme.accent.darker (0.75f));
        g.setFont (DysektLookAndFeel::makeFont (15.0f, true));
        g.drawFittedText (info.name.toUpperCase(), title, juce::Justification::centredLeft, 1);

        auto subtitle = card.toNearestInt().withTrimmedLeft (16).withTrimmedTop (29).removeFromTop (13);
        g.setColour (theme.accent.darker (0.55f));
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
    //  MockupSliderLnF — one shared thin-groove/flat-thumb vocabulary for
    //  VOLUME and PAN, differing only in how the fill anchors: VOLUME fills
    //  from the left edge (level), PAN fills outward from centre (bipolar
    //  offset). Both carry a small tick at their "zero" value — unity gain
    //  for VOLUME, hard centre for PAN — and read their accent colour from
    //  the active theme rather than a hardcoded colour, so the strip matches
    //  whichever skin is active. Selected per-slider via "mockupPan".
    //==========================================================================
    // DYSEKT-METRO pass: the groove is a tapered wedge rather than a flat
    // bar — thin at the "quiet"/edge end, thick at the "loud"/centre end —
    // so the taper itself communicates level, not just the fill length.
    // VOLUME tapers thin→thick left→right (single ramp, since VOLUME only
    // has one direction of travel away from silence). PAN tapers thin at
    // centre to thick at both edges (symmetric, since panning hard either
    // way is equally "more extreme"), matching VOLUME's visual language
    // instead of just staying a flat bar. Both keep sharp corners and the
    // zero-tick; the thumb becomes a slim rectangle, not a rounded capsule.
    struct MockupSliderLnF : public juce::LookAndFeel_V4
    {
        static constexpr float kThinHalf  = 1.0f;   // half-height at the wedge's thin end
        static constexpr float kThickHalf = 7.0f;   // half-height at the wedge's thick end

        // Half-height of the wedge at horizontal position t (0..1 across the groove).
        static float halfHeightAt (float t, bool isPan)
        {
            if (! isPan)
                return kThinHalf + (kThickHalf - kThinHalf) * juce::jlimit (0.0f, 1.0f, t);

            // Symmetric: thin at centre (t=0.5), thick at both edges.
            const float distFromCentre = std::abs (t - 0.5f) * 2.0f;
            return kThinHalf + (kThickHalf - kThinHalf) * juce::jlimit (0.0f, 1.0f, distFromCentre);
        }

        // Builds the wedge outline between two x positions (in local slider
        // pixels) as a closed path, so the base groove and the accent fill
        // are cut from literally the same taper and never look mismatched.
        static juce::Path wedgeSegment (float x0, float x1, int x, int width, float centreY, bool isPan)
        {
            juce::Path p;
            if (x1 <= x0) return p;
            const float t0 = (x0 - (float) x) / (float) width;
            const float t1 = (x1 - (float) x) / (float) width;
            const float h0 = halfHeightAt (t0, isPan);
            const float h1 = halfHeightAt (t1, isPan);
            p.startNewSubPath (x0, centreY - h0);
            p.lineTo (x1, centreY - h1);
            p.lineTo (x1, centreY + h1);
            p.lineTo (x0, centreY + h0);
            p.closeSubPath();
            return p;
        }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                                const juce::Slider::SliderStyle, juce::Slider& slider) override
        {
            const bool isPan = slider.getProperties().getWithDefault ("mockupPan", false);
            const float centreY = (float) y + (float) height * 0.5f;
            const juce::Colour accent = getTheme().accent;

            // Pixel position of this slider's "zero" value (unity gain for
            // VOLUME, hard centre for PAN) via the same linear mapping used
            // to derive sliderPos, since neither slider is skewed.
            const double range = slider.getMaximum() - slider.getMinimum();
            const float zeroT  = range > 0.0 ? (float) ((0.0 - slider.getMinimum()) / range) : 0.5f;
            const float zeroX  = (float) x + zeroT * (float) width;

            // Base wedge — full width, dark
            g.setColour (juce::Colour (0xFF1A1A1A));
            g.fillPath (wedgeSegment ((float) x, (float) (x + width), x, width, centreY, isPan));

            // Accent fill wedge: left-anchored for VOLUME, centre-anchored for PAN.
            // Sampled from the same wedgeSegment() so the taper always lines
            // up exactly with the base, whatever the fill span turns out to be.
            const float fillX0 = isPan ? juce::jmin (zeroX, sliderPos) : (float) x;
            const float fillX1 = isPan ? juce::jmax (zeroX, sliderPos) : sliderPos;
            if (fillX1 > fillX0 + 0.5f)
            {
                g.setColour (accent);
                g.fillPath (wedgeSegment (fillX0, fillX1, x, width, centreY, isPan));
            }

            // Zero tick
            g.setColour (juce::Colours::white.withAlpha (0.30f));
            g.fillRect (juce::Rectangle<float> (1.0f, 10.0f).withCentre ({ zeroX, centreY }));

            // Thumb — slim flat rectangle, sharp corners, no glow
            const juce::Rectangle<float> thumb (3.0f, (float) height - 2.0f);
            g.setColour (juce::Colours::white);
            g.fillRect (thumb.withCentre ({ sliderPos, centreY }));
        }
    };

    SequencerEngine& engine;
    int  selectedTrack   = -1;

    MockupSliderLnF  mockupSliderLnF;
    juce::TextButton muteButton, soloButton, recordButton;
    juce::ComboBox   channelBox;
    juce::Slider     volumeSlider, panSlider;

    bool hasTrack() const { return juce::isPositiveAndBelow (selectedTrack, engine.getNumTracks()); }

    // DYSEKT-METRO pass: M/S/R stay three distinct accent colours (amber /
    // gold / red) — that per-button colour is a functional cue, not just
    // decoration — but move from an alpha-tinted outline to a solid flat
    // fill so they read as tiles rather than translucent chips. Off-state
    // is a dimmed solid fill (not a wash) so the button never looks empty;
    // on-state is the full accent colour with a colour-matched dark label
    // instead of white, since white-on-saturated reads muddy at full
    // opacity for the lighter (gold) swatch in particular.
    void configureButton (juce::TextButton& button, const juce::String& text, juce::Colour colour)
    {
        button.setButtonText (text);
        button.setClickingTogglesState (true);
        button.setColour (juce::TextButton::buttonColourId,   colour.darker (0.55f));
        button.setColour (juce::TextButton::buttonOnColourId, colour);
        button.setColour (juce::TextButton::textColourOffId,  colour.brighter (0.35f));
        button.setColour (juce::TextButton::textColourOnId,   colour.darker (0.75f));
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
