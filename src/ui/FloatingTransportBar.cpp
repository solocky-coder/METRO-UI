#include "FloatingTransportBar.h"
#include <cmath>
#include "../metro/MetroColours.h"
#include "../metro/MetroMetrics.h"
#include "../metro/MetroTypography.h"
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/MidiClip.h"
#include "DysektLookAndFeel.h"

using namespace dysekt::metro;   // MetroColours' Base/Text/Accent/Transport + MetroMetrics/MetroTypography —
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
    configureTransportButton (toStartButton, "▮◀", Text::Secondary, "Return to start");
    configureTransportButton (backButton,    "◀◀", Text::Secondary, "Step back one bar");
    configureTransportButton (playButton,    "▶",  Transport::Play, "Play");
    configureTransportButton (stopButton,    "■",  Accent::Orange, "Stop");
    configureTransportButton (recordButton,  "●",  Transport::Record, "Record");
    configureTransportButton (cycleButton,   "⟳",  Accent::Cyan, "Toggle looping");
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
        field->setTooltip ("Cycle locator — enter bars.beats.ticks");
        field->onTextChange = [this] { updateLocatorsFromEditors(); };
        addAndMakeVisible (*field);
    }

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
void FloatingTransportBar::mouseDown (const juce::MouseEvent& e)
{
    if (computeLayout().titleStrip.contains (e.getPosition()))
        dragger.startDraggingComponent (this, e);
}

void FloatingTransportBar::mouseDrag (const juce::MouseEvent& e)
{
    if (isOnDesktop() && computeLayout().titleStrip.contains (e.getMouseDownPosition()))
        dragger.dragComponent (this, e, nullptr);
}

void FloatingTransportBar::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (computeLayout().titleStrip.contains (e.getPosition()) && onDockRequested)
        onDockRequested();
}

//==============================================================================
// A single transport-first row, sized to exactly what it needs — no dead
// space above or below. Left to right: musical position + transport cluster,
// then the editable L/R locators, then BPM / GRID / LINK on the far right.
FloatingTransportBar::Layout FloatingTransportBar::computeLayout() const
{
    Layout L;
    auto area = getLocalBounds();

    L.titleStrip = area.removeFromTop (MetroMetrics::grid * 3);
    area.reduce (MetroMetrics::grid, 0);
    area.removeFromTop (MetroMetrics::grid * 2);

    const int rowH = MetroMetrics::largeControlHeight;
    auto row = area.removeFromTop (rowH);

    // ── Left: musical position + transport cluster ──────────────────────
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
    row.removeFromLeft (MetroMetrics::grid * 2);

    // ── Far right: BPM, grid snap, link — one row, in that order ────────
    L.tempoCaption = row.removeFromLeft (MetroMetrics::grid * 4);
    L.tempoField   = row.removeFromLeft (MetroMetrics::grid * 6);
    row.removeFromLeft (MetroMetrics::grid);
    L.gridField    = row.removeFromLeft (MetroMetrics::grid * 9);
    row.removeFromLeft (MetroMetrics::grid);
    L.linkField    = row.removeFromLeft (MetroMetrics::grid * 8);

    return L;
}

void FloatingTransportBar::resized()
{
    const auto L = computeLayout();

    auto strip = L.titleStrip.reduced (MetroMetrics::halfGrid, MetroMetrics::quarterGrid);
    pinButton.setBounds (strip.removeFromLeft (MetroMetrics::grid * 6));
    dockButton.setBounds (strip.removeFromRight (MetroMetrics::grid * 7));

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
    g.setColour (Text::Muted);
    const auto stripCentre = L.titleStrip.getCentre();
    for (int i = 0; i < 2; ++i)
        g.fillRoundedRectangle ((float) stripCentre.x - 23.0f, (float) stripCentre.y - 4.0f + i * 6.0f,
                                46.0f, 3.0f, 1.5f);

    g.setColour (Text::Muted);
    g.setFont (MetroTypography::caption());
    g.drawText ("TRANSPORT", L.titleStrip.withTrimmedLeft (MetroMetrics::grid * 8), juce::Justification::centredLeft);

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
