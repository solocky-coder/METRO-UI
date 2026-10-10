#include "MetroPianoRoll.h"
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "MetroTypography.h"

namespace dysekt::metro
{
namespace
{
    constexpr int kToolbarHeight   = MetroMetrics::grid * 6;
    constexpr int kRulerHeight     = 20;
    constexpr int kKeyGutterWidth  = 56;
    constexpr int kPixelsPerBeat   = 80;
    constexpr int kResizeGrabPx    = 6;
    constexpr int kDefaultVelocity = 100;

    int stepsPerBeatFor (StepResolution r) noexcept { return (int) r; }

    bool isBlackKeyNote (int note) noexcept
    {
        static const bool table[12] = { false, true, false, true, false, false,
                                        true, false, true, false, true, false };
        return table[((note % 12) + 12) % 12];
    }
}

//==============================================================================
MetroPianoRoll::MetroPianoRoll (SequencerEngine& sequencer)
    : engine (sequencer)
{
    setWantsKeyboardFocus (true);
    setOpaque (true);

    titleLabel.setText ("PIANO ROLL", juce::dontSendNotification);
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

    setUpToggleButton (followButton);
    followButton.onClick = [this]
    {
        followEnabled = ! followEnabled;
        updateToolbarToggleStates();
    };

    collapseButton.setClickingTogglesState (false);
    addAndMakeVisible (collapseButton);
    collapseButton.onClick = [this] { setCollapsed (! collapsed); };

    updateToolbarToggleStates();
    updateToolbarTitle();

    // Start scrolled to a reasonable octave range around middle C.
    rowScrollPx = (kHighestNote - 72) * kRowHeight;

    startTimerHz (30);
}

MetroPianoRoll::~MetroPianoRoll() = default;

int MetroPianoRoll::collapsedHeight() { return kToolbarHeight; }

//==============================================================================
//  Active clip
//==============================================================================
void MetroPianoRoll::setActiveClip (int trackIndex, int clipIndex)
{
    activeTrackIndex = trackIndex;
    activeClipIndex  = clipIndex;
    activeTrackInfo  = engine.getTrackInfo (trackIndex);
    activeClipInfo   = engine.getClipInfo (trackIndex, clipIndex);

    // A different clip means the previous selection/gesture state no
    // longer refers to anything meaningful.
    selectedNoteIndices.clear();
    dragMode = DragMode::none;
    dragNoteIndex = -1;
    hoverNoteIndex = -1;
    tickScrollPx = 0;

    updateToolbarTitle();
    clampScroll();
    repaint();
}

void MetroPianoRoll::clearActiveClip()
{
    activeTrackIndex = -1;
    activeClipIndex  = -1;
    selectedNoteIndices.clear();
    dragMode = DragMode::none;
    dragNoteIndex = -1;
    hoverNoteIndex = -1;
    updateToolbarTitle();
    repaint();
}

MidiClip* MetroPianoRoll::resolveClip() const
{
    if (activeTrackIndex < 0 || activeClipIndex < 0)
        return nullptr;
    return engine.getClip (activeTrackIndex, activeClipIndex);
}

void MetroPianoRoll::updateToolbarTitle()
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
//  Resolution / toolbar state
//==============================================================================
void MetroPianoRoll::setResolution (StepResolution newResolution)
{
    if (resolution == newResolution)
        return;
    resolution = newResolution;
    snapTicks = MidiClip::kPPQ / stepsPerBeatFor (resolution);
    updateToolbarToggleStates();
    repaint();
}

void MetroPianoRoll::updateToolbarToggleStates()
{
    res8Button.setToggleState  (resolution == StepResolution::eighth,       juce::dontSendNotification);
    res16Button.setToggleState (resolution == StepResolution::sixteenth,    juce::dontSendNotification);
    res32Button.setToggleState (resolution == StepResolution::thirtySecond, juce::dontSendNotification);
    followButton.setToggleState (followEnabled, juce::dontSendNotification);
}

//==============================================================================
//  Grid geometry
//==============================================================================
float MetroPianoRoll::pixelsPerTick() const noexcept
{
    return (float) kPixelsPerBeat / (float) MidiClip::kPPQ;
}

int MetroPianoRoll::gridContentWidth() const
{
    const int64_t lengthTicks = juce::jmax ((int64_t) MidiClip::kPPQ * 4, activeClipInfo.lengthTicks);
    return juce::jmax (1, (int) ((float) lengthTicks * pixelsPerTick()));
}

int MetroPianoRoll::gridContentHeight() const
{
    return (kHighestNote - kLowestNote + 1) * kRowHeight;
}

void MetroPianoRoll::clampScroll()
{
    const auto grid = gridBounds();
    const int maxScrollX = juce::jmax (0, gridContentWidth()  - grid.getWidth());
    const int maxScrollY = juce::jmax (0, gridContentHeight() - grid.getHeight());
    tickScrollPx = juce::jlimit (0, maxScrollX, tickScrollPx);
    rowScrollPx  = juce::jlimit (0, maxScrollY, rowScrollPx);
}

juce::Rectangle<int> MetroPianoRoll::toolbarBounds() const
{
    return { 0, 0, getWidth(), kToolbarHeight };
}

juce::Rectangle<int> MetroPianoRoll::rulerBounds() const
{
    return { kKeyGutterWidth, kToolbarHeight, juce::jmax (0, getWidth() - kKeyGutterWidth), kRulerHeight };
}

juce::Rectangle<int> MetroPianoRoll::keyGutterBounds() const
{
    const int top = kToolbarHeight + kRulerHeight;
    return { 0, top, kKeyGutterWidth, juce::jmax (0, getHeight() - top) };
}

juce::Rectangle<int> MetroPianoRoll::gridBounds() const
{
    const int top = kToolbarHeight + kRulerHeight;
    return { kKeyGutterWidth, top, juce::jmax (0, getWidth() - kKeyGutterWidth), juce::jmax (0, getHeight() - top) };
}

int MetroPianoRoll::xForTick (int64_t tick) const
{
    return gridBounds().getX() - tickScrollPx + (int) ((float) tick * pixelsPerTick());
}

int64_t MetroPianoRoll::tickForX (int x, bool snap) const
{
    const auto grid = gridBounds();
    const int64_t tick = (int64_t) (((float) (x - grid.getX() + tickScrollPx)) / pixelsPerTick());
    if (! snap || snapTicks <= 0)
        return juce::jmax ((int64_t) 0, tick);
    const int64_t snapped = ((tick + snapTicks / 2) / snapTicks) * snapTicks;
    return juce::jmax ((int64_t) 0, snapped);
}

int MetroPianoRoll::yForNote (int note) const
{
    const int rowIndex = kHighestNote - juce::jlimit (kLowestNote, kHighestNote, note);
    return gridBounds().getY() + rowIndex * kRowHeight - rowScrollPx;
}

int MetroPianoRoll::noteForY (int y) const
{
    const auto grid = gridBounds();
    const int rowIndex = (y - grid.getY() + rowScrollPx) / kRowHeight;
    const int note = kHighestNote - rowIndex;
    return juce::jlimit (kLowestNote, kHighestNote, note);
}

bool MetroPianoRoll::isBlackKey (int note) const noexcept { return isBlackKeyNote (note); }

juce::Rectangle<int> MetroPianoRoll::boundsForNote (const MidiNote& n) const
{
    const int x1 = xForTick (n.startTick);
    const int x2 = xForTick (n.endTick());
    const int y  = yForNote (n.note);
    return { x1, y, juce::jmax (2, x2 - x1), kRowHeight - 2 };
}

//==============================================================================
//  Layout
//==============================================================================
void MetroPianoRoll::resized()
{
    auto toolbar = toolbarBounds().reduced (MetroMetrics::compactPadding, MetroMetrics::halfGrid);

    titleLabel.setBounds (toolbar.removeFromLeft (140));
    toolbar.removeFromLeft (MetroMetrics::compactPadding);
    clipNameLabel.setBounds (toolbar.removeFromLeft (juce::jmax (60, toolbar.getWidth() / 3)));

    collapseButton.setBounds (toolbar.removeFromRight (MetroMetrics::iconButtonSize));
    toolbar.removeFromRight (MetroMetrics::compactPadding);
    followButton.setBounds (toolbar.removeFromRight (72));
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
void MetroPianoRoll::paint (juce::Graphics& g)
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
        drawEmptyState (g, "PIANO ROLL IS AVAILABLE FOR MIDI CLIPS");
        return;
    }

