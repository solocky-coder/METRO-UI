// =============================================================================
//  Sf2InstrumentWorkspace.cpp  —  3-column SF2 instrument workspace
// =============================================================================
//  See Sf2InstrumentWorkspace.h for the full design rationale. This is a
//  from-scratch rewrite drafted against sf2-metro-reference-keyboard.svg,
//  not a reflow of the old panel. The FineTune stepper is retained as a
//  deliberate compatibility deviation; it has no separate control in the SVG.
// =============================================================================
#include "Sf2InstrumentWorkspace.h"
#include "DysektLookAndFeel.h"
#include "UIHelpers.h"
#include "../PluginProcessor.h"
#include <algorithm>
#include <cmath>

// =============================================================================
//  Shared SF2 track colour
// =============================================================================
juce::Colour Sf2InstrumentWorkspace::trackColourForPreset (const Sf2PresetInfo& preset)
{
    // Palette now lives in UIHelpers::sf2TrackColourForPreset() so
    // Sf2ProgramGrid can share the exact same colour without depending on
    // this (heavier) workspace class. This wrapper is kept for existing
    // call sites (PluginEditor.cpp, PresetListModel).
    return UIHelpers::sf2TrackColourForPreset (preset);
}

// =============================================================================
//  PresetListModel — backs column 1's juce::ListBox
// =============================================================================
//  Thin adapter: presents Sf2InstrumentWorkspace::presetList, filtered by
//  filteredPresetIndices (rebuilt from the search box text), and forwards
//  clicks back to the owner. The underlying Sf2ProgramGrid keeps holding the
//  real preset/channel data (so getProgramGrid() stays correct for
//  PluginEditor's mixerPanel wiring) — this model is presentation-only.
class Sf2InstrumentWorkspace::PresetListModel : public juce::ListBoxModel
{
public:
    explicit PresetListModel (Sf2InstrumentWorkspace& ownerIn) : owner (ownerIn) {}

    int getNumRows() override
    {
        return (int) owner.filteredPresetIndices.size();
    }

    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override
    {
        if (rowNumber < 0 || rowNumber >= (int) owner.filteredPresetIndices.size())
            return;

        const int presetIdx = owner.filteredPresetIndices[(size_t) rowNumber];
        if (presetIdx < 0 || presetIdx >= (int) owner.presetList.size())
            return;

        const auto& info = owner.presetList[(size_t) presetIdx];
        const auto& theme = getTheme();
        juce::Rectangle<int> row (0, 0, width, height);

        const bool isCurrent = presetIdx == owner.effectiveDisplayPresetIndex();

        const auto& chMap = owner.programGrid.getPresetChannels();
        const auto chIt = chMap.find (presetIdx);
        const bool assigned = chIt != chMap.end() && chIt->second >= 1;
        const auto presetColour = Sf2InstrumentWorkspace::trackColourForPreset (info);

        // An assigned preset is the same musical object as its Arranger track,
        // so give it that track's colour. Preview-only rows deliberately retain
        // the theme accent, preserving the existing audition treatment.
        if (rowIsSelected || isCurrent)
        {
            const auto highlightColour = assigned ? presetColour : theme.accent;
            g.setColour (highlightColour.withAlpha (0.16f));
            g.fillRect (row);
            g.setColour (highlightColour);
            g.fillRect (row.removeFromLeft (4));
        }
        else if (assigned)
        {
            g.setColour (presetColour.withAlpha (0.08f));
            g.fillRect (row);
            g.setColour (presetColour.withAlpha (0.78f));
            g.fillRect (row.removeFromLeft (4));
        }

        auto textArea = row.reduced (12, 0);
        auto badgeArea = textArea.removeFromRight (48);

        g.setFont (DysektLookAndFeel::makeFont (17.f, isCurrent));
        g.setColour (assigned ? presetColour : theme.foreground);
        g.drawText (info.name, textArea, juce::Justification::centredLeft, true);

        g.setFont (DysektLookAndFeel::makeFont (14.f, true));
        g.setColour (assigned ? presetColour.withAlpha (0.82f)
                              : theme.foreground.withAlpha (0.55f));
        juce::String badge = juce::String (info.preset).paddedLeft ('0', 3);
        if (assigned)
            badge = "CH" + juce::String (chIt->second);
        g.drawText (badge, badgeArea, juce::Justification::centredRight);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (row < 0 || row >= (int) owner.filteredPresetIndices.size())
            return;
        const int presetIdx = owner.filteredPresetIndices[(size_t) row];

        if (e.mods.isPopupMenu())
            owner.handlePresetRightClicked (presetIdx, e.getScreenPosition());
        else
            owner.handlePresetLeftClicked (presetIdx);
    }

private:
    Sf2InstrumentWorkspace& owner;
};

