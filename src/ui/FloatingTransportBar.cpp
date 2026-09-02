#include "FloatingTransportBar.h"
#include <cmath>
#include "../metro/MetroColours.h"
#include "../metro/MetroMetrics.h"
#include "../metro/MetroTypography.h"
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/MidiClip.h"
#include "DysektLookAndFeel.h"


using namespace dysekt::metro; // MetroColours' Base/Text/Accent/Transport + MetroMetrics/MetroTypography —
 // kept as this panel's own chrome palette, independent of whichever
 // main-window theme is active. See ArrangeView::showFloatingTransport().
namespace
{
    constexpr float transportTextSize = 26.0f; // Match the musical-position readouts.

 // Mirrors PluginEditor.cpp's getSettingsDir() convention (same app-data
 // folder, "DYSEKT-SF") without pulling in that file — this panel's
 // position is its own small piece of state, not part of settings.yaml.
    juce::File getSettingsDir()
    {
 return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("DYSEKT-SF");
    }


 void configureChrome (juce::TextButton& b, const juce::String& text, const juce::String& tooltip)
    {
        b.setButtonText (text);
        b.setTooltip (tooltip);
        b.setColour (juce::TextButton::buttonColourId, Base::Elevated);
        b.setColour (juce::TextButton::textColourOffId, Text::Secondary);
        b.setColour (juce::TextButton::textColourOnId, Base::White);
        // Without this, DysektLookAndFeel::drawButtonBackground's default
        // fill path (this panel never calls setLookAndFeel(), so it's
        // subject to that LookAndFeel like everything else in the app —
        // see PluginEditor.cpp's setDefaultLookAndFeel()) ignores
        // buttonColourId entirely and always paints theme.button/
        // theme.accent instead of the palette configured here. Latent bug
        // predating the docked-transport redesign pass — surfaced by
        // TransportBar.h now matching this file colour-for-colour, which
        // only means anything if this file's own colours actually render.
        b.getProperties().set ("flatFill", true);
    }


} // namespace