    drawGrid (g);
    drawNotes (g);
    drawRuler (g);
    drawKeyGutter (g);

    if (dragMode == DragMode::marquee)
    {
        g.setColour (Accent::Cyan.withAlpha (0.15f));
        g.fillRect (marqueeRect);
        g.setColour (Accent::Cyan);
        g.drawRect (marqueeRect, 1);
    }
}

void MetroPianoRoll::drawToolbarBackdrop (juce::Graphics& g)
{
    const auto bounds = toolbarBounds();
    g.setColour (Base::SurfaceAlt);
    g.fillRect (bounds);
    g.setColour (Base::Border);
    g.drawHorizontalLine (bounds.getBottom() - 1, 0.0f, (float) getWidth());
}

void MetroPianoRoll::drawEmptyState (juce::Graphics& g, const juce::String& message)
{
    auto area = getLocalBounds().withTrimmedTop (kToolbarHeight);
    g.setColour (Base::Background);
    g.fillRect (area);
    g.setColour (Text::Muted);
    g.setFont (MetroTypography::sectionTitle());
    g.drawFittedText (message, area.reduced (MetroMetrics::largeGap), juce::Justification::centred, 2);
}

void MetroPianoRoll::drawRuler (juce::Graphics& g)
{
    const auto ruler = rulerBounds();
    g.setColour (Base::Surface);
    g.fillRect (ruler);

    g.saveState();
    g.reduceClipRegion (ruler);
    g.setFont (MetroTypography::caption());

    const int64_t beatTicks = MidiClip::kPPQ;
    const int64_t firstBeat = (tickScrollPx > 0) ? (int64_t) ((float) tickScrollPx / pixelsPerTick() / (float) beatTicks) : 0;
    const int totalBeats = juce::jmax (1, (int) (gridContentWidth() / juce::jmax (1, kPixelsPerBeat)) + 2);

    juce::int64 playTick = -1;
    if (engine.isPlaying())
    {
        const int64_t rel = engine.getPlayheadTick() - activeClipInfo.startTick;
        if (rel >= 0 && rel < activeClipInfo.lengthTicks)
            playTick = rel;
    }

    for (int64_t beat = firstBeat; beat <= firstBeat + totalBeats; ++beat)
    {
        const int x = xForTick (beat * beatTicks);
        if (x < ruler.getX() - kPixelsPerBeat || x > ruler.getRight())
            continue;

        const bool onBar = (beat % 4 == 0);
        g.setColour (onBar ? Text::Primary : Text::Muted);
        if (onBar)
            g.drawText (juce::String (beat / 4 + 1), x + 2, ruler.getY(), kPixelsPerBeat, ruler.getHeight(),
                       juce::Justification::centredLeft, false);
        g.drawVerticalLine (x, (float) (onBar ? ruler.getY() : ruler.getY() + ruler.getHeight() / 2),
                           (float) ruler.getBottom());
    }

    if (playTick >= 0)
    {
        g.setColour (Accent::Cyan);
        g.drawVerticalLine (xForTick (playTick), (float) ruler.getY(), (float) ruler.getBottom());
    }

    g.setColour (Base::Border);
    g.drawHorizontalLine (ruler.getBottom() - 1, (float) ruler.getX(), (float) ruler.getRight());
    g.restoreState();
}

