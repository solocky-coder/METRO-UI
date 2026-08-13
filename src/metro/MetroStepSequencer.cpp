#include "MetroStepSequencer.h"
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "MetroTypography.h"

namespace dysekt::metro
{
namespace
{
    constexpr int kRowHeight        = 42;
    constexpr int kStepHeaderHeight = 26;
    constexpr int kToolbarHeight    = MetroMetrics::grid * 6;
    constexpr int kVelocityLaneHeight = 90;
    constexpr int kMinRowHeaderWidth = 128;
    constexpr int kMaxRowHeaderWidth = 176;
    constexpr int kRowHeaderGridGap = 20;
    constexpr int kMinStepWidth = 32;
    constexpr int kMaxStepWidth = 64;
    constexpr int kStepGap = 4;
    constexpr int kGroupGapExtra = 14;
    constexpr int kDefaultVelocity = 100;
    constexpr float kCellCornerRadius = 0.0f;
    constexpr float kRowHeaderCornerRadius = 3.0f;

    //==========================================================================
    //  A short set of General MIDI percussion names, used both as the
    //  MAIN-track default row mapping and as a fallback label for any
    //  drum-kit note that isn't already in that default set.
    //==========================================================================
    struct GmDrumEntry { int note; const char* name; };

    const GmDrumEntry kGmDrumTable[] = {
        { 35, "AC BASS DRUM" }, { 36, "KICK" },      { 37, "RIM" },        { 38, "SNARE" },
        { 39, "CLAP" },         { 40, "SNARE 2" },   { 41, "LO TOM" },     { 42, "CL HAT" },
        { 43, "HI TOM 2" },     { 44, "PD HAT" },    { 45, "MID TOM" },    { 46, "OP HAT" },
        { 47, "LO MID TOM" },   { 48, "HI TOM" },    { 49, "CRASH" },      { 50, "HI TOM 2" },
        { 51, "RIDE" },         { 52, "CHINA" },     { 53, "RIDE BELL" },  { 54, "TAMB" },
        { 55, "SPLASH" },       { 56, "COWBELL" },   { 57, "CRASH 2" },    { 58, "VIBRASLAP" },
        { 59, "RIDE 2" },       { 60, "HI BONGO" },  { 61, "LO BONGO" },   { 62, "MT CONGA" },
        { 63, "OP CONGA" },     { 64, "LO CONGA" },  { 65, "HI TIMBALE" }, { 66, "LO TIMBALE" },
        { 67, "HI AGOGO" },     { 68, "LO AGOGO" },  { 69, "CABASA" },     { 70, "MARACAS" },
        { 75, "CLAVES" },
    };

    juce::String gmDrumName (int note)
    {
        for (auto& e : kGmDrumTable)
            if (e.note == note)
                return e.name;
        return {};
    }

    // The MAIN-track / drum-kit default row mapping — a familiar MPC-style
    // 16-pad layout. Chosen from kGmDrumTable so the two stay consistent.
    constexpr int kDefaultRowNotes[] = { 36, 38, 42, 46, 37, 39, 41, 45, 48, 49, 51, 56, 54, 70, 75, 43 };

    // Per-row colours for that same default mapping, so each pad reads as
    // its own instrument at a glance (kick/snare/hat/clap etc. each get a
    // distinct hue) instead of the whole kit sharing one flat track colour.
    // Parallel array, same order/length as kDefaultRowNotes.
    const Colour kDefaultRowColours[] = {
        Accent::Blue,    // 36 KICK
        Accent::Pink,    // 38 SNARE
        Accent::Cyan,    // 42 CL HAT
        Accent::Cyan,    // 46 OP HAT
        Accent::Orange,  // 37 RIM
        Accent::Purple,  // 39 CLAP
        Accent::Green,   // 41 LO TOM
        Accent::Green,   // 45 MID TOM
        Accent::Green,   // 48 HI TOM
        Accent::Red,     // 49 CRASH
        Accent::Yellow,  // 51 RIDE
        Accent::Orange,  // 56 COWBELL
        Accent::Purple,  // 54 TAMB
        Accent::Yellow,  // 70 MARACAS
        Accent::Orange,  // 75 CLAVES
        Accent::Red,     // 43 HI TOM 2
    };

    // Fallback cycle for any drum note outside the default mapping (extras
    // pulled in from clip content), so those rows are still colourful
    // rather than defaulting to a single track colour.
    const Colour kFallbackDrumColours[] = {
        Accent::Blue, Accent::Pink, Accent::Cyan, Accent::Purple,
        Accent::Green, Accent::Orange, Accent::Yellow, Accent::Red,
    };

