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
#include "TransportIconButton.h"
#include "ContextMenuButton.h"


class SequencerEngine;


/**
    FloatingTransportBar


    One component, two presentations — docked (parented inline into the host,
    stretched to whatever width it's given, FLOAT button in the title strip)
    and floating (a real desktop window at a fixed size, PIN/DOCK buttons
    instead). setDocked() switches between the two; the transport content
    itself — glyphs, colours, position readout, locators, BPM/GRID/LINK — is
    identical either way, since it's the same computeLayout()/resized()/
    paint() doing the drawing in both cases. This replaces the old split
    where ArrangeView's docked bar (TransportBar) was a second, hand-mirrored
    implementation of this one's look.

    Usage — docked (e.g. ArrangeView):
        transport.setDocked (true);
        transport.setViewButtons (&mixerBtn, &arrangeBtn, &eqBtn);
        transport.onFloatRequested = [this] { transport.setDocked (false); transport.show(); };
        addAndMakeVisible (transport);   // host positions it via setBounds() as normal

    Usage — floating:
        floatingTransport->onDockRequested = [this] { floatingTransport->hide();
                                                        floatingTransport->setDocked (true); }; // re-parent
        floatingTransport->show();   // adds itself to the desktop at its last position

    show()/hide() decide whether the component is currently living on the
    desktop as its own top-level window or sitting invisible; setDocked()
    is orthogonal to that and only affects layout/chrome — the host is still
    responsible for actually parenting/unparenting the component when
    switching between the two (see ArrangeView::showFloatingTransport()/
    dockTransport()).
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

 /** Switches between docked chrome (FLOAT button, no drag grip, content
     *  row centred to fill whatever width the host gives it) and floating
     *  chrome (PIN/DOCK buttons, drag-grip title strip, content row
     *  left-aligned to exactly fill this panel's own fixed size). Purely a
     *  layout/painting switch — does not itself reparent the component or
     *  touch the desktop; see this class's header comment. */
    void setDocked (bool isDocked)
    {
        if (docked == isDocked) return;
        docked = isDocked;
        setMouseCursor (docked ? juce::MouseCursor::NormalCursor
                                : juce::MouseCursor::DraggingHandCursor);
        pinButton.setVisible (! docked);
        dockButton.setVisible (! docked);
        floatButton.setVisible (docked);
        syncViewButtons();
        resized();
        repaint();
    }

    bool isDockedMode() const noexcept { return docked; }

    /** Docks (or undocks, with nullptrs) external view-switcher buttons —
     *  e.g. ArrangeView's Mixer/Arranger/EQ toggle — into the title strip's
     *  left side in place of this panel's own internal mixer/arrange/eq
     *  buttons. Only takes effect while docked (see setDocked()) — while
     *  floating, this panel always shows its own internal buttons, since a
     *  desktop window can't share components with the main window. Ownership
     *  of the passed buttons stays with the caller; this class only
     *  reparents + positions them, same contract TransportBar::setViewButtons()
     *  used to have. */
    void setViewButtons (juce::TextButton* mixerBtn, juce::TextButton* arrangeBtn, juce::TextButton* eqBtn)
    {
        viewMixerBtn   = mixerBtn;
        viewArrangeBtn = arrangeBtn;
        viewEqBtn      = eqBtn;
        for (auto* button : { viewMixerBtn, viewArrangeBtn, viewEqBtn })
            if (button != nullptr)
                button->getProperties().set ("transportFontSize", 26.0);
        syncViewButtons();
        resized();
    }

    /** Fired when the docked FLOAT button is clicked. Only shown/active while
     *  docked — see setDocked(). The host decides what "floating" means
     *  (typically setDocked(false) followed by show()). */
    std::function<void()> onFloatRequested;

 /** Grid-snap resolution selected in the GRID combo, in ticks (MidiClip::kPPQ
     *  units) — same mapping TransportBar::getSnapTicks() uses for its own
     *  snapCombo, so the two controls behave identically. 0 means no snap. */
 int64_t getSnapTicks() const;

 /** Item id of the currently selected grid-snap resolution — 1="1/1" through
     *  6="1/32", the same ids gridCombo/gridButtons use internally. Lets an
     *  external control (e.g. ArrangeView's own quantize buttons, drawn in
     *  its ARRANGE header rather than this panel) mirror the shared snap
     *  selection without reaching into gridCombo directly. */
 int getSnapItemId() const noexcept { return gridCombo.getSelectedId(); }

 /** Sets the grid-snap selection directly by item id (see getSnapItemId()),
     *  same effect as picking it from gridCombo or clicking a gridButtons
     *  entry — gridCombo stays the single source of truth either way (see
     *  this class's header comment), so every other snap-aware control,
     *  including ArrangeView's quantize buttons, updates in lockstep. */
 void setSnapItemId (int itemId) { gridCombo.setSelectedId (itemId, juce::sendNotificationSync); }


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
        juce::Rectangle<int> pinField, dockField, floatField;   // right side of titleStrip — pin+dock while floating, float while docked
        juce::Rectangle<int> positionField;
        juce::Rectangle<int> locatorsField;
        juce::Rectangle<int> transportRow;
        juce::Rectangle<int> setLeftButton, setRightButton;
        juce::Rectangle<int> tempoCaption, tempoField;
        juce::Rectangle<int> gridField;         // compact dropdown slot — used when there isn't room for gridButtonsField
        juce::Rectangle<int> gridButtonsField;  // wider radio-button slot — used when there is
        bool gridButtonsFit = false;            // which of the two the row actually has room for
        juce::Rectangle<int> linkField;
 int divider0 = 0, divider1 = 0, divider2 = 0;
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

    /** A juce::Label that also responds to mouse-wheel scrolling, instead
     *  of leaving the event unhandled — which is what plain juce::Label
     *  does, since it has no wheel handling of its own — for an ancestor
     *  Viewport (the arranger's horizontal scroll area, when docked) to
     *  pick up and pan the timeline with instead. Used for tempoLabel so
     *  scrolling over the BPM field adjusts the tempo rather than moving
     *  the arranger. */
    class WheelAdjustableLabel final : public juce::Label
    {
    public:
        /** Fired on each wheel notch with a +1/-1 direction; not fired
         *  while the label is being text-edited, so an in-progress typed
         *  value can't be clobbered by an accidental scroll. */
        std::function<void (int direction)> onWheelStep;

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            if (onWheelStep != nullptr && wheel.deltaY != 0.0f && ! isBeingEdited())
                onWheelStep (wheel.deltaY > 0.0f ? 1 : -1);
            else
                Label::mouseWheelMove (e, wheel);
        }
    };

    /** Tick step for one wheel notch on a given locator segment (0=bar,
     *  1=beat, 2=tick). Bar/beat steps are exact musical units; the tick
     *  step is a coarse-enough fraction of a beat to feel deliberate rather
     *  than needing hundreds of notches to move anywhere. */
    static int64_t segmentStepTicks (int segment) noexcept;


 /** Makes the external Mixer/Arranger/GlobalEq switcher (set via
     *  setViewButtons()) and this panel's own internal one match the
     *  current docked flag: while docked, each non-null external button is
     *  (re)parented into this panel via addAndMakeVisible() and the
     *  internal buttons are hidden; while floating, external buttons are
     *  just hidden — left parented wherever they already are, since a
     *  floating desktop window can't host components owned by the main
     *  window — and the internal buttons are shown instead.
     *
     *  Shared by setDocked() and setViewButtons() so either one calling
     *  alone still leaves both sets of buttons in a consistent state: e.g.
     *  re-docking after a float with no fresh setViewButtons() call still
     *  reparents the external buttons back into this panel, rather than
     *  merely toggling setVisible() on whatever parent they were last
     *  shown in — which is what previously let an external button appear
     *  at a stale position from a different layout context, doubled up
     *  with this panel's own internal one. */
    void syncViewButtons()
    {
        for (auto* button : { viewMixerBtn, viewArrangeBtn, viewEqBtn })
        {
            if (button == nullptr)
                continue;
            if (docked)
                addAndMakeVisible (*button);
            else
                button->setVisible (false);
        }
        mixerButton.setVisible (! docked);
        arrangeButton.setVisible (! docked);
        eqButton.setVisible (! docked);
    }

 void timerCallback() override;
 void updateTempoFromEditor();
 void setLeftLocatorToPlayhead();
 void setRightLocatorToPlayhead();
 void showLinkContextMenu();
 void updateLocatorsFromEditors();
 void adjustLeftLocator (int segment, int direction);
 void adjustRightLocator (int segment, int direction);
 void adjustPlayhead (int segment, int direction);
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
    juce::TextButton floatButton { "Float" };   // docked-mode counterpart to pin/dock
    juce::TextButton mixerButton   { "Mixer" };
    juce::TextButton arrangeButton { "Arranger" };
    juce::TextButton eqButton      { "Global Eq" };
    juce::ComponentDragger dragger;
 bool draggingTitleStrip = false;
 bool docked = false;

 // Externally-owned view switcher, docked in via setViewButtons() — only
 // used while docked; see that method's comment.
 juce::TextButton* viewMixerBtn   = nullptr;
 juce::TextButton* viewArrangeBtn = nullptr;
 juce::TextButton* viewEqBtn      = nullptr;


 // ── Bottom row: transport controls ──────────────────────────────────
    TransportIconButton toStartButton;
    TransportIconButton backButton;
    TransportIconButton playButton;
    TransportIconButton stopButton;
    TransportIconButton recordButton;
    TransportIconButton cycleButton;
    juce::TextButton setLeftButton  { "SET LEFT" };
    juce::TextButton setRightButton { "SET RIGHT" };


 // ── Top row: individually editable, centred L/R values ──────────────
    // Wheel-scrollable like the locator fields below — see MusicalPositionLabel;
    // scrolling over the bar/beat/tick digits nudges the playhead by that unit.
    MusicalPositionLabel positionLabel;
    MusicalPositionLabel leftLocatorLabel;
    MusicalPositionLabel rightLocatorLabel;
 int64_t leftLocatorTick  = 0;
 int64_t rightLocatorTick = 0;


 // ── Far right: BPM, grid snap, link — one row, in that order ───────
    WheelAdjustableLabel tempoLabel;
    juce::ComboBox   gridCombo;

    // Same grid-snap choices as gridCombo, shown instead of it — one
    // exclusively-toggled button per option — whenever the row has enough
    // spare width to lay them out directly (see computeLayout()'s
    // gridButtonsFit). gridCombo stays the single source of truth for the
    // current selection either way (see getSnapTicks()); these buttons just
    // set its selected id and mirror it back in timerCallback().
    static constexpr int kNumGridOptions = 6;
    juce::TextButton gridButtons[kNumGridOptions];

    ContextMenuButton linkButton { "LINK" };

    // Beat-synced glow intensity for linkButton, recomputed each timerCallback()
    // tick from AbletonLink::getPhase (i.e. Link's own beat clock, not a local
    // BPM guess) — 1.0 right on the beat, decaying across the rest of it.
    // Painted as a soft halo around L.linkField in paint(); see timerCallback().
    float linkPulseAlpha = 0.0f;


 JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloatingTransportBar)
};