//==============================================================================
FloatingTransportBar::FloatingTransportBar (SequencerEngine& sequencer, AbletonLink* link)
    : engine (sequencer), linkPtr (link)
{
 setOpaque (true);
 // The exposed title strip is a drag handle whenever this component is on
 // the desktop; child controls keep their normal click behaviour.
 setMouseCursor (juce::MouseCursor::DraggingHandCursor);


 // ── Title strip chrome ───────────────────────────────────────────────
 configureChrome (pinButton, "PIN", "Keep this panel above other windows");
    pinButton.setClickingTogglesState (true);
    pinButton.onStateChange = [this] { setAlwaysOnTop (pinButton.getToggleState()); };
 addAndMakeVisible (pinButton);


 configureChrome (dockButton, "DOCK", "Dock the transport back into the main window");
    dockButton.onClick = [this] { if (onDockRequested) onDockRequested(); };
 addAndMakeVisible (dockButton);


 configureChrome (floatButton, "FLOAT", "Detach the transport into a floating panel");
    floatButton.getProperties().set ("transportFontSize", transportTextSize);
    floatButton.onClick = [this] { if (onFloatRequested) onFloatRequested(); };
    floatButton.setVisible (false);   // shown only once setDocked(true) is called
 addAndMakeVisible (floatButton);


 // ── View switcher — Mixer / Arranger / Global EQ ────────────────────────
 configureChrome (mixerButton, "MIXER", "Switch to the mixer");
 configureChrome (arrangeButton, "ARRANGER", "Switch to the arranger");
 configureChrome (eqButton, "GLOBAL EQ", "Switch to the global EQ");
 for (auto* b : { &mixerButton, &arrangeButton, &eqButton })
    {
        b->getProperties().set ("transportFontSize", transportTextSize);
        b->setColour (juce::TextButton::textColourOffId, Base::White);
        b->setColour (juce::TextButton::textColourOnId, Base::White);
    }
    mixerButton.onClick   = [this] { if (onMixerRequested)    onMixerRequested(); };
    arrangeButton.onClick = [this] { if (onArrangerRequested) onArrangerRequested(); };
    eqButton.onClick      = [this] { if (onGlobalEqRequested) onGlobalEqRequested(); };
 for (auto* b : { &mixerButton, &arrangeButton, &eqButton })
addAndMakeVisible (*b);


 // ── Tempo (BPM) — lives in the far-right BPM/GRID/LINK row ─────────────
    tempoLabel.setEditable (true, true, false);
    tempoLabel.setJustificationType (juce::Justification::centred);
    tempoLabel.setFont (DysektLookAndFeel::makeMonoFont (transportTextSize, true));
    tempoLabel.setColour (juce::Label::backgroundColourId, Base::Background);
    tempoLabel.setColour (juce::Label::textColourId, Transport::Tempo);
    tempoLabel.setTooltip ("Tempo in beats per minute (20-999)");
    tempoLabel.onEditorShow = [this]
    {
 if (auto* ed = tempoLabel.getCurrentTextEditor())
            ed->setInputRestrictions (6, "0123456789.");
    };
    tempoLabel.onTextChange = [this] { updateTempoFromEditor(); };
 addAndMakeVisible (tempoLabel);


 // ── Musical position ─────────────────────────────────────────────────
    positionLabel.setJustificationType (juce::Justification::centred);
    positionLabel.setFont (DysektLookAndFeel::makeMonoFont (26.0f, true));
    positionLabel.setColour (juce::Label::backgroundColourId, Base::Background);
    positionLabel.setColour (juce::Label::textColourId, Accent::Orange);
    positionLabel.setTooltip ("Playhead position — scroll over a segment to nudge it");
    positionLabel.onSegmentScroll = [this] (int segment, int direction) { adjustPlayhead (segment, direction); };
 addAndMakeVisible (positionLabel);


 // ── Transport cluster ────────────────────────────────────────────────
 // Vector icons via TransportIconButton — platform/typeface-independent,
 // so no risk of missing-glyph boxes on the detached desktop component
 // the way the old ASCII labels (this comment used to warn about) could
 // have been if the embedded UI typeface ever changed.
 toStartButton.configure (TransportIcons::Kind::ToStart, Accent::Purple,    "Return to start");
 backButton.configure    (TransportIcons::Kind::Back,    Accent::Yellow,    "Step back one bar");
 playButton.configure    (TransportIcons::Kind::Play,    Transport::Play,   "Play");
 stopButton.configure    (TransportIcons::Kind::Stop,    Accent::Orange,    "Stop");
 recordButton.configure  (TransportIcons::Kind::Record,  Transport::Record, "Record");
 cycleButton.configure   (TransportIcons::Kind::Loop,    Accent::Cyan,      "Toggle looping");
    playButton.setClickingTogglesState (true);
    recordButton.setClickingTogglesState (true);
    cycleButton.setClickingTogglesState (true);


    toStartButton.onClick = [this] { engine.rewind(); };
    backButton.onClick = [this]
    {
 const auto barTicks = MidiClip::kPPQ * 4;
 const auto current  = engine.getPlayheadTick();
        engine.seekToTick (juce::jmax<int64_t> (0, current - barTicks));
    };
    playButton.onClick = [this]
    {
        engine.play();
        playButton.setToggleState (engine.isPlaying(), juce::dontSendNotification);
    };
    stopButton.onClick   = [this] { engine.stop(); };
    recordButton.onStateChange = [this] { engine.setRecording (recordButton.getToggleState()); };
    cycleButton.onStateChange  = [this] { engine.setLooping (cycleButton.getToggleState()); };


 // Order matches most DAW transports (Cubase/Nuendo-style, per the rest of
 // this panel's iconography): to-start, rewind, play, stop, record, cycle.
 for (auto* b : { &toStartButton, &backButton, &playButton, &stopButton, &recordButton, &cycleButton })
 addAndMakeVisible (*b);


 // ── Locators ─────────────────────────────────────────────────────────
 // Separate editable fields keep both numbers centred and make the cycle
 // range directly writable in familiar bars.beats.ticks notation.
 for (auto* field : { &leftLocatorLabel, &rightLocatorLabel })
    {
        field->setEditable (true, true, false);
        field->setJustificationType (juce::Justification::centred);
        field->setFont (DysektLookAndFeel::makeMonoFont (26.0f, true)); // matches positionLabel's size/weight
        field->setColour (juce::Label::backgroundColourId, Base::Background);
        field->setColour (juce::Label::textColourId, Text::Primary);
        field->setTooltip ("Cycle locator — click to type bars.beats.ticks, "
                           "or scroll over a segment to nudge it");
        field->onTextChange = [this] { updateLocatorsFromEditors(); };
 addAndMakeVisible (*field);
    }
    leftLocatorLabel.onSegmentScroll  = [this] (int segment, int direction) { adjustLeftLocator (segment, direction); };
    rightLocatorLabel.onSegmentScroll = [this] (int segment, int direction) { adjustRightLocator (segment, direction); };


 // The locator values are written directly in the upper row, matching the
 // compact floating-transport layout. The legacy capture buttons remain
 // available for host integrations but are not shown in this panel.
 configureChrome (setLeftButton, "SET LEFT", "Set the left cycle locator to the playhead");
 configureChrome (setRightButton, "SET RIGHT", "Set the right locator to the playhead");
    setLeftButton.onClick  = [this] { setLeftLocatorToPlayhead(); };
    setRightButton.onClick = [this] { setRightLocatorToPlayhead(); };


 // ── Grid snap ────────────────────────────────────────────────────────
    gridCombo.addItem ("1/1",  1);
    gridCombo.addItem ("1/2",  2);
    gridCombo.addItem ("1/4",  3);
    gridCombo.addItem ("1/8",  4);
    gridCombo.addItem ("1/16", 5);
    gridCombo.addItem ("1/32", 6);
    gridCombo.setSelectedId (5, juce::dontSendNotification);
    gridCombo.getProperties().set ("transportFontSize", transportTextSize);
    gridCombo.setColour (juce::ComboBox::backgroundColourId, Base::Elevated);
    gridCombo.setColour (juce::ComboBox::textColourId, Text::Primary);
    gridCombo.setColour (juce::ComboBox::outlineColourId, Base::Border);
 addAndMakeVisible (gridCombo);


 // ── Ableton Link ─────────────────────────────────────────────────────
 if (linkPtr != nullptr)
    {
 configureChrome (linkButton, "LINK", "Toggle Ableton Link");
        linkButton.getProperties().set ("transportFontSize", transportTextSize);
        linkButton.setClickingTogglesState (true);
        linkButton.setColour (juce::TextButton::buttonOnColourId, Accent::Purple.withAlpha (0.35f));
        linkButton.onStateChange = [this] { if (linkPtr) linkPtr->setEnabled (linkButton.getToggleState()); };
 addAndMakeVisible (linkButton);
    }


    leftLocatorTick = engine.getLoopStartTick();
    rightLocatorTick = engine.getLoopEndTick();


 // Compact single-row transport: position + transport, locators, and
 // BPM/GRID/LINK laid out side by side (see computeLayout()).
 // 152 -> 160: computeLayout()'s undocked rightW grew by 8 grid units
 // (wider tempoField + roomier gaps between the BPM/GRID/LINK frames —
 // see the comment there), so the fixed floating-window width needs the
 // same increase or the left-anchored position/transport/locator block
 // would get squeezed short a grid unit.
 setSize (MetroMetrics::grid * 160, MetroMetrics::grid * 13); // widened for the larger L/R locator fields
 startTimerHz (20);
}