    int stepsPerBeatFor (StepResolution r) noexcept { return (int) r; }
}

//==============================================================================
MetroStepSequencer::MetroStepSequencer (SequencerEngine& sequencer)
    : engine (sequencer)
{
    setWantsKeyboardFocus (true);
    setOpaque (true);

    titleLabel.setText ("STEP SEQUENCER", juce::dontSendNotification);
    titleLabel.setFont (MetroTypography::sectionTitle());
    titleLabel.setColour (juce::Label::textColourId, Text::Primary);
    titleLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (titleLabel);

    clipNameLabel.setFont (MetroTypography::label());
    clipNameLabel.setColour (juce::Label::textColourId, Text::Secondary);
    clipNameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (clipNameLabel);

    auto setUpToggleButton = [this] (juce::TextButton& b)
    {
        b.setClickingTogglesState (false);
        addAndMakeVisible (b);
    };

    setUpToggleButton (res8Button);
    setUpToggleButton (res16Button);
    setUpToggleButton (res32Button);
    res8Button.onClick  = [this] { setResolution (StepResolution::eighth); };
    res16Button.onClick = [this] { setResolution (StepResolution::sixteenth); };
    res32Button.onClick = [this] { setResolution (StepResolution::thirtySecond); };

    setUpToggleButton (len8Button);
    setUpToggleButton (len16Button);
    setUpToggleButton (len32Button);
    setUpToggleButton (len64Button);
    len8Button.onClick  = [this] { setNumSteps (8); };
    len16Button.onClick = [this] { setNumSteps (16); };
    len32Button.onClick = [this] { setNumSteps (32); };
    len64Button.onClick = [this] { setNumSteps (64); };

    setUpToggleButton (followButton);
    followButton.onClick = [this]
    {
        followEnabled = ! followEnabled;
        updateToolbarToggleStates();
    };

    collapseButton.setClickingTogglesState (false);
    addAndMakeVisible (collapseButton);
    collapseButton.onClick = [this] { setCollapsed (! collapsed); };

    updateStepTicks();
    updateToolbarToggleStates();
    updateToolbarTitle();

    startTimerHz (30);
}

MetroStepSequencer::~MetroStepSequencer() = default;

int MetroStepSequencer::collapsedHeight() { return kToolbarHeight; }

//==============================================================================
//  Active clip
//==============================================================================
void MetroStepSequencer::setActiveClip (int trackIndex, int clipIndex)
{
    activeTrackIndex = trackIndex;
    activeClipIndex  = clipIndex;
    activeTrackInfo  = engine.getTrackInfo (trackIndex);
    activeClipInfo   = engine.getClipInfo (trackIndex, clipIndex);

    // A different clip means the previous selection/gesture state no
    // longer refers to anything meaningful.
    selectedSteps.clear();
    touchedThisGesture.clear();
    isDraggingSteps = false;
    velocityDragActive = false;
    stepScrollPx = 0;
    rowScrollPx  = 0;
    focusedRow  = 0;
    focusedStep = 0;
    lastPlayheadStep = -1;

    rebuildRows();
    updateToolbarTitle();
    repaint();
}

void MetroStepSequencer::clearActiveClip()
{
    activeTrackIndex = -1;
    activeClipIndex  = -1;
    rows.clear();
    selectedSteps.clear();
    touchedThisGesture.clear();
    isDraggingSteps = false;
    velocityDragActive = false;
    lastPlayheadStep = -1;
    updateToolbarTitle();
    repaint();
}

MidiClip* MetroStepSequencer::resolveClip() const
{
    if (activeTrackIndex < 0 || activeClipIndex < 0)
        return nullptr;
    return engine.getClip (activeTrackIndex, activeClipIndex);
}

void MetroStepSequencer::updateToolbarTitle()
{
    if (activeTrackIndex < 0)
    {
        clipNameLabel.setText ({}, juce::dontSendNotification);
        return;
    }

    const auto trackLabel = activeTrackInfo.name.isNotEmpty()
                               ? activeTrackInfo.name.toUpperCase()
                               : ("TRACK " + juce::String (activeTrackIndex + 1));
    clipNameLabel.setText (trackLabel + " - CLIP " + juce::String (activeClipIndex + 1),
                           juce::dontSendNotification);
}

//==============================================================================
//  Row model
//==============================================================================
void MetroStepSequencer::rebuildRows()
{
    rows.clear();
    auto* clip = resolveClip();

    const bool isMainTrack = activeTrackInfo.type == TrackType::MainSlice;
    // Bank 128 is the General MIDI convention for a percussion bank —
    // used here to tell a drum-kit SF preset apart from a melodic one.
    const bool isDrumKit = activeTrackInfo.type == TrackType::SfPlayer
                             && ! activeTrackInfo.isSfzInstrument
                             && activeTrackInfo.preset.bank == 128;

    std::set<int> mappedNotes;

    if (isMainTrack || isDrumKit)
    {
        constexpr int numDefaultRows = (int) (sizeof (kDefaultRowNotes) / sizeof (kDefaultRowNotes[0]));
        for (int i = 0; i < numDefaultRows; ++i)
        {
            const int note = kDefaultRowNotes[i];
            const auto name = gmDrumName (note);
            rows.push_back ({ name.isNotEmpty() ? name : juce::String (note), note, kDefaultRowColours[i] });
            mappedNotes.insert (note);
        }
    }
    else
    {
        // Chromatic instrument (ChromaticSlice track, SFZ instrument, or a
        // melodic SF2 preset) — a configurable note range, centred on
        // whatever notes already exist in the clip and defaulting to two
        // octaves around middle C when the clip is empty.
        int lowest = 60, highest = 60;
        if (clip != nullptr)
            for (auto& n : clip->getNotes())
            {
                lowest  = juce::jmin (lowest, n.note);
                highest = juce::jmax (highest, n.note);
            }

        const int rangeLow  = juce::jlimit (0, 127, juce::jmin (lowest, 60) - 6);
        const int rangeHigh = juce::jlimit (0, 127, juce::jmax (highest, 60) + 6);

        for (int note = rangeHigh; note >= rangeLow; --note)
        {
            rows.push_back ({ juce::MidiMessage::getMidiNoteName (note, true, true, 3), note,
                              activeTrackInfo.colour });
            mappedNotes.insert (note);
        }
    }

    // Always fold in any note already present in the clip, even if it
    // falls outside the default mapping above, so existing content is
    // never hidden from the editor.
    if (clip != nullptr)
    {
        std::set<int> extras;
        for (auto& n : clip->getNotes())
            if (mappedNotes.find (n.note) == mappedNotes.end())
                extras.insert (n.note);

        constexpr int numFallbackColours = (int) (sizeof (kFallbackDrumColours) / sizeof (kFallbackDrumColours[0]));
        int extraIndex = 0;
        for (int note : extras)
        {
            const auto gmName = gmDrumName (note);
            const auto name = gmName.isNotEmpty() ? gmName
                                                  : juce::MidiMessage::getMidiNoteName (note, true, true, 3);
            const bool isDrumRow = isMainTrack || isDrumKit;
            const auto colour = isDrumRow ? kFallbackDrumColours[extraIndex++ % numFallbackColours]
                                           : activeTrackInfo.colour;
            rows.push_back ({ name, note, colour });
        }
    }

    focusedRow = rows.empty() ? 0 : juce::jlimit (0, (int) rows.size() - 1, focusedRow);
    clampScroll();
}

//==============================================================================
//  Grid geometry
//==============================================================================
void MetroStepSequencer::updateStepTicks()
{
    stepTicks = MidiClip::kPPQ / stepsPerBeatFor (resolution);
}

void MetroStepSequencer::setResolution (StepResolution newResolution)
{
    if (resolution == newResolution)
        return;
    resolution = newResolution;
    updateStepTicks();
    updateToolbarToggleStates();
    clampScroll();
    repaint();
}

void MetroStepSequencer::setNumSteps (int newNumSteps)
{
    if (numSteps == newNumSteps)
        return;
    numSteps = newNumSteps;
    updateToolbarToggleStates();
    clampScroll();
    repaint();
}

void MetroStepSequencer::updateToolbarToggleStates()
{
    res8Button.setToggleState  (resolution == StepResolution::eighth,       juce::dontSendNotification);
    res16Button.setToggleState (resolution == StepResolution::sixteenth,    juce::dontSendNotification);
    res32Button.setToggleState (resolution == StepResolution::thirtySecond, juce::dontSendNotification);

    len8Button.setToggleState  (numSteps == 8,  juce::dontSendNotification);
    len16Button.setToggleState (numSteps == 16, juce::dontSendNotification);
    len32Button.setToggleState (numSteps == 32, juce::dontSendNotification);
    len64Button.setToggleState (numSteps == 64, juce::dontSendNotification);

    followButton.setToggleState (followEnabled, juce::dontSendNotification);
}

int MetroStepSequencer::rowHeaderWidth() const
{
    return juce::jlimit (kMinRowHeaderWidth, kMaxRowHeaderWidth, getWidth() / 8);
}

int MetroStepSequencer::stepCellWidth() const
{
    const int visibleWidth = juce::jmax (0, gridBounds().getWidth());
    const int groupCount = juce::jmax (0, (numSteps - 1) / 4);
    const int totalGapPx = juce::jmax (0, numSteps - 1) * kStepGap + groupCount * kGroupGapExtra;
    const int available = juce::jmax (0, visibleWidth - totalGapPx);
    const int fitWidth = numSteps > 0 ? available / numSteps : kMinStepWidth;
    return juce::jlimit (kMinStepWidth, kMaxStepWidth, fitWidth <= 0 ? kMinStepWidth : fitWidth);
}

int MetroStepSequencer::gridContentWidth() const
{
    const int cw = stepCellWidth();
    const int groupCount = juce::jmax (0, (numSteps - 1) / 4);
    return numSteps * cw + juce::jmax (0, numSteps - 1) * kStepGap + groupCount * kGroupGapExtra;
}

int MetroStepSequencer::gridContentHeight() const
{
    return (int) rows.size() * kRowHeight;
}

void MetroStepSequencer::clampScroll()
{
    const auto grid = gridBounds();
    const int maxScrollX = juce::jmax (0, gridContentWidth()  - grid.getWidth());
    const int maxScrollY = juce::jmax (0, gridContentHeight() - grid.getHeight());
    stepScrollPx = juce::jlimit (0, maxScrollX, stepScrollPx);
    rowScrollPx  = juce::jlimit (0, maxScrollY, rowScrollPx);
}

juce::Rectangle<int> MetroStepSequencer::toolbarBounds() const
{
    return { 0, 0, getWidth(), kToolbarHeight };
}

juce::Rectangle<int> MetroStepSequencer::stepHeaderBounds() const
{
    const int x = rowHeaderWidth() + kRowHeaderGridGap;
    return { x, kToolbarHeight, juce::jmax (0, getWidth() - x), kStepHeaderHeight };
}

juce::Rectangle<int> MetroStepSequencer::rowHeaderBounds() const
{
    const int top = kToolbarHeight + kStepHeaderHeight;
    const int bottom = getHeight() - kVelocityLaneHeight;
    return { 0, top, rowHeaderWidth(), juce::jmax (0, bottom - top) };
}

juce::Rectangle<int> MetroStepSequencer::gridBounds() const
{
    const int top = kToolbarHeight + kStepHeaderHeight;
    const int bottom = getHeight() - kVelocityLaneHeight;
    const int x = rowHeaderWidth() + kRowHeaderGridGap;
    return { x, top, juce::jmax (0, getWidth() - x), juce::jmax (0, bottom - top) };
}

juce::Rectangle<int> MetroStepSequencer::velocityLaneBounds() const
{
    const int x = rowHeaderWidth() + kRowHeaderGridGap;
    return { x, getHeight() - kVelocityLaneHeight,
             juce::jmax (0, getWidth() - x), kVelocityLaneHeight };
}

int MetroStepSequencer::gridOffsetX() const
{
    const int contentW = gridContentWidth();
    const int visibleW = gridBounds().getWidth();
    return contentW < visibleW ? (visibleW - contentW) / 2 : 0;
}

int MetroStepSequencer::gridOffsetY() const
{
    const int contentH = gridContentHeight();
    const int visibleH = gridBounds().getHeight();
    return contentH < visibleH ? (visibleH - contentH) / 2 : 0;
}

juce::Rectangle<int> MetroStepSequencer::cellBounds (int rowIndex, int stepIndex) const
{
    const auto grid = gridBounds();
    const int cw = stepCellWidth();
    int x = grid.getX() + gridOffsetX() - stepScrollPx;
    for (int s = 0; s < stepIndex; ++s)
    {
        x += cw + kStepGap;
        if (s % 4 == 3)
            x += kGroupGapExtra;
    }
    const int y = grid.getY() + gridOffsetY() + rowIndex * kRowHeight - rowScrollPx;
    return { x, y, cw, kRowHeight - 2 };
}

int MetroStepSequencer::rowAtY (int y) const
{
    const auto grid = gridBounds();
    if (y < grid.getY() || y >= grid.getBottom())
        return -1;
    const int idx = (y - grid.getY() - gridOffsetY() + rowScrollPx) / kRowHeight;
    return (idx >= 0 && idx < (int) rows.size()) ? idx : -1;
}

int MetroStepSequencer::stepAtX (int x) const
{
    const auto grid = gridBounds();
    if (x < grid.getX() || x >= grid.getRight())
        return -1;
    const int cw = stepCellWidth();
    int cx = grid.getX() + gridOffsetX() - stepScrollPx;
    for (int s = 0; s < numSteps; ++s)
    {
        if (x >= cx && x < cx + cw)
            return s;
        cx += cw + kStepGap;
        if (s % 4 == 3)
            cx += kGroupGapExtra;
    }
    return -1;
}

//==============================================================================
//  Layout
//==============================================================================
void MetroStepSequencer::resized()
{
    auto toolbar = toolbarBounds().reduced (MetroMetrics::compactPadding, MetroMetrics::halfGrid);

    titleLabel.setBounds (toolbar.removeFromLeft (140));
    toolbar.removeFromLeft (MetroMetrics::compactPadding);
    clipNameLabel.setBounds (toolbar.removeFromLeft (juce::jmax (60, toolbar.getWidth() / 3)));

    collapseButton.setBounds (toolbar.removeFromRight (MetroMetrics::iconButtonSize));
    toolbar.removeFromRight (MetroMetrics::compactPadding);
    followButton.setBounds (toolbar.removeFromRight (72));
    toolbar.removeFromRight (MetroMetrics::compactPadding);

    auto lenArea = toolbar.removeFromRight (4 * 32 + 3 * MetroMetrics::quarterGrid);
    len64Button.setBounds (lenArea.removeFromRight (32)); lenArea.removeFromRight (MetroMetrics::quarterGrid);
    len32Button.setBounds (lenArea.removeFromRight (32)); lenArea.removeFromRight (MetroMetrics::quarterGrid);
    len16Button.setBounds (lenArea.removeFromRight (32)); lenArea.removeFromRight (MetroMetrics::quarterGrid);
    len8Button.setBounds  (lenArea.removeFromRight (32));
    toolbar.removeFromRight (MetroMetrics::compactPadding);

    auto resArea = toolbar.removeFromRight (3 * 44 + 2 * MetroMetrics::quarterGrid);
    res32Button.setBounds (resArea.removeFromRight (44)); resArea.removeFromRight (MetroMetrics::quarterGrid);
    res16Button.setBounds (resArea.removeFromRight (44)); resArea.removeFromRight (MetroMetrics::quarterGrid);
    res8Button.setBounds  (resArea.removeFromRight (44));

    clampScroll();
}

//==============================================================================
//  Painting
//==============================================================================
void MetroStepSequencer::paint (juce::Graphics& g)
{
    g.fillAll (Base::Background);
    drawToolbarBackdrop (g);

    if (collapsed)
        return;

    if (activeTrackIndex < 0)
    {
        drawEmptyState (g, "SELECT A MIDI CLIP TO EDIT");
        return;
    }

    if (resolveClip() == nullptr)
    {
        drawEmptyState (g, "STEP EDITOR IS AVAILABLE FOR MIDI CLIPS");
        return;
    }

    drawStepHeader (g);
    drawRowHeaders (g);
    drawGrid (g);
    drawVelocityLane (g);
}

void MetroStepSequencer::drawToolbarBackdrop (juce::Graphics& g)
{
    const auto bounds = toolbarBounds();
    g.setColour (Base::SurfaceAlt);
    g.fillRect (bounds);
    g.setColour (Base::Border);
    g.drawHorizontalLine (bounds.getBottom() - 1, 0.0f, (float) getWidth());
}

void MetroStepSequencer::drawEmptyState (juce::Graphics& g, const juce::String& message)
{
    auto area = getLocalBounds().withTrimmedTop (kToolbarHeight);
    g.setColour (Base::Background);
    g.fillRect (area);
    g.setColour (Text::Muted);
    g.setFont (MetroTypography::sectionTitle());
    g.drawFittedText (message, area.reduced (MetroMetrics::largeGap), juce::Justification::centred, 2);
}

void MetroStepSequencer::drawStepHeader (juce::Graphics& g)
{
    const auto header = stepHeaderBounds();
    g.setColour (Base::Surface);
    g.fillRect (header);

    g.saveState();
    g.reduceClipRegion (header);
    g.setFont (MetroTypography::caption());

    const int playStep = currentPlayheadStep();

    for (int s = 0; s < numSteps; ++s)
    {
        const auto cell = cellBounds (0, s).withY (header.getY()).withHeight (header.getHeight());
        if (! cell.intersects (header))
            continue;

        const bool emphasise = (s % 4 == 0);
        g.setColour (s == playStep ? Accent::Cyan
                     : emphasise    ? Text::Primary
                                    : Text::Muted);
        g.drawText (juce::String (s + 1), cell, juce::Justification::centred, false);
    }

    g.setColour (Base::Border);
    g.drawHorizontalLine (header.getBottom() - 1, (float) header.getX(), (float) header.getRight());
    g.restoreState();
}

void MetroStepSequencer::drawRowHeaders (juce::Graphics& g)
{
    const auto header = rowHeaderBounds();
    g.setColour (Base::SurfaceAlt);
    g.fillRect (header);

    g.saveState();
    g.reduceClipRegion (header);
    g.setFont (MetroTypography::label());

    for (int r = 0; r < (int) rows.size(); ++r)
    {
        auto cell = cellBounds (r, 0).withX (header.getX()).withWidth (header.getWidth());
        if (! cell.intersects (header))
            continue;

        const auto cellF = cell.toFloat();

        if (r == focusedRow && hasKeyboardFocus (true))
        {
            g.setColour (Base::Elevated);
            g.fillRoundedRectangle (cellF, kRowHeaderCornerRadius);
        }
        else
        {
            g.setColour (Base::Surface);
            g.fillRoundedRectangle (cellF, kRowHeaderCornerRadius);
        }

        g.setColour (Base::Border);
        g.drawRoundedRectangle (cellF, kRowHeaderCornerRadius, 1.0f);

        // Accent bar: a small inset rounded pill, not a flush full-height
        // strip — margin from the box edges on all sides, rounded caps.
        constexpr float kPillWidth  = 8.0f;
        constexpr float kPillMargin = 7.0f;
        auto pill = cellF.reduced (0.0f, kPillMargin).withX (cellF.getX() + kPillMargin).withWidth (kPillWidth);
        g.setColour (rows[(size_t) r].colour);
        g.fillRoundedRectangle (pill, kPillWidth * 0.5f);

        auto textArea = cell;
        textArea.removeFromLeft ((int) (kPillMargin * 2.0f + kPillWidth));

        g.setColour (Text::Primary);
        g.drawText (rows[(size_t) r].name, textArea.reduced (MetroMetrics::halfGrid, 0),
                   juce::Justification::centredLeft, true);
    }

    g.setColour (Base::Border);
    g.drawVerticalLine (header.getRight() - 1, (float) header.getY(), (float) header.getBottom());
    g.restoreState();
}

void MetroStepSequencer::drawGrid (juce::Graphics& g)
{
    const auto grid = gridBounds();
    g.setColour (Base::Surface);
    g.fillRect (grid);

    g.saveState();
    g.reduceClipRegion (grid);

    auto* clip = resolveClip();
    const int playStep = currentPlayheadStep();

    for (int r = 0; r < (int) rows.size(); ++r)
    {
        const auto& row = rows[(size_t) r];

        for (int s = 0; s < numSteps; ++s)
        {
            const auto cell = cellBounds (r, s);
            if (! cell.intersects (grid))
                continue;

            const int64_t tick = s * stepTicks;
            const int noteIndex = clip != nullptr ? clip->hitTest (tick, row.midiNote) : -1;
            const bool active = noteIndex >= 0;
            const bool hovered  = (r == hoverRow && s == hoverStep);
            const bool selected = selectedSteps.count ({ r, s }) > 0;
            const bool onPlayCol = (s == playStep);
            const auto cellF = cell.toFloat();

            // Uniform dark base for every cell — grouping/beat emphasis
            // comes from the header text and the wider group gaps in
            // cellBounds(), not from a filled band across the row.
            g.setColour (Base::Surface);
            g.fillRoundedRectangle (cellF, kCellCornerRadius);

            if (active)
            {
                const int velocity = noteIndex >= 0 ? clip->getNotes()[noteIndex].velocity : kDefaultVelocity;
                const float intensity = juce::jlimit (0.6f, 1.0f, (float) velocity / 127.0f);
                g.setColour (row.colour.withAlpha (intensity));
                g.fillRoundedRectangle (cellF, kCellCornerRadius);
            }

            if (onPlayCol)
            {
                g.setColour (Accent::Cyan.withAlpha (0.18f));
                g.fillRoundedRectangle (cellF, kCellCornerRadius);
            }

            if (hovered)
            {
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillRoundedRectangle (cellF, kCellCornerRadius);
            }

            // Every cell gets a crisp hairline outline — this is what
            // gives the grid its "boxed pad" look in the mockup, rather
            // than flat colour with no separation between cells.
            g.setColour (Base::Border);
            g.drawRoundedRectangle (cellF, kCellCornerRadius, 1.0f);

            if (selected)
            {
                g.setColour (Accent::Cyan);
                g.drawRoundedRectangle (cellF, kCellCornerRadius, 1.5f);
            }
            else if (hovered)
            {
                g.setColour (Accent::Cyan.withAlpha (0.5f));
                g.drawRoundedRectangle (cellF, kCellCornerRadius, 1.0f);
            }

            if (r == focusedRow && s == focusedStep && hasKeyboardFocus (true))
            {
                g.setColour (Accent::Cyan);
                g.drawRoundedRectangle (cellF.expanded (1.0f), kCellCornerRadius, 1.5f);
            }
        }
    }

    // Bar boundaries (every 4 beats == 16 steps at 1/16) get a stronger
    // separator than the per-beat group gap already provides.
    const int stepsPerBar = stepsPerBeatFor (resolution) * 4;
    if (stepsPerBar > 0)
        for (int s = stepsPerBar; s < numSteps; s += stepsPerBar)
        {
            const auto cell = cellBounds (0, s);
            if (cell.getX() < grid.getX() || cell.getX() > grid.getRight())
                continue;
            g.setColour (Base::Border);
            g.drawVerticalLine (cell.getX() - kGroupGapExtra / 2, (float) grid.getY(), (float) grid.getBottom());
        }

    g.restoreState();
}

void MetroStepSequencer::drawVelocityLane (juce::Graphics& g)
{
    const auto lane = velocityLaneBounds();
    g.setColour (Base::SurfaceAlt);
    g.fillRect (lane);
    g.setColour (Base::Border);
    g.drawHorizontalLine (lane.getY(), (float) lane.getX(), (float) lane.getRight());

    auto* clip = resolveClip();
    if (clip == nullptr)
        return;

    const bool haveFocusedRow = focusedRow >= 0 && focusedRow < (int) rows.size();

    if (! haveFocusedRow)
    {
        g.setColour (Text::Muted);
        g.setFont (MetroTypography::small());
        g.drawFittedText ("SELECT A ROW TO EDIT VELOCITY", lane.reduced (MetroMetrics::compactPadding),
                          juce::Justification::centred, 1);
        return;
    }

    g.saveState();
    g.reduceClipRegion (lane);

    const auto& row = rows[(size_t) focusedRow];

    for (int s = 0; s < numSteps; ++s)
    {
        const auto cell = cellBounds (0, s).withY (lane.getY() + 2).withHeight (lane.getHeight() - 4);
        if (! cell.intersects (lane))
            continue;

        const int64_t tick = s * stepTicks;
        const int noteIndex = clip->hitTest (tick, row.midiNote);

        if (noteIndex < 0)
        {
            g.setColour (Base::Border);
            g.drawHorizontalLine (cell.getBottom(), (float) cell.getX(), (float) cell.getRight());
            continue;
        }

        const int velocity = clip->getNotes()[noteIndex].velocity;
        const float frac = juce::jlimit (0.0f, 1.0f, (float) velocity / 127.0f);
        auto bar = cell.withY (cell.getBottom() - (int) (frac * cell.getHeight()))
                      .withHeight ((int) (frac * cell.getHeight()));

        const bool selected = selectedSteps.count ({ focusedRow, s }) > 0;
        g.setColour (selected ? Accent::Cyan : Text::Secondary);
        g.fillRect (bar);
    }

    g.restoreState();
}

//==============================================================================
//  Mouse interaction
//==============================================================================
void MetroStepSequencer::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    if (collapsed || activeTrackIndex < 0 || resolveClip() == nullptr)
        return;

