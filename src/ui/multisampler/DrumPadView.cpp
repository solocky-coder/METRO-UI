#include "DrumPadView.h"
#include "../../PluginProcessor.h"
#include "../../audio/SfzZoneColours.h"
#include "../AvixLookAndFeel.h"
#include <algorithm>

using DrumPadModel::kNumBanks;
using DrumPadModel::kPadsPerBank;

DrumPadView::DrumPadView (AvixProcessor& p, MultisamplerInstrument& i)
    : processor (p), instrument (i)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setMouseClickGrabsKeyboardFocus (false);
    rebuild();
}

DrumPadView::~DrumPadView()
{
    stopTimer();
    releaseHeldNote();
}

// ── Model ────────────────────────────────────────────────────────────────

void DrumPadView::rebuild()
{
    pads = DrumPadModel::buildPadModel (instrument.zones);
    repaint();
}

void DrumPadView::rebuildAndSelectBank()
{
    releaseHeldNote();
    pads = DrumPadModel::buildPadModel (instrument.zones);
    bank = DrumPadModel::selectInitialBank (pads);
    selectedNote = -1;
    repaint();
}

void DrumPadView::setBank (int newBank)
{
    newBank = juce::jlimit (0, kNumBanks - 1, newBank);
    if (newBank == bank) return;
    releaseHeldNote();           // no stuck notes when switching mid-hold
    bank = newBank;
    repaint();
}

void DrumPadView::setSelectedNote (int note)
{
    selectedNote = (note >= 0 && note <= 127) ? note : -1;
    if (selectedNote >= 0)
        setBank (selectedNote / kPadsPerBank);
    repaint();
}

juce::String DrumPadView::noteName (int n)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    // C3 = 60 (Ableton / Logic convention), so GM kick 36 = C1.
    return juce::String (names[n % 12]) + juce::String (n / 12 - 2);
}

juce::Colour DrumPadView::padColour (const DrumPadModel::PadInfo& p) const
{
    return p.hasCustomColour ? juce::Colour (p.customColourArgb)
                             : SfzZoneColours::zoneColour (p.colourIndex);
}

// ── Audition ─────────────────────────────────────────────────────────────

void DrumPadView::releaseHeldNote()
{
    if (heldNote >= 0)
        processor.pushSfz2UiNote (heldNote, 0, false);
    heldNote = -1;
}

int DrumPadView::velocityForClick (int padIndexInBank, juce::Point<int> pos) const
{
    // Click height -> velocity: top edge ~40, bottom edge 127 (harder low on
    // the pad, like hitting the far side of a drum pad).
    const auto r = padBounds[(size_t) padIndexInBank];
    if (r.getHeight() <= 0) return 100;
    const float frac = juce::jlimit (0.0f, 1.0f, (float) (pos.y - r.getY()) / (float) r.getHeight());
    return juce::jlimit (1, 127, juce::roundToInt (40.0f + frac * 87.0f));
}

// ── Painting ─────────────────────────────────────────────────────────────