FloatingTransportBar::~FloatingTransportBar()
{
 stopTimer();
 if (isOnDesktop())
 savePosition();
}


//==============================================================================
void FloatingTransportBar::show()
{
 if (! isOnDesktop())
    {
 setOpaque (true);
 // NOT windowIsTemporary: that flag makes the OS treat this as a
 // transient popup (like a tooltip or menu), which auto-hides the
 // instant the main UI window is clicked/activated, and can also
 // break the drag capture below. This panel needs to behave like a
 // normal (if borderless) persistent floating window instead.
 addToDesktop (juce::ComponentPeer::windowHasDropShadow);
 restorePosition();
    }
 setVisible (true);
 toFront (true);
}


void FloatingTransportBar::hide()
{
 if (! isOnDesktop())
 return;


 savePosition();
 removeFromDesktop();
}


//==============================================================================
int64_t FloatingTransportBar::getSnapTicks() const
{
 // Same item-id -> tick mapping as TransportBar::getSnapTicks(); gridCombo
 // has no "Free" entry here, so an unrecognised/unset id also falls
 // through to 0 (no snap) rather than crashing on a missing case.
 const int64_t ppq = MidiClip::kPPQ;
 switch (gridCombo.getSelectedId())
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


//==============================================================================
void FloatingTransportBar::mouseDown (const juce::MouseEvent& e)
{
 // Capture eligibility at mouse-down. Once the desktop component moves,
 // getMouseDownPosition() is expressed in its new local coordinates and
 // can no longer be used reliably to test the original title-strip hit.
 // isOnDesktop() alone already rules out dragging while docked (a docked
 // instance is parented inline, never on the desktop), but check docked
 // explicitly too — harmless belt-and-braces if a caller ever calls
 // show() without first calling setDocked(false).
    draggingTitleStrip = isOnDesktop() && ! docked
                      && computeLayout().titleStrip.contains (e.getPosition());


 if (draggingTitleStrip)
        dragger.startDraggingComponent (this, e);
}


void FloatingTransportBar::mouseDrag (const juce::MouseEvent& e)
{
 if (draggingTitleStrip)
        dragger.dragComponent (this, e, nullptr);
}


void FloatingTransportBar::mouseUp (const juce::MouseEvent&)
{
    draggingTitleStrip = false;
}


void FloatingTransportBar::mouseDoubleClick (const juce::MouseEvent& e)
{
 if (! docked && computeLayout().titleStrip.contains (e.getPosition()) && onDockRequested)
 onDockRequested();
}


//==============================================================================
// A single transport-first row, sized to exactly what it needs — no dead
// space above or below. Left to right: musical position + transport cluster,
// then the editable L/R locators, then BPM / GRID / LINK on the far right.
//
// Docked mode reuses every field width unchanged — same content, same
// proportions, so the bar looks identical whether it's living in its own
// 1024px-wide floating window or stretched across an arranger that's twice
// that width. Position + transport + locators are left-anchored right
// after the view-switcher in both modes (the floating window is sized to
// fit its content exactly, so this is a no-op there); a docked host's
// width varies with whatever window/monitor it's living in, and any
// leftover space is left where it naturally falls — pinned to the right,
// ahead of the right-pinned BPM/GRID/LINK group — rather than split around
// the cluster, which used to grow into a visible gap on both sides of the
// cluster on a wide host window.
FloatingTransportBar::Layout FloatingTransportBar::computeLayout() const
{
    Layout L;
 auto area = getLocalBounds();


    L.titleStrip = area.removeFromTop (MetroMetrics::grid * 3);

    // ── Title strip: view switcher on the left, pin+dock (floating) or
    // float (docked) grouped on the right. While docked, none of this
    // lives in the title strip any more — the view switcher and the
    // Float button both move down into the content row below (see there)
    // — so the docked title strip is left empty. ───────────────────────
    if (! docked)
    {
        auto strip = L.titleStrip.reduced (MetroMetrics::halfGrid, MetroMetrics::quarterGrid);
        L.dockField = strip.removeFromRight (MetroMetrics::grid * 7);
        strip.removeFromRight (MetroMetrics::halfGrid);
        L.pinField  = strip.removeFromRight (MetroMetrics::grid * 6);
        strip.removeFromLeft (MetroMetrics::grid * 6); // leave room for the drag grip / left margin
        L.mixerButtonField   = strip.removeFromLeft (MetroMetrics::grid * 8);
        strip.removeFromLeft (MetroMetrics::halfGrid);
        L.arrangeButtonField = strip.removeFromLeft (MetroMetrics::grid * 13);
        strip.removeFromLeft (MetroMetrics::halfGrid);
        L.eqButtonField      = strip.removeFromLeft (MetroMetrics::grid * 13);
    }

    area.reduce (MetroMetrics::grid, 0);
    area.removeFromTop (MetroMetrics::grid * 2);


 const int rowH = MetroMetrics::largeControlHeight;
 auto row = area.removeFromTop (rowH);


 // ── Docked-only: MIXER / ARRANGER / GLOBAL EQ view switcher now lives
 // at the far left of the content row instead of the title strip above. ─
    if (docked)
    {
        L.mixerButtonField   = row.removeFromLeft (MetroMetrics::grid * 8);
        row.removeFromLeft (MetroMetrics::halfGrid);
        L.arrangeButtonField = row.removeFromLeft (MetroMetrics::grid * 13);
        row.removeFromLeft (MetroMetrics::halfGrid);
        L.eqButtonField      = row.removeFromLeft (MetroMetrics::grid * 13);
        row.removeFromLeft (MetroMetrics::grid * 2); // gap before the left-anchored cluster
    }


 // ── Far right: BPM, grid snap, Float (docked only), link — one row, in
 // that order. Pinned to the right edge in both modes, so it's peeled
 // off the row first. ────────────────────────────────────────────────
    {
        // Each of these fields gets its own rounded frameField() border
        // (see paint(), pad = 3px on every side). A 1-grid (8px) raw gap
        // here only leaves ~2px of clear space between two adjacent
        // frames once both have expanded into it — not enough for their
        // matching 4px corner radii, so the rounded corners visually
        // collide into a pinched "hourglass" seam instead of reading as
        // two separate boxes. Use a 2-grid gap between every framed field
        // so the corners always clear each other with room to spare.
        const int fieldGap = MetroMetrics::grid * 2;
        // 9 grid units — same width as gridField — rather than 6: at
        // tempoLabel's 16pt bold mono font, "120.00" needs more than the
        // old 48px afforded and was getting ellipsized ("120…") instead
        // of showing the actual value.
        const int tempoFieldW = MetroMetrics::grid * 9;
        const int rightW = MetroMetrics::grid * (docked ? 45 : 37);
        auto right = row.removeFromRight (rightW);
        L.tempoCaption = right.removeFromLeft (MetroMetrics::grid * 4);
        L.tempoField   = right.removeFromLeft (tempoFieldW);
        right.removeFromLeft (fieldGap);
        L.gridField    = right.removeFromLeft (MetroMetrics::grid * 9);
        right.removeFromLeft (fieldGap);
        if (docked)
        {
            L.floatField = right.removeFromLeft (MetroMetrics::grid * 7);
            right.removeFromLeft (fieldGap);
        }
        L.linkField    = right;
        row.removeFromRight (MetroMetrics::grid * 2); // gap before the pinned block
    }


 // ── Left/middle: musical position + transport cluster + L/R locators,
 // as one block. Left-anchored in both modes. Docked mode used to centre
 // this block in whatever space was left between the view-switcher
 // (left) and the BPM/GRID/FLOAT/LINK group (right) -- fine on a narrow
 // host window (little/no leftover space to centre into), but the pad
 // grows with the window on a wide one, opening a gap on both sides of
 // the cluster instead of staying put. Left-anchoring keeps the cluster
 // snug against the view-switcher at any host width, matching floating
 // mode's own (already left-anchored) layout. ───────────────────────

    L.positionField = row.removeFromLeft (MetroMetrics::grid * 21);
    row.removeFromLeft (MetroMetrics::grid);
    L.divider0 = row.getX();
    row.removeFromLeft (MetroMetrics::grid);
    L.transportRow = row.removeFromLeft (MetroMetrics::grid * 37);


    row.removeFromLeft (MetroMetrics::grid * 2);
    L.divider1 = row.getX();
    row.removeFromLeft (MetroMetrics::grid * 2);


 // ── Middle: directly writable L/R locators ───────────────────────────
    // 21 grid units per field - exactly matching positionField's width, since
    // both now show the same 10-character bars.beats.ticks format at the
    // same 26pt bold mono size (the old 11-grid width plus "L "/"R " prefix
    // is gone now that the prefix moved fully to the separate caption).
    L.locatorsField = row.removeFromLeft (MetroMetrics::grid * 48);


    row.removeFromLeft (MetroMetrics::grid * 2);
    L.divider2 = row.getX();


 return L;
}


void FloatingTransportBar::resized()
{
 const auto L = computeLayout();


    if (docked)
    {
        floatButton.setBounds (L.floatField);
    }
    else
    {
        dockButton.setBounds (L.dockField);
        pinButton.setBounds (L.pinField);
    }

    // While docked, the externally-owned buttons (set via setViewButtons())
    // occupy these fields instead of this panel's own mixer/arrange/eq —
    // setDocked()/setViewButtons() already hide whichever set isn't in use.
    if (docked && (viewMixerBtn != nullptr || viewArrangeBtn != nullptr || viewEqBtn != nullptr))
    {
        if (viewMixerBtn   != nullptr) viewMixerBtn->setBounds (L.mixerButtonField);
        if (viewArrangeBtn != nullptr) viewArrangeBtn->setBounds (L.arrangeButtonField);
        if (viewEqBtn      != nullptr) viewEqBtn->setBounds (L.eqButtonField);
    }
    else
    {
        mixerButton.setBounds (L.mixerButtonField);
        arrangeButton.setBounds (L.arrangeButtonField);
        eqButton.setBounds (L.eqButtonField);
    }


 // L/R captions sit outside their fields so the numerical values stay truly centred.
 auto locators = L.locatorsField;
    locators.removeFromLeft (MetroMetrics::grid * 2);
    leftLocatorLabel.setBounds (locators.removeFromLeft (MetroMetrics::grid * 21));
    locators.removeFromLeft (MetroMetrics::grid * 2);
    rightLocatorLabel.setBounds (locators.removeFromLeft (MetroMetrics::grid * 21));


    positionLabel.setBounds (L.positionField);
 auto transport = L.transportRow;
 const int btnGap = MetroMetrics::halfGrid;
 const int n = 6;
 const int btnW = (transport.getWidth() - btnGap * (n - 1)) / n;
 for (auto* b : { &toStartButton, &backButton, &playButton, &stopButton, &recordButton, &cycleButton })
    {
        b->setBounds (transport.removeFromLeft (btnW));
        transport.removeFromLeft (btnGap);
    }


    setLeftButton.setVisible (false);
    setRightButton.setVisible (false);
    tempoLabel.setBounds (L.tempoField);
    gridCombo.setBounds (L.gridField);
 if (linkPtr != nullptr)
        linkButton.setBounds (L.linkField);
}


//==============================================================================
void FloatingTransportBar::paint (juce::Graphics& g)
{
 const auto L = computeLayout();


    g.fillAll (Base::Background);


    g.setColour (Base::SurfaceAlt);
    g.fillRect (L.titleStrip);
    g.setColour (Base::Border);
    g.drawHorizontalLine (L.titleStrip.getBottom(), 0.0f, (float) getWidth());


 // Drag grip — two short bars centred in the strip, matching the mockup.
 // Floating-only: a docked instance can't be dragged (it's parented
 // inline, not on the desktop), so the grip would be a false affordance.
 if (! docked)
    {
        g.setColour (Text::Muted);
 const auto stripCentre = L.titleStrip.getCentre();
 for (int i = 0; i < 2; ++i)
            g.fillRoundedRectangle ((float) stripCentre.x - 23.0f, (float) stripCentre.y - 4.0f + i * 6.0f,
 46.0f, 3.0f, 1.5f);
    }


 // Locator captions are painted in the two-grid slots immediately before
 // their editable fields. Keep the caption rectangles two grids wide; using
 // a translated copy of the full label bounds paints L/R over the locator
 // values instead of inside the space reserved for them in resized().
    g.setColour (Text::Muted);
    g.setFont (MetroTypography::caption());
 const auto captionWidth = MetroMetrics::grid * 2;
 const auto leftCaption  = leftLocatorLabel.getBounds().withWidth (captionWidth)
                                                .translated (-captionWidth, 0);
 const auto rightCaption = rightLocatorLabel.getBounds().withWidth (captionWidth)
                                                 .translated (-captionWidth, 0);
    g.drawText ("L", leftCaption,  juce::Justification::centred);
    g.drawText ("R", rightCaption, juce::Justification::centred);


 // BPM is the one far-right field whose value alone ("120.00") wouldn't
 // otherwise be self-explanatory the way GRID (shows "1/16") and LINK
 // (shows its own name) already are — everything else reads fine without
 // a caption, which is what let row 1 shrink down to just the readouts.
    g.setColour (Text::Muted);
    g.setFont (DysektLookAndFeel::makeFont (transportTextSize));
    g.drawText ("BPM", L.tempoCaption, juce::Justification::centredLeft);


    g.setColour (Base::Border);
 for (int x : { L.divider0, L.divider1, L.divider2 })
        g.drawVerticalLine (x, (float) (L.titleStrip.getBottom() + MetroMetrics::grid),
                           (float) (getHeight() - MetroMetrics::grid));

 // ── Per-element frames — every functional cluster on the content row
 // gets its own bordered box instead of relying on the thin dividers
 // alone to imply grouping: position, transport buttons, locators, BPM,
 // grid snap, float (docked only), link.
 //
 // Clamped to a 1px-inset safe area rather than drawn from the raw
 // expanded rect: docked height leaves zero margin below the content row
 // (title + gap + row already sums to the full component height), so an
 // unclamped pad could push a box's bottom edge past this component's own
 // bounds and visually collide with whatever ArrangeView draws directly
 // beneath it. Intersecting keeps every frame strictly inside this panel.
    const auto safeArea = getLocalBounds().reduced (1);
    auto frameField = [&] (juce::Rectangle<int> field, int pad = 3)
    {
        if (field.isEmpty())
            return;
        const auto box = field.expanded (pad).getIntersection (safeArea);
        if (box.isEmpty())
            return;
        g.setColour (Base::Border);
        g.drawRoundedRectangle (box.toFloat(), 4.0f, 1.0f);
    };
    frameField (L.positionField);
    frameField (L.transportRow);
    frameField (L.locatorsField);
    frameField (L.tempoCaption.getUnion (L.tempoField));
    frameField (L.gridField);
 if (docked)
        frameField (L.floatField);
    frameField (L.linkField);

 // ── Tab-strip group border — wraps MIXER / ARRANGER / GLOBAL EQ in one
 // bordered group so the three read as a single switcher instead of three
 // buttons loose in open space. ──────────────────────────────────────────
    if (! L.mixerButtonField.isEmpty() && ! L.eqButtonField.isEmpty())
    {
        auto tabGroup = L.mixerButtonField.getUnion (L.eqButtonField).expanded (3, 3);
        g.setColour (Base::Border);
        g.drawRoundedRectangle (tabGroup.toFloat(), 4.0f, 1.0f);
    }

    // ── Outer frame — a full border around the whole panel, so it reads as
    // one closed, framed surface rather than bleeding into whatever sits
    // beside or beneath it (ArrangeView's header when docked).
    g.setColour (Base::Border);
    g.drawRect (getLocalBounds(), 1);
}


//==============================================================================
void FloatingTransportBar::timerCallback()
{
 const bool playing = engine.isPlaying();
 if (playButton.getToggleState() != playing)
        playButton.setToggleState (playing, juce::dontSendNotification);


 const bool recording = engine.isRecording();
 if (recordButton.getToggleState() != recording)
        recordButton.setToggleState (recording, juce::dontSendNotification);


 const bool looping = engine.isLooping();
 if (cycleButton.getToggleState() != looping)
        cycleButton.setToggleState (looping, juce::dontSendNotification);


 if (! tempoLabel.isBeingEdited())
        tempoLabel.setText (juce::String (engine.getBpm(), 2), juce::dontSendNotification);


    positionLabel.setText (formatMusicalPosition (engine.getPlayheadBeats()), juce::dontSendNotification);


 if (! leftLocatorLabel.isBeingEdited())
        // No "L " prefix here - the separate muted caption in paint() supplies
        // that, so the field shows a pure numeric value at the same
        // width/font as positionField instead of a longer, prefixed string.
        leftLocatorLabel.setText (formatMusicalPosition ((double) leftLocatorTick / (double) MidiClip::kPPQ),
                                  juce::dontSendNotification);
 if (! rightLocatorLabel.isBeingEdited())
        rightLocatorLabel.setText (formatMusicalPosition ((double) rightLocatorTick / (double) MidiClip::kPPQ),
                                   juce::dontSendNotification);


 if (linkPtr != nullptr)
    {
 const int peers = linkPtr->getPeerCount();
        linkButton.setButtonText (peers > 0 ? ("LINK " + juce::String (peers)) : "LINK");
        linkButton.setToggleState (linkPtr->isEnabled(), juce::dontSendNotification);
    }
}


void FloatingTransportBar::updateTempoFromEditor()
{
 const float bpm = tempoLabel.getText().getFloatValue();
 if (bpm >= 20.0f && bpm <= 999.0f)
        engine.setBpm (bpm);
}


void FloatingTransportBar::setLeftLocatorToPlayhead()
{
    leftLocatorTick = engine.getPlayheadTick();
 if (rightLocatorTick <= leftLocatorTick)
        rightLocatorTick = leftLocatorTick + MidiClip::kPPQ;
    engine.setLoopRange (leftLocatorTick, rightLocatorTick);
}


void FloatingTransportBar::setRightLocatorToPlayhead()
{
    rightLocatorTick = engine.getPlayheadTick();
 if (rightLocatorTick <= leftLocatorTick)
        rightLocatorTick = leftLocatorTick + MidiClip::kPPQ;
    engine.setLoopRange (leftLocatorTick, rightLocatorTick);
}


int64_t FloatingTransportBar::segmentStepTicks (int segment) noexcept
{
    switch (segment)
    {
        case 0:  return MidiClip::kPPQ * 4;               // bar
        case 1:  return MidiClip::kPPQ;                    // beat
        default: return juce::jmax<int64_t> (1, MidiClip::kPPQ / 32); // tick (fine)
    }
}


void FloatingTransportBar::adjustLeftLocator (int segment, int direction)
{
    const int64_t delta = segmentStepTicks (segment) * (int64_t) direction;
    leftLocatorTick = juce::jmax<int64_t> (0, leftLocatorTick + delta);
 if (rightLocatorTick <= leftLocatorTick)
        rightLocatorTick = leftLocatorTick + MidiClip::kPPQ;
    engine.setLoopRange (leftLocatorTick, rightLocatorTick);
}


void FloatingTransportBar::adjustRightLocator (int segment, int direction)
{
    const int64_t delta = segmentStepTicks (segment) * (int64_t) direction;
    rightLocatorTick = juce::jmax<int64_t> (leftLocatorTick + MidiClip::kPPQ, rightLocatorTick + delta);
    engine.setLoopRange (leftLocatorTick, rightLocatorTick);
}


void FloatingTransportBar::adjustPlayhead (int segment, int direction)
{
    const int64_t delta   = segmentStepTicks (segment) * (int64_t) direction;
    const int64_t newTick = juce::jmax<int64_t> (0, engine.getPlayheadTick() + delta);
    engine.seekToTick (newTick);
}


void FloatingTransportBar::updateLocatorsFromEditors()
{
 const auto left = parseMusicalPosition (leftLocatorLabel.getText());
 const auto right = parseMusicalPosition (rightLocatorLabel.getText());
 if (left < 0 || right < 0)
 return;


    leftLocatorTick = left;
    rightLocatorTick = juce::jmax (right, leftLocatorTick + (int64_t) MidiClip::kPPQ);
    engine.setLoopRange (leftLocatorTick, rightLocatorTick);
}


int64_t FloatingTransportBar::parseMusicalPosition (const juce::String& text)
{
 auto value = text.trim().removeCharacters ("LRlr ");
 const auto parts = juce::StringArray::fromTokens (value, ".", "");
 if (parts.size() != 3)
 return -1;
 const int bar = parts[0].getIntValue();
 const int beat = parts[1].getIntValue();
 const int tick = parts[2].getIntValue();
 if (bar < 1 || beat < 1 || beat > 4 || tick < 0 || tick >= MidiClip::kPPQ)
 return -1;
 return ((int64_t) (bar - 1) * 4 + (beat - 1)) * MidiClip::kPPQ + tick;
}


juce::String FloatingTransportBar::formatMusicalPosition (double beats)
{
 if (beats < 0.0)
        beats = 0.0;
 const int bar  = (int) (beats / 4.0) + 1;
 const int beat = (int) std::fmod (beats, 4.0) + 1;
 const int tick = (int) (std::fmod (beats, 1.0) * (double) MidiClip::kPPQ);
 return juce::String::formatted ("%03d.%02d.%03d", bar, beat, tick);
}


//==============================================================================
juce::File FloatingTransportBar::getPositionFile()
{
 return getSettingsDir().getChildFile ("floating_transport_position.xml");
}


void FloatingTransportBar::restorePosition()
{
 const auto file = getPositionFile();
 int x = 120, y = 120;


 if (file.existsAsFile())
    {
 if (auto xml = juce::XmlDocument::parse (file))
        {
            x = xml->getIntAttribute ("x", x);
            y = xml->getIntAttribute ("y", y);
        }
    }


 // Clamp onto whichever display currently contains that point, in case
 // the panel was last closed on a monitor that is no longer connected.
 const auto& displays = juce::Desktop::getInstance().getDisplays();
 const auto* display  = displays.getDisplayForPoint (juce::Point<int> (x, y));
 const auto  area     = (display != nullptr) ? display->userArea
                                                 : juce::Rectangle<int> (0, 0, 1920, 1080);
    x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - getWidth()), x);
    y = juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - getHeight()), y);


 setTopLeftPosition (x, y);
}


void FloatingTransportBar::savePosition() const
{
 const auto dir = getSettingsDir();
 if (! dir.exists())
        dir.createDirectory();


    juce::XmlElement xml ("FLOATING_TRANSPORT");
    xml.setAttribute ("x", getX());
    xml.setAttribute ("y", getY());
    xml.writeTo (getPositionFile());
}