// =============================================================================
//  CompactKeyboard — dedicated scrollable keyboard strip
// =============================================================================
//  KeysPanel (the shared full-instrument keyboard) is a 128-key component
//  with an attached sample-zone matrix and no way to restrict its visible
//  range. Embedding it in this ~110px docked strip meant its zone-matrix
//  placeholder ("No zones loaded") consumed most of the height and squeezed
//  75 white keys into column 3's width — exactly the "empty black area" /
//  "keyboard too narrow" problems reported against the first build. This is
//  a small purpose-built replacement: just the keys, a 2-octave scrollable
//  window ('<'/'>' shift by a full octave, and it auto-jumps to follow
//  incoming MIDI/preview notes — see shiftOctave()/scrollToNoteIfHidden()),
//  with note-on highlighting read directly from processor.sfzActiveNotes and
//  click/drag preview routed through the same sfzUiNoteOn/OffRequest atomics
//  KeysPanel used. Note-naming matches this codebase's existing convention
//  (see KeysPanel.cpp: octave = note/12 - 2, so MIDI 60 = C3).
class Sf2InstrumentWorkspace::CompactKeyboard : public juce::Component,
                                                 private juce::Timer
{
public:
    explicit CompactKeyboard (Sf2InstrumentWorkspace& ownerIn) : owner (ownerIn)
    {
        // Runs its own repaint timer rather than relying on the parent
        // workspace's repaint() to cascade down here — real MIDI note-on/off
        // arrives on the audio thread asynchronously, so this can't wait for
        // a mouse-driven or workspace-level repaint to notice the change.
        startTimerHz (30);

        addAndMakeVisible (octDownBtn);
        addAndMakeVisible (octUpBtn);
        octDownBtn.onClick = [this] { shiftOctave (-1); };
        octUpBtn  .onClick = [this] { shiftOctave (+1); };
    }

    void resized() override { layoutButtons(); rebuildKeyRects(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour::fromString ("FF0a1216"));
        for (auto& k : keyRects) if (! k.isBlack) drawKey (g, k);
        for (auto& k : keyRects) if (k.isBlack)   drawKey (g, k);
        g.setColour (juce::Colour::fromString ("FF263b45"));
        g.drawRect (getLocalBounds(), 1);
    }

    void mouseDown (const juce::MouseEvent& e) override { handlePress (e.getPosition()); }
    void mouseDrag (const juce::MouseEvent& e) override { handlePress (e.getPosition()); }
    void mouseUp   (const juce::MouseEvent&)   override { releaseHeld(); }
    void mouseExit (const juce::MouseEvent&)   override { releaseHeld(); }

    /** For the "KEYBOARD | C3-C5" header label, which is painted by the
     *  owner workspace rather than this component — kept in sync with
     *  whatever window is actually showing instead of a hardcoded string. */
    juce::String getRangeLabel() const
    {
        return noteName (lowNote) + "-" + noteName (lowNote + kWindowSemitones);
    }

private:
    void timerCallback() override
    {
        const uint64_t lo = owner.processor.sfzActiveNotes[0].load (std::memory_order_relaxed);
        const uint64_t hi = owner.processor.sfzActiveNotes[1].load (std::memory_order_relaxed);
        if (lo != lastLo || hi != lastHi)
        {
            lastLo = lo;
            lastHi = hi;

            // Jump the visible window to follow incoming MIDI/preview notes,
            // same idea as KeysPanel::scrollToOctaveForNote() — find the
            // lowest currently-active note and, if it's outside the current
            // 2-octave window, shift the window to start at that note's own
            // octave.
            for (int n = 0; n < 128; ++n)
            {
                const uint64_t word = (n < 64) ? lo : hi;
                const int      bit  = (n < 64) ? n  : (n - 64);
                if ((word >> bit) & 1)
                {
                    scrollToNoteIfHidden (n);
                    break;
                }
            }

            repaint();
        }
    }

    uint64_t lastLo { 0 }, lastHi { 0 };

    struct KeyRect { juce::Rectangle<float> bounds; int note; bool isBlack; };
    std::vector<KeyRect> keyRects;
    int heldNote { -1 };

    // ── Scrollable window ────────────────────────────────────────────────
    // Shows kWindowSemitones (2 octaves) starting at lowNote, which is
    // always kept a multiple of 12 (a C) so the window always starts/ends
    // cleanly on octave boundaries. '<'/'>' shift by a full octave;
    // timerCallback() above also auto-shifts it to follow incoming notes.
    static constexpr int kWindowSemitones = 24;   // C..C two octaves later
    static constexpr int kMaxLowNote      = 96;   // (96..120) is the top window that still fits 0-127
    int lowNote { 60 };   // C3

    juce::TextButton octDownBtn { "<" }, octUpBtn { ">" };
    static constexpr int kBtnW = 18;   // matches KeysPanel's kTransposeBtnW
    juce::Rectangle<int> keysArea;

    void layoutButtons()
    {
        auto b = getLocalBounds();
        octDownBtn.setBounds (b.removeFromLeft (kBtnW));
        octUpBtn  .setBounds (b.removeFromRight (kBtnW));
        keysArea = b;
        octDownBtn.setEnabled (lowNote > 0);
        octUpBtn  .setEnabled (lowNote < kMaxLowNote);
    }

    void shiftOctave (int dir)
    {
        const int next = juce::jlimit (0, kMaxLowNote, lowNote + dir * 12);
        if (next == lowNote) return;
        lowNote = next;
        layoutButtons();
        rebuildKeyRects();
        repaint();
        owner.repaint();   // the "KEYBOARD | ..." range label lives on the owner
    }

    void scrollToNoteIfHidden (int note)
    {
        if (note < 0 || note > 127) return;
        if (note >= lowNote && note <= lowNote + kWindowSemitones) return;   // already visible

        const int targetLow = juce::jlimit (0, kMaxLowNote, (note / 12) * 12);
        if (targetLow == lowNote) return;
        lowNote = targetLow;
        layoutButtons();
        rebuildKeyRects();
        owner.repaint();   // the "KEYBOARD | ..." range label lives on the owner
    }

    static juce::String noteName (int note)
    {
        // Delegates to UIHelpers::midiNoteToName() — see that function's doc
        // comment for why this used to be its own hand-copied implementation.
        return UIHelpers::midiNoteToName (note);
    }

    void rebuildKeyRects()
    {
        keyRects.clear();
        if (keysArea.getWidth() <= 0 || keysArea.getHeight() <= 0) return;

        static const int semiToWhite[12] = { 0,-1,1,-1,2, 3,-1,4,-1,5,-1,6 };

        const int highNote = lowNote + kWindowSemitones;

        int numWhite = 0;
        for (int n = lowNote; n <= highNote; ++n)
            if (semiToWhite[n % 12] >= 0) ++numWhite;

        const float originX = (float) keysArea.getX();
        const float w = (float) keysArea.getWidth() / (float) numWhite;
        const float h = (float) keysArea.getHeight();
        const float blackW = w * 0.62f;
        const float blackH = h * 0.6f;

        int whiteIdx = 0;
        for (int n = lowNote; n <= highNote; ++n)
        {
            if (semiToWhite[n % 12] >= 0)
            {
                keyRects.push_back ({ { originX + whiteIdx * w, 0.f, w, h }, n, false });
                ++whiteIdx;
            }
        }
        for (int n = lowNote; n <= highNote; ++n)
        {
            if (semiToWhite[n % 12] < 0)
            {
                int precedingWhites = 0;
                for (int m = lowNote; m < n; ++m)
                    if (semiToWhite[m % 12] >= 0) ++precedingWhites;
                keyRects.push_back ({ { originX + precedingWhites * w - blackW * 0.5f, 0.f, blackW, blackH }, n, true });
            }
        }
    }

    int noteAt (juce::Point<int> p) const
    {
        const auto pf = p.toFloat();
        for (auto& k : keyRects) if (k.isBlack && k.bounds.contains (pf)) return k.note;
        for (auto& k : keyRects) if (! k.isBlack && k.bounds.contains (pf)) return k.note;
        return -1;
    }

    void handlePress (juce::Point<int> pos)
    {
        const int note = noteAt (pos);
        if (note == heldNote) return;
        releaseHeld();
        if (note >= 0)
        {
            owner.processor.sfzUiNoteOnRequest.store (note, std::memory_order_relaxed);
            heldNote = note;
        }
        repaint();
    }

    void releaseHeld()
    {
        if (heldNote >= 0)
        {
            owner.processor.sfzUiNoteOffRequest.store (heldNote, std::memory_order_relaxed);
            heldNote = -1;
            repaint();
        }
    }

    bool isNoteActive (int note) const
    {
        if (note < 64)
            return ((owner.processor.sfzActiveNotes[0].load (std::memory_order_relaxed) >> note) & 1ull) != 0;
        return ((owner.processor.sfzActiveNotes[1].load (std::memory_order_relaxed) >> (note - 64)) & 1ull) != 0;
    }

    void drawKey (juce::Graphics& g, const KeyRect& k) const
    {
        const auto& theme = getTheme();
        const bool active = k.note == heldNote || isNoteActive (k.note);

        juce::Colour base = k.isBlack ? juce::Colour::fromString ("FF10181c")
                                       : juce::Colour::fromString ("FFe8edf0");
        if (active) base = theme.accent;

        g.setColour (base);
        g.fillRect (k.bounds);
        g.setColour (juce::Colour::fromString ("FF060a0c").withAlpha (0.85f));
        g.drawRect (k.bounds, 1.f);
    }

    Sf2InstrumentWorkspace& owner;
};

// =============================================================================
//  Constructor / destructor
// =============================================================================

Sf2InstrumentWorkspace::Sf2InstrumentWorkspace (DysektProcessor& p)
    : channelFxPanel (p), processor (p)
{
    // The grid is retained purely as the shared data model (channel
    // assignments, preset list) that PluginEditor reads via getProgramGrid();
    // it is never painted or made visible — column 1 is a search+list now.
    programGrid.onPresetSelected        = nullptr;
    programGrid.onChannelChanged        = nullptr;
    programGrid.onPreviewToggled        = nullptr;
    programGrid.onAssignedPresetClicked = nullptr;
    addChildComponent (programGrid);
    programGrid.setBounds ({});

    // ── Column 1 — search box + preset list ───────────────────────────────────
    searchBox.setTextToShowWhenEmpty ("Search presets", getTheme().foreground.withAlpha (0.45f));
    searchBox.setMultiLine (false);
    searchBox.setFont (DysektLookAndFeel::makeFont (16.f));
    searchBox.setColour (juce::TextEditor::backgroundColourId, getTheme().darkBar.darker (0.2f));
    searchBox.setColour (juce::TextEditor::textColourId, getTheme().foreground);
    searchBox.onTextChange = [this]
    {
        currentSearchFilter = searchBox.getText();
        rebuildFilteredPresetRows();
        presetListBox.updateContent();
        repaint();
    };
    addAndMakeVisible (searchBox);

    presetListModel = std::make_unique<PresetListModel> (*this);
    presetListBox.setModel (presetListModel.get());
    presetListBox.setRowHeight (44);
    presetListBox.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    presetListBox.setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (presetListBox);

    browseButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser> (
            "Load SoundFont",
            juce::File::getSpecialLocation (juce::File::userMusicDirectory),
            "*.sf2");

        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto result = fc.getResult();
                if (result.existsAsFile())
                    onFileChosen (result);
            });
    };
    addAndMakeVisible (browseButton);

    compactKeyboard = std::make_unique<CompactKeyboard> (*this);
    addAndMakeVisible (*compactKeyboard);
    addAndMakeVisible (channelFxPanel);   // always visible — Column 3 has no tab switch

    startTimerHz (30);
}

Sf2InstrumentWorkspace::~Sf2InstrumentWorkspace() = default;

// =============================================================================
//  Preset selection / channel assignment
// =============================================================================
//  These carry over the exact assignment rules from the old panel/grid
//  (channels 1-2 reserved, chromatic-slice/sfzPlayer2 channels blocked,
//  1:1 MIDI-channel-to-FluidSynth-channel routing) — only the input path
//  changed, from grid-cell clicks to list-row clicks.