void MetroPianoRoll::drawKeyGutter (juce::Graphics& g)
{
    const auto gutter = keyGutterBounds();
    g.setColour (Base::SurfaceAlt);
    g.fillRect (gutter);

    g.saveState();
    g.reduceClipRegion (gutter);
    g.setFont (MetroTypography::caption());

    for (int note = kHighestNote; note >= kLowestNote; --note)
    {
        const auto cell = juce::Rectangle<int> (gutter.getX(), yForNote (note), gutter.getWidth(), kRowHeight);
        if (! cell.intersects (gutter))
            continue;

        const bool isC = (note % 12) == 0;
        g.setColour (isBlackKey (note) ? Base::Background : Base::SurfaceAlt.brighter (0.15f));
        g.fillRect (cell.reduced (0, 0).withTrimmedRight (isBlackKey (note) ? gutter.getWidth() / 3 : 0));

        if (isC)
        {
            g.setColour (Text::Secondary);
            g.drawText (juce::MidiMessage::getMidiNoteName (note, true, true, 3), cell.reduced (4, 0),
                       juce::Justification::centredLeft, false);
        }

        g.setColour (Base::Border);
        g.drawHorizontalLine (cell.getBottom(), (float) cell.getX(), (float) cell.getRight());
    }

    g.setColour (Base::Border);
    g.drawVerticalLine (gutter.getRight() - 1, (float) gutter.getY(), (float) gutter.getBottom());
    g.restoreState();
}

