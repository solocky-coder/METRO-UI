// =============================================================================
//  Sf2InstrumentWorkspace.cpp  —  persistent 3-column SF2 instrument workspace
// =============================================================================
#include "Sf2InstrumentWorkspace.h"
#include "DysektLookAndFeel.h"
#include "UIHelpers.h"
#include "../PluginProcessor.h"
#include <algorithm>

// =============================================================================
//  Constructor / destructor
// =============================================================================

Sf2InstrumentWorkspace::Sf2InstrumentWorkspace (DysektProcessor& p)
    : keysPanel (p), channelFxPanel (p), processor (p)
{
    keysPanel.setEngineSource (KeysPanel::EngineSource::SfPlayer);
    keysPanel.setSlicerHighlightEnabled (false);
    keysPanel.setSf2PresetListMode (false);

    // ── SF2 program grid — always visible, no popup open/close state. ────────
    // Left-click auditions the preset (onPreviewToggled); it must NOT close
    // anything since the grid is a permanent column now.
    programGrid.onPresetSelected = nullptr;

    // ── Multitimbral channel assignment ───────────────────────────────────────
    // Ported verbatim from SfzDropdownPanel: right-click assigns a MIDI
    // channel (1-16) or 0 to deactivate. Routing is strictly 1:1 — incoming
    // MIDI ch N plays FluidSynth channel N-1. Channels 1-2 are permanently
    // reserved (1=Slicer, 2=SFZ-Player); channels owned by a chromatic slice
    // or currently occupied by sfzPlayer2 are also rejected.
    programGrid.onChannelChanged = [this] (int presetIdx, int ch)
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
            // Deactivate — silence all FluidSynth channels, then reload only
            // the still-assigned presets.
            for (int c = 0; c < 16; ++c)
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
    };

    // ── Preview toggle: left-click radio ──────────────────────────────────────
    programGrid.onPreviewToggled = [this] (int idx)
    {
        if (idx < 0)
        {
            processor.sfzPlayer.clearPreview();
            return;
        }
        if (idx >= (int) presetList.size()) return;
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
            return;  // already routed on a real channel

        processor.sfzPlayer.previewPreset (info.bank, info.preset);
    };

    programGrid.onAssignedPresetClicked = [this] (int idx)
    {
        if (idx < 0 || idx >= (int) presetList.size()) return;
        processor.sfzPlayer.setDisplayPresetIndex (idx);
    };

    addAndMakeVisible (programGrid);
    addAndMakeVisible (keysPanel);
    addChildComponent (channelFxPanel);   // hidden until >1 channel assigned

    startTimerHz (30);
}

Sf2InstrumentWorkspace::~Sf2InstrumentWorkspace() = default;

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
    const int w = bounds.getWidth();
    const int colPresetsW = juce::roundToInt ((float) w * kColPresetsFrac);
    const int colVoiceW   = juce::roundToInt ((float) w * kColVoiceFrac);

    auto col1 = bounds.removeFromLeft (colPresetsW);
    auto col2 = bounds.removeFromLeft (colVoiceW);
    auto col3 = bounds;   // remainder (~38%)

    programGrid.setBounds (col1.reduced (kPad / 2));

    // ── Column 2: voice knobs, channel-range spinner, note meter, keyboard ────
    col2.reduce (kPad, kPad);

    auto knobRow1 = col2.removeFromTop (kKnobH);
    transZone = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    fineZone  = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    panZone   = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    volZone   = knobRow1.removeFromLeft (kKnobW);

    col2.removeFromTop (kPad);
    auto knobRow2 = col2.removeFromTop (kKnobH);
    rvMixZone  = knobRow2.removeFromLeft (kKnobW);
    knobRow2.removeFromLeft (kPad);
    rvSizeZone = knobRow2.removeFromLeft (kKnobW);

    col2.removeFromTop (kPad);
    auto chRow = col2.removeFromTop (28);
    {
        const int btnW = 26, numW = 32, gap = 6, sepW = 20, labelW = 26;
        const int widgetW = labelW + gap + btnW + numW + btnW + gap + sepW + gap + btnW + numW + btnW;
        auto z = chRow.withSizeKeepingCentre (juce::jmin (widgetW, chRow.getWidth()), chRow.getHeight());
        chRangeLabelZone = z.removeFromLeft (labelW);
        z.removeFromLeft (gap);
        chLowDec   = z.removeFromLeft (btnW);
        chLowLabel = z.removeFromLeft (numW);
        chLowInc   = z.removeFromLeft (btnW);
        z.removeFromLeft (gap);
        z.removeFromLeft (sepW);
        z.removeFromLeft (gap);
        chHighDec   = z.removeFromLeft (btnW);
        chHighLabel = z.removeFromLeft (numW);
        chHighInc   = z.removeFromLeft (btnW);
    }

    col2.removeFromTop (kPad);
    noteMeterZone = col2.removeFromTop (16);

    col2.removeFromTop (kPad);
    keyboardZone = col2;   // keyboard fills the rest of column 2
    keysPanel.setBounds (keyboardZone);

    // ── Column 3: per-channel FX mixer or single-channel hint ─────────────────
    col3.reduce (kPad, kPad);
    if (countAssignedChannels() > 1)
    {
        channelFxPanel.setVisible (true);
        channelFxPanel.setBounds (col3);
        singleChannelHintZone = {};
    }
    else
    {
        channelFxPanel.setVisible (false);
        channelFxPanel.setBounds ({});
        singleChannelHintZone = col3;
    }
}