void DrumPadView::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();
    const auto area = getLocalBounds().toFloat();
    g.setColour (theme.waveformBg);
    g.fillRoundedRectangle (area, 6.0f * uiScale);

    // Bank bar
    for (int b = 0; b < kNumBanks; ++b)
    {
        const auto r = bankBounds[(size_t) b].toFloat();
        const bool sel = (b == bank);
        g.setColour (sel ? theme.accent.withAlpha (0.85f) : theme.button);
        g.fillRoundedRectangle (r, 4.0f * uiScale);
        g.setColour (sel ? theme.waveformBg : theme.foreground.withAlpha (0.85f));
        // Label is just "BANK n" -- the note range is not shown. Font follows the
        // button height so it stays large and readable.
        g.setFont (juce::FontOptions (juce::jlimit (13.0f * uiScale, 24.0f * uiScale, r.getHeight() * 0.46f), juce::Font::bold));
        g.drawFittedText ("BANK " + juce::String (b + 1),
                          r.toNearestInt().reduced ((int) (4 * uiScale), 0), juce::Justification::centred, 1, 0.8f);
        if (DrumPadModel::bankHasZones (pads, b))
        {
            g.setColour (sel ? theme.waveformBg : theme.accent);
            g.fillEllipse (r.getRight() - 8.0f * uiScale, r.getY() + 3.0f * uiScale, 4.0f * uiScale, 4.0f * uiScale);
        }
    }

    // ZONE SYNC button (right end of the bank row)
    {
        const auto r    = zoneSyncBounds.toFloat();
        const int  look = zoneSyncLook();
        const bool on   = (look >= 2);
        const bool held = (look == 3);
        g.setColour (on ? theme.accent.withAlpha (held ? 0.40f : 0.85f) : theme.button);
        g.fillRoundedRectangle (r, 4.0f * uiScale);
        if (look == 1)   // switched on, but MIDI FOLLOW is off so nothing will sync yet
        {
            g.setColour (theme.accent.withAlpha (0.6f));
            g.drawRoundedRectangle (r.reduced (1.0f), 4.0f * uiScale, 1.5f);
        }
        g.setColour ((on && ! held) ? theme.waveformBg : theme.foreground.withAlpha (0.85f));
        g.setFont (juce::FontOptions (juce::jlimit (11.0f * uiScale, 20.0f * uiScale, r.getHeight() * 0.40f), juce::Font::bold));
        g.drawFittedText (held ? "SYNC HELD" : "ZONE SYNC",
                          r.toNearestInt().reduced ((int) (4 * uiScale), 0), juce::Justification::centred, 1, 0.8f);
    }

    // Pads
    const int baseNote = bank * kPadsPerBank;
    for (int i = 0; i < kPadsPerBank; ++i)
    {
        const int n = baseNote + i;
        const auto& p = pads[(size_t) n];
        auto r = padBounds[(size_t) i].toFloat();
        const bool populated = p.layers > 0;
        const bool lit       = active[(size_t) n];
        const bool selected  = (n == selectedNote);
        const bool dropHover = (n == dropHoverNote);
        const float corner   = 5.0f * uiScale;
        const auto col       = populated ? padColour (p) : theme.foreground;

        g.setColour (populated ? col.withAlpha (p.isRoot ? 0.30f : 0.13f) : theme.darkBar);
        g.fillRoundedRectangle (r, corner);
        if (lit)
        {
            g.setColour (theme.accent.withAlpha (0.28f));
            g.fillRoundedRectangle (r, corner);
        }

        if (populated)
        {
            g.setColour (lit ? theme.accent : col);
            g.drawRoundedRectangle (r.reduced (0.7f), corner, lit ? 2.2f * uiScale : 1.0f * uiScale);
        }
        else
        {
            juce::Path outline;
            outline.addRoundedRectangle (r.reduced (0.7f), corner);
            juce::Path dashed;
            const float dashes[] = { 4.0f * uiScale, 3.0f * uiScale };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dashes, 2);
            g.setColour (dropHover ? theme.accent : theme.foreground.withAlpha (0.25f));
            g.fillPath (dashed);
        }
        if (selected || dropHover)
        {
            g.setColour (theme.accent);
            g.drawRoundedRectangle (r.reduced (1.5f), corner, 2.0f * uiScale);
        }

        // Only the sample name is drawn on a pad -- big and bold so it reads at a
        // glance on stage. Size follows the pad (the grid stretches with the
        // window), clamped so tiny windows stay legible and huge ones don't
        // shout. Long names wrap to two lines, then shrink-to-fit.
        if (populated)
        {
            const float fontSize = juce::jlimit (13.0f * uiScale, 34.0f * uiScale, r.getHeight() * 0.30f);
            g.setColour (theme.foreground);
            g.setFont (juce::FontOptions (fontSize, juce::Font::bold));
            g.drawFittedText (p.name, r.reduced (8.0f * uiScale, 4.0f * uiScale).toNearestInt(),
                              juce::Justification::centred, 2, 0.7f);
        }
    }
}