    // Velocity lane: click/drag on the currently-focused row's bars.
    if (velocityLaneBounds().contains (e.getPosition()))
    {
        const int step = stepAtX (e.x);
        if (step >= 0)
            beginVelocityDrag (focusedRow, step, e.y);
        return;
    }

    const int row  = rowAtY (e.y);
    const int step = stepAtX (e.x);
    if (row < 0 || step < 0)
        return;

    if (e.mods.isPopupMenu())
    {
        showStepContextMenu (row, step, e.getScreenPosition());
        return;
    }

    focusedRow  = row;
    focusedStep = step;

    auto* clip = resolveClip();
    const auto& r = rows[(size_t) row];
    const bool currentlyActive = clip->hitTest (step * stepTicks, r.midiNote) >= 0;

    if (e.mods.isShiftDown() && currentlyActive)
    {
        // Shift-click an active step: adjust the selection rather than
        // toggling the note, so the user can build a set to delete/copy.
        auto key = std::make_pair (row, step);
        if (selectedSteps.count (key) > 0)
            selectedSteps.erase (key);
        else
            selectedSteps.insert (key);
        repaint();
        return;
    }

    undoManager.beginNewTransaction();
    isDraggingSteps = true;
    dragIsAdding = ! currentlyActive;
    touchedThisGesture.clear();
    applyStepDuringGesture (row, step, dragIsAdding);
}