void MetroPianoRoll::drawGrid (juce::Graphics& g)
{
    const auto grid = gridBounds();
    g.setColour (Base::Surface);
    g.fillRect (grid);

    g.saveState();
    g.reduceClipRegion (grid);

    for (int note = kHighestNote; note >= kLowestNote; --note)
    {
        const auto row = juce::Rectangle<int> (grid.getX(), yForNote (note), grid.getWidth(), kRowHeight);
        if (! row.intersects (grid))
            continue;
        g.setColour (isBlackKey (note) ? Base::Background : Base::SurfaceAlt);
        g.fillRect (row);
    }

    const int64_t beatTicks = MidiClip::kPPQ;
    const int totalBeats = juce::jmax (1, (int) (gridContentWidth() / juce::jmax (1, kPixelsPerBeat)) + 2);
    const int64_t firstBeat = (tickScrollPx > 0) ? (int64_t) ((float) tickScrollPx / pixelsPerTick() / (float) beatTicks) : 0;

    for (int64_t beat = firstBeat; beat <= firstBeat + totalBeats; ++beat)
    {
        const int x = xForTick (beat * beatTicks);
        if (x < grid.getX() || x > grid.getRight())
            continue;
        g.setColour (beat % 4 == 0 ? Base::Border : Base::Border.withAlpha (0.4f));
        g.drawVerticalLine (x, (float) grid.getY(), (float) grid.getBottom());
    }

    int64_t playTick = -1;
    if (engine.isPlaying())
    {
        const int64_t rel = engine.getPlayheadTick() - activeClipInfo.startTick;
        if (rel >= 0 && rel < activeClipInfo.lengthTicks)
            playTick = rel;
    }
    if (playTick >= 0)
    {
        g.setColour (Accent::Cyan.withAlpha (0.5f));
        g.drawVerticalLine (xForTick (playTick), (float) grid.getY(), (float) grid.getBottom());
    }

    g.restoreState();
}

void MetroPianoRoll::drawNotes (juce::Graphics& g)
{
    auto* clip = resolveClip();
    if (clip == nullptr)
        return;

    const auto grid = gridBounds();
    g.saveState();
    g.reduceClipRegion (grid);

    const auto& notes = clip->getNotes();
    for (int i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes.getReference (i);
        const auto bounds = boundsForNote (n);
        if (! bounds.intersects (grid))
            continue;

        const bool selected = selectedNoteIndices.count (i) > 0;
        const bool hovered   = (i == hoverNoteIndex);
        const float intensity = juce::jlimit (0.4f, 1.0f, (float) n.velocity / 127.0f);

        g.setColour (activeTrackInfo.colour.withAlpha (intensity));
        g.fillRoundedRectangle (bounds.toFloat(), 2.0f);

        if (selected)
        {
            g.setColour (Accent::Cyan);
            g.drawRoundedRectangle (bounds.toFloat().expanded (0.5f), 2.0f, 1.5f);
        }
        else if (hovered)
        {
            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.drawRoundedRectangle (bounds.toFloat(), 2.0f, 1.0f);
        }
    }

    g.restoreState();
}

//==============================================================================
//  Hit testing
//==============================================================================
int MetroPianoRoll::noteIndexAt (juce::Point<int> localPos) const
{
    auto* clip = resolveClip();
    if (clip == nullptr || ! gridBounds().contains (localPos))
        return -1;

    const auto& notes = clip->getNotes();
    // Search back-to-front so the most recently-added (visually topmost on
    // overlap) note wins, matching typical piano-roll click behaviour.
    for (int i = notes.size() - 1; i >= 0; --i)
        if (boundsForNote (notes.getReference (i)).contains (localPos))
            return i;
    return -1;
}

