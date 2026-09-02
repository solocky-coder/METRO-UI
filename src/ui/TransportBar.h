#pragma once
#include <cmath>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/MidiClip.h"
#include "../sequencer/AbletonLink.h"
#include "../metro/MetroColours.h"
#include "../metro/MetroTypography.h"
#include "DysektLookAndFeel.h"
#include "UIHelpers.h"
#include "TransportIconButton.h"

using namespace dysekt::metro;   // Base/Text/Accent/Transport — same chrome
                                  // palette FloatingTransportBar.cpp uses,
                                  // deliberately independent of whichever
                                  // main-window theme is active. See this
                                  // class's header comment below.

//==============================================================================
//  TransportBar
//
//  Docked-transport redesign pass: this row now matches
//  FloatingTransportBar content-for-content and colour-for-colour (same
//  vector-icon transport cluster — see TransportIconButton.h, which
//  replaced the old |</<</>/[]/REC/LOOP ASCII-glyph TextButtons — and
//  per-button tints, same amber zero-padded bar.beat.tick position readout,
//  same editable/wheel-scrollable L/R cycle locators, same BPM/GRID/LINK
//  trio) rather than the previous unrelated BACK/PLAY/STOP/REC/LOOP
//  text-button + plain-text BPM design. Colours come from MetroColours directly rather than
//  DysektLookAndFeel's theme, for the same reason FloatingTransportBar.cpp
//  does that (see its own top-of-file comment) — this row and its floating
//  twin now share one chrome palette, so docking/undocking never changes
//  how the transport looks, only where it lives.
//
//  Deliberately still ONE row at the same fixed height its host reserves
//  for it (kTransportH — 32px in PianoRollPanel; ArrangeView no longer uses
//  this class, see FloatingTransportBar's own docked mode for that) —
//  compact and self-contained, with no MIXER/ARRANGER/GLOBAL EQ switcher
//  and no Float button wiring beyond what setViewButtons()/onFloatRequested
//  already make optional for hosts (like PianoRollPanel) that never call
//  them.
//==============================================================================
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    /** Fired when the Float button is clicked. Owner (ArrangeView) decides what
     *  "floating" means — see ArrangeView::showFloatingTransport(). */
    std::function<void()> onFloatRequested;

    TransportBar (SequencerEngine& seq, AbletonLink* link = nullptr)
        : engine (seq), linkPtr (link)
    {
        setOpaque (true);

        // ── Transport cluster ───────────────────────────────────────────
        // Same glyphs, same order, same per-button tints as
        // FloatingTransportBar's transport cluster — see TransportIconButton.h
        // for the shared vector-icon glyph + hover/active-state chrome this
        // mirrors (Base::Surface off-fill, tint-at-32%-alpha on-fill,
        // tint-coloured icon off / white icon on).
        toStartBtn.configure (TransportIcons::Kind::ToStart, Accent::Purple,    "Return to start");
        backBtn.configure    (TransportIcons::Kind::Back,    Accent::Yellow,    "Step back one bar");
        playBtn.configure    (TransportIcons::Kind::Play,    Transport::Play,   "Play");
        stopBtn.configure    (TransportIcons::Kind::Stop,    Accent::Orange,    "Stop");
        recBtn.configure     (TransportIcons::Kind::Record,  Transport::Record, "Record");
        loopBtn.configure    (TransportIcons::Kind::Loop,    Accent::Cyan,      "Toggle looping");
        playBtn.setClickingTogglesState (true);
        recBtn.setClickingTogglesState (true);
        loopBtn.setClickingTogglesState (true);
        for (auto* b : { &toStartBtn, &backBtn, &playBtn, &stopBtn, &recBtn, &loopBtn })
            addAndMakeVisible (*b);

        configureChrome (floatBtn, "FLOAT", "Detach the transport into a floating panel");
        floatBtn.onClick = [this] { if (onFloatRequested) onFloatRequested(); };
        addAndMakeVisible (floatBtn);

        toStartBtn.onClick = [this] { engine.rewind(); };
        backBtn.onClick = [this]
        {
            const auto barTicks = MidiClip::kPPQ * 4;
            const auto current  = engine.getPlayheadTick();
            engine.seekToTick (juce::jmax<int64_t> (0, current - barTicks));
        };
        playBtn.onClick = [this] { engine.play(); playBtn.setToggleState (engine.isPlaying(), juce::dontSendNotification); };
        stopBtn.onClick = [this] { engine.stop(); };
        recBtn.onStateChange  = [this] { engine.setRecording (recBtn.getToggleState()); };
        loopBtn.onStateChange = [this] { engine.setLooping   (loopBtn.getToggleState()); };

        // ── Musical position — amber, zero-padded bar.beat.tick, same
        // format as FloatingTransportBar::formatMusicalPosition() ────────
        posLabel.setJustificationType (juce::Justification::centred);
        posLabel.setFont (DysektLookAndFeel::makeMonoFont (14.0f, true));
        posLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        posLabel.setColour (juce::Label::textColourId, Accent::Orange);
        posLabel.setTooltip ("Playhead position — scroll over a segment to nudge it");
        posLabel.onSegmentScroll = [this] (int segment, int direction) { adjustPlayhead (segment, direction); };
        addAndMakeVisible (posLabel);

        // ── L/R cycle locators — editable + segment-wheel-scrollable,
        // same MusicalPositionLabel behaviour FloatingTransportBar's
        // leftLocatorLabel/rightLocatorLabel use (see that class's nested
        // MusicalPositionLabel for the segment math this mirrors) ────────
        leftLocatorTick  = engine.getLoopStartTick();
        rightLocatorTick = engine.getLoopEndTick();
        for (auto* field : { &leftLocatorLabel, &rightLocatorLabel })
        {
            field->setEditable (true, true, false);
            field->setJustificationType (juce::Justification::centred);
            field->setFont (DysektLookAndFeel::makeMonoFont (11.0f));
            field->setColour (juce::Label::backgroundColourId, Base::Surface);
            field->setColour (juce::Label::textColourId, Text::Primary);
            field->setTooltip ("Cycle locator — click to type bars.beats.ticks, "
                               "or scroll over a segment to nudge it");
            field->onTextChange = [this] { updateLocatorsFromEditors(); };
            addAndMakeVisible (*field);
        }
        leftLocatorLabel.onSegmentScroll  = [this] (int segment, int direction) { adjustLeftLocator (segment, direction); };
        rightLocatorLabel.onSegmentScroll = [this] (int segment, int direction) { adjustRightLocator (segment, direction); };

        // ── BPM ──────────────────────────────────────────────────────────
        bpmLabel.setFont (DysektLookAndFeel::makeMonoFont (13.f, true));
        bpmLabel.setJustificationType (juce::Justification::centred);
        bpmLabel.setEditable (true, true, false);
        bpmLabel.setColour (juce::Label::backgroundColourId, Base::Surface);
        bpmLabel.setColour (juce::Label::textColourId, Transport::Tempo);
        bpmLabel.setTooltip ("Tempo in beats per minute (20-999)");
        bpmLabel.onEditorShow = [this]
        {
            if (auto* ed = bpmLabel.getCurrentTextEditor())
            {
                ed->setColour (juce::TextEditor::backgroundColourId, Base::Elevated);
                ed->setColour (juce::TextEditor::textColourId,       Transport::Tempo);
                ed->setInputRestrictions (6, "0123456789.");
            }
        };
        bpmLabel.onTextChange = [this]
        {
            float v = bpmLabel.getText().getFloatValue();
            if (v >= 20.f && v <= 999.f) engine.setBpm (v);
        };
        addAndMakeVisible (bpmLabel);

        // ── Grid snap — same item list FloatingTransportBar's gridCombo
        // uses (no "Free" entry there); kept here too, since dropping an
        // existing, useful snap mode isn't something "look the same" asks
        // for — this combo's appearance now matches, its contents didn't
        // need to shrink to match. ───────────────────────────────────────
        snapCombo.addItem ("1/1",  1);
        snapCombo.addItem ("1/2",  2);
        snapCombo.addItem ("1/4",  3);
        snapCombo.addItem ("1/8",  4);
        snapCombo.addItem ("1/16", 5);
        snapCombo.addItem ("1/32", 6);
        snapCombo.addItem ("Free", 7);
        snapCombo.setSelectedId (4, juce::dontSendNotification);
        snapCombo.setColour (juce::ComboBox::backgroundColourId, Base::Elevated);
        snapCombo.setColour (juce::ComboBox::textColourId, Text::Primary);
        snapCombo.setColour (juce::ComboBox::outlineColourId, Base::Border);
        addAndMakeVisible (snapCombo);

        // ── Ableton Link ─────────────────────────────────────────────────
        if (linkPtr != nullptr)
        {
            configureChrome (linkBtn, "LINK", "Toggle Ableton Link");
            linkBtn.setClickingTogglesState (true);
            linkBtn.setColour (juce::TextButton::buttonOnColourId, Accent::Purple.withAlpha (0.35f));
            linkBtn.onStateChange = [this] { if (linkPtr) linkPtr->setEnabled (linkBtn.getToggleState()); };
            addAndMakeVisible (linkBtn);
        }

        startTimerHz (20);
    }

    ~TransportBar() override { stopTimer(); }

    /** Docks (or undocks, with nullptrs) external view-switcher buttons — e.g.
     *  the Mixer / Arranger toggle — into the far left of the transport row.
     *  Ownership stays with the caller; TransportBar only reparents + positions. */
    void setViewButtons (juce::TextButton* mixerBtn, juce::TextButton* arrangeBtn, juce::TextButton* eqBtn)
    {
        viewMixerBtn   = mixerBtn;
        viewArrangeBtn = arrangeBtn;
        viewEqBtn      = eqBtn;
        if (viewMixerBtn   != nullptr) addAndMakeVisible (*viewMixerBtn);
        if (viewArrangeBtn != nullptr) addAndMakeVisible (*viewArrangeBtn);
        if (viewEqBtn      != nullptr) addAndMakeVisible (*viewEqBtn);
        resized();
    }

    //==========================================================================
    int64_t getSnapTicks() const
    {
        const int64_t ppq = MidiClip::kPPQ;
        switch (snapCombo.getSelectedId())
        {
            case 1: return ppq * 4;
            case 2: return ppq * 2;
            case 3: return ppq;
            case 4: return ppq / 2;
            case 5: return ppq / 4;
            case 6: return ppq / 8;
            default: return 0;
        }
    }

    static int64_t snapTick (int64_t tick, int64_t snapTicks) noexcept
    {
        if (snapTicks <= 0) return tick;
        return ((tick + snapTicks / 2) / snapTicks) * snapTicks;
    }

    //==========================================================================
    void resized() override
    {
        auto b = getLocalBounds().reduced (4, 0);
        const int contentH = juce::jmax (1, b.getHeight() - 4);
        const int contentY = (getHeight() - contentH) / 2;
        b.setY (contentY);
        b.setHeight (contentH);
        const int btnH    = contentH;
        const int transBtnW = 34;   // compact — 6 of these plus everything
                                     // else still needs to fit one 32px row
        const int floatW  = 46;
        const int bpmW    = 56;
        const int snapW   = 58;
        const int posW    = 92;     // fits "NNN.NN.NNN" at 14pt mono
        const int locW    = 78;     // fits "L NNN.NN.NNN" at 11pt mono
        const int linkW   = 48;
        const int gap     = 3;

        // ── Far left: view switcher (Mixer / Arranger / EQ), when docked ──
        if (viewMixerBtn != nullptr || viewArrangeBtn != nullptr || viewEqBtn != nullptr)
        {
            constexpr int arrangeWidth = 92;
            constexpr int mixerWidth   = 70;
            constexpr int eqWidth      = 76;
            auto left = b.removeFromLeft (mixerWidth + gap + arrangeWidth + gap + eqWidth);
            if (viewMixerBtn != nullptr)
                viewMixerBtn->setBounds (left.removeFromLeft (mixerWidth));
            left.removeFromLeft (gap);
            if (viewArrangeBtn != nullptr)
                viewArrangeBtn->setBounds (left.removeFromLeft (arrangeWidth));
            left.removeFromLeft (gap);
            if (viewEqBtn != nullptr)
                viewEqBtn->setBounds (left.removeFromLeft (eqWidth));
            b.removeFromLeft (gap * 3);
        }

        // ── Far right: FLOAT -> LINK -> GRID -> BPM ────────────────────────
        floatBtn.setBounds (b.removeFromRight (floatW)); b.removeFromRight (gap * 2);
        if (linkPtr != nullptr)
        {
            linkBtn.setBounds (b.removeFromRight (linkW));
            b.removeFromRight (gap);
        }
        snapCombo.setBounds (b.removeFromRight (snapW)); b.removeFromRight (gap * 2);
        bpmLabel .setBounds (b.removeFromRight (bpmW));  b.removeFromRight (gap * 2);

        // ── Left of remaining space: transport cluster, position, locators —
        // same left-to-right order as FloatingTransportBar's content row
        // (position + transport, then L/R locators), just one row instead
        // of stacked, and everything narrower to match. ───────────────────
        const int y = b.getY();
        int x = b.getX();
        for (auto* btn : { &toStartBtn, &backBtn, &playBtn, &stopBtn, &recBtn, &loopBtn })
        {
            btn->setBounds (x, y, transBtnW, btnH);
            x += transBtnW + gap;
        }
        x += gap * 2;
        posLabel.setBounds (x, y, posW, btnH);
        x += posW + gap * 3;
        leftLocatorLabel.setBounds (x, y, locW, btnH);
        x += locW + gap;
        rightLocatorLabel.setBounds (x, y, locW, btnH);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Base::SurfaceAlt);
        g.setColour (Base::Border);
        g.fillRect (getLocalBounds().removeFromBottom (1));

        // Locator captions, painted the same way FloatingTransportBar's
        // paint() draws its own "L"/"R" — outside the editable field so the
        // numeric value inside stays centred.
        g.setColour (Text::Muted);
        g.setFont (MetroTypography::caption());
        g.drawText ("L", leftLocatorLabel.getBounds().translated (-14, 0).withWidth (14),
                    juce::Justification::centred);
        g.drawText ("R", rightLocatorLabel.getBounds().translated (-14, 0).withWidth (14),
                    juce::Justification::centred);
    }