void MetroStepSequencer::mouseDrag (const juce::MouseEvent& e)
{
    if (velocityDragActive)
    {
        continueVelocityDrag (e.y);
        return;
    }

    if (! isDraggingSteps)
        return;

    const int row  = rowAtY (e.y);
    const int step = stepAtX (e.x);
    if (row >= 0 && step >= 0)
        applyStepDuringGesture (row, step, dragIsAdding);
}

void MetroStepSequencer::mouseUp (const juce::MouseEvent&)
{
    isDraggingSteps = false;
    velocityDragActive = false;
    velocityDragStep = -1;
    touchedThisGesture.clear();
}

void MetroStepSequencer::mouseMove (const juce::MouseEvent& e)
{
    const int row  = rowAtY (e.y);
    const int step = stepAtX (e.x);
    if (row != hoverRow || step != hoverStep)
    {
        hoverRow = row;
        hoverStep = step;
        repaint();
    }
}

void MetroStepSequencer::mouseExit (const juce::MouseEvent&)
{
    if (hoverRow != -1 || hoverStep != -1)
    {
        hoverRow = hoverStep = -1;
        repaint();
    }
}

void MetroStepSequencer::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f && wheel.deltaX == 0.0f)
        return;

    if (e.mods.isShiftDown())
        stepScrollPx -= (int) (wheel.deltaY * 120.0f);
    else
        rowScrollPx -= (int) (wheel.deltaY * kRowHeight * 3.0f);

    clampScroll();
    repaint();
}