void Sf2InstrumentWorkspace::layoutNarrow (juce::Rectangle<int> bounds)
{
    // Narrow-width stacking: presets on top, voice controls in the middle,
    // channel mixer at the bottom — each gets a third of the height.
    const int h = bounds.getHeight();
    auto col1 = bounds.removeFromTop (h / 3);
    auto col2 = bounds.removeFromTop (h / 3);
    auto col3 = bounds;

    programGrid.setBounds (col1.reduced (kPad / 2));

    col2.reduce (kPad, kPad);
    auto knobRow1 = col2.removeFromTop (kKnobH);
    transZone = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    fineZone  = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    panZone   = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    volZone   = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    rvMixZone  = knobRow1.removeFromLeft (kKnobW);
    knobRow1.removeFromLeft (kPad);
    rvSizeZone = knobRow1.removeFromLeft (kKnobW);

    col2.removeFromTop (kPad);
    auto chRow = col2.removeFromTop (28);
    {
        const int btnW = 26, numW = 32, gap = 6, sepW = 20, labelW = 26;
        const int widgetW = labelW + gap + btnW + numW + btnW + gap + sepW + gap + btnW + numW + btnW;
        auto z = chRow.withSizeKeepingCentre (juce::jmin (widgetW, chRow.getWidth()), chRow.getHeight());
        chRangeLabelZone = z.removeFromLeft (labelW);
        z.removeFromLeft (gap);
        chLowDec   = z.removeFromLeft (btnW);
        chLowLabel = z.removeFromLeft (numW);
        chLowInc   = z.removeFromLeft (btnW);
        z.removeFromLeft (gap);
        z.removeFromLeft (sepW);
        z.removeFromLeft (gap);
        chHighDec   = z.removeFromLeft (btnW);
        chHighLabel = z.removeFromLeft (numW);
        chHighInc   = z.removeFromLeft (btnW);
    }
    col2.removeFromTop (kPad);
    noteMeterZone = col2.removeFromTop (16);
    col2.removeFromTop (kPad);
    keyboardZone = col2;
    keysPanel.setBounds (keyboardZone);

    col3.reduce (kPad, kPad);
    if (countAssignedChannels() > 1)
    {
        channelFxPanel.setVisible (true);
        channelFxPanel.setBounds (col3);
        singleChannelHintZone = {};
    }
    else
    {
        channelFxPanel.setVisible (false);
        channelFxPanel.setBounds ({});
        singleChannelHintZone = col3;
    }
}

// =============================================================================
//  Paint
// =============================================================================

