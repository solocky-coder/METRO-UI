#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"
#include <cmath>

//==============================================================================
//  TrackInspector — compact, selected-track control surface docked to the left
//  of ArrangeView's TrackHeaderStrip.
//
//  Arranger inspector redesign pass. Four fixes/additions over the previous
//  layout, all driven by real existing data (no invented state):
//   1. Identity swatch now reads info.colour (the track's own colour) instead
//      of theme.accent — the old version painted every track's identity card
//      in the app's single global accent colour, so it could never actually
//      confirm which track was selected; a purple track in the list could
//      show a cyan card here.
//   2. M/S/R now share one literal colour set with TrackHeaderStrip's row
//      copy of the same controls (see that header's own comment) — Solo and
//      Record already agreed; Mute previously used a totally different
//      green/dark-red scheme there versus this component's amber. Both now
//      use the same lit-when-engaged amber.
//   3. A lightweight activity indicator next to R, reusing
//      SequencerEngine::getMidiActivityAndClear() — the same discrete
//      on/off proxy TrackHeaderStrip's own per-row meter already uses (see
//      that header's comment on why it isn't true peak metering: no
//      continuous per-track level is plumbed from SequencerEngine yet).
//   4. PART changed from a 16-item juce::ComboBox to a 4x4 grid of number
//      tiles — quicker to scan/click for a small fixed set than opening a
//      menu — plus a preset name/bank/program readout using
//      SequencerTrackInfo::preset, which existed but was never surfaced
//      anywhere in this panel.
//   5. A track-colour swatch picker at the bottom, backed by
//      SequencerEngine::setTrackColour() (new — see its declaration
//      comment). Uses a small fixed palette independent of the active
//      UI theme, matching how track colour already works throughout this
//      app (SequencerTrack::colour is per-track user choice, not
//      theme-derived) and how most DAWs present a track-colour picker as a
//      fixed swatch set regardless of chrome skin.
//
//  No OUTPUT/routing control here, unlike the Multisampler zone inspector
//  this pass was modelled on — deliberately: SequencerEngine tracks don't
//  have a per-track output bus concept at all (see the class comment below
//  on why routing stays fixed). Adding one would be decorative, not real.
//
//  No MIDI Out / Monitor controls: output routing is fixed (engine owns the
//  destination per track type) and there is no separate audio monitor path
//  to gate — this project records MIDI only. Only SoundFont tracks expose a
//  MIDI part/channel selector, since they alone are multi-timbral in the
//  current instrument model — Slice and Chromatic tracks are always pinned
//  to their fixed engine channel. Every SfPlayer track (SF2 preset or real
//  .sfz instrument alike) does carry preset info, so the preset readout
//  shows for both; only the part grid itself stays gated to real
//  .sfz-instrument tracks — see partButtons' onClick below for why
//  reassigning a plain SF2 preset track's channel must stay routed through
//  the SF2-PLAYER panel's own menu instead.
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
        // as three distinct controls rather than three cyan copies. Amber
        // (0xffc99140) is shared verbatim with TrackHeaderStrip's row copy
        // of Mute — see this class's header comment, point 2.
        configureButton (muteButton,    "M", juce::Colour (0xffc99140));
        configureButton (soloButton,    "S", juce::Colour (0xffd1b34c));
        configureButton (recordButton,  "R", juce::Colour (0xffd95454));
        for (auto* b : { &muteButton, &soloButton, &recordButton })
            b->getProperties().set ("flatFill", true);

        // 4x4 part-number grid, replacing the old 16-item PART dropdown —
        // see this class's header comment, point 4. Only ever shown for
        // real .sfz-instrument tracks (see refresh()), same gating the old
        // ComboBox used and for the same reason: reassigning a plain SF2
        // preset track's channel here would desync
        // processor.sfPlayerChannelMask (see the onClick body below).
        for (int ch = 0; ch < 16; ++ch)
        {
            auto* b = partButtons.add (new juce::TextButton (juce::String (ch + 1)));
            b->setClickingTogglesState (false);   // radio-style via refresh(), not per-button toggle
            b->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff1c2028));
            b->setColour (juce::TextButton::buttonOnColourId, getTheme().accent);
            b->setColour (juce::TextButton::textColourOffId,  juce::Colours::white.withAlpha (0.55f));
            b->setColour (juce::TextButton::textColourOnId,   juce::Colours::black.withAlpha (0.85f));
            b->onClick = [this, ch]
            {
                if (! hasTrack()) return;
                const auto info = engine.getTrackInfo (selectedTrack);
                // Same call the old channelBox.onChange made — see this
                // class's header comment for why this path is only ever
                // reached for real .sfz-instrument tracks.
                if (info.type == TrackType::SfPlayer && info.isSfzInstrument)
                    engine.addSfzTrack (info.name, ch, info.colour);
            };
            addChildComponent (b);   // hidden until refresh() shows the grid
        }

        // Fixed swatch palette for the track-colour picker — independent of
        // the active UI theme, see this class's header comment point 5.
        // Matches the 16-colour named palette used for the Slicer's "Slice
        // Color" menu (SliceLane.cpp / WaveformView.cpp) and the
        // Multisampler's zone-colour menu (ZoneMapView.cpp), so a colour
        // picked here reads the same everywhere in the app.
        static const juce::Colour kSwatches[] = {
            juce::Colour (0xFF00C8FF), juce::Colour (0xFF00FF87),
            juce::Colour (0xFFFFE800), juce::Colour (0xFFFF6B00),
            juce::Colour (0xFFFF2D55), juce::Colour (0xFFFF2D9A),
            juce::Colour (0xFFB44FFF), juce::Colour (0xFF4A80FF),
            juce::Colour (0xFF00BFFF), juce::Colour (0xFF00FFD0),
            juce::Colour (0xFFA8FF3E), juce::Colour (0xFFFFD700),
            juce::Colour (0xFFFF7F50), juce::Colour (0xFFFF00FF),
            juce::Colour (0xFFE8E8E8), juce::Colour (0xFF888888),
        };
        for (auto swatchColour : kSwatches)
        {
            auto* b = colourButtons.add (new juce::TextButton());
            b->setClickingTogglesState (false);
            b->setColour (juce::TextButton::buttonColourId, swatchColour);
            // Without this, DysektLookAndFeel::drawButtonBackground falls
            // through to its default fill path, which ignores
            // buttonColourId entirely and always paints theme.button (a
            // generic UI grey) for a non-toggled, non-hovered button — the
            // exact "all swatches render grey" bug this property fixes. See
            // its use on muteButton/soloButton/recordButton above, and
            // DysektLookAndFeel.cpp's own comment on the flatFill branch.
            b->getProperties().set ("flatFill", true);
            b->onClick = [this, swatchColour]
            {
                if (hasTrack())
                    engine.setTrackColour (selectedTrack, swatchColour);
                refresh();
            };
            addChildComponent (b);
        }

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

        for (auto* control : { static_cast<juce::Component*> (&volumeSlider),
                                static_cast<juce::Component*> (&panSlider) })
            addAndMakeVisible (*control);

        muteButton.onClick = [this]
        {
            if (hasTrack())
                engine.setTrackEnabled (selectedTrack, ! engine.getTrackInfo (selectedTrack).enabled);
            refresh();
        };
        recordButton.onClick  = [this]
        {
            if (! hasTrack()) return;
            // Same toggle logic TrackHeaderStrip's row R uses (see its
            // mouseDown) — arms/disarms THIS track as the recording
            // target, not global engine.setRecording(). That's a
            // transport-wide on/off completely unrelated to which track
            // is armed; wiring R to it meant the inspector's button could
            // show "on" while a totally different track (or none) was
            // actually armed, and clicking it didn't arm the selected
            // track at all.
            engine.setRecordingTrack (selectedTrack == engine.getRecordingTrackIndex() ? -1 : selectedTrack);
        };
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
        recordButton.setToggleState  (selectedTrack == engine.getRecordingTrackIndex(), juce::dontSendNotification);
        volumeSlider.setValue (info.volumeDb, juce::dontSendNotification);
        panSlider.setValue (info.pan * 100.0, juce::dontSendNotification);

        // Only real .sfz-instrument tracks show the part grid — see this
        // class's header comment for why plain SF2 preset tracks stay
        // reassignable only from the SF2-PLAYER panel's own menu.
        showPartGrid = info.type == TrackType::SfPlayer && info.isSfzInstrument;
        for (int ch = 0; ch < 16; ++ch)
        {
            auto* b = partButtons[ch];
            b->setVisible (showPartGrid);
            b->setToggleState (showPartGrid && info.midiChannel == ch, juce::dontSendNotification);
        }

        // Preset readout shows for every SfPlayer track, sfz or not — see
        // this class's header comment point 4.
        showPreset = info.type == TrackType::SfPlayer;
        presetName = info.preset.name.isNotEmpty() ? info.preset.name : "(no preset)";
        presetTag  = "BANK " + juce::String (info.preset.bank).paddedLeft ('0', 3)
                   + juce::String::charToString (juce::juce_wchar (0x00B7))   // middle dot
                   + " PGM " + juce::String (info.preset.preset).paddedLeft ('0', 3);

        for (auto* b : colourButtons)
            b->setVisible (true);

        resized();
        repaint();
    }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        if (! hasTrack())
        {
            for (auto* b : partButtons)   b->setBounds ({});
            for (auto* b : colourButtons) b->setBounds ({});
            return;
        }

        // ── Identity row ────────────────────────────────────────────────
        area.removeFromTop (kIdentityH);
        area.removeFromTop (kGapM);

        // ── State row (M/S/R + activity dot) ───────────────────────────
        const int buttonY = area.getY();
        const int meterW  = 20;
        const int buttonGap = 5;
        const int buttonW = juce::jmax (25, (area.getWidth() - meterW - 3 * buttonGap) / 3);
        muteButton  .setBounds (area.getX() + 0 * (buttonW + buttonGap), buttonY, buttonW, 25);
        soloButton  .setBounds (area.getX() + 1 * (buttonW + buttonGap), buttonY, buttonW, 25);
        recordButton.setBounds (area.getX() + 2 * (buttonW + buttonGap), buttonY, buttonW, 25);
        activityDotBounds = { area.getX() + 3 * (buttonW + buttonGap), buttonY + 6, meterW, 13 };
        area.removeFromTop (25 + kGapL);

        // ── Instrument section (SfPlayer tracks only) ──────────────────
        const auto info = engine.getTrackInfo (selectedTrack);
        if (info.type == TrackType::SfPlayer)
        {
            area.removeFromTop (kSectionLabelH + kGapS);

            if (showPartGrid)
            {
                constexpr int cols = 4, rows = 4, gap = 2;
                const int cellW = (area.getWidth() - (cols - 1) * gap) / cols;
                const int cellH = 22;
                for (int ch = 0; ch < 16; ++ch)
                {
                    const int col = ch % cols, row = ch / cols;
                    partButtons[ch]->setBounds (area.getX() + col * (cellW + gap),
                                                 area.getY() + row * (cellH + gap),
                                                 cellW, cellH);
                }
                area.removeFromTop (rows * cellH + (rows - 1) * gap + kGapM);
            }
            else
            {
                for (auto* b : partButtons) b->setBounds ({});
            }

            presetRowBounds = area.removeFromTop (kPresetRowH);
            area.removeFromTop (kGapL);
        }
        else
        {
            for (auto* b : partButtons) b->setBounds ({});
        }

        // ── Channel section ─────────────────────────────────────────────
        area.removeFromTop (kSectionLabelH + kGapS);
        layoutField (area, volumeSlider);
        layoutField (area, panSlider);
        area.removeFromTop (kGapL);

        // ── Track colour section ────────────────────────────────────────
        // 16 swatches at the old single-row spacing would need ~380px in a
        // 192px-wide inspector column, so wrap into two rows of 8 (matches
        // the panel width exactly: 8*20 + 7*4 = 188px).
        area.removeFromTop (kSectionLabelH + kGapS);
        constexpr int swatchSize = 20, swatchGap = 4, swatchCols = 8;
        for (int i = 0; i < colourButtons.size(); ++i)
        {
            const int col = i % swatchCols, row = i / swatchCols;
            colourButtons[i]->setBounds (area.getX() + col * (swatchSize + swatchGap),
                                          area.getY() + row * (swatchSize + swatchGap),
                                          swatchSize, swatchSize);
        }
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

        // ── Identity row: swatch + name + type badge + channel tag ─────
        // Swatch reads info.colour — the track's OWN colour — not
        // theme.accent. See this class's header comment, point 1.
        auto idRow = content.removeFromTop (kIdentityH);
        auto swatch = idRow.removeFromLeft (kIdentityH).reduced (2).toFloat();
        g.setColour (info.colour);
        g.fillRect (swatch);
        g.setColour (info.colour.contrasting (0.85f));
        g.setFont (DysektLookAndFeel::makeFont (14.0f, true));
        g.drawText (info.name.substring (0, 1).toUpperCase(), swatch.toNearestInt(),
                    juce::Justification::centred, false);

        idRow.removeFromLeft (10);
        auto nameArea = idRow.removeFromTop (17);
        g.setColour (theme.foreground);
        g.setFont (DysektLookAndFeel::makeFont (14.0f, true));
        g.drawFittedText (info.name, nameArea, juce::Justification::centredLeft, 1);

        auto metaArea = idRow.removeFromTop (14);
        juce::String typeTag = trackTypeName (info.type);
        const int badgeW = juce::jmin (metaArea.getWidth() - 4,
            juce::GlyphArrangement::getStringWidthInt (DysektLookAndFeel::makeFont (8.5f, true), typeTag) + 12);
        auto badgeR = metaArea.removeFromLeft (badgeW);
        g.setColour (theme.button);
        g.fillRect (badgeR);
        g.setColour (theme.foreground.withAlpha (0.6f));
        g.setFont (DysektLookAndFeel::makeFont (8.5f, true));
        g.drawText (typeTag, badgeR, juce::Justification::centred, false);

        metaArea.removeFromLeft (6);
        g.setColour (theme.foreground.withAlpha (0.4f));
        g.setFont (DysektLookAndFeel::makeFont (8.5f, false));
        g.drawText ("CH " + juce::String (info.midiChannel + 1), metaArea, juce::Justification::centredLeft, false);

        content.removeFromTop (kGapM);

        // ── State row: M/S/R already drawn by the buttons themselves;
        // just the activity dot here, using the same discrete on/off
        // hold-counter TrackHeaderStrip's own meter uses (see this
        // class's header comment, point 3). ──────────────────────────
        content.removeFromTop (25 + kGapL);
        g.setColour (theme.foreground.withAlpha (0.3f));
        g.setFont (DysektLookAndFeel::makeFont (8.0f, true));
        g.drawText ("ACT", activityDotBounds.withTrimmedBottom (activityDotBounds.getHeight() - 8),
                    juce::Justification::centredLeft, false);
        auto dot = activityDotBounds.withTrimmedTop (8).withHeight (5).toFloat();
        g.setColour (theme.button);
        g.fillRect (dot);
        if (activityHoldTicks > 0)
        {
            g.setColour (juce::Colour (0xff52c9a0));
            g.fillRect (dot.withWidth (dot.getWidth() * 0.7f));
        }

        // ── Instrument section ───────────────────────────────────────────
        if (info.type == TrackType::SfPlayer)
        {
            sectionLabel (g, "INSTRUMENT", content.removeFromTop (kSectionLabelH));
            content.removeFromTop (kGapS);

            if (showPartGrid)
            {
                constexpr int rows = 4, gap = 2;
                content.removeFromTop (rows * 22 + (rows - 1) * gap + kGapM);
            }

            if (showPreset)
            {
                auto row = presetRowBounds;
                g.setColour (theme.foreground.withAlpha (0.85f));
                g.setFont (DysektLookAndFeel::makeFont (11.5f, false));
                g.drawFittedText (presetName, row.removeFromTop (16), juce::Justification::centredLeft, 1);
                g.setColour (theme.foreground.withAlpha (0.4f));
                g.setFont (DysektLookAndFeel::makeFont (9.0f, false));
                g.drawText (presetTag, row.removeFromTop (14), juce::Justification::centredLeft, false);
            }
            content.removeFromTop (kPresetRowH + kGapL);
        }

        // ── Channel section ───────────────────────────────────────────────
        sectionLabel (g, "CHANNEL", content.removeFromTop (kSectionLabelH));
        content.removeFromTop (kGapS);
        drawFieldLabel (g, "VOLUME", volumeSlider);
        drawFieldLabel (g, "PAN", panSlider);
        content.removeFromTop (42 + 42 + kGapL);

        // ── Track colour section ─────────────────────────────────────────
        sectionLabel (g, "TRACK COLOR", content.removeFromTop (kSectionLabelH));
        content.removeFromTop (kGapS);
        // Selection ring around whichever swatch matches the track's
        // current colour exactly.
        for (auto* b : colourButtons)
        {
            if (b->findColour (juce::TextButton::buttonColourId) == info.colour)
            {
                g.setColour (theme.foreground);
                g.drawRect (b->getBounds().expanded (2), 1);
                break;
            }
        }
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
    juce::Slider     volumeSlider, panSlider;

    juce::OwnedArray<juce::TextButton> partButtons;     // 16, radio-style — see refresh()
    juce::OwnedArray<juce::TextButton> colourButtons;   // fixed palette — see constructor

    bool showPartGrid = false;
    bool showPreset   = false;
    juce::String presetName, presetTag;
    juce::Rectangle<int> presetRowBounds;
    juce::Rectangle<int> activityDotBounds;

    // Layout constants shared between resized() and paint() so the two
    // never drift apart — the exact bug the old hardcoded-literal layout
    // (see this file's git history) already caused once before.
    static constexpr int kIdentityH     = 40;
    static constexpr int kSectionLabelH = 16;
    static constexpr int kPresetRowH    = 30;
    static constexpr int kGapS = 6, kGapM = 10, kGapL = 14;

    int  activityHoldTicks = 0;
    static constexpr int kActivityHoldTicks = 3;   // same hold length TrackHeaderStrip's kHoldTicks uses

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
        if (! visible)
        {
            for (auto* b : partButtons)   b->setVisible (false);
            for (auto* b : colourButtons) b->setVisible (false);
        }
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
        g.drawLine ((float) bounds.getX() + 76.0f, ruleY, (float) bounds.getRight(), ruleY, 1.0f);
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

            // Record-arm target is engine-wide state that can change out
            // from under this component — e.g. from TrackHeaderStrip's own
            // R button (see its mouseDown) — so it needs the same live poll
            // Mute/Solo get above, not just the one-shot set in refresh().
            // (It's deliberately NOT touched by track selection — see
            // ArrangeView::selectTrack() — so this poll is just to stay in
            // sync with the *other* arm control, not with selection.)
            const bool shouldBeArmed = selectedTrack == engine.getRecordingTrackIndex();
            if (recordButton.getToggleState() != shouldBeArmed)
                recordButton.setToggleState (shouldBeArmed, juce::dontSendNotification);

            // Activity indicator — same discrete hold-counter approach as
            // TrackHeaderStrip's own per-row meter (see this class's header
            // comment, point 3); intentionally not true peak metering.
            bool needsRepaint = false;
            if (engine.getMidiActivityAndClear (selectedTrack))
            {
                activityHoldTicks = kActivityHoldTicks;
                needsRepaint = true;
            }
            else if (activityHoldTicks > 0)
            {
                --activityHoldTicks;
                needsRepaint = true;
            }
            if (needsRepaint) repaint();
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackInspector)
};