//==============================================================================
//  Editing — undoable actions
//==============================================================================
namespace
{
    /** Adds and/or removes a batch of notes as a single undo step. Notes to
     *  remove are located by (tick, note) rather than by index, mirroring
     *  MetroStepSequencer's ClipEditAction — a MidiClip's internal note
     *  array can be re-sorted/re-indexed by other edits between when this
     *  action is created and when it is undone. */
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

    /** Resizes one note's duration, located by (tick, note) — identity is
     *  stable across a resize (start tick and pitch don't change), so this
     *  mirrors MetroStepSequencer's SetVelocityAction exactly. Several of
     *  these are performed under one undoManager transaction while a
     *  resize-drag gesture is in progress, so one Cmd+Z undoes the whole
     *  drag back to its pre-drag length. */
    class ResizeNoteAction final : public juce::UndoableAction
    {
    public:
        ResizeNoteAction (MidiClip& c, int64_t tick, int note, int64_t newDur, int64_t oldDur)
            : clip (c), noteTick (tick), midiNote (note), newDuration (newDur), oldDuration (oldDur) {}

        bool perform() override { return apply (newDuration); }
        bool undo()    override { return apply (oldDuration); }
        int getSizeInUnits() override { return (int) sizeof (*this); }

    private:
        bool apply (int64_t duration)
        {
            const int idx = clip.hitTest (noteTick, midiNote);
            if (idx < 0)
                return false;
            clip.resizeNote (idx, duration);
            return true;
        }

        MidiClip& clip;
        int64_t noteTick;
        int midiNote;
        int64_t newDuration, oldDuration;
    };

    /** Moves one note (start tick and/or pitch), located by its *previous*
     *  (tick, note) identity. Unlike ResizeNoteAction, a move changes the
     *  identity used to find the note again, so perform()/undo() must each
     *  locate by the endpoint they are moving *from*. Several of these are
     *  chained under one transaction during a move-drag, each one's "from"
     *  matching the previous one's "to", so the whole drag still collapses
     *  into a single undo step. */
    class MoveNoteAction final : public juce::UndoableAction
    {
    public:
        MoveNoteAction (MidiClip& c, int64_t fromTick, int fromNote, int64_t toTick, int toNote)
            : clip (c), fromTick (fromTick), fromNote (fromNote), toTick (toTick), toNote (toNote) {}

        bool perform() override { return apply (fromTick, fromNote, toTick, toNote); }
        bool undo()    override { return apply (toTick, toNote, fromTick, fromNote); }
        int getSizeInUnits() override { return (int) sizeof (*this); }

    private:
        bool apply (int64_t locateTick, int locateNote, int64_t newTick, int newNote)
        {
            const int idx = clip.hitTest (locateTick, locateNote);
            if (idx < 0)
                return false;
            clip.moveNote (idx, newTick, newNote);
            return true;
        }

        MidiClip& clip;
        int64_t fromTick, toTick;
        int fromNote, toNote;
    };
}

void MetroPianoRoll::deleteSelectedNotes()
{
    auto* clip = resolveClip();
    if (clip == nullptr || selectedNoteIndices.empty())
        return;

    std::vector<MidiNote> toRemove;
    const auto& notes = clip->getNotes();
    for (int idx : selectedNoteIndices)
        if (idx >= 0 && idx < notes.size())
            toRemove.push_back (notes.getReference (idx));

    if (toRemove.empty())
        return;

    undoManager.beginNewTransaction();
    undoManager.perform (new ClipEditAction (*clip, {}, std::move (toRemove)));
    selectedNoteIndices.clear();
    repaint (gridBounds());
}

void MetroPianoRoll::selectAllNotes()
{
    auto* clip = resolveClip();
    if (clip == nullptr)
        return;

    selectedNoteIndices.clear();
    for (int i = 0; i < clip->getNotes().size(); ++i)
        selectedNoteIndices.insert (i);
    repaint (gridBounds());
}

void MetroPianoRoll::copySelectedNotes()
{
    auto* clip = resolveClip();
    if (clip == nullptr || selectedNoteIndices.empty())
        return;

    clipboard.clear();
    const auto& notes = clip->getNotes();
    for (int idx : selectedNoteIndices)
        if (idx >= 0 && idx < notes.size())
            clipboard.push_back (notes.getReference (idx));
}