void Sf2InstrumentWorkspace::handlePresetLeftClicked (int idx)
{
    if (idx < 0 || idx >= (int) presetList.size()) return;
    const auto& info = presetList[(size_t) idx];

    processor.sfzPlayer.setDisplayPresetIndex (idx);

    if (processor.sf2PreviewRequestedBank.load (std::memory_order_relaxed)    != info.bank ||
        processor.sf2PreviewRequestedProgram.load (std::memory_order_relaxed) != info.preset)
    {
        processor.sf2PreviewRequestedBank.store    (info.bank,   std::memory_order_relaxed);
        processor.sf2PreviewRequestedProgram.store (info.preset, std::memory_order_relaxed);
        processor.loadSoundFontAsync (processor.sfzPlayer.getLoadedFile(),
                                       SoundFontLoadTarget::SfPlayer,
                                       info.bank, info.preset);
    }

    const auto& chMap = programGrid.getPresetChannels();
    if (chMap.count (idx) && chMap.at (idx) >= 1)
    {
        channelFxPanel.setSelectedChannel (chMap.at (idx) - 1);
        repaint();
        return;   // already routed on a real channel — just select, don't re-preview
    }

    channelFxPanel.setSelectedChannel (-1);
    processor.sfzPlayer.previewPreset (info.bank, info.preset);
    repaint();
}

void Sf2InstrumentWorkspace::handlePresetRightClicked (int idx, juce::Point<int> screenPos)
{
    if (idx < 0 || idx >= (int) presetList.size()) return;

    juce::PopupMenu menu;
    menu.addItem (100, "Remove channel assignment");
    for (int ch = 3; ch <= 16; ++ch)
    {
        const uint32_t chromaMask = processor.chromaticSliceChannelMask.load (std::memory_order_relaxed);
        const uint32_t sfz2Mask   = processor.sfzPlayer2ChannelMask.load (std::memory_order_relaxed);
        const bool blocked = (chromaMask | sfz2Mask) & (1u << ch);
        menu.addItem (ch, "Assign MIDI channel " + juce::String (ch),
                      ! blocked);
    }

    const float ms = DysektLookAndFeel::getMenuScale();
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
            .withParentComponent (getTopLevelComponent())
            .withStandardItemHeight ((int) (24 * ms)),
        [this, idx] (int result)
        {
            if (result == 100)      handleChannelAssigned (idx, 0);
            else if (result >= 3 && result <= 16) handleChannelAssigned (idx, result);
        });
}

void Sf2InstrumentWorkspace::handleChannelAssigned (int presetIdx, int ch)
{
    if (presetIdx < 0 || presetIdx >= (int) presetList.size()) return;
    const auto& info = presetList[(size_t) presetIdx];

    if (ch != 0 && ch < 3)
        return;   // channel 1/2 reserved — silently reject
    if (ch >= 1 && ch <= 16)
    {
        const uint32_t chromaMask = processor.chromaticSliceChannelMask.load (std::memory_order_relaxed);
        const uint32_t sfz2Mask   = processor.sfzPlayer2ChannelMask.load (std::memory_order_relaxed);
        if ((chromaMask | sfz2Mask) & (1u << ch))
            return;   // channel is slicer-owned or sfzPlayer2-owned — silently reject
    }

    if (ch == 0)
    {
        // Channels 0/1 (MIDI ch 1/2) are reserved for the Slicer/SFZ-Player
        // and are never valid targets, so the sweep starts at 2.
        for (int c = 2; c < 16; ++c)
            processor.sfzPlayer.setPresetOnChannel (c, 0, 0);

        const auto& chMap = programGrid.getPresetChannels();
        for (auto& kv : chMap)
        {
            if (kv.first == presetIdx) continue;
            if (kv.second >= 3 && kv.second <= 16 && kv.first < (int) presetList.size())
                processor.sfzPlayer.setPresetOnChannel (kv.second - 1,
                                                        presetList[(size_t) kv.first].bank,
                                                        presetList[(size_t) kv.first].preset);
        }

        if (onPresetChannelAssigned)
            onPresetChannelAssigned (info, 0);

        uint32_t mask = 0u;
        const auto& chMap2 = programGrid.getPresetChannels();
        for (const auto& kv : chMap2)
        {
            if (kv.first == presetIdx) continue;
            if (kv.second >= 3 && kv.second <= 16)
                mask |= (1u << kv.second);
        }
        processor.sfPlayerChannelMask.store      (mask, std::memory_order_relaxed);
        processor.savedSfPlayerChannelMask.store (mask, std::memory_order_relaxed);
    }
    else
    {
        processor.sfzPlayer.setPresetOnChannel (ch - 1, info.bank, info.preset);

        uint32_t mask = processor.sfPlayerChannelMask.load (std::memory_order_relaxed);
        mask |= (1u << ch);
        processor.sfPlayerChannelMask.store      (mask, std::memory_order_relaxed);
        processor.savedSfPlayerChannelMask.store (mask, std::memory_order_relaxed);

        if (onPresetChannelAssigned)
            onPresetChannelAssigned (info, ch);
    }

    presetListBox.updateContent();
    repaint();
}

void Sf2InstrumentWorkspace::rebuildFilteredPresetRows()
{
    filteredPresetIndices.clear();
    filteredPresetIndices.reserve (presetList.size());

    const juce::String needle = currentSearchFilter.trim().toLowerCase();
    for (int i = 0; i < (int) presetList.size(); ++i)
    {
        if (needle.isEmpty() || presetList[(size_t) i].name.toLowerCase().contains (needle))
            filteredPresetIndices.push_back (i);
    }
}

// =============================================================================
//  Layout
// =============================================================================

void Sf2InstrumentWorkspace::resized()
{
    auto bounds = getLocalBounds();
    if (bounds.getWidth() < kNarrowThreshold)
        layoutNarrow (bounds);
    else
        layoutWide (bounds);
}

