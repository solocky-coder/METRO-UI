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

    /** Fired when the user double-clicks the title strip, or presses the
        dock button — the host owns what "docking" means (e.g. re-parenting
        this component back inline, or simply hiding it in favour of an
        already-docked MetroTransportBar). This class only manages its own
        desktop window lifecycle; it never re-parents itself. */
    std::function<void()> onDockRequested;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    //==========================================================================
    //  Layout regions, computed once per resized() and reused by paint()
    //==========================================================================
    //  Two content rows below the title strip:
    //    Row 1: transport cluster + SET LEFT/RIGHT
    //    Row 2: musical position + editable L/R locators
    //            | far-right BPM / GRID / LINK
    struct Layout
    {
        juce::Rectangle<int> titleStrip;
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

    void timerCallback() override;
    void updateTempoFromEditor();
    void setLeftLocatorToPlayhead();
    void setRightLocatorToPlayhead();
    void updateLocatorsFromEditors();
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
    juce::ComponentDragger dragger;

    // ── Row 1: filled transport icons, plus locator capture buttons ─────
    juce::TextButton toStartButton { "▮◀" };
    juce::TextButton backButton    { "◀◀" };
    juce::TextButton playButton    { "▶" };
    juce::TextButton stopButton    { "■" };
    juce::TextButton recordButton  { "●" };
    juce::TextButton cycleButton   { "⟳" };
    juce::TextButton setLeftButton  { "SET LEFT" };
    juce::TextButton setRightButton { "SET RIGHT" };

    // ── Row 2: position plus individually editable, centered L/R values ─
    juce::Label positionLabel;
    juce::Label leftLocatorLabel;
    juce::Label rightLocatorLabel;
    int64_t leftLocatorTick  = 0;
    int64_t rightLocatorTick = 0;

    // ── Far right: BPM, grid snap, link — one row, in that order ───────
    juce::Label      tempoLabel;
    juce::ComboBox   gridCombo;
    juce::TextButton linkButton { "LINK" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloatingTransportBar)
};
