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


 void configureTransportButton (juce::TextButton& b, const juce::String& text,
                                   juce::Colour tint, const juce::String& tooltip)
    {
        b.setButtonText (text);
        b.setTooltip (tooltip);
        b.setColour (juce::TextButton::buttonColourId, Base::Surface);
        b.setColour (juce::TextButton::buttonOnColourId, tint.withAlpha (0.32f));
        b.setColour (juce::TextButton::textColourOffId, tint.brighter (0.15f));
        b.setColour (juce::TextButton::textColourOnId, Base::White);
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
    floatButton.onClick = [this] { if (onFloatRequested) onFloatRequested(); };
    floatButton.setVisible (false);   // shown only once setDocked(true) is called
 addAndMakeVisible (floatButton);


 // ── View switcher — Mixer / Arranger / Global EQ ────────────────────────
 configureChrome (mixerButton, "MIXER", "Switch to the mixer");
 configureChrome (arrangeButton, "ARRANGER", "Switch to the arranger");
 configureChrome (eqButton, "GLOBAL EQ", "Switch to the global EQ");
 for (auto* b : { &mixerButton, &arrangeButton, &eqButton })
    {
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
    tempoLabel.setFont (DysektLookAndFeel::makeMonoFont (16.0f, true));
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
 addAndMakeVisible (positionLabel);


 // ── Transport cluster ────────────────────────────────────────────────
 // Use ASCII labels here: the embedded UI typeface does not include the
 // Unicode transport glyphs, which otherwise render as missing characters
 // on the detached desktop component.
 configureTransportButton (toStartButton, "|<",   Text::Secondary,   "Return to start");
 configureTransportButton (backButton,    "<<",   Text::Secondary,   "Step back one bar");
 configureTransportButton (playButton,    ">",    Transport::Play,   "Play");
 configureTransportButton (stopButton,    "[]",   Accent::Orange,    "Stop");
 configureTransportButton (recordButton,  "REC",  Transport::Record, "Record");
 configureTransportButton (cycleButton,   "LOOP", Accent::Cyan,      "Toggle looping");
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
        field->setFont (DysektLookAndFeel::makeMonoFont (13.0f));
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
    gridCombo.setColour (juce::ComboBox::backgroundColourId, Base::Elevated);
    gridCombo.setColour (juce::ComboBox::textColourId, Text::Primary);
    gridCombo.setColour (juce::ComboBox::outlineColourId, Base::Border);
 addAndMakeVisible (gridCombo);


 // ── Ableton Link ─────────────────────────────────────────────────────
 if (linkPtr != nullptr)
    {
 configureChrome (linkButton, "LINK", "Toggle Ableton Link");
        linkButton.setClickingTogglesState (true);
        linkButton.setColour (juce::TextButton::buttonOnColourId, Accent::Purple.withAlpha (0.35f));
        linkButton.onStateChange = [this] { if (linkPtr) linkPtr->setEnabled (linkButton.getToggleState()); };
 addAndMakeVisible (linkButton);
    }


    leftLocatorTick = engine.getLoopStartTick();
    rightLocatorTick = engine.getLoopEndTick();


 // Compact single-row transport: position + transport, locators, and
 // BPM/GRID/LINK laid out side by side (see computeLayout()).
 setSize (MetroMetrics::grid * 128, MetroMetrics::grid * 13);
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
// that width. The one thing docked mode does differently: the floating
// window is sized to fit its content exactly, so left-anchoring position +
// transport + locators after the title strip's margin already fills the
// panel with no dead space. A docked host's width varies, so that same
// left-anchored block would drift toward the left edge, leaving a growing
// gap before the right-pinned BPM/GRID/LINK group as the host widens —
// docked mode centres that block in whatever space is actually left instead.
FloatingTransportBar::Layout FloatingTransportBar::computeLayout() const
{
    Layout L;
 auto area = getLocalBounds();


    L.titleStrip = area.removeFromTop (MetroMetrics::grid * 3);

    // ── Title strip: view switcher on the left, pin+dock (floating) or
    // float (docked) grouped on the right ──────────────────────────────
    {
        auto strip = L.titleStrip.reduced (MetroMetrics::halfGrid, MetroMetrics::quarterGrid);
        if (docked)
        {
            L.floatField = strip.removeFromRight (MetroMetrics::grid * 7);
        }
        else
        {
            L.dockField = strip.removeFromRight (MetroMetrics::grid * 7);
            strip.removeFromRight (MetroMetrics::halfGrid);
            L.pinField  = strip.removeFromRight (MetroMetrics::grid * 6);
        }
        strip.removeFromLeft (MetroMetrics::grid * 6); // leave room for the drag grip / left margin
        L.mixerButtonField   = strip.removeFromLeft (MetroMetrics::grid * 6);
        strip.removeFromLeft (MetroMetrics::halfGrid);
        L.arrangeButtonField = strip.removeFromLeft (MetroMetrics::grid * 8);
        strip.removeFromLeft (MetroMetrics::halfGrid);
        L.eqButtonField      = strip.removeFromLeft (MetroMetrics::grid * 8);
    }

    area.reduce (MetroMetrics::grid, 0);
    area.removeFromTop (MetroMetrics::grid * 2);


 const int rowH = MetroMetrics::largeControlHeight;
 auto row = area.removeFromTop (rowH);


 // ── Far right: BPM, grid snap, link — one row, in that order. Pinned to
 // the right edge in both modes, so it's peeled off the row first. ─────
    {
        auto right = row.removeFromRight (MetroMetrics::grid * 29);
        L.tempoCaption = right.removeFromLeft (MetroMetrics::grid * 4);
        L.tempoField   = right.removeFromLeft (MetroMetrics::grid * 6);
        right.removeFromLeft (MetroMetrics::grid);
        L.gridField    = right.removeFromLeft (MetroMetrics::grid * 9);
        right.removeFromLeft (MetroMetrics::grid);
        L.linkField    = right;
        row.removeFromRight (MetroMetrics::grid * 2); // gap before the pinned block
    }


 // ── Left/middle: musical position + transport cluster + L/R locators,
 // as one block. Centred in whatever's left of the row when docked;
 // left-anchored (i.e. no-op offset) when floating. ─────────────────────
    constexpr int clusterUnits = 21 + 1 + 37 + 2 + 2 + 28; // position+gap+transport+gap+gap+locators
    if (docked)
    {
        const int clusterW = clusterUnits * MetroMetrics::grid;
        const int pad = juce::jmax (0, (row.getWidth() - clusterW) / 2);
        row.removeFromLeft (pad);
    }


    L.positionField = row.removeFromLeft (MetroMetrics::grid * 21);
    row.removeFromLeft (MetroMetrics::grid);
    L.transportRow = row.removeFromLeft (MetroMetrics::grid * 37);


    row.removeFromLeft (MetroMetrics::grid * 2);
    L.divider1 = row.getX();
    row.removeFromLeft (MetroMetrics::grid * 2);


 // ── Middle: directly writable L/R locators ───────────────────────────
    L.locatorsField = row.removeFromLeft (MetroMetrics::grid * 28);


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
    leftLocatorLabel.setBounds (locators.removeFromLeft (MetroMetrics::grid * 11));
    locators.removeFromLeft (MetroMetrics::grid * 2);
    rightLocatorLabel.setBounds (locators.removeFromLeft (MetroMetrics::grid * 11));


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


 // Locator captions are painted separately so their editable values can be centred.
    g.setColour (Text::Muted);
    g.setFont (MetroTypography::caption());
    g.drawText ("L", leftLocatorLabel.getBounds().translated (-MetroMetrics::grid * 2, 0), juce::Justification::centred);
    g.drawText ("R", rightLocatorLabel.getBounds().translated (-MetroMetrics::grid * 2, 0), juce::Justification::centred);


 // BPM is the one far-right field whose value alone ("120.00") wouldn't
 // otherwise be self-explanatory the way GRID (shows "1/16") and LINK
 // (shows its own name) already are — everything else reads fine without
 // a caption, which is what let row 1 shrink down to just the readouts.
    g.setColour (Text::Muted);
    g.setFont (MetroTypography::caption());
    g.drawText ("BPM", L.tempoCaption, juce::Justification::centredLeft);


    g.setColour (Base::Border);
 for (int x : { L.divider1, L.divider2 })
        g.drawVerticalLine (x, (float) (L.titleStrip.getBottom() + MetroMetrics::grid),
                           (float) (getHeight() - MetroMetrics::grid));
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
        leftLocatorLabel.setText ("L " + formatMusicalPosition ((double) leftLocatorTick / (double) MidiClip::kPPQ),
                                  juce::dontSendNotification);
 if (! rightLocatorLabel.isBeingEdited())
        rightLocatorLabel.setText ("R " + formatMusicalPosition ((double) rightLocatorTick / (double) MidiClip::kPPQ),
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