void Sf2InstrumentWorkspace::layoutWide (juce::Rectangle<int> bounds)
{
    topBarZone = bounds.removeFromTop (kTopBarH);
    loadedPillZone = topBarZone.removeFromRight (170).reduced (4);

    const int w = bounds.getWidth();
    const int colPresetsW = juce::roundToInt ((float) w * kColPresetsFrac);
    const int topSectionW = juce::roundToInt ((float) w * kColVoiceFrac);   // cap for Sections A/B only

    col1Zone = bounds.removeFromLeft (colPresetsW);
    col2Zone = bounds;   // remainder — merged voice + channel-mixer panel

    // ── Column 1 — search / list / bank footer / browse ────────────────────
    {
        auto c = col1Zone.reduced (kPad);
        searchZone = c.removeFromTop (26);
        c.removeFromTop (kPad);
        browseButtonZone = c.removeFromBottom (24);
        c.removeFromBottom (kPad);
        bankFooterZone = c.removeFromBottom (18);
        c.removeFromBottom (kPad / 2);
        listZone = c;

        searchBox.setBounds (searchZone);
        presetListBox.setBounds (listZone);
        browseButton.setBounds (browseButtonZone);
    }

    // ── Column 2 — merged voice + channel-mixer panel ───────────────────────
    // Sections A/B/C stack top-to-bottom in one continuous scan, no vertical
    // divider between them — see header comment for the rationale/scope of
    // each section.
    auto c = col2Zone.reduced (kPad);

    // ── Section A — active preset header + 3 knobs (+ fine stepper) ────────
    // Width capped to topSectionW and left-aligned, so the knob row keeps
    // its original pixel layout instead of stretching across the full
    // merged width.
    {
        auto top = c.removeFromTop (180 - kPad);
        auto topSection = top.removeFromLeft (juce::jmin (topSectionW, top.getWidth()));

        activePresetHeaderZone = topSection.removeFromTop (54);
        topSection.removeFromTop (kPad);

        // Knob cell is label(16) + circle + value(20), not just the circle —
        // drawKnob() carves those three sub-rects out of whatever bounds it's
        // given, so the cell passed in has to be tall enough to hold all three
        // without cramming the circle. Trimmed from 96 to 84 — still plenty
        // of room for the circle, and the 12px it frees up goes toward
        // Section C below (see the elastic keyboard/mixer split there),
        // which needed the room far more than this knob row's slack did.
        constexpr int kKnobCellH = 84;
        auto knobRow = topSection.removeFromTop (kKnobCellH);
        const int knobGap = (knobRow.getWidth() - 3 * kKnobW) / 4;
        knobRow.removeFromLeft (knobGap);
        levelZone = knobRow.removeFromLeft (kKnobW);
        knobRow.removeFromLeft (knobGap);
        transZone = knobRow.removeFromLeft (kKnobW);
        knobRow.removeFromLeft (knobGap);
        panZone   = knobRow.removeFromLeft (kKnobW);

        // Fine-tune stepper — deliberately small/secondary, docked under
        // Transpose; see header comment for why this exists at all.
        fineZone = juce::Rectangle<int> (transZone.getX(), knobRow.getY() + kKnobCellH + 6,
                                          transZone.getWidth(), 16);
    }
    c.removeFromTop (kPad);

    // ── Section B — SF2 filter + global reverb sliders ──────────────────────
    // Same width cap as Section A so the two read as one coherent block.
    // Row height trimmed from 34 to 30 and inter-row gap from kPad to 6 —
    // drawSlider() only needs a 16px label row + a 12px track, so 34px
    // always had a few spare px per row; freed up for Section C below.
    {
        auto filterSection = c.removeFromTop (4 * 30 + 3 * 6);
        filterSection = filterSection.removeFromLeft (juce::jmin (topSectionW, filterSection.getWidth()));

        filterCutoffZone = filterSection.removeFromTop (30);
        filterSection.removeFromTop (6);
        filterResonanceZone = filterSection.removeFromTop (30);
        filterSection.removeFromTop (6);
        reverbSendZone = filterSection.removeFromTop (30);
        filterSection.removeFromTop (6);
        reverbDampZone = filterSection.removeFromTop (30);
    }
    c.removeFromTop (kPad);

    // ── Section C — channel mixer, note activity, keyboard ──────────────────
    // Full merged-column width (no cap) — this is where the extra room
    // freed up by dropping the old column 3 divider actually goes.
    //
    // BUGFIX #1: this used to hand the keyboard/meter their full fixed pixel
    // budget unconditionally and give Sf2ChannelFxPanel whatever was left —
    // which, once Sections A/B/paddings had taken their share out of a
    // realistic window height, routinely rounded down to ~0px. The mixer
    // (the actual point of the column 2/3 merge) would silently vanish
    // while the keyboard underneath it rendered at full size.
    //
    // BUGFIX #2: the first fix over-corrected — giving the mixer "whatever
    // was left" also meant that on a tall window it could be handed far
    // more height than it ever needed, sitting as dead space below its
    // content.
    //
    // REDESIGN: Sf2ChannelFxPanel moved from a column-per-channel grid to a
    // row-per-channel list (see the header comment there for why). Its
    // ideal height is no longer a single fixed constant — it's genuinely
    // proportional to how many channels are active right now
    // (getActiveChannelCount() * rowHeight()), so we ask it directly rather
    // than guessing. Capped at a handful of rows before it's more useful to
    // just let the panel scroll internally and hand the rest to the
    // keyboard. Below the comfort threshold we fall back to the same
    // elastic behaviour as before: shrink the keyboard toward its floor
    // first, then let the mixer itself shrink (it'll simply show fewer
    // rows before needing to scroll) rather than either one collapsing to
    // nothing.
    {
        constexpr int kKeyboardIdealH = 110;
        constexpr int kKeyboardFloorH = 56;
        constexpr int kMixerVisibleRowsMax = 5;    // beyond this, scrolling the list is more
                                                    // useful than the panel eating further height
        constexpr int kMixerMinH = 44;             // ~1.25 rows — enough to hint "more below,
                                                    // scroll for it" rather than vanishing

        const int rowH = (int) Sf2ChannelFxPanel::rowHeight();
        const int activeRows = juce::jmax (1, channelFxPanel.getActiveChannelCount());
        const int kMixerIdealH = juce::jmin (activeRows, kMixerVisibleRowsMax) * rowH;

        // Fixed chrome around the keyboard/mixer split: pad below keyboard,
        // gap above the note meter, the meter itself, its label, pad, the
        // "CHANNEL MIXER" label, and the pad under it.
        const int chromeH = kPad + 8 + 20 + 14 + 6 + 18 + 4;
        const int splittableH = juce::jmax (0, c.getHeight() - chromeH);

        int keyboardH, mixerH;
        if (splittableH >= kMixerIdealH + kKeyboardIdealH)
        {
            // Plenty of room: cap the mixer at its ideal size (matching its
            // actual row count) instead of stretching it, and hand every
            // extra px to the keyboard.
            mixerH    = kMixerIdealH;
            keyboardH = splittableH - mixerH;
        }
        else if (splittableH >= kMixerMinH + kKeyboardFloorH)
        {
            // Tight but workable: keyboard gives way first, toward its floor.
            keyboardH = juce::jmax (kKeyboardFloorH, splittableH - kMixerIdealH);
            mixerH    = splittableH - keyboardH;
        }
        else
        {
            // Genuinely starved: keyboard sits at its floor, mixer gets
            // whatever's left (it'll just show fewer rows before scrolling).
            keyboardH = juce::jmin (kKeyboardFloorH, splittableH);
            mixerH    = juce::jmax (0, splittableH - keyboardH);
        }

        keyboardZone = c.removeFromBottom (keyboardH);
        c.removeFromBottom (kPad);
        compactKeyboard->setBounds (keyboardZone);

        // Note activity meter docked immediately above the keyboard.
        c.removeFromBottom (8);
        noteMeterZone = c.removeFromBottom (20);
        noteActivityLabelZone = c.removeFromBottom (14);
        c.removeFromBottom (6);

        mixerLabelZone = c.removeFromTop (18);
        c.removeFromTop (kPad / 2);

        channelFxPanel.setBounds (c.removeFromTop (mixerH));
    }
}

void Sf2InstrumentWorkspace::layoutNarrow (juce::Rectangle<int> bounds)
{
    // Narrow-width stacking: presets on top, voice/envelope in the middle,
    // performance/FX + keyboard at the bottom — each gets roughly a third.
    topBarZone = bounds.removeFromTop (kTopBarH);
    loadedPillZone = topBarZone.removeFromRight (170).reduced (4);

    // Narrow-width stacking: presets get the top third; the merged voice +
    // mixer panel (Sections A/B/C, same as layoutWide but full-width
    // throughout — no topSectionW cap since there's only one width here)
    // gets the remaining two-thirds, stacked internally exactly as in the
    // wide layout.
    const int h = bounds.getHeight();
    col1Zone = bounds.removeFromTop (h / 3);
    col2Zone = bounds;

    {
        auto c = col1Zone.reduced (kPad);
        searchZone = c.removeFromTop (26);
        c.removeFromTop (kPad);
        browseButtonZone = c.removeFromBottom (24);
        c.removeFromBottom (kPad);
        bankFooterZone = c.removeFromBottom (18);
        c.removeFromBottom (kPad / 2);
        listZone = c;

        searchBox.setBounds (searchZone);
        presetListBox.setBounds (listZone);
        browseButton.setBounds (browseButtonZone);
    }

    auto c = col2Zone.reduced (kPad);

    // ── Section A — active preset header + 3 knobs (+ fine stepper) ────────
    {
        activePresetHeaderZone = c.removeFromTop (40);
        c.removeFromTop (kPad);
        constexpr int kKnobCellH = 96;
        auto knobRow = c.removeFromTop (kKnobCellH);
        const int knobGap = (knobRow.getWidth() - 3 * kKnobW) / 4;
        knobRow.removeFromLeft (knobGap);
        levelZone = knobRow.removeFromLeft (kKnobW);
        knobRow.removeFromLeft (knobGap);
        transZone = knobRow.removeFromLeft (kKnobW);
        knobRow.removeFromLeft (knobGap);
        panZone   = knobRow.removeFromLeft (kKnobW);
        fineZone  = juce::Rectangle<int> (transZone.getX(), knobRow.getY() + kKnobCellH + 6,
                                           transZone.getWidth(), 16);
        c.removeFromTop (26);
    }

    // ── Section B — SF2 filter + global reverb sliders ──────────────────────
    {
        filterCutoffZone = c.removeFromTop (30);
        c.removeFromTop (6);
        filterResonanceZone = c.removeFromTop (30);
        c.removeFromTop (6);
        reverbSendZone = c.removeFromTop (30);
        c.removeFromTop (6);
        reverbDampZone = c.removeFromTop (30);
    }
    c.removeFromTop (kPad);

    // ── Section C — channel mixer, note activity, keyboard ──────────────────
    // Same capped-mixer / surplus-to-keyboard split as layoutWide — see the
    // comment there for why the mixer can't just take "whatever's left".
    {
        constexpr int kKeyboardIdealH = 100;
        constexpr int kKeyboardFloorH = 52;
        constexpr int kMixerVisibleRowsMax = 5;
        constexpr int kMixerMinH = 44;

        const int rowH = (int) Sf2ChannelFxPanel::rowHeight();
        const int activeRows = juce::jmax (1, channelFxPanel.getActiveChannelCount());
        const int kMixerIdealH = juce::jmin (activeRows, kMixerVisibleRowsMax) * rowH;

        const int chromeH = kPad + 8 + 20 + 14 + 6 + 18 + 4;
        const int splittableH = juce::jmax (0, c.getHeight() - chromeH);

        int keyboardH, mixerH;
        if (splittableH >= kMixerIdealH + kKeyboardIdealH)
        {
            mixerH    = kMixerIdealH;
            keyboardH = splittableH - mixerH;
        }
        else if (splittableH >= kMixerMinH + kKeyboardFloorH)
        {
            keyboardH = juce::jmax (kKeyboardFloorH, splittableH - kMixerIdealH);
            mixerH    = splittableH - keyboardH;
        }
        else
        {
            keyboardH = juce::jmin (kKeyboardFloorH, splittableH);
            mixerH    = juce::jmax (0, splittableH - keyboardH);
        }

        keyboardZone = c.removeFromBottom (keyboardH);
        c.removeFromBottom (kPad);
        compactKeyboard->setBounds (keyboardZone);

        c.removeFromBottom (8);
        noteMeterZone = c.removeFromBottom (20);
        noteActivityLabelZone = c.removeFromBottom (14);
        c.removeFromBottom (6);

        mixerLabelZone = c.removeFromTop (18);
        c.removeFromTop (kPad / 2);

        channelFxPanel.setBounds (c.removeFromTop (mixerH));
    }
}