void MetroPianoRoll::pasteClipboard()
{
    auto* clip = resolveClip();
    if (clip == nullptr || clipboard.empty())
        return;

    undoManager.beginNewTransaction();
    undoManager.perform (new ClipEditAction (*clip, clipboard, {}));
    repaint (gridBounds());
}

void MetroPianoRoll::commitDragIfAny()
{
    auto* clip = resolveClip();
    if (clip == nullptr)
    {
        dragMode = DragMode::none;
        return;
    }

    if (dragMode == DragMode::moveNote || dragMode == DragMode::resizeNote || dragMode == DragMode::createNote)
        clip->resortNotes();
    else if (dragMode == DragMode::marquee)
    {
        auto worldRect = marqueeRect;
        selectedNoteIndices.clear();
        const auto& notes = clip->getNotes();
        for (int i = 0; i < notes.size(); ++i)
            if (boundsForNote (notes.getReference (i)).intersects (worldRect))
                selectedNoteIndices.insert (i);
    }

    dragMode = DragMode::none;
    dragNoteIndex = -1;
    repaint (gridBounds());
}

//==============================================================================
//  Mouse interaction
//==============================================================================
void MetroPianoRoll::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    if (collapsed || activeTrackIndex < 0)
        return;

    auto* clip = resolveClip();
    if (clip == nullptr)
        return;

    if (rulerBounds().contains (e.getPosition()))
    {
        engine.seekToTick (activeClipInfo.startTick + tickForX (e.x, false));
        return;
    }

    if (! gridBounds().contains (e.getPosition()))
        return;

    if (e.mods.isPopupMenu())
    {
        const int idx = noteIndexAt (e.getPosition());
        if (idx >= 0)
            showNoteContextMenu (idx, e.getScreenPosition());
        return;
    }

    const int idx = noteIndexAt (e.getPosition());

    if (idx >= 0)
    {
        if (e.mods.isShiftDown())
        {
            if (selectedNoteIndices.count (idx) > 0)
                selectedNoteIndices.erase (idx);
            else
                selectedNoteIndices.insert (idx);
            repaint (gridBounds());
            return;
        }

        if (selectedNoteIndices.count (idx) == 0)
        {
            selectedNoteIndices.clear();
            selectedNoteIndices.insert (idx);
        }

        const auto& n = clip->getNotes().getReference (idx);
        dragNoteOriginal = n;
        dragNoteIndex = idx;
        dragStartPos = e.getPosition();

        const auto bounds = boundsForNote (n);
        dragMode = (e.x >= bounds.getRight() - kResizeGrabPx) ? DragMode::resizeNote : DragMode::moveNote;

        undoManager.beginNewTransaction();
        repaint (gridBounds());
        return;
    }

    if (e.mods.isShiftDown())
    {
        dragMode = DragMode::marquee;
        dragStartPos = e.getPosition();
        marqueeRect = { dragStartPos, dragStartPos };
        return;
    }

    // Empty cell, plain click: draw a new note, sized to one snap unit,
    // then let the rest of the drag resize it (click-drag-to-draw, the
    // conventional piano-roll gesture).
    MidiNote n;
    n.note         = noteForY (e.y);
    n.velocity     = kDefaultVelocity;
    n.startTick    = tickForX (e.x, true);
    n.durationTick = juce::jmax ((int64_t) 1, snapTicks);

    undoManager.beginNewTransaction();
    undoManager.perform (new ClipEditAction (*clip, { n }, {}));
    const int newIdx = clip->hitTest (n.startTick, n.note);

    selectedNoteIndices.clear();
    if (newIdx >= 0)
        selectedNoteIndices.insert (newIdx);

    dragNoteOriginal = n;
    dragNoteIndex = newIdx;
    dragStartPos = e.getPosition();
    dragMode = DragMode::createNote;
    repaint (gridBounds());
}