void MetroStepSequencer::focusGained (juce::Component::FocusChangeType) { repaint(); }
void MetroStepSequencer::focusLost   (juce::Component::FocusChangeType) { repaint(); }

//==============================================================================
//  Editing
//==============================================================================
namespace
{
    /** Adds and/or removes a batch of notes as a single undo step. Notes to
     *  remove are located by (tick, note) rather than by index, since a
     *  MidiClip's internal note array can be re-sorted/re-indexed by other
     *  edits between when this action is created and when it is undone. */
    class ClipEditAction final : public juce::UndoableAction
    {
    public:
        ClipEditAction (MidiClip& c, std::vector<MidiNote> toAdd, std::vector<MidiNote> toRemove)
            : clip (c), notesToAdd (std::move (toAdd)), notesToRemove (std::move (toRemove)) {}

        bool perform() override
        {
            for (auto& n : notesToRemove)
            {
                const int idx = clip.hitTest (n.startTick, n.note);
                if (idx >= 0)
                    clip.removeNote (idx);
            }
            for (auto& n : notesToAdd)
                clip.addNote (n);
            return true;
        }

        bool undo() override
        {
            for (auto& n : notesToAdd)
            {
                const int idx = clip.hitTest (n.startTick, n.note);
                if (idx >= 0)
                    clip.removeNote (idx);
            }
            for (auto& n : notesToRemove)
                clip.addNote (n);
            return true;
        }