// =============================================================================
//  Paint
// =============================================================================

void Sf2InstrumentWorkspace::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();
    g.fillAll (juce::Colour::fromString ("FF080e12"));   // mockup's literal panel bg
    g.setColour (juce::Colour::fromString ("FF263b45"));
    g.drawRect (getLocalBounds(), 2);

    // ── Top bar ─────────────────────────────────────────────────────────────
    g.setColour (juce::Colour::fromString ("FF10191e"));
    g.fillRect (topBarZone);
    g.setColour (theme.foreground.withAlpha (0.8f));
    g.setFont (DysektLookAndFeel::makeFont (18.f, true));
    g.drawText ("SF2 INSTRUMENT PANEL", topBarZone.withTrimmedLeft (16),
                juce::Justification::centredLeft);

    if (! loadedPillZone.isEmpty())
    {
        const bool loaded = processor.sfzPlayer.getLoadedFile().existsAsFile();
        g.setColour (juce::Colour::fromString ("FF142c34"));
        g.fillRoundedRectangle (loadedPillZone.toFloat(), 3.f);
        g.setColour (loaded ? juce::Colour::fromString ("FF83d96b") : theme.foreground.withAlpha (0.3f));
        g.fillEllipse (loadedPillZone.getX() + 10.f, loadedPillZone.getCentreY() - 4.f, 8.f, 8.f);
        g.setColour (juce::Colour::fromString ("FFc9efd0"));
        g.setFont (DysektLookAndFeel::makeFont (14.f, true));
        g.drawText (loaded ? "SOUNDFONT LOADED" : "NO SOUNDFONT",
                    loadedPillZone.withTrimmedLeft (24), juce::Justification::centredLeft);
    }

    // ── Column separator ─────────────────────────────────────────────────────
    // Only between column 1 (presets) and column 2 (merged voice + mixer
    // panel) now — no divider inside column 2 itself; that seam was the
    // whole point of the merge.
    g.setColour (theme.separator);
    if (getWidth() >= kNarrowThreshold)
        g.drawVerticalLine (col1Zone.getRight(), (float) topBarZone.getBottom(), (float) getHeight());
    else
        g.drawHorizontalLine (col1Zone.getBottom(), 0.f, (float) getWidth());

    // ── Column 1 footer ─────────────────────────────────────────────────────
    g.setFont (DysektLookAndFeel::makeFont (16.f, true));
    g.setColour (theme.foreground.withAlpha (0.6f));
    g.drawText ("BANK 000  |  GENERAL MIDI", bankFooterZone, juce::Justification::centredLeft);

    // ── Section A — active preset header ────────────────────────────────────────────────────────────────────────
    {
        const int idx = effectiveDisplayPresetIndex();
        juce::String name = "No preset selected";
        juce::String meta;
        bool  hasCh      = false;
        juce::Colour nameColour = theme.foreground;

        if (idx >= 0 && idx < (int) presetList.size())
        {
            const auto& info = presetList[(size_t) idx];
            name = info.name;

            // Same lookup PresetListModel::paintListBoxItem() uses for each
            // row's "CHxx" badge — the actual per-preset multitimbral channel
            // assignment. (SfzPlayer::getMidiChannel() is a different, unrelated
            // "filter channel" value for live-note filtering, defaults to 16,
            // and was never the right source for this readout — it's why this
            // line always showed "CH --" regardless of the preset's real
            // assignment shown elsewhere, e.g. in the Performance & FX panel.)
            const auto& chMap = programGrid.getPresetChannels();
            const auto  chIt  = chMap.find (idx);
            hasCh = chIt != chMap.end() && chIt->second >= 1;

            meta = "BANK " + juce::String (info.bank).paddedLeft ('0', 3)
                 + "  |  PROGRAM " + juce::String (info.preset).paddedLeft ('0', 3)
                 + "  |  CH " + (hasCh ? juce::String (chIt->second).paddedLeft ('0', 2)
                                       : juce::String ("--"));

            // Same object as the highlighted row in column 1, and, once
            // assigned, the same object as its Arranger track — so this
            // header should read in that track's colour too, exactly like
            // the list row already does, instead of the plain foreground.
            if (hasCh)
                nameColour = UIHelpers::sf2TrackColourForPreset (info);
        }
        // IMPORTANT: operate on a local copy. Calling removeFromTop() directly
        // on the activePresetHeaderZone *member* would permanently shrink the
        // stored rectangle by a little more on every single repaint (30/sec),
        // which is exactly the bug that made this text vanish after ~1 second
        // of runtime in the first build.
        auto header = activePresetHeaderZone;
        g.setFont (DysektLookAndFeel::makeFont (14.f, true));
        g.setColour (theme.foreground.withAlpha (0.6f));
        g.drawText ("ACTIVE PRESET", header.removeFromTop (16),
                    juce::Justification::centredLeft);
        auto nameRow = header.removeFromTop (26);
        // Keep the technical bank/program/channel readout on the same line as
        // the friendly preset name, reserving enough space for it first.
        const int metaWidth = juce::jmin (300, nameRow.getWidth() * 2 / 3);
        auto metaRow = nameRow.removeFromRight (metaWidth);
        nameRow.removeFromRight (kPad);

        g.setFont (DysektLookAndFeel::makeFont (24.f, true));
        g.setColour (nameColour);
        g.drawFittedText (name, nameRow, juce::Justification::centredLeft, 1, 0.7f);
        g.setFont (DysektLookAndFeel::makeFont (14.f));
        g.setColour (hasCh ? nameColour.withAlpha (0.85f) : theme.foreground.withAlpha (0.6f));
        g.drawFittedText (meta, metaRow, juce::Justification::centredRight, 1, 0.7f);
    }

    // ── Section A — 3 knobs + fine-tune stepper ──────────────────────────────────────────────────────────
    drawKnob (g, levelZone, volToNorm (processor.sfzPlayer.getVolume()), "LEVEL",
              juce::String (juce::roundToInt (processor.sfzPlayer.getVolume() * 50.f)) + "%");
    drawKnob (g, transZone, transToNorm (processor.sfzPlayer.getTranspose()), "TRANSPOSE",
              juce::String (processor.sfzPlayer.getTranspose()));
    drawKnob (g, panZone, panToNorm (processor.sfzPlayer.getPan()), "PAN",
              juce::String (juce::roundToInt (processor.sfzPlayer.getPan() * 100.f)));

    g.setFont (DysektLookAndFeel::makeFont (12.f));
    g.setColour (theme.foreground.withAlpha (0.5f));
    g.drawText ("FINE " + juce::String (juce::roundToInt (processor.sfzPlayer.getFineTune())) + "c",
                fineZone, juce::Justification::centred);

    // ── Section B — SF2 filter + reverb controls (permanent, no tabs) ───────────────────────
    {
        const float cutoffHz = processor.sfzPlayer.getSf2FilterCutoff();
        const juce::String cutoffStr = cutoffHz >= 1000.f
            ? juce::String (cutoffHz / 1000.f, 2) + " kHz"
            : juce::String (juce::roundToInt (cutoffHz)) + " Hz";

        drawSlider (g, filterCutoffZone, cutoffToNorm (cutoffHz), "CUTOFF", cutoffStr);
        drawSlider (g, filterResonanceZone, resonanceToNorm (processor.sfzPlayer.getSf2FilterResonance()),
                    "RESONANCE", juce::String (juce::roundToInt (processor.sfzPlayer.getSf2FilterResonance())) + "%");
        drawSlider (g, reverbSendZone, processor.sfzPlayer.getReverbMix() / 100.f, "REVERB SEND",
                    juce::String (juce::roundToInt (processor.sfzPlayer.getReverbMix())) + "%");
        drawSlider (g, reverbDampZone, processor.sfzPlayer.getReverbDamp() / 100.f, "REVERB DAMP",
                    juce::String (juce::roundToInt (processor.sfzPlayer.getReverbDamp())) + "%");
    }

    // ── Section C — channel mixer label ─────────────────────────────────────
    // Marks where the merged panel switches scope from "the selected
    // preset's global controls" (Sections A/B above) to "every assigned
    // channel's mixer strip" (Sf2ChannelFxPanel, painted separately as a
    // child component immediately below this label).
    g.setFont (DysektLookAndFeel::makeFont (13.f, true));
    g.setColour (theme.foreground.withAlpha (0.55f));
    g.drawText ("CHANNEL MIXER", mixerLabelZone, juce::Justification::centredLeft);

    // ── Section C — note activity meter (always visible, above keyboard) ───
    g.setFont (DysektLookAndFeel::makeFont (13.f, true));
    g.setColour (theme.foreground.withAlpha (0.55f));
    g.drawText ("NOTE ACTIVITY", noteActivityLabelZone, juce::Justification::centredLeft);
    drawNoteMeter (g, noteMeterZone);

    // ── Keyboard label ──────────────────────────────────────────────────────
    g.setFont (DysektLookAndFeel::makeFont (14.f, true));
    g.setColour (theme.foreground.withAlpha (0.6f));
    g.drawText ("KEYBOARD  |  " + (compactKeyboard != nullptr ? compactKeyboard->getRangeLabel() : "C3-C5"),
                keyboardZone.withHeight (16).translated (0, -18),
                juce::Justification::centredLeft);
}