void DrumPadView::resized()
{
    auto r = getLocalBounds().reduced ((int) (8 * uiScale));
    auto banks = r.removeFromTop ((int) (36 * uiScale));
    const int gap = juce::jmax (2, (int) (4 * uiScale));

    // ZONE SYNC button sits at the right end of the bank row; banks share the rest.
    zoneSyncBounds = banks.removeFromRight ((int) (104 * uiScale));
    banks.removeFromRight (gap * 2);
    const int bw  = (banks.getWidth() - (kNumBanks - 1) * gap) / kNumBanks;
    for (int b = 0; b < kNumBanks; ++b)
        bankBounds[(size_t) b] = { banks.getX() + b * (bw + gap), banks.getY(), bw, banks.getHeight() };

    r.removeFromTop ((int) (8 * uiScale));
    const int cellW = (r.getWidth()  - 3 * gap) / 4;
    const int cellH = (r.getHeight() - 3 * gap) / 4;
    for (int i = 0; i < kPadsPerBank; ++i)
        padBounds[(size_t) i] = { r.getX() + (i % 4) * (cellW + gap),
                                  r.getY() + (i / 4) * (cellH + gap), cellW, cellH };
}

// ── Hit testing / mouse ──────────────────────────────────────────────────

bool DrumPadView::isZoneSyncHeld() const
{
    const bool playing   = processor.sequencer.isPlaying()
                           || processor.hostTransportPlaying.load (std::memory_order_relaxed);
    const bool recording = processor.sequencer.isRecording();
    return playing && ! recording;
}

int DrumPadView::zoneSyncLook() const
{
    if (! processor.padZoneSyncEnabled.load (std::memory_order_relaxed)) return 0;
    if (! processor.midiSelectsSlice.load (std::memory_order_relaxed))   return 1;
    return isZoneSyncHeld() ? 3 : 2;
}

int DrumPadView::zoneIndexForNote (int note, int velocity) const
{
    note = juce::jlimit (0, 127, note);
    const auto& zones = instrument.zones;
    for (int zi = 0; zi < (int) zones.size(); ++zi)
    {
        const auto& z = zones[(size_t) zi];
        if (z.enabled && z.keyInRange (note) && z.velocityInRange (velocity))
            return zi;
    }
    return pads[(size_t) note].zoneIndex;
}

int DrumPadView::hitTest (juce::Point<int> pos) const
{
    if (zoneSyncBounds.contains (pos)) return kHitZoneSync;
    for (int b = 0; b < kNumBanks; ++b)
        if (bankBounds[(size_t) b].contains (pos)) return -100 - b;
    for (int i = 0; i < kPadsPerBank; ++i)
        if (padBounds[(size_t) i].contains (pos)) return bank * kPadsPerBank + i;
    return -1;
}

void DrumPadView::mouseDown (const juce::MouseEvent& e)
{
    const int hit = hitTest (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (hit >= 0 && onPadContextMenu)
        {
            selectedNote = hit;
            repaint();
            onPadContextMenu (hit, e.getScreenPosition());
        }
        return;
    }

    if (hit == kHitZoneSync)
    {
        processor.padZoneSyncEnabled.store (! processor.padZoneSyncEnabled.load (std::memory_order_relaxed),
                                            std::memory_order_relaxed);
        repaint();
        return;
    }
    if (hit <= -100) { setBank (-100 - hit); return; }
    if (hit < 0) return;

    selectedNote = hit;
    const auto& p = pads[(size_t) hit];
    if (p.layers > 0)
    {
        releaseHeldNote();
        heldNote = hit;
        processor.pushSfz2UiNote (hit, velocityForClick (hit - bank * kPadsPerBank, e.getPosition()), true);
        if (onPadSelected && p.zoneIndex >= 0)
            onPadSelected (p.zoneIndex);
    }
    repaint();
}

void DrumPadView::mouseUp (const juce::MouseEvent&)
{
    releaseHeldNote();
}

void DrumPadView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (w.deltaY > 0.0f)      setBank (bank - 1);
    else if (w.deltaY < 0.0f) setBank (bank + 1);
}

void DrumPadView::visibilityChanged()
{
    if (isVisible())
    {
        // Don't replay notes played while the view was hidden: start from "now".
        lastNoteOnSeq = processor.sfz2NoteOnSeq.load (std::memory_order_acquire);
        startTimerHz (30);
    }
    else
    {
        stopTimer();
        releaseHeldNote();
        dropHoverNote = -1;
    }
}