        int getSizeInUnits() override
        {
            return (int) sizeof (*this) + (int) ((notesToAdd.size() + notesToRemove.size()) * sizeof (MidiNote));
        }

    private:
        MidiClip& clip;
        std::vector<MidiNote> notesToAdd;
        std::vector<MidiNote> notesToRemove;
    };

    /** Sets one note's velocity, located by (tick, note). Used for the
     *  velocity-lane drag gesture, where several of these may be performed
     *  under one undoManager transaction. */
    class SetVelocityAction final : public juce::UndoableAction
    {
    public:
        SetVelocityAction (MidiClip& c, int64_t tick, int note, int newVel, int oldVel)
            : clip (c), noteTick (tick), midiNote (note), newVelocity (newVel), oldVelocity (oldVel) {}

        bool perform() override { return apply (newVelocity); }
        bool undo()    override { return apply (oldVelocity); }
        int getSizeInUnits() override { return (int) sizeof (*this); }

    private:
        bool apply (int velocity)
        {
            const int idx = clip.hitTest (noteTick, midiNote);
            if (idx < 0)
                return false;
            clip.setNoteVelocity (idx, velocity);
            return true;
        }

        MidiClip& clip;
        int64_t noteTick;
        int midiNote;
        int newVelocity, oldVelocity;
    };
}

void MetroStepSequencer::applyStepDuringGesture (int rowIndex, int stepIndex, bool adding)
{
    const auto key = std::make_pair (rowIndex, stepIndex);
    if (touchedThisGesture.count (key) > 0)
        return;

    auto* clip = resolveClip();
    if (clip == nullptr || rowIndex < 0 || rowIndex >= (int) rows.size())
        return;

    touchedThisGesture.insert (key);

    const int64_t tick = stepIndex * stepTicks;
    const int midiNote = rows[(size_t) rowIndex].midiNote;
    const bool currentlyActive = clip->hitTest (tick, midiNote) >= 0;

    // Painting shouldn't re-toggle a cell the gesture already covers, and
    // shouldn't undo the direction established by the first cell touched.
    if (currentlyActive == adding)
        return;

    if (adding)
    {
        MidiNote n; n.note = midiNote; n.velocity = kDefaultVelocity; n.startTick = tick; n.durationTick = stepTicks;
        undoManager.perform (new ClipEditAction (*clip, { n }, {}));
    }
    else
    {
        MidiNote n; n.note = midiNote; n.startTick = tick;
        undoManager.perform (new ClipEditAction (*clip, {}, { n }));
    }

    repaint (cellBounds (rowIndex, stepIndex).expanded (2));
    repaint (velocityLaneBounds());
}