void Sf2InstrumentWorkspace::drawKnob (juce::Graphics& g, juce::Rectangle<int> bounds,
                                        float normalised, const juce::String& label,
                                        const juce::String& valueStr) const
{
    // IMPORTANT: bounds here is the FULL cell (label + circle + value), not
    // just the circle. Carving three non-overlapping sub-rects out of it —
    // rather than the old approach of drawing the label above bounds.getY()
    // and the value inside bounds.getBottom() — is what guarantees they can
    // never visually collide, regardless of knob size or font metrics.
    auto cell = bounds;
    auto labelRow = cell.removeFromTop (16);
    auto valueRow = cell.removeFromBottom (20);
    auto circleZone = cell;   // whatever's left in the middle

    const auto& theme = getTheme();
    const float cx = (float) circleZone.getCentreX();
    const float cy = (float) circleZone.getCentreY();
    const float radius = (float) juce::jmin (circleZone.getWidth(), circleZone.getHeight()) * 0.42f;

    constexpr float startAngle = juce::MathConstants<float>::pi * 1.2f;
    constexpr float endAngle   = juce::MathConstants<float>::pi * 2.8f;
    const float norm = juce::jlimit (0.f, 1.f, normalised);
    const float fillAngle = startAngle + norm * (endAngle - startAngle);

    juce::Path trackArc;
    trackArc.addCentredArc (cx, cy, radius, radius, 0.f, startAngle, endAngle, true);
    g.setColour (theme.button);
    g.strokePath (trackArc, juce::PathStrokeType (3.f));

    juce::Path fillArc;
    fillArc.addCentredArc (cx, cy, radius, radius, 0.f, startAngle, fillAngle, true);
    g.setColour (theme.accent);
    g.strokePath (fillArc, juce::PathStrokeType (3.f));

    const float px = cx + radius * 0.6f * std::sin (fillAngle);
    const float py = cy - radius * 0.6f * std::cos (fillAngle);
    g.drawLine (cx, cy, px, py, 1.5f);

    g.setFont (DysektLookAndFeel::makeFont (14.f));
    g.setColour (theme.foreground.withAlpha (0.65f));
    g.drawText (label, labelRow, juce::Justification::centred);

    g.setColour (theme.foreground);
    g.setFont (DysektLookAndFeel::makeFont (16.f, true));
    g.drawText (valueStr, valueRow, juce::Justification::centred);
}

void Sf2InstrumentWorkspace::drawSlider (juce::Graphics& g, juce::Rectangle<int> bounds,
                                          float normalised, const juce::String& label,
                                          const juce::String& valueStr) const
{
    if (bounds.isEmpty()) return;
    const auto& theme = getTheme();

    g.setFont (DysektLookAndFeel::makeFont (14.f, true));
    g.setColour (theme.foreground.withAlpha (0.65f));
    auto labelRow = bounds.removeFromTop (16);
    g.drawText (label, labelRow, juce::Justification::centredLeft);
    g.setColour (theme.foreground);
    g.setFont (DysektLookAndFeel::makeFont (14.f, true));
    g.drawText (valueStr, labelRow, juce::Justification::centredRight);

    auto track = juce::Rectangle<int> (bounds.getX(), bounds.getY() + 4, bounds.getWidth(), 12);
    g.setColour (theme.button);   // was near-invisible against panel bg; theme.button reads clearly at 0%
    g.fillRoundedRectangle (track.toFloat(), 2.f);
    g.setColour (theme.separator);
    g.drawRoundedRectangle (track.toFloat(), 2.f, 1.f);

    auto fill = track.toFloat().reduced (2.f);
    fill.setWidth (juce::jmax (0.f, fill.getWidth() * juce::jlimit (0.f, 1.f, normalised)));
    g.setColour (theme.accent);
    g.fillRoundedRectangle (fill, 1.f);
}

void Sf2InstrumentWorkspace::drawNoteMeter (juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty()) return;
    const auto& theme = getTheme();

    constexpr int kBars = 8;
    const float barW = (float) bounds.getWidth() / (float) kBars;
    for (int i = 0; i < kBars; ++i)
    {
        // Deterministic per-bar shaping (not random) so the meter looks like
        // an equaliser rather than jittering noise: middle bars read taller.
        const float shape = 1.f - std::abs ((float) i - (kBars - 1) * 0.5f) / ((kBars - 1) * 0.5f);
        const float h = juce::jlimit (0.f, 1.f, noteActivityLevel * (0.35f + 0.65f * shape));

        // Empty-slot background first, so idle bars read as a visible track
        // rather than disappearing into the panel background.
        g.setColour (theme.button);
        g.fillRect (bounds.getX() + i * barW + 1.5f, (float) bounds.getY(), barW - 3.f, (float) bounds.getHeight());

        if (h > 0.02f)
        {
            g.setColour (theme.accent);
            g.fillRect (bounds.getX() + i * barW + 1.5f,
                        (float) bounds.getBottom() - h * (float) bounds.getHeight(),
                        barW - 3.f, h * (float) bounds.getHeight());
        }
    }
}

// =============================================================================
//  Timer — meters, LED-style state, program-grid data refresh
// =============================================================================

void Sf2InstrumentWorkspace::timerCallback()
{
    presetList = processor.sfzPlayer.getPresetList();

    if (programGrid.getPresets().empty() && ! presetList.empty())
    {
        programGrid.setPresets (presetList,
                                effectiveDisplayPresetIndex(),
                                processor.sfzPlayer.getMidiChannel());
        restoreGridChannelAssignments();
        rebuildFilteredPresetRows();
        presetListBox.updateContent();
    }

    // ── MIDI channel range (keeps program-grid assignment state current) ───────
    {
        const uint32_t mask = processor.sfPlayerChannelMask.load (std::memory_order_relaxed) & DysektProcessor::kSf2AllowedMidiChannelMask;
        cachedChLow  = 0;
        cachedChHigh = 0;
        if (mask != 0)
        {
            for (int c = 3; c <= 16; ++c)  if (mask & (1u << c)) { cachedChLow  = c; break; }
            for (int c = 16; c >= 3; --c)  if (mask & (1u << c)) { cachedChHigh = c; break; }
        }
    }

    programGrid.setChannelRange    (cachedChLow, cachedChHigh);
    programGrid.setBlockedChannels (
        processor.chromaticSliceChannelMask.load (std::memory_order_relaxed)
        | processor.sfzPlayer2ChannelMask.load (std::memory_order_relaxed));

    // ── Note-activity meter — decaying level derived from sfzActiveNotes ──────
    {
        const uint64_t lo = processor.sfzActiveNotes[0].load (std::memory_order_relaxed);
        const uint64_t hi = processor.sfzActiveNotes[1].load (std::memory_order_relaxed);
        const bool anyActive = (lo != 0) || (hi != 0);

        if (anyActive)
        {
            idleTicks = 0;
            noteActivityLevel = juce::jmin (1.0f, noteActivityLevel + 0.35f);
        }
        else
        {
            noteActivityLevel *= 0.85f;
            if (noteActivityLevel < 0.01f)
            {
                noteActivityLevel = 0.f;
                ++idleTicks;
            }
        }
    }

    // ── Column-3 tab availability can change as assignments come and go ──────
    refreshChannelFxLabels();

    if (idleTicks > 6)
        repaint (noteMeterZone);
    else
        repaint();
}