void DrumPadView::timerCallback()
{
    // Reads the sfzPlayer2 active-note mask, so pads also light for MIDI from
    // the host / arranger / hardware, not just on-screen clicks.
    bool changed = false;

    // MIDI follow: show the bank of the note that just played -- but ONLY while
    // the app-wide MIDI FOLLOW button (processor.midiSelectsSlice) is active.
    // The sequence counter is consumed either way, so notes played while follow
    // is off are never replayed when it is switched on. Switches to the note's
    // bank AND marks that pad as the selected pad (setSelectedNote does both).
    //
    // ZONE SYNC (separate button, off by default): additionally selects the note's
    // zone in ZONES / the edit panel via onPadSelected -- the same call a pad click
    // makes. It is skipped (the bank + pad highlight still follow) while:
    //   - playback is running (internal sequencer or host transport), unless the
    //     sequencer is recording -> see isZoneSyncHeld(); resumes by itself on stop,
    //     without jumping to a note played in the meantime;
    //   - a mouse button is down, so a note can't swap the zone under a drag in
    //     the edit panel;
    //   - the pad is empty (nothing to select: the current zone stays).
    {
        const std::uint32_t seq = processor.sfz2NoteOnSeq.load (std::memory_order_acquire);
        if (seq != lastNoteOnSeq)
        {
            lastNoteOnSeq = seq;
            if (processor.midiSelectsSlice.load (std::memory_order_relaxed))
            {
                const int note = processor.sfz2LastNoteOn.load (std::memory_order_relaxed);
                if (note >= 0 && note < DrumPadModel::kNumNotes)
                {
                    setSelectedNote (note);

                    if (processor.padZoneSyncEnabled.load (std::memory_order_relaxed)
                        && ! isZoneSyncHeld()
                        && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown()
                        && onPadSelected)
                    {
                        const int vel = (int) processor.sfz2LastNoteOnVelocity[note].load (std::memory_order_relaxed);
                        const int zi  = zoneIndexForNote (note, vel);
                        if (zi >= 0 && zi < (int) instrument.zones.size())
                            onPadSelected (zi);
                    }
                }
            }
        }
    }

    // Repaint the ZONE SYNC button when its look changes (toggled, MIDI FOLLOW
    // switched, or playback started/stopped).
    {
        const int look = zoneSyncLook();
        if (look != zoneSyncLookShown) { zoneSyncLookShown = look; changed = true; }
    }

    for (int n = 0; n < DrumPadModel::kNumNotes; ++n)
    {
        const bool on = (processor.sfz2ActiveNotes[n / 64].load (std::memory_order_relaxed)
                         & (std::uint64_t (1) << (n % 64))) != 0;
        if (on != active[(size_t) n]) { active[(size_t) n] = on; changed = true; }
    }
    if (changed) repaint();
}

// ── Drag & drop (sample -> empty pad) ────────────────────────────────────

bool DrumPadView::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const auto ext = juce::File (f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac"
            || ext == ".ogg" || ext == ".mp3")
            return true;
    }
    return false;
}

void DrumPadView::fileDragEnter (const juce::StringArray&, int x, int y) { fileDragMove ({}, x, y); }

void DrumPadView::fileDragMove (const juce::StringArray&, int x, int y)
{
    const int hit = hitTest ({ x, y });
    const int target = (hit >= 0 && pads[(size_t) hit].layers == 0) ? hit : -1;   // empty pads only
    if (target != dropHoverNote) { dropHoverNote = target; repaint(); }
}

void DrumPadView::fileDragExit (const juce::StringArray&)
{
    dropHoverNote = -1;
    repaint();
}

void DrumPadView::filesDropped (const juce::StringArray& files, int x, int y)
{
    const int hit = hitTest ({ x, y });
    dropHoverNote = -1;
    repaint();
    if (hit < 0 || pads[(size_t) hit].layers != 0 || ! onSampleDropped)
        return;

    for (const auto& f : files)
        if (isInterestedInFileDrag (juce::StringArray (f)))
        {
            onSampleDropped (juce::File (f), hit);   // one sample per drop: the trim dialog is modal
            return;
        }
}