void MetroStepSequencer::toggleFocusedStep()
{
    if (resolveClip() == nullptr || rows.empty())
        return;

    auto* clip = resolveClip();
    const auto& r = rows[(size_t) focusedRow];
    const bool currentlyActive = clip->hitTest (focusedStep * stepTicks, r.midiNote) >= 0;

    undoManager.beginNewTransaction();
    touchedThisGesture.clear();
    applyStepDuringGesture (focusedRow, focusedStep, ! currentlyActive);
}

void MetroStepSequencer::deleteSelectedSteps()
{
    auto* clip = resolveClip();
    if (clip == nullptr || selectedSteps.empty())
        return;

    std::vector<MidiNote> toRemove;
    for (auto& key : selectedSteps)
    {
        const int rowIdx = key.first, stepIdx = key.second;
        if (rowIdx < 0 || rowIdx >= (int) rows.size())
            continue;
        const int midiNote = rows[(size_t) rowIdx].midiNote;
        const int64_t tick = stepIdx * stepTicks;
        if (clip->hitTest (tick, midiNote) >= 0)
        {
            MidiNote n; n.note = midiNote; n.startTick = tick;
            toRemove.push_back (n);
        }
    }

    if (toRemove.empty())
        return;

    undoManager.beginNewTransaction();
    undoManager.perform (new ClipEditAction (*clip, {}, std::move (toRemove)));
    selectedSteps.clear();
    repaint (gridBounds());
    repaint (velocityLaneBounds());
}

void MetroStepSequencer::selectAllActiveSteps()
{
    auto* clip = resolveClip();
    if (clip == nullptr)
        return;

    selectedSteps.clear();
    for (int r = 0; r < (int) rows.size(); ++r)
        for (int s = 0; s < numSteps; ++s)
            if (clip->hitTest (s * stepTicks, rows[(size_t) r].midiNote) >= 0)
                selectedSteps.insert ({ r, s });

    repaint (gridBounds());
    repaint (velocityLaneBounds());
}

void MetroStepSequencer::copySelectedSteps()
{
    auto* clip = resolveClip();
    if (clip == nullptr || selectedSteps.empty())
        return;

    clipboard.clear();
    for (auto& key : selectedSteps)
    {
        const int rowIdx = key.first, stepIdx = key.second;
        if (rowIdx < 0 || rowIdx >= (int) rows.size())
            continue;
        const int idx = clip->hitTest (stepIdx * stepTicks, rows[(size_t) rowIdx].midiNote);
        if (idx >= 0)
            clipboard.push_back (clip->getNotes()[idx]);
    }
}

void MetroStepSequencer::pasteClipboard()
{
    auto* clip = resolveClip();
    if (clip == nullptr || clipboard.empty())
        return;

    undoManager.beginNewTransaction();
    undoManager.perform (new ClipEditAction (*clip, clipboard, {}));
    repaint (gridBounds());
    repaint (velocityLaneBounds());
}

void MetroStepSequencer::moveFocus (int rowDelta, int stepDelta, bool extendSelection)
{
    if (rows.empty())
        return;

    focusedRow  = juce::jlimit (0, (int) rows.size() - 1, focusedRow + rowDelta);
    focusedStep = juce::jlimit (0, numSteps - 1, focusedStep + stepDelta);

    if (extendSelection)
        selectedSteps.insert ({ focusedRow, focusedStep });

    // Keep the newly-focused cell in view.
    const auto grid = gridBounds();
    const auto cell = cellBounds (focusedRow, focusedStep);
    if (cell.getY() < grid.getY())
        rowScrollPx -= (grid.getY() - cell.getY());
    else if (cell.getBottom() > grid.getBottom())
        rowScrollPx += (cell.getBottom() - grid.getBottom());

    if (cell.getX() < grid.getX())
        stepScrollPx -= (grid.getX() - cell.getX());
    else if (cell.getRight() > grid.getRight())
        stepScrollPx += (cell.getRight() - grid.getRight());

    clampScroll();
    repaint();
}

void MetroStepSequencer::showStepContextMenu (int rowIndex, int stepIndex, juce::Point<int> screenPos)
{
    auto* clip = resolveClip();
    if (clip == nullptr || rowIndex < 0 || rowIndex >= (int) rows.size())
        return;

    const int midiNote = rows[(size_t) rowIndex].midiNote;
    const int64_t tick = stepIndex * stepTicks;
    const int noteIndex = clip->hitTest (tick, midiNote);
    if (noteIndex < 0)
        return; // Nothing to act on for an empty cell.

    // NOTE: MidiNote currently has no mute or probability field, so this
    // first pass only offers the edits the data model actually supports.
    // Extending MidiNote for those (Phase 6 — see the integration spec) is
    // a deliberate follow-up, not an oversight.
    juce::PopupMenu menu;
    menu.addItem (1, "SET VELOCITY 64");
    menu.addItem (2, "SET VELOCITY 100");
    menu.addItem (3, "SET VELOCITY 127");
    menu.addSeparator();
    menu.addItem (4, "DELETE STEP");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                        [this, rowIndex, stepIndex, tick, midiNote] (int result)
    {
        auto* c = resolveClip();
        if (c == nullptr)
            return;

        const int idx = c->hitTest (tick, midiNote);
        if (idx < 0)
            return;

        undoManager.beginNewTransaction();

        if (result == 4)
        {
            MidiNote n; n.note = midiNote; n.startTick = tick;
            undoManager.perform (new ClipEditAction (*c, {}, { n }));
        }
        else if (result == 1 || result == 2 || result == 3)
        {
            const int newVel = result == 1 ? 64 : result == 2 ? 100 : 127;
            const int oldVel = c->getNotes()[idx].velocity;
            undoManager.perform (new SetVelocityAction (*c, tick, midiNote, newVel, oldVel));
        }

        repaint (cellBounds (rowIndex, stepIndex).expanded (2));
        repaint (velocityLaneBounds());
    });
}