void Sf2InstrumentWorkspace::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();
    UIHelpers::drawTexturedPanel (g, getLocalBounds().toFloat(), theme.darkBar,
                                   UIHelpers::PanelZone::Chassis);

    g.setColour (theme.separator);
    // Column separators mirror the actual column bounds so they stay correct
    // in both wide and narrow (stacked) layouts.
    if (getWidth() >= kNarrowThreshold)
    {
        g.drawVerticalLine (programGrid.getRight(), 0.f, (float) getHeight());
        g.drawVerticalLine (keyboardZone.isEmpty() ? programGrid.getRight()
                                                    : keysPanel.getRight(),
                            0.f, (float) getHeight());
    }
    else
    {
        g.drawHorizontalLine (programGrid.getBottom(), 0.f, (float) getWidth());
        g.drawHorizontalLine (keysPanel.getBottom() > 0 ? juce::jmax (keysPanel.getBottom(), noteMeterZone.getBottom())
                                                         : programGrid.getBottom(),
                              0.f, (float) getWidth());
    }

    // ── Voice knobs ────────────────────────────────────────────────────────────
    drawKnob (g, transZone,  transToNorm (processor.sfzPlayer.getTranspose()), "TRN",
              juce::String (processor.sfzPlayer.getTranspose()));
    drawKnob (g, fineZone,   fineToNorm  (processor.sfzPlayer.getFineTune()),  "FINE",
              juce::String (juce::roundToInt (processor.sfzPlayer.getFineTune())) + "c");
    drawKnob (g, panZone,    panToNorm   (processor.sfzPlayer.getPan()),       "PAN",
              juce::String (juce::roundToInt (processor.sfzPlayer.getPan() * 100.f)));
    drawKnob (g, volZone,    volToNorm   (processor.sfzPlayer.getVolume()),    "VOL",
              juce::String (juce::roundToInt (processor.sfzPlayer.getVolume() * 50.f)) + "%");
    drawKnob (g, rvMixZone,  processor.sfzPlayer.getReverbMix()  / 100.f,      "REV MIX",
              juce::String (juce::roundToInt (processor.sfzPlayer.getReverbMix())) + "%");
    drawKnob (g, rvSizeZone, processor.sfzPlayer.getReverbSize() / 100.f,      "REV SIZE",
              juce::String (juce::roundToInt (processor.sfzPlayer.getReverbSize())) + "%");

    // ── Channel-range spinner ──────────────────────────────────────────────────
    g.setFont (DysektLookAndFeel::makeFont (11.f, true));
    g.setColour (theme.foreground.withAlpha (0.7f));
    g.drawText ("CH", chRangeLabelZone, juce::Justification::centredLeft);

    auto drawSpinnerBtn = [&] (juce::Rectangle<int> r, const char* glyph)
    {
        g.setColour (theme.button);
        g.fillRoundedRectangle (r.toFloat().reduced (2.f), 3.f);
        g.setColour (theme.foreground);
        g.drawText (glyph, r, juce::Justification::centred);
    };
    drawSpinnerBtn (chLowDec,  "\u25c2");
    drawSpinnerBtn (chLowInc,  "\u25b8");
    drawSpinnerBtn (chHighDec, "\u25c2");
    drawSpinnerBtn (chHighInc, "\u25b8");

    g.setColour (theme.foreground);
    g.setFont (DysektLookAndFeel::makeFont (12.f, true));
    g.drawText (cachedChLow  > 0 ? juce::String (cachedChLow)  : juce::String ("-"), chLowLabel,  juce::Justification::centred);
    g.drawText (cachedChHigh > 0 ? juce::String (cachedChHigh) : juce::String ("-"), chHighLabel, juce::Justification::centred);

    // ── Note-activity meter ────────────────────────────────────────────────────
    drawNoteMeter (g, noteMeterZone);

    // ── Single-channel hint (column 3, when the mixer is hidden) ──────────────
    if (! singleChannelHintZone.isEmpty())
    {
        g.setColour (theme.foreground.withAlpha (0.4f));
        g.setFont (DysektLookAndFeel::makeFont (13.f));
        g.drawText ("Assign a second MIDI channel\nto open the per-channel mixer",
                    singleChannelHintZone, juce::Justification::centred);
    }
}

void Sf2InstrumentWorkspace::drawKnob (juce::Graphics& g, juce::Rectangle<int> bounds,
                                        float normalised, const juce::String& label,
                                        const juce::String& valueStr) const
{
    const auto& theme = getTheme();
    const float cx = (float) bounds.getCentreX();
    const float cy = (float) bounds.getCentreY() - 4.f;
    const float radius = (float) juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.32f;

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

    g.setFont (DysektLookAndFeel::makeFont (10.f));
    g.setColour (theme.foreground.withAlpha (0.6f));
    g.drawText (label, bounds.withY (bounds.getY() - 2).withHeight (12), juce::Justification::centredTop);

    g.setColour (theme.foreground);
    g.setFont (DysektLookAndFeel::makeFont (11.f, true));
    g.drawText (valueStr, bounds.withY (bounds.getBottom() - 14).withHeight (14), juce::Justification::centredBottom);
}