private:
    SequencerEngine&  engine;
    AbletonLink*      linkPtr = nullptr;

    TransportIconButton toStartBtn, backBtn, playBtn, stopBtn, recBtn, loopBtn;
    juce::TextButton  floatBtn { "FLOAT" };
    juce::TextButton  linkBtn  { "LINK" };
    juce::Label       bpmLabel;
    juce::ComboBox    snapCombo;

    /** Same segment-scrollable bar.beat.tick label FloatingTransportBar's
     *  MusicalPositionLabel is — duplicated here rather than shared, same
     *  as the rest of this class's relationship to that one (see this
     *  class's header comment): each owns its own layout entirely, and the
     *  two were never sharing implementation even before this pass. */
    class MusicalPositionLabel final : public juce::Label
    {
    public:
        std::function<void (int segment, int direction)> onSegmentScroll;

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (onSegmentScroll != nullptr && wheel.deltaY != 0.0f)
                onSegmentScroll (segmentAtX (e.x), wheel.deltaY > 0.0f ? 1 : -1);
            else
                Label::mouseWheelMove (e, wheel);
        }

    private:
        // Locates the wheel by measuring where the text's own two '.'
        // separators actually fall, rather than assuming a fixed digit
        // layout — this works whether the label carries a "L "/"R " prefix
        // (the locator fields) or none at all (the plain position readout).
        int segmentAtX (int x) const
        {
            const auto text = getText();
            if (text.isEmpty()) return 1;
            const int firstDot  = text.indexOfChar ('.');
            const int secondDot = firstDot < 0 ? -1 : text.indexOfChar (firstDot + 1, '.');
            if (firstDot < 0 || secondDot < 0) return 1;

            const auto f = getFont();
            const auto measure = [&f] (const juce::String& s) -> float
            {
                juce::GlyphArrangement ga;
                ga.addLineOfText (f, s, 0.0f, 0.0f);
                return ga.getBoundingBox (0, -1, true).getWidth();
            };
            const float totalW    = measure (text);
            const float startX    = ((float) getWidth() - totalW) * 0.5f;
            const float firstDotX  = startX + measure (text.substring (0, firstDot));
            const float secondDotX = startX + measure (text.substring (0, secondDot));
            const float fx = (float) x;
            if (fx < firstDotX)  return 0;
            if (fx < secondDotX) return 1;
            return 2;
        }
    };

    // Wheel-scrollable like the locator fields below — see MusicalPositionLabel;
    // scrolling over the bar/beat/tick digits nudges the playhead by that unit.
    MusicalPositionLabel posLabel;
    MusicalPositionLabel leftLocatorLabel, rightLocatorLabel;
    int64_t leftLocatorTick  = 0;
    int64_t rightLocatorTick = 0;

    // Externally-owned view switcher (Mixer / Arranger / Eq), docked in via setViewButtons().
    juce::TextButton* viewMixerBtn   = nullptr;
    juce::TextButton* viewArrangeBtn = nullptr;
    juce::TextButton* viewEqBtn      = nullptr;

    static void configureChrome (juce::TextButton& b, const juce::String& text, const juce::String& tooltip)
    {
        b.setButtonText (text);
        b.setTooltip (tooltip);
        b.setColour (juce::TextButton::buttonColourId, Base::Elevated);
        b.setColour (juce::TextButton::textColourOffId, Text::Secondary);
        b.setColour (juce::TextButton::textColourOnId, Base::White);
        // Without this, DysektLookAndFeel::drawButtonBackground's default
        // fill path ignores buttonColourId entirely and always paints
        // theme.button/theme.accent instead — see this class's header
        // comment. Same fix TrackInspector's track-colour swatches needed.
        b.getProperties().set ("flatFill", true);
    }

    static int64_t segmentStepTicks (int segment) noexcept
    {
        switch (segment)
        {
            case 0:  return MidiClip::kPPQ * 4;
            case 1:  return MidiClip::kPPQ;
            default: return juce::jmax<int64_t> (1, MidiClip::kPPQ / 32);
        }
    }

    void setLeftLocatorToPlayhead()
    {
        leftLocatorTick = engine.getPlayheadTick();
        if (rightLocatorTick <= leftLocatorTick)
            rightLocatorTick = leftLocatorTick + MidiClip::kPPQ;
        engine.setLoopRange (leftLocatorTick, rightLocatorTick);
    }

    void adjustLeftLocator (int segment, int direction)
    {
        const int64_t delta = segmentStepTicks (segment) * (int64_t) direction;
        leftLocatorTick = juce::jmax<int64_t> (0, leftLocatorTick + delta);
        if (rightLocatorTick <= leftLocatorTick)
            rightLocatorTick = leftLocatorTick + MidiClip::kPPQ;
        engine.setLoopRange (leftLocatorTick, rightLocatorTick);
    }

    void adjustRightLocator (int segment, int direction)
    {
        const int64_t delta = segmentStepTicks (segment) * (int64_t) direction;
        rightLocatorTick = juce::jmax<int64_t> (leftLocatorTick + MidiClip::kPPQ, rightLocatorTick + delta);
        engine.setLoopRange (leftLocatorTick, rightLocatorTick);
    }

    void adjustPlayhead (int segment, int direction)
    {
        const int64_t delta   = segmentStepTicks (segment) * (int64_t) direction;
        const int64_t newTick = juce::jmax<int64_t> (0, engine.getPlayheadTick() + delta);
        engine.seekToTick (newTick);
    }

    void updateLocatorsFromEditors()
    {
        const auto left  = parseMusicalPosition (leftLocatorLabel.getText());
        const auto right = parseMusicalPosition (rightLocatorLabel.getText());
        if (left < 0 || right < 0) return;
        leftLocatorTick  = left;
        rightLocatorTick = juce::jmax (right, leftLocatorTick + (int64_t) MidiClip::kPPQ);
        engine.setLoopRange (leftLocatorTick, rightLocatorTick);
    }

    static int64_t parseMusicalPosition (const juce::String& text)
    {
        auto value = text.trim().removeCharacters ("LRlr ");
        const auto parts = juce::StringArray::fromTokens (value, ".", "");
        if (parts.size() != 3) return -1;
        const int bar = parts[0].getIntValue();
        const int beat = parts[1].getIntValue();
        const int tick = parts[2].getIntValue();
        if (bar < 1 || beat < 1 || beat > 4 || tick < 0 || tick >= MidiClip::kPPQ) return -1;
        return ((int64_t) (bar - 1) * 4 + (beat - 1)) * MidiClip::kPPQ + tick;
    }

    static juce::String formatMusicalPosition (double beats)
    {
        if (beats < 0.0) beats = 0.0;
        const int bar  = (int) (beats / 4.0) + 1;
        const int beat = (int) std::fmod (beats, 4.0) + 1;
        const int tick = (int) (std::fmod (beats, 1.0) * (double) MidiClip::kPPQ);
        return juce::String::formatted ("%03d.%02d.%03d", bar, beat, tick);
    }

    void timerCallback() override
    {
        const bool playing = engine.isPlaying();
        if (playBtn.getToggleState() != playing)
            playBtn.setToggleState (playing, juce::dontSendNotification);

        const bool recording = engine.isRecording();
        if (recBtn.getToggleState() != recording)
            recBtn.setToggleState (recording, juce::dontSendNotification);

        const bool looping = engine.isLooping();
        if (loopBtn.getToggleState() != looping)
            loopBtn.setToggleState (looping, juce::dontSendNotification);

        if (! bpmLabel.isBeingEdited())
        {
            juce::String s = juce::String (engine.getBpm(), 2);
            if (bpmLabel.getText() != s) bpmLabel.setText (s, juce::dontSendNotification);
        }

        posLabel.setText (formatMusicalPosition (engine.getPlayheadBeats()), juce::dontSendNotification);

        if (! leftLocatorLabel.isBeingEdited())
            leftLocatorLabel.setText ("L " + formatMusicalPosition ((double) leftLocatorTick / (double) MidiClip::kPPQ),
                                      juce::dontSendNotification);
        if (! rightLocatorLabel.isBeingEdited())
            rightLocatorLabel.setText ("R " + formatMusicalPosition ((double) rightLocatorTick / (double) MidiClip::kPPQ),
                                       juce::dontSendNotification);

        if (linkPtr != nullptr)
        {
            const int peers = linkPtr->getPeerCount();
            juce::String ls = peers > 0 ? ("LINK " + juce::String (peers)) : "LINK";
            if (linkBtn.getButtonText() != ls) linkBtn.setButtonText (ls);
            linkBtn.setToggleState (linkPtr->isEnabled(), juce::dontSendNotification);
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};