void MetroStepSequencer::beginVelocityDrag (int rowIndex, int stepIndex, int /*startY*/)
{
    auto* clip = resolveClip();
    if (clip == nullptr || rowIndex < 0 || rowIndex >= (int) rows.size())
        return;

    const int midiNote = rows[(size_t) rowIndex].midiNote;
    if (clip->hitTest (stepIndex * stepTicks, midiNote) < 0)
        return; // Only existing notes can be velocity-dragged.

    focusedRow = rowIndex;
    velocityDragActive = true;
    velocityDragStep = stepIndex;
    undoManager.beginNewTransaction();
}

void MetroStepSequencer::continueVelocityDrag (int y)
{
    if (! velocityDragActive || velocityDragStep < 0)
        return;

    auto* clip = resolveClip();
    if (clip == nullptr || focusedRow < 0 || focusedRow >= (int) rows.size())
        return;

    const auto lane = velocityLaneBounds();
    const float frac = juce::jlimit (0.0f, 1.0f, (float) (lane.getBottom() - y) / (float) juce::jmax (1, lane.getHeight()));
    const int newVelocity = juce::jlimit (1, 127, (int) juce::roundToInt (frac * 127.0f));

    const int midiNote = rows[(size_t) focusedRow].midiNote;
    const int64_t tick = velocityDragStep * stepTicks;
    const int idx = clip->hitTest (tick, midiNote);
    if (idx < 0)
        return;

    const int oldVelocity = clip->getNotes()[idx].velocity;
    if (oldVelocity == newVelocity)
        return;

    undoManager.perform (new SetVelocityAction (*clip, tick, midiNote, newVelocity, oldVelocity));
    repaint (velocityLaneBounds());
    repaint (cellBounds (focusedRow, velocityDragStep).expanded (2));
}

//==============================================================================
//  Keyboard
//==============================================================================
bool MetroStepSequencer::keyPressed (const juce::KeyPress& key)
{
    if (collapsed || activeTrackIndex < 0 || resolveClip() == nullptr)
        return false;

    const bool cmd = key.getModifiers().isCommandDown();
    const bool shift = key.getModifiers().isShiftDown();

    if (key == juce::KeyPress::escapeKey)
    {
        if (! selectedSteps.empty())
        {
            selectedSteps.clear();
            repaint();
            return true;
        }
        return false; // let the arranger clear its own selection next.
    }

    if (key == juce::KeyPress::spaceKey)      { toggleFocusedStep(); return true; }
    if (key == juce::KeyPress::deleteKey ||
        key == juce::KeyPress::backspaceKey)  { deleteSelectedSteps(); return true; }

    if (key == juce::KeyPress::leftKey)  { moveFocus (0, -1, shift); return true; }
    if (key == juce::KeyPress::rightKey) { moveFocus (0,  1, shift); return true; }
    if (key == juce::KeyPress::upKey)    { moveFocus (-1, 0, shift); return true; }
    if (key == juce::KeyPress::downKey)  { moveFocus (1,  0, shift); return true; }

    if (cmd && shift && key.isKeyCode ('Z')) { undoManager.redo(); repaint(); return true; }
    if (cmd && key.isKeyCode ('Z'))          { undoManager.undo(); repaint(); return true; }
    if (cmd && key.isKeyCode ('A'))          { selectAllActiveSteps(); return true; }
    if (cmd && key.isKeyCode ('C'))          { copySelectedSteps(); return true; }
    if (cmd && key.isKeyCode ('V'))          { pasteClipboard(); return true; }

    return false;
}

//==============================================================================
//  Collapse
//==============================================================================
void MetroStepSequencer::setCollapsed (bool shouldBeCollapsed)
{
    if (collapsed == shouldBeCollapsed)
        return;
    collapsed = shouldBeCollapsed;
    if (onCollapsedChanged != nullptr)
        onCollapsedChanged (collapsed);
    repaint();
}

//==============================================================================
//  Playback feedback
//==============================================================================
int MetroStepSequencer::currentPlayheadStep() const
{
    if (activeTrackIndex < 0 || ! engine.isPlaying())
        return -1;

    const int64_t playTick = engine.getPlayheadTick();
    const int64_t clipStart = activeClipInfo.startTick;
    const int64_t clipEnd   = activeClipInfo.endTick();

    if (playTick < clipStart || playTick >= clipEnd)
        return -1; // Playback is outside the selected clip.

    const int64_t relativeTick = playTick - clipStart;
    const int64_t patternTicks = (int64_t) numSteps * stepTicks;
    if (relativeTick >= patternTicks)
        return -1; // Past the visible pattern window.

    return (int) (relativeTick / stepTicks);
}

void MetroStepSequencer::updatePlayheadAndFollow()
{
    const int step = currentPlayheadStep();
    if (step == lastPlayheadStep)
        return;

    if (lastPlayheadStep >= 0)
        repaint (cellBounds (0, lastPlayheadStep).withY (0).withHeight (getHeight()).expanded (2, 0));
    if (step >= 0)
        repaint (cellBounds (0, step).withY (0).withHeight (getHeight()).expanded (2, 0));

    lastPlayheadStep = step;

    if (! followEnabled || step < 0)
        return;

    // Scroll only when the playhead reaches an edge region, rather than
    // recentring on every tick.
    const auto grid = gridBounds();
    const auto cell = cellBounds (0, step);
    const int edgeMargin = stepCellWidth() * 2;

    if (cell.getX() < grid.getX() + edgeMargin)
        stepScrollPx = juce::jmax (0, stepScrollPx - (grid.getX() + edgeMargin - cell.getX()));
    else if (cell.getRight() > grid.getRight() - edgeMargin)
        stepScrollPx += (cell.getRight() - (grid.getRight() - edgeMargin));

    clampScroll();
}

void MetroStepSequencer::timerCallback()
{
    if (collapsed || activeTrackIndex < 0)
        return;
    updatePlayheadAndFollow();
}

} // namespace dysekt::metro