void Sf2InstrumentWorkspace::drawNoteMeter (juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    if (bounds.isEmpty()) return;
    const auto& theme = getTheme();
    g.setColour (theme.button);
    g.fillRoundedRectangle (bounds.toFloat(), 2.f);
    g.setColour (theme.accent);
    auto fill = bounds.toFloat();
    fill.setWidth (fill.getWidth() * juce::jlimit (0.f, 1.f, noteActivityLevel));
    g.fillRoundedRectangle (fill, 2.f);
    g.setColour (theme.foreground.withAlpha (0.6f));
    g.setFont (DysektLookAndFeel::makeFont (9.f));
    g.drawText ("NOTE ACTIVITY", bounds, juce::Justification::centred);
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
                                processor.sfzPlayer.getCurrentPresetIndex(),
                                processor.sfzPlayer.getMidiChannel());
        restoreGridChannelAssignments();
    }

    // ── MIDI channel range (for the spinner display) ──────────────────────────
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
    // Throttle down when idle: once the level has decayed to ~0 and no notes
    // are active, skip repainting the meter zone every tick.
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

    // ── Column-3 mixer visibility can change as assignments come and go ──────
    refreshChannelFxLabels();

    // Skip the (relatively expensive) full repaint once things have settled:
    // still repaint the note-meter strip alone so its decay finishes smoothly.
    if (idleTicks > 6)
        repaint (noteMeterZone);
    else
        repaint();
}

int Sf2InstrumentWorkspace::countAssignedChannels() const noexcept
{
    int n = 0;
    for (int c = 0; c < 16; ++c)
        if (assignedChannelMask & (1u << c)) ++n;
    return n;
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
    for (auto& ap : sf2Presets)
        if (ap.ch >= 1 && ap.ch <= 16)
            channelFxPanel.setChannelLabel (ap.ch - 1, ap.name);

    if (maskChanged)
        resized();   // channel count crossing the >1 threshold changes column 3
}

// =============================================================================
//  Program grid data plumbing
// =============================================================================

void Sf2InstrumentWorkspace::restoreGridChannelAssignments()
{
    std::unordered_map<int, int> chMap;
    for (const auto& ap : sf2Presets)
    {
        for (int i = 0; i < (int) presetList.size(); ++i)
        {
            if (presetList[(size_t) i].name == ap.name)
            {
                chMap[i] = ap.ch;
                break;
            }
        }
    }
    programGrid.setPresetChannels (chMap);
}

void Sf2InstrumentWorkspace::notifyPresetChannelChanged (const juce::String& presetName,
                                                          int midiCh1Based)
{
    if (midiCh1Based == 0)
    {
        sf2Presets.erase (std::remove_if (sf2Presets.begin(), sf2Presets.end(),
            [&] (const AssignedPreset& ap) { return ap.name == presetName; }),
            sf2Presets.end());
    }
    else
    {
        bool found = false;
        for (auto& ap : sf2Presets)
            if (ap.name == presetName) { ap.ch = midiCh1Based; found = true; break; }
        if (! found)
            sf2Presets.push_back ({ presetName, midiCh1Based });
    }

    refreshChannelFxLabels();
    resized();
    repaint();
}

