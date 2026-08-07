/*
    DYSEKT 2

    FloatingTransportBar.h

    A detachable transport panel: cycle/loop, tempo, musical position, the
    transport button cluster, locators, grid snap, and Ableton Link — the
    same set of controls the docked TransportBar shows under the arranger,
    but able to leave the main window entirely and float as its own
    borderless panel that remembers where it was left.

    Ported from the (never-instantiated) metro/ UI shell — see
    ArrangeView::showFloatingTransport()/dockTransport() for the live
    wiring, and TransportBar's onFloatRequested for the docked "Float"
    button that triggers it.

    Deliberately does NOT wrap juce::DocumentWindow (see SlotWindow.h /
    PianoRollPanel.h for that shape elsewhere in the app). Those give the OS
    title bar; this panel needs a custom strip — grip dots, a pin toggle, a
    dock button — matching the reference mockup, so it owns its chrome and
    drags itself via ComponentDragger instead.
*/
#pragma once


#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/AbletonLink.h"


class SequencerEngine;


/**
    FloatingTransportBar


    Usage:
        floatingTransport = std::make_unique<FloatingTransportBar> (engine, linkPtr);
        floatingTransport->onDockRequested = [this] { showDocked(); undock.reset(); };
        floatingTransport->show();   // adds itself to the desktop at its last position

    The panel is a plain juce::Component the whole time — show()/hide() decide
    whether it is currently living on the desktop as its own top-level window
    or sitting invisible, so callers never juggle two different objects for
    the docked and floating states.
*/
class FloatingTransportBar final : public juce::Component,
                                   private juce::Timer
{
public:
 explicit FloatingTransportBar (SequencerEngine& sequencer, AbletonLink* link = nullptr);
 ~FloatingTransportBar() override;


 /** Adds this component to the desktop as a borderless floating window at
        its last remembered screen position (or a sensible default the first
        time), and brings it to the front. */
 void show();


 /** Removes this component from the desktop, saving its current position
        for next time. The component itself is not destroyed. */
 void hide();


 bool isFloating() const noexcept { return isOnDesktop(); }


 /** Grid-snap resolution selected in the GRID combo, in ticks (MidiClip::kPPQ
     *  units) — same mapping TransportBar::getSnapTicks() uses for its own
     *  snapCombo, so the two controls behave identically. 0 means no snap. */
 int64_t getSnapTicks() const;


 /** Fired when the user double-clicks the title strip, or presses the
        dock button — the host owns what "docking" means (e.g. re-parenting
        this component back inline, or simply hiding it in favour of an
        already-docked MetroTransportBar). This class only manages its own
        desktop window lifecycle; it never re-parents itself. */
    std::function<void()> onDockRequested;

    /** Fired when the MIXER / ARRANGER / GLOBAL EQ title-strip buttons are
        clicked — the host (ArrangeView, forwarding from SlotWindowContent)
        decides what showing that view means; this panel only reports the
        request. */
    std::function<void()> onMixerRequested;
    std::function<void()> onArrangerRequested;
    std::function<void()> onGlobalEqRequested;


 void paint (juce::Graphics&) override;
 void resized() override;


 void mouseDown (const juce::MouseEvent&) override;
 void mouseDrag (const juce::MouseEvent&) override;
 void mouseUp (const juce::MouseEvent&) override;
 void mouseDoubleClick (const juce::MouseEvent&) override;


private:
 //==========================================================================
 //  Layout regions, computed once per resized() and reused by paint()
 //==========================================================================
 //  A single content row below the title strip, left to right:
 //    musical position + transport cluster | editable L/R locators | BPM / GRID / LINK
 struct Layout
    {
        juce::Rectangle<int> titleStrip;
        juce::Rectangle<int> mixerButtonField, arrangeButtonField, eqButtonField;
        juce::Rectangle<int> positionField;
        juce::Rectangle<int> locatorsField;
        juce::Rectangle<int> transportRow;
        juce::Rectangle<int> setLeftButton, setRightButton;
        juce::Rectangle<int> tempoCaption, tempoField;
        juce::Rectangle<int> gridField;
        juce::Rectangle<int> linkField;
 int divider1 = 0, divider2 = 0;
    };
    Layout computeLayout() const;


 /** An editable "003.02.120" (bar.beat.tick) label whose three components
     *  can each be mouse-wheel-scrolled independently — hovering over the
     *  bar digits and scrolling steps by a bar, over the beat digits steps
     *  by a beat, over the tick digits steps by a tick — without disturbing
     *  the other two. Text editing (click to type a value) still works
     *  exactly as juce::Label already provides; this only adds the wheel
     *  behaviour on top. Assumes a monospaced font and a fixed "P NNN.NN.NNN"
     *  layout (P = the "L "/"R " prefix this panel always sets), which is
     *  all this class's owner ever puts in it. */
    class MusicalPositionLabel final : public juce::Label
    {
    public:
        /** Fired on each wheel notch: which segment (0 = bar, 1 = beat,
         *  2 = tick) the cursor was over, and a +1/-1 direction. The owner
         *  decides the tick step for each segment and applies/clamps the
         *  result — this class only reports where the wheel happened. */
        std::function<void (int segment, int direction)> onSegmentScroll;

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (onSegmentScroll != nullptr && wheel.deltaY != 0.0f)
                onSegmentScroll (segmentAtX (e.x), wheel.deltaY > 0.0f ? 1 : -1);
            else
                Label::mouseWheelMove (e, wheel);
        }

    private:
        int segmentAtX (int x) const
        {
            const auto text = getText();
            if (text.isEmpty()) return 1;
            const auto f = getFont();
            const auto measure = [&f] (const juce::String& s) -> float
            {
                juce::GlyphArrangement ga;
                ga.addLineOfText (f, s, 0.0f, 0.0f);
                return ga.getBoundingBox (0, -1, true).getWidth();
            };
            const float totalW = measure (text);
            const float startX = ((float) getWidth() - totalW) * 0.5f;
            const float charW  = measure ("0");
            if (charW <= 0.0f) return 1;
            const int idx = (int) (((float) x - startX) / charW);
            // "P NNN.NN.NNN": idx 0-4 = prefix+space+bar, 5-8 = '.'+beat+'.', 9+ = tick.
            if (idx <= 4) return 0;
            if (idx <= 8) return 1;
            return 2;
        }
    };

    /** Tick step for one wheel notch on a given locator segment (0=bar,
     *  1=beat, 2=tick). Bar/beat steps are exact musical units; the tick
     *  step is a coarse-enough fraction of a beat to feel deliberate rather
     *  than needing hundreds of notches to move anywhere. */
    static int64_t segmentStepTicks (int segment) noexcept;


 void timerCallback() override;
 void updateTempoFromEditor();
 void setLeftLocatorToPlayhead();
 void setRightLocatorToPlayhead();
 void updateLocatorsFromEditors();
 void adjustLeftLocator (int segment, int direction);
 void adjustRightLocator (int segment, int direction);
 static juce::String formatMusicalPosition (double beats);
 static int64_t parseMusicalPosition (const juce::String& text);


 static juce::File getPositionFile();
 void restorePosition();
 void savePosition() const;


    SequencerEngine& engine;
    AbletonLink*     linkPtr = nullptr;


 // ── Title strip chrome ──────────────────────────────────────────────
    juce::TextButton pinButton   { "Pin" };
    juce::TextButton dockButton  { "Dock" };
    juce::TextButton mixerButton   { "Mixer" };
    juce::TextButton arrangeButton { "Arranger" };
    juce::TextButton eqButton      { "Global Eq" };
    juce::ComponentDragger dragger;
 bool draggingTitleStrip = false;


 // ── Bottom row: transport controls ──────────────────────────────────
    juce::TextButton toStartButton { "|<" };
    juce::TextButton backButton    { "<<" };
    juce::TextButton playButton    { ">" };
    juce::TextButton stopButton    { "[]" };
    juce::TextButton recordButton  { "REC" };
    juce::TextButton cycleButton   { "LOOP" };
    juce::TextButton setLeftButton  { "SET LEFT" };
    juce::TextButton setRightButton { "SET RIGHT" };


 // ── Top row: individually editable, centred L/R values ──────────────
    juce::Label positionLabel;
    MusicalPositionLabel leftLocatorLabel;
    MusicalPositionLabel rightLocatorLabel;
 int64_t leftLocatorTick  = 0;
 int64_t rightLocatorTick = 0;


 // ── Far right: BPM, grid snap, link — one row, in that order ───────
    juce::Label      tempoLabel;
    juce::ComboBox   gridCombo;
    juce::TextButton linkButton { "LINK" };


 JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloatingTransportBar)
};