void MetroPianoRoll::mouseDrag (const juce::MouseEvent& e)
{
    auto* clip = resolveClip();
    if (clip == nullptr)
        return;

    switch (dragMode)
    {
        case DragMode::moveNote:
        {
            if (dragNoteIndex < 0 || dragNoteIndex >= clip->getNotes().size())
                return;

            const int64_t deltaTicks = tickForX (e.x, false) - tickForX (dragStartPos.x, false);
            const int64_t rawStart = dragNoteOriginal.startTick + deltaTicks;
            const int64_t newStart = juce::jmax ((int64_t) 0,
                                                 snapTicks > 0 ? ((rawStart + snapTicks / 2) / snapTicks) * snapTicks
                                                              : rawStart);

            const int deltaRows = (int) std::round ((float) (dragStartPos.y - e.y) / (float) kRowHeight);
            const int newNote = juce::jlimit (kLowestNote, kHighestNote, dragNoteOriginal.note + deltaRows);

            const auto& current = clip->getNotes().getReference (dragNoteIndex);
            if (current.startTick != newStart || current.note != newNote)
            {
                undoManager.perform (new MoveNoteAction (*clip, current.startTick, current.note, newStart, newNote));
                repaint (gridBounds());
            }
            return;
        }

        case DragMode::resizeNote:
        case DragMode::createNote:
        {
            if (dragNoteIndex < 0 || dragNoteIndex >= clip->getNotes().size())
                return;

            const int64_t rawEnd = tickForX (e.x, false);
            const int64_t snappedEnd = snapTicks > 0 ? ((rawEnd + snapTicks / 2) / snapTicks) * snapTicks : rawEnd;
            const int64_t newDuration = juce::jmax (snapTicks > 0 ? snapTicks : (int64_t) 1,
                                                     snappedEnd - dragNoteOriginal.startTick);

            const auto& current = clip->getNotes().getReference (dragNoteIndex);
            if (current.durationTick != newDuration)
            {
                undoManager.perform (new ResizeNoteAction (*clip, dragNoteOriginal.startTick, dragNoteOriginal.note,
                                                           newDuration, current.durationTick));
                repaint (gridBounds());
            }
            return;
        }

        case DragMode::marquee:
            marqueeRect = juce::Rectangle<int> (dragStartPos, e.getPosition());
            repaint (gridBounds());
            return;

        case DragMode::none:
        default:
            return;
    }
}

void MetroPianoRoll::mouseUp (const juce::MouseEvent&)
{
    commitDragIfAny();
}

void MetroPianoRoll::mouseMove (const juce::MouseEvent& e)
{
    const int idx = noteIndexAt (e.getPosition());
    if (idx != hoverNoteIndex)
    {
        hoverNoteIndex = idx;
        repaint (gridBounds());
    }

    if (idx >= 0)
    {
        auto* clip = resolveClip();
        if (clip != nullptr && idx < clip->getNotes().size())
        {
            const auto bounds = boundsForNote (clip->getNotes().getReference (idx));
            setMouseCursor (e.x >= bounds.getRight() - kResizeGrabPx ? juce::MouseCursor::LeftRightResizeCursor
                                                                     : juce::MouseCursor::NormalCursor);
            return;
        }
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void MetroPianoRoll::mouseExit (const juce::MouseEvent&)
{
    if (hoverNoteIndex != -1)
    {
        hoverNoteIndex = -1;
        repaint (gridBounds());
    }
}

void MetroPianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f && wheel.deltaX == 0.0f)
        return;

    if (e.mods.isShiftDown())
        tickScrollPx -= (int) (wheel.deltaY * kPixelsPerBeat);
    else
        rowScrollPx -= (int) (wheel.deltaY * kRowHeight * 3.0f);

    clampScroll();
    repaint();
}

void MetroPianoRoll::focusGained (juce::Component::FocusChangeType) { repaint(); }
void MetroPianoRoll::focusLost   (juce::Component::FocusChangeType) { repaint(); }

void MetroPianoRoll::showNoteContextMenu (int noteIndex, juce::Point<int> screenPos)
{
    auto* clip = resolveClip();
    if (clip == nullptr || noteIndex < 0 || noteIndex >= clip->getNotes().size())
        return;

    const auto n = clip->getNotes()[noteIndex];

    juce::PopupMenu menu;
    menu.addItem (1, "SET VELOCITY 64");
    menu.addItem (2, "SET VELOCITY 100");
    menu.addItem (3, "SET VELOCITY 127");
    menu.addSeparator();
    menu.addItem (4, "DELETE NOTE");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                        [this, tick = n.startTick, note = n.note] (int result)
    {
        auto* c = resolveClip();
        if (c == nullptr)
            return;

        const int idx = c->hitTest (tick, note);
        if (idx < 0)
            return;

        undoManager.beginNewTransaction();

        if (result == 4)
        {
            MidiNote toRemove; toRemove.note = note; toRemove.startTick = tick;
            undoManager.perform (new ClipEditAction (*c, {}, { toRemove }));
            selectedNoteIndices.clear();
        }
        else if (result == 1 || result == 2 || result == 3)
        {
            c->setNoteVelocity (idx, result == 1 ? 64 : result == 2 ? 100 : 127);
        }

        repaint (gridBounds());
    });
}