void Sf2InstrumentWorkspace::refreshChannelFxLabels()
{
    uint16_t mask = 0;
    for (auto& ap : sf2Presets)
        if (ap.ch >= 1 && ap.ch <= 16)
            mask |= (uint16_t) (1u << (ap.ch - 1));

    const bool maskChanged = (mask != assignedChannelMask);
    assignedChannelMask = mask;

    channelFxPanel.setActiveChannelMask (mask);
    for (int ch = 0; ch < 16; ++ch)
        channelFxPanel.setChannelColour (ch, {});
    for (const auto& ap : sf2Presets)
        if (ap.ch >= 1 && ap.ch <= 16)
        {
            channelFxPanel.setChannelLabel (ap.ch - 1, ap.preset.name);
            channelFxPanel.setChannelColour (ap.ch - 1, trackColourForPreset (ap.preset));
        }

    if (maskChanged)
        resized();
}

// =============================================================================
//  Program grid data plumbing
// =============================================================================

int Sf2InstrumentWorkspace::effectiveDisplayPresetIndex() const
{
    const int displayed = processor.sfzPlayer.getDisplayPresetIndex();
    return displayed >= 0 ? displayed : processor.sfzPlayer.getCurrentPresetIndex();
}

void Sf2InstrumentWorkspace::restoreGridChannelAssignments()
{
    std::unordered_map<int, int> chMap;
    for (const auto& ap : sf2Presets)
    {
        for (int i = 0; i < (int) presetList.size(); ++i)
        {
            if (presetList[(size_t) i].bank == ap.preset.bank
                && presetList[(size_t) i].preset == ap.preset.preset)
            {
                chMap[i] = ap.ch;
                break;
            }
        }
    }
    programGrid.setPresetChannels (chMap);
}

void Sf2InstrumentWorkspace::notifyPresetChannelChanged (const Sf2PresetInfo& preset,
                                                          int midiCh1Based)
{
    const auto matchesPreset = [&] (const AssignedPreset& ap)
    {
        return ap.preset.bank == preset.bank && ap.preset.preset == preset.preset;
    };

    if (midiCh1Based == 0)
    {
        sf2Presets.erase (std::remove_if (sf2Presets.begin(), sf2Presets.end(), matchesPreset),
                          sf2Presets.end());
    }
    else
    {
        bool found = false;
        for (auto& ap : sf2Presets)
            if (matchesPreset (ap)) { ap.preset = preset; ap.ch = midiCh1Based; found = true; break; }
        if (! found)
            sf2Presets.push_back ({ preset, midiCh1Based });
    }

    restoreGridChannelAssignments();
    refreshChannelFxLabels();
    presetListBox.updateContent();
    resized();
    repaint();
}

void Sf2InstrumentWorkspace::selectPresetForTrack (int presetBank, int presetProgram)
{
    // Matches against the currently-loaded preset list by bank+program —
    // the track's own stored preset link — not programGrid's channel map
    // (see header comment: that map is populated only by this panel's own
    // right-click assignment flow and knows nothing about tracks assigned
    // via the Arranger/TrackInspector).
    for (int i = 0; i < (int) presetList.size(); ++i)
    {
        const auto& p = presetList[(size_t) i];
        if (p.bank == presetBank && p.preset == presetProgram)
        {
            // Reuses the exact list-row-click path: updates the ACTIVE
            // PRESET header/envelope and, since this preset is already
            // routed to a real channel, its own early-return means no
            // audible preview note gets fired just from selecting a track.
            handlePresetLeftClicked (i);
            return;
        }
    }
    // No matching preset in the currently-loaded list (wrong/no .sf2 file
    // loaded yet, etc.) — leave the panel as-is rather than guessing.
}

void Sf2InstrumentWorkspace::onFileChosen (const juce::File& f)
{
    if (f.getFileExtension().toLowerCase() != ".sf2")
        return;   // SF2-PLAYER only accepts .sf2 — silently ignore anything else

    processor.sfzPlayer.loadFile (f, processor.fileLoadPool);

    // Reset the UI-only display index — otherwise a previously-browsed index
    // from the old file's preset list could point at the wrong (or an
    // out-of-range) row in the new one. effectiveDisplayPresetIndex() will
    // fall back to the engine's real current preset until the user browses.
    processor.sfzPlayer.setDisplayPresetIndex (-1);

    processor.sf2PreviewRequestedBank.store    (-1, std::memory_order_relaxed);
    processor.sf2PreviewRequestedProgram.store (-1, std::memory_order_relaxed);
    processor.loadSoundFontAsync (f, SoundFontLoadTarget::SfPlayer);
    processor.sfPlayerChannelMask.store (1u << 3, std::memory_order_relaxed); // ch3 default

    repaint();

    if (onFileLoaded)
        onFileLoaded (f);
}

void Sf2InstrumentWorkspace::panelDidShow()
{
    presetList = processor.sfzPlayer.getPresetList();

    programGrid.setPresets (presetList,
                            effectiveDisplayPresetIndex(),
                            processor.sfzPlayer.getMidiChannel());
    restoreGridChannelAssignments();
    rebuildFilteredPresetRows();
    presetListBox.updateContent();

    resized();
    repaint();
}

// =============================================================================
//  File drag-and-drop
// =============================================================================

bool Sf2InstrumentWorkspace::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).getFileExtension().toLowerCase() == ".sf2")
            return true;
    return false;
}

void Sf2InstrumentWorkspace::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
    {
        juce::File file (f);
        if (file.getFileExtension().toLowerCase() == ".sf2")
        {
            onFileChosen (file);
            return;
        }
    }
}

// =============================================================================
//  MIDI Learn menu (right-click on a knob/slider)
// =============================================================================

void Sf2InstrumentWorkspace::showMidiLearnMenu (int fieldId, juce::Point<int> screenPos)
{
    const bool mapped = processor.midiLearn.isMapped (fieldId);
    juce::PopupMenu menu;
    menu.addItem (1, "Learn MIDI CC");
    if (mapped)
        menu.addItem (2, "Clear (" + processor.midiLearn.getLabelText (fieldId) + ")");

    float ms = DysektLookAndFeel::getMenuScale();
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
            .withParentComponent (getTopLevelComponent())
            .withStandardItemHeight ((int) (24 * ms)),
        [this, fieldId] (int result)
        {
            if (result == 1)      { processor.midiLearn.armLearn (fieldId);     repaint(); }
            else if (result == 2) { processor.midiLearn.clearMapping (fieldId); repaint(); }
        });
}

// =============================================================================
//  Mouse events
// =============================================================================

void Sf2InstrumentWorkspace::mouseDown (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();

    // ── Right-click — MIDI Learn menu on a knob/slider ────────────────────────
    if (e.mods.isRightButtonDown())
    {
        using F = DysektProcessor::SliceParamField;
        struct { juce::Rectangle<int>& zone; int fieldId; } fields[] =
        {
            { levelZone,       F::FieldSfzVol         },
            { transZone,       F::FieldSfzTranspose   },
            { panZone,         F::FieldSfzPan         },
            { fineZone,        F::FieldSfzFineTune    },
            { reverbSendZone,  F::FieldSfzReverbMix   },
            { reverbDampZone,  F::FieldSfzReverbDamp  },
        };
        for (auto& kf : fields)
            if (kf.zone.contains (pos))
            {
                showMidiLearnMenu (kf.fieldId, e.getScreenPosition());
                return;
            }
        return;
    }

    // ── Knob/slider drag start ─────────────────────────────────────────────
    struct { juce::Rectangle<int>& zone; ActiveKnob id; float val; } controls[] =
    {
        { levelZone,      ActiveKnob::Level,      volToNorm  (processor.sfzPlayer.getVolume())    },
        { transZone,      ActiveKnob::Transpose,  transToNorm (processor.sfzPlayer.getTranspose())},
        { panZone,        ActiveKnob::Pan,        panToNorm  (processor.sfzPlayer.getPan())        },
        { fineZone,       ActiveKnob::FineTune,   fineToNorm (processor.sfzPlayer.getFineTune())   },
        { reverbSendZone, ActiveKnob::ReverbSend, processor.sfzPlayer.getReverbMix()  / 100.0f     },
        { reverbDampZone, ActiveKnob::ReverbDamp, processor.sfzPlayer.getReverbDamp() / 100.0f     },
        { filterCutoffZone,    ActiveKnob::FilterCutoff,    cutoffToNorm    (processor.sfzPlayer.getSf2FilterCutoff())    },
        { filterResonanceZone, ActiveKnob::FilterResonance, resonanceToNorm (processor.sfzPlayer.getSf2FilterResonance()) },
    };
    for (auto& c : controls)
    {
        if (c.zone.contains (pos))
        {
            activeKnob   = c.id;
            dragStartY   = pos.y;
            dragStartVal = c.val;
            return;
        }
    }
}