void Sf2InstrumentWorkspace::onFileChosen (const juce::File& f)
{
    if (f.getFileExtension().toLowerCase() != ".sf2")
        return;   // SF2-PLAYER only accepts .sf2 — silently ignore anything else

    processor.sfzPlayer.loadFile (f, processor.fileLoadPool);

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
                            processor.sfzPlayer.getCurrentPresetIndex(),
                            processor.sfzPlayer.getMidiChannel());
    restoreGridChannelAssignments();

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
//  MIDI Learn menu (right-click on a voice knob)
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

    // ── Channel-range spinners ─────────────────────────────────────────────
    auto adjustChannel = [&] (bool isLow, int delta)
    {
        const uint32_t curMask = processor.sfPlayerChannelMask.load (std::memory_order_relaxed) & DysektProcessor::kSf2AllowedMidiChannelMask;
        int lo = 0, hi = 0;
        if (curMask != 0)
        {
            for (int c = 3; c <= 16; ++c)  if (curMask & (1u << c)) { lo = c; break; }
            for (int c = 16; c >= 3; --c)  if (curMask & (1u << c)) { hi = c; break; }
        }
        if (lo == 0) lo = 3;
        if (hi == 0) hi = lo;

        const uint32_t reservedMask = processor.chromaticSliceChannelMask.load (std::memory_order_relaxed)
                                     | processor.sfzPlayer2ChannelMask.load (std::memory_order_relaxed);
        auto isFree = [&] (int ch) -> bool
        {
            if (ch < 3 || ch > 16) return false;
            return ! (reservedMask & (1u << ch));
        };

        if (isLow)
        {
            int newLo = juce::jlimit (3, hi, lo + delta);
            while (newLo >= 3 && newLo <= hi && ! isFree (newLo))
                newLo += delta > 0 ? 1 : -1;
            newLo = juce::jlimit (3, hi, newLo);
            if (isFree (newLo))
            {
                uint32_t mask = 0u;
                for (int c = newLo; c <= hi; ++c) if (isFree (c)) mask |= (1u << c);
                processor.sfPlayerChannelMask.store      (mask, std::memory_order_relaxed);
                processor.savedSfPlayerChannelMask.store (mask, std::memory_order_relaxed);
            }
        }
        else
        {
            int newHi = juce::jlimit (lo, 16, hi + delta);
            while (newHi >= lo && newHi <= 16 && ! isFree (newHi))
                newHi += delta > 0 ? 1 : -1;
            newHi = juce::jlimit (lo, 16, newHi);
            if (isFree (newHi))
            {
                uint32_t mask = 0u;
                for (int c = lo; c <= newHi; ++c) if (isFree (c)) mask |= (1u << c);
                processor.sfPlayerChannelMask.store      (mask, std::memory_order_relaxed);
                processor.savedSfPlayerChannelMask.store (mask, std::memory_order_relaxed);
            }
        }
        repaint();
    };

    if (chLowDec .contains (pos)) { adjustChannel (true,  -1); return; }
    if (chLowInc .contains (pos)) { adjustChannel (true,  +1); return; }
    if (chHighDec.contains (pos)) { adjustChannel (false, -1); return; }
    if (chHighInc.contains (pos)) { adjustChannel (false, +1); return; }

    // ── Right-click — MIDI Learn menu on the voice knobs ──────────────────────
    if (e.mods.isRightButtonDown())
    {
        using F = DysektProcessor::SliceParamField;
        struct { juce::Rectangle<int>& zone; int fieldId; } knobFields[] =
        {
            { transZone,   F::FieldSfzTranspose },
            { fineZone,    F::FieldSfzFineTune   },
            { panZone,     F::FieldSfzPan        },
            { volZone,     F::FieldSfzVol        },
            { rvMixZone,   F::FieldSfzReverbMix  },
            { rvSizeZone,  F::FieldSfzReverbSize },
        };
        for (auto& kf : knobFields)
            if (kf.zone.contains (pos))
            {
                showMidiLearnMenu (kf.fieldId, e.getScreenPosition());
                return;
            }
        return;
    }

    // ── Knob drag start ────────────────────────────────────────────────────────
    struct { juce::Rectangle<int>& zone; ActiveKnob id; float val; } knobs[] =
    {
        { transZone,  ActiveKnob::Transpose, transToNorm (processor.sfzPlayer.getTranspose()) },
        { fineZone,   ActiveKnob::FineTune,  fineToNorm  (processor.sfzPlayer.getFineTune())   },
        { panZone,    ActiveKnob::Pan,       panToNorm   (processor.sfzPlayer.getPan())        },
        { volZone,    ActiveKnob::Volume,    volToNorm   (processor.sfzPlayer.getVolume())     },
        { rvMixZone,  ActiveKnob::ReverbMix, processor.sfzPlayer.getReverbMix()  / 100.0f      },
        { rvSizeZone, ActiveKnob::ReverbSize,processor.sfzPlayer.getReverbSize() / 100.0f      },
    };
    for (auto& k : knobs)
    {
        if (k.zone.contains (pos))
        {
            activeKnob   = k.id;
            dragStartY   = pos.y;
            dragStartVal = k.val;
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
        case ActiveKnob::Transpose:  processor.sfzPlayer.setTranspose (normToTrans (newNorm)); break;
        case ActiveKnob::FineTune:   processor.sfzPlayer.setFineTune  (normToFine  (newNorm)); break;
        case ActiveKnob::Pan:        processor.sfzPlayer.setPan       (normToPan   (newNorm)); break;
        case ActiveKnob::Volume:     processor.sfzPlayer.setVolume    (normToVol   (newNorm)); break;
        case ActiveKnob::ReverbMix:  processor.sfzPlayer.setReverbMix  (newNorm * 100.0f);     break;
        case ActiveKnob::ReverbSize: processor.sfzPlayer.setReverbSize (newNorm * 100.0f);     break;
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
    if (transZone.contains  (pos)) { processor.sfzPlayer.setTranspose (0);     repaint(); }
    if (fineZone.contains   (pos)) { processor.sfzPlayer.setFineTune  (0.0f);  repaint(); }
    if (panZone.contains    (pos)) { processor.sfzPlayer.setPan       (0.0f);  repaint(); }
    if (volZone.contains    (pos)) { processor.sfzPlayer.setVolume    (1.0f);  repaint(); }
    if (rvMixZone.contains  (pos)) { processor.sfzPlayer.setReverbMix  (0.0f);  repaint(); }
    if (rvSizeZone.contains (pos)) { processor.sfzPlayer.setReverbSize (50.0f); repaint(); }
}

void Sf2InstrumentWorkspace::mouseWheelMove (const juce::MouseEvent& e,
                                              const juce::MouseWheelDetails& w)
{
    const auto  pos  = e.getPosition();
    const float step = w.deltaY * (e.mods.isShiftDown() ? 0.01f : 0.05f);
    auto adjustNorm = [&] (float current, float s) { return juce::jlimit (0.f, 1.f, current + s); };

    if (transZone.contains (pos))
        processor.sfzPlayer.setTranspose (normToTrans (adjustNorm (transToNorm (processor.sfzPlayer.getTranspose()), step)));
    else if (fineZone.contains (pos))
        processor.sfzPlayer.setFineTune (normToFine (adjustNorm (fineToNorm (processor.sfzPlayer.getFineTune()), step)));
    else if (panZone.contains (pos))
        processor.sfzPlayer.setPan (normToPan (adjustNorm (panToNorm (processor.sfzPlayer.getPan()), step)));
    else if (volZone.contains (pos))
        processor.sfzPlayer.setVolume (normToVol (adjustNorm (volToNorm (processor.sfzPlayer.getVolume()), step)));
    else if (rvMixZone.contains (pos))
        processor.sfzPlayer.setReverbMix  (juce::jlimit (0.0f, 100.0f, processor.sfzPlayer.getReverbMix()  + step * 100.0f));
    else if (rvSizeZone.contains (pos))
        processor.sfzPlayer.setReverbSize (juce::jlimit (0.0f, 100.0f, processor.sfzPlayer.getReverbSize() + step * 100.0f));

    repaint();
}

// ── Knob normalisation helpers (identical mapping to SfzDropdownPanel's) ─────
float Sf2InstrumentWorkspace::volToNorm   (float linear) const { return juce::jlimit (0.f, 1.f, linear * 0.5f); }
float Sf2InstrumentWorkspace::normToVol   (float n)      const { return n * 2.0f; }
float Sf2InstrumentWorkspace::transToNorm (int semi)     const { return ((float) semi + 24.0f) / 48.0f; }
int   Sf2InstrumentWorkspace::normToTrans (float n)      const { return juce::roundToInt (n * 48.0f - 24.0f); }
float Sf2InstrumentWorkspace::panToNorm   (float p)      const { return (p + 1.0f) * 0.5f; }
float Sf2InstrumentWorkspace::normToPan   (float n)      const { return n * 2.0f - 1.0f; }
float Sf2InstrumentWorkspace::fineToNorm  (float cents)  const { return (cents + 100.0f) / 200.0f; }
float Sf2InstrumentWorkspace::normToFine  (float n)      const { return n * 200.0f - 100.0f; }