//==============================================================================
//  Keyboard
//==============================================================================
bool MetroPianoRoll::keyPressed (const juce::KeyPress& key)
{
    if (collapsed || activeTrackIndex < 0 || resolveClip() == nullptr)
        return false;

    const bool cmd = key.getModifiers().isCommandDown();
    const bool shift = key.getModifiers().isShiftDown();

    if (key == juce::KeyPress::escapeKey)
    {
        if (! selectedNoteIndices.empty())
        {
            selectedNoteIndices.clear();
            repaint (gridBounds());
            return true;
        }
        return false; // let the arranger clear its own selection next.
    }

    if (key == juce::KeyPress::deleteKey ||
        key == juce::KeyPress::backspaceKey) { deleteSelectedNotes(); return true; }

    if (cmd && shift && key.isKeyCode ('Z')) { undoManager.redo(); repaint(); return true; }
    if (cmd && key.isKeyCode ('Z'))          { undoManager.undo(); repaint(); return true; }
    if (cmd && key.isKeyCode ('A'))          { selectAllNotes(); return true; }
    if (cmd && key.isKeyCode ('C'))          { copySelectedNotes(); return true; }
    if (cmd && key.isKeyCode ('V'))          { pasteClipboard(); return true; }

    if ((key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey ||
        key == juce::KeyPress::upKey   || key == juce::KeyPress::downKey) && ! selectedNoteIndices.empty())
    {
        auto* clip = resolveClip();
        if (clip == nullptr)
            return false;

        const int64_t tickDelta = key == juce::KeyPress::leftKey  ? -snapTicks
                                 : key == juce::KeyPress::rightKey ?  snapTicks : 0;
        const int noteDelta = key == juce::KeyPress::upKey ? 1 : key == juce::KeyPress::downKey ? -1 : 0;

        undoManager.beginNewTransaction();
        for (int idx : selectedNoteIndices)
        {
            if (idx < 0 || idx >= clip->getNotes().size())
                continue;
            const auto& n = clip->getNotes().getReference (idx);
            const int64_t newStart = juce::jmax ((int64_t) 0, n.startTick + tickDelta);
            const int newNote = juce::jlimit (kLowestNote, kHighestNote, n.note + noteDelta);
            if (newStart != n.startTick || newNote != n.note)
                undoManager.perform (new MoveNoteAction (*clip, n.startTick, n.note, newStart, newNote));
        }
        clip->resortNotes();
        repaint (gridBounds());
        return true;
    }

    return false;
}

//==============================================================================
//  Collapse
//==============================================================================
void MetroPianoRoll::setCollapsed (bool shouldBeCollapsed)
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
void MetroPianoRoll::updatePlayheadAndFollow()
{
    if (activeTrackIndex < 0 || ! engine.isPlaying())
        return;

    const int64_t playTick = engine.getPlayheadTick();
    const int64_t rel = playTick - activeClipInfo.startTick;
    if (rel < 0 || rel >= activeClipInfo.lengthTicks)
        return;

    repaint (gridBounds());
    repaint (rulerBounds());

    if (! followEnabled)
        return;

    const auto grid = gridBounds();
    const int x = xForTick (rel);
    const int edgeMargin = kPixelsPerBeat * 2;

    if (x < grid.getX() + edgeMargin)
        tickScrollPx = juce::jmax (0, tickScrollPx - (grid.getX() + edgeMargin - x));
    else if (x > grid.getRight() - edgeMargin)
        tickScrollPx += (x - (grid.getRight() - edgeMargin));

    clampScroll();
}

void MetroPianoRoll::timerCallback()
{
    if (collapsed || activeTrackIndex < 0)
        return;
    updatePlayheadAndFollow();
}

} // namespace dysekt::metro