void Sf2InstrumentWorkspace::mouseDrag (const juce::MouseEvent& e)
{
    if (activeKnob == ActiveKnob::None) return;
    const float delta   = (float) (dragStartY - e.getPosition().y) / 120.0f;
    const float newNorm = juce::jlimit (0.f, 1.f, dragStartVal + delta);

    switch (activeKnob)
    {
        case ActiveKnob::Level:      processor.sfzPlayer.setVolume     (normToVol   (newNorm)); break;
        case ActiveKnob::Transpose:  processor.sfzPlayer.setTranspose  (normToTrans (newNorm)); break;
        case ActiveKnob::Pan:        processor.sfzPlayer.setPan        (normToPan   (newNorm)); break;
        case ActiveKnob::FineTune:   processor.sfzPlayer.setFineTune   (normToFine  (newNorm)); break;
        case ActiveKnob::ReverbSend: processor.sfzPlayer.setReverbMix  (newNorm * 100.0f);      break;
        case ActiveKnob::ReverbDamp: processor.sfzPlayer.setReverbDamp (newNorm * 100.0f);      break;
        case ActiveKnob::FilterCutoff:    processor.sfzPlayer.setSf2FilterCutoff    (normToCutoff    (newNorm)); break;
        case ActiveKnob::FilterResonance: processor.sfzPlayer.setSf2FilterResonance (normToResonance (newNorm)); break;
        default: break;
    }
    repaint();
}

void Sf2InstrumentWorkspace::mouseUp (const juce::MouseEvent&)
{
    activeKnob = ActiveKnob::None;
}

void Sf2InstrumentWorkspace::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (levelZone.contains      (pos)) { processor.sfzPlayer.setVolume     (1.0f);  repaint(); }
    if (transZone.contains      (pos)) { processor.sfzPlayer.setTranspose (0);     repaint(); }
    if (panZone.contains        (pos)) { processor.sfzPlayer.setPan       (0.0f);  repaint(); }
    if (fineZone.contains       (pos)) { processor.sfzPlayer.setFineTune  (0.0f);  repaint(); }
    if (reverbSendZone.contains (pos)) { processor.sfzPlayer.setReverbMix  (0.0f);  repaint(); }
    if (reverbDampZone.contains (pos)) { processor.sfzPlayer.setReverbDamp (50.0f); repaint(); }
    if (filterCutoffZone.contains (pos))
        { processor.sfzPlayer.setSf2FilterCutoff    (SfzPlayer::kSf2FilterCutoffDefaultHz);     repaint(); }
    if (filterResonanceZone.contains (pos))
        { processor.sfzPlayer.setSf2FilterResonance (SfzPlayer::kSf2FilterResonanceDefaultPct); repaint(); }
}

void Sf2InstrumentWorkspace::mouseWheelMove (const juce::MouseEvent& e,
                                              const juce::MouseWheelDetails& w)
{
    const auto  pos  = e.getPosition();
    const float step = w.deltaY * (e.mods.isShiftDown() ? 0.01f : 0.05f);
    auto adjustNorm = [&] (float current, float s) { return juce::jlimit (0.f, 1.f, current + s); };

    if (levelZone.contains (pos))
        processor.sfzPlayer.setVolume (normToVol (adjustNorm (volToNorm (processor.sfzPlayer.getVolume()), step)));
    else if (transZone.contains (pos))
        processor.sfzPlayer.setTranspose (normToTrans (adjustNorm (transToNorm (processor.sfzPlayer.getTranspose()), step)));
    else if (panZone.contains (pos))
        processor.sfzPlayer.setPan (normToPan (adjustNorm (panToNorm (processor.sfzPlayer.getPan()), step)));
    else if (fineZone.contains (pos))
        processor.sfzPlayer.setFineTune (normToFine (adjustNorm (fineToNorm (processor.sfzPlayer.getFineTune()), step)));
    else if (reverbSendZone.contains (pos))
        processor.sfzPlayer.setReverbMix  (juce::jlimit (0.0f, 100.0f, processor.sfzPlayer.getReverbMix()  + step * 100.0f));
    else if (reverbDampZone.contains (pos))
        processor.sfzPlayer.setReverbDamp (juce::jlimit (0.0f, 100.0f, processor.sfzPlayer.getReverbDamp() + step * 100.0f));
    else if (filterCutoffZone.contains (pos))
        processor.sfzPlayer.setSf2FilterCutoff (normToCutoff (adjustNorm (cutoffToNorm (processor.sfzPlayer.getSf2FilterCutoff()), step)));
    else if (filterResonanceZone.contains (pos))
        processor.sfzPlayer.setSf2FilterResonance (juce::jlimit (0.0f, 100.0f, processor.sfzPlayer.getSf2FilterResonance() + step * 100.0f));

    repaint();
}

// ── Knob normalisation helpers (identical mapping to the old panel's) ───────
float Sf2InstrumentWorkspace::volToNorm   (float linear) const { return juce::jlimit (0.f, 1.f, linear * 0.5f); }
float Sf2InstrumentWorkspace::normToVol   (float n)      const { return n * 2.0f; }
float Sf2InstrumentWorkspace::transToNorm (int semi)     const { return ((float) semi + 24.0f) / 48.0f; }
int   Sf2InstrumentWorkspace::normToTrans (float n)      const { return juce::roundToInt (n * 48.0f - 24.0f); }
float Sf2InstrumentWorkspace::panToNorm   (float p)      const { return (p + 1.0f) * 0.5f; }
float Sf2InstrumentWorkspace::normToPan   (float n)      const { return n * 2.0f - 1.0f; }
float Sf2InstrumentWorkspace::fineToNorm  (float cents)  const { return (cents + 100.0f) / 200.0f; }
float Sf2InstrumentWorkspace::normToFine  (float n)      const { return n * 200.0f - 100.0f; }

// Cutoff: logarithmic mapping across the full UI range, appropriate for a
// frequency control (equal drag distance = equal perceived pitch change).
float Sf2InstrumentWorkspace::cutoffToNorm (float hz) const
{
    const float lo = std::log2 (SfzPlayer::kSf2FilterCutoffMinHz);
    const float hi = std::log2 (SfzPlayer::kSf2FilterCutoffMaxHz);
    hz = juce::jlimit (SfzPlayer::kSf2FilterCutoffMinHz, SfzPlayer::kSf2FilterCutoffMaxHz, hz);
    return juce::jlimit (0.f, 1.f, (std::log2 (hz) - lo) / (hi - lo));
}

float Sf2InstrumentWorkspace::normToCutoff (float n) const
{
    const float lo = std::log2 (SfzPlayer::kSf2FilterCutoffMinHz);
    const float hi = std::log2 (SfzPlayer::kSf2FilterCutoffMaxHz);
    n = juce::jlimit (0.f, 1.f, n);
    return std::pow (2.0f, lo + n * (hi - lo));
}

// Resonance: plain linear mapping over its 0-100% UI range.
float Sf2InstrumentWorkspace::resonanceToNorm (float pct) const
{
    return juce::jlimit (0.f, 1.f, (pct - SfzPlayer::kSf2FilterResonanceMinPct)
                                  / (SfzPlayer::kSf2FilterResonanceMaxPct - SfzPlayer::kSf2FilterResonanceMinPct));
}

float Sf2InstrumentWorkspace::normToResonance (float n) const
{
    n = juce::jlimit (0.f, 1.f, n);
    return SfzPlayer::kSf2FilterResonanceMinPct
         + n * (SfzPlayer::kSf2FilterResonanceMaxPct - SfzPlayer::kSf2FilterResonanceMinPct);
}
