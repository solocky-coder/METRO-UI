#include "FloatingTransportBar.h"
#include <cmath>
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "MetroTypography.h"
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/MidiClip.h"
#include "../ui/DysektLookAndFeel.h"

namespace dysekt::metro
{
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

    // ── Title strip chrome ───────────────────────────────────────────────
    configureChrome (pinButton, "PIN", "Keep this panel above other windows");
    pinButton.setClickingTogglesState (true);
    pinButton.onStateChange = [this] { setAlwaysOnTop (pinButton.getToggleState()); };
    addAndMakeVisible (pinButton);

    configureChrome (dockButton, "DOCK", "Dock the transport back into the main window");
    dockButton.onClick = [this] { if (onDockRequested) onDockRequested(); };
    addAndMakeVisible (dockButton);

    // ── Cycle / loop ─────────────────────────────────────────────────────
    configureTransportButton (cycleButton, "LOOP", Accent::Cyan, "Toggle looping");
    cycleButton.setClickingTogglesState (true);
    cycleButton.onStateChange = [this] { engine.setLooping (cycleButton.getToggleState()); };


    // ── Tempo ────────────────────────────────────────────────────────────
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
    configureTransportButton (toStartButton, "|<",  Text::Secondary, "Return to start");
    configureTransportButton (backButton,    "<<",  Text::Secondary, "Step back one bar");
    configureTransportButton (playButton,    ">",   Transport::Play, "Play");
    configureTransportButton (stopButton,    "[]",  Accent::Orange,  "Stop");
    configureTransportButton (recordButton,  "REC", Transport::Record, "Record");
    playButton.setClickingTogglesState (true);
    recordButton.setClickingTogglesState (true);

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

    for (auto* b : { &toStartButton, &backButton, &playButton, &stopButton, &recordButton, &cycleButton })
        addAndMakeVisible (*b);

    // ── Locators ─────────────────────────────────────────────────────────
    locatorsLabel.setJustificationType (juce::Justification::centredLeft);
    locatorsLabel.setFont (DysektLookAndFeel::makeMonoFont (13.0f));
    locatorsLabel.setColour (juce::Label::backgroundColourId, Base::Background);
    locatorsLabel.setColour (juce::Label::textColourId, Text::Primary);
    addAndMakeVisible (locatorsLabel);

    configureChrome (setLeftButton, "SET LEFT", "Set the left locator to the playhead");
    configureChrome (setRightButton, "SET RIGHT", "Set the right locator to the playhead");
    setLeftButton.onClick  = [this] { setLeftLocatorToPlayhead(); };
    setRightButton.onClick = [this] { setRightLocatorToPlayhead(); };
    addAndMakeVisible (setLeftButton);
    addAndMakeVisible (setRightButton);

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

    // Two content rows plus the title strip; no unused vertical well below the controls.
    setSize (MetroMetrics::grid * 120, MetroMetrics::grid * 16);
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
        addToDesktop (juce::ComponentPeer::windowIsTemporary
                       | juce::ComponentPeer::windowHasDropShadow);
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
FloatingTransportBar::Layout FloatingTransportBar::computeLayout() const
{
    Layout L;
    auto area = getLocalBounds();

    L.titleStrip = area.removeFromTop (MetroMetrics::grid * 3);
    area.reduce (MetroMetrics::panelPadding, MetroMetrics::panelPadding);

    const int labelH = MetroMetrics::grid * 2;
    const int gap    = MetroMetrics::grid * 2;

    // ── Centre: musical position above the complete transport cluster ───
    auto centreCol = area.removeFromLeft (MetroMetrics::grid * 43);
    L.positionLabel = centreCol.removeFromTop (labelH);
    L.positionField = centreCol.removeFromTop (MetroMetrics::largeControlHeight);
    centreCol.removeFromTop (MetroMetrics::halfGrid);
    L.transportRow = centreCol.removeFromTop (MetroMetrics::largeControlHeight);

    area.removeFromLeft (gap);
    L.divider1 = area.getX();
    area.removeFromLeft (gap);

    // ── Right: locators ──────────────────────────────────────────────────
    auto rightCol = area.removeFromLeft (MetroMetrics::grid * 30);
    L.locatorsLabel = rightCol.removeFromTop (labelH);
    L.locatorsField = rightCol.removeFromTop (MetroMetrics::controlHeight);
    rightCol.removeFromTop (MetroMetrics::halfGrid);
    auto setRow = rightCol.removeFromTop (MetroMetrics::largeControlHeight);
    L.setLeftButton  = setRow.removeFromLeft (setRow.getWidth() / 2 - MetroMetrics::halfGrid);
    setRow.removeFromLeft (MetroMetrics::grid);
    L.setRightButton = setRow;

    area.removeFromLeft (gap);
    L.divider2 = area.getX();
    area.removeFromLeft (gap);

    // ── Far right: a single BPM → GRID → LINK row ───────────────────────
    auto farRow = area.removeFromBottom (MetroMetrics::largeControlHeight);
    L.tempoField = farRow.removeFromLeft (MetroMetrics::grid * 10);
    farRow.removeFromLeft (MetroMetrics::halfGrid);
    L.gridField = farRow.removeFromLeft (MetroMetrics::grid * 10);
    farRow.removeFromLeft (MetroMetrics::halfGrid);
    L.linkField = farRow;

    return L;
}

void FloatingTransportBar::resized()
{
    const auto L = computeLayout();

    // Title strip: pin on the left of the grip area, dock at the far right.
    auto strip = L.titleStrip.reduced (MetroMetrics::halfGrid, MetroMetrics::quarterGrid);
    pinButton.setBounds (strip.removeFromLeft (MetroMetrics::grid * 6));
    dockButton.setBounds (strip.removeFromRight (MetroMetrics::grid * 7));

    tempoLabel.setBounds (L.tempoField);

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

    locatorsLabel.setBounds (L.locatorsField);
    setLeftButton.setBounds (L.setLeftButton);
    setRightButton.setBounds (L.setRightButton);

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

    g.setColour (Text::Muted);
    g.setFont (MetroTypography::caption());
    g.drawText ("MUSICAL POSITION", L.positionLabel, juce::Justification::centredBottom);
    g.drawText ("LOCATORS", L.locatorsLabel, juce::Justification::bottomLeft);

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

    locatorsLabel.setText ("L " + formatMusicalPosition ((double) leftLocatorTick / (double) MidiClip::kPPQ)
                            + "   R " + formatMusicalPosition ((double) rightLocatorTick / (double) MidiClip::kPPQ),
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
    // The engine's loop always wraps at tick 0 (see SequencerEngine::
    // getLengthTicks — "governs playhead wrap"); it has no independent
    // loop-start offset yet. Until that lands, the left locator doubles as
    // an explicit seek target rather than a true loop start.
    leftLocatorTick = engine.getPlayheadTick();
    engine.seekToTick (leftLocatorTick);
}

void FloatingTransportBar::setRightLocatorToPlayhead()
{
    rightLocatorTick = engine.getPlayheadTick();
    if (rightLocatorTick > leftLocatorTick)
        engine.setLengthTicks (rightLocatorTick - leftLocatorTick);
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
    const auto* display  = displays.getDisplayForPoint ({ x, y });
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
} // namespace dysekt::metro
