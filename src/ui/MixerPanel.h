#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <unordered_map>
#include <vector>
#include "../audio/SfzPlayer.h"   // Sf2PresetInfo

class DysektProcessor;

/**
 * MixerPanel — horizontal row mixer.
 *
 * Each row = one slice.  Columns: GAIN · PAN · FCUT · PRES · MUTE GRP · OUT
 * Clicking a row fires CmdSelectSlice.
 * Knobs write per-slice values directly (no global fallback from this panel).
 * Scrollable — all slices visible.
 *
 * Below the slice rows sits the SF-PLAYER section:
 *   • One header row  (SF-PLAYER label, master SF2 vol/pan, aggregate meter)
 *   • N channel rows  (one per assigned preset — GAIN · PAN only)
 * Channel rows are populated via setActiveChannels().
 */
class MixerPanel : public juce::Component,
                   public juce::Timer
{
public:
    /** Fixed panel height used by PluginEditor layout. */
    static constexpr int kPanelH      = 260;
    /** Height of the column-header row. */
    static constexpr int kHeaderH     = 28;
    /** Height of each slice row.
        Bumped 38 -> 46 -> 52 to keep giving value text and knobs more
        vertical room as fonts grow. Panel height (kPanelH) is unchanged, so
        taller rows mean fewer rows fit before scrolling — an intentional
        tradeoff for readability. */
    static constexpr int kRowH        = 52;
    /** Height of the master row at the bottom. */
    static constexpr int kMasterH     = 56;
    /** Height of the SF-PLAYER header row. */
    static constexpr int kSf2RowH     = 52;
    /** Height of each per-channel sub-row under the SF-PLAYER row. */
    static constexpr int kSf2ChRowH   = 42;
    /** Width of the slice name column. */
    static constexpr int kNameColW    = 88;
    /** Width of each knob column.
        Widened from 84 -> 108 (24px/col x 8 cols = 192px reclaimed) so the
        GAIN/PAN/FCUT/PRES/OUT knobs, sliders, and value text have more
        breathing room. The peak meter (drawn in whatever space remains
        after these columns) narrows by the same 192px as a direct
        consequence — no separate meter cap needed, and no dead space is
        introduced since the meter still fills 100% of what's left. */
    static constexpr int kKnobColW    = 108;
    /** Number of knob columns (GAIN PAN FCUT PRES MUTE CHRO OUT — meter is after). */
    static constexpr int kNumCols     = 8;
    /** Width of the horizontal peak meter column after OUT. */
    static constexpr int kMeterColW   = 120;

    explicit MixerPanel (DysektProcessor& p);
    ~MixerPanel() override;

    /** Fired when the user clicks a row that corresponds to one of the three
     *  player tabs, so the owner can switch the main UI to match.
     *  @param uiMode  0 = Slicer (slice row), 1 = SFZ-PLAYER (sfzPlayer2 row),
     *                 2 = SF2-PLAYER (SF-PLAYER header or channel row).
     *  Not fired for clicks on the master row — no player tab corresponds
     *  to the overall output. */
    std::function<void(int uiMode)> onTrackSelected;

    void paint   (juce::Graphics&) override;
    void resized () override;

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove   (const juce::MouseEvent&,
                           const juce::MouseWheelDetails&) override;

    /** Called from editor timer — snapshot version check, repaint if stale. */
    void updateFromSnapshot();

    /** Called when the SF2 preset→channel assignment changes.
        Rebuilds the per-channel sub-rows under the SF-PLAYER row. */
    void setActiveChannels (const std::vector<Sf2PresetInfo>& presets,
                            const std::unordered_map<int,int>& presetChannels);

    // Timer (drives hold decay and repaints while voices are active)
    void timerCallback() override
    {
        static constexpr float kHoldDecay = 0.94f;   // ~40 dB/s visual falloff
        bool anyHeld = false;
        for (int i = 0; i < kMaxHoldSlices; ++i)
        {
            holdL[i] *= kHoldDecay;
            holdR[i] *= kHoldDecay;
            if (holdL[i] > 0.001f || holdR[i] > 0.001f) anyHeld = true;
        }
        if (anyHeld) repaint();
    }

private:
    // ── Column layout ─────────────────────────────────────────────────────
public:
    /// Resolves the theme colour key represented by whatever's under this
    /// point (a slice row's colour tint, or a knob/badge drawn in accent),
    /// for the Theme Editor's PICK mode (see PluginEditor::resolveThemeKeyAt).
    /// Returns an empty string when nothing specific is identifiable, so the
    /// caller can fall back to the panel's own general theme tag.
    juce::String themeKeyAt (juce::Point<int> p) const;

private:
    enum Col { ColGain=0, ColPan, ColFcut, ColPres, ColMute, ColChro, ColLegato, ColOut };

    // ── Hit-test cell ─────────────────────────────────────────────────────
    struct Cell {
        int  row       { -1 };    // >=0 = slice index; -1 = master; -2 = sf2 header; -3 = sfz2 (real SFZ-Player) row; -4- = sf2 channel
        Col  col       { ColGain };
        juce::Rectangle<int> bounds;
        bool isMaster  { false };
        bool isSf2     { false }; // true = SF-PLAYER header row
        bool isSf2Ch   { false }; // true = SF2 per-channel sub-row
        bool isSfz2    { false }; // true = SFZ-Player (sfzPlayer2 / real .sfz engine) row
        bool isSfz2Ch  { false }; // true = per-zone sub-row under the SFZ-Player row
        int  sf2Channel{ -1 };    // FluidSynth channel index (0-based) when isSf2Ch
        int  sfz2ZoneIdx{ -1 };   // sliceManager2 slice index (real, not display-row) when isSfz2Ch
    };

    // ── Drawing helpers ───────────────────────────────────────────────────
    void drawHeader       (juce::Graphics&) const;
    void drawSliceRow     (juce::Graphics&, int rowY, int sliceIdx, bool selected) const;
    void drawMasterRow    (juce::Graphics&, int rowY) const;
    void drawSf2Row       (juce::Graphics&, int rowY) const;
    void drawSf2ChannelRow(juce::Graphics&, int rowY, int channel,
                           const Sf2PresetInfo& preset) const;
    /** Row for sfzPlayer2 — the real .sfz-file engine ("SFZ-PLAYER" tab).
        Only occupies space (and is drawn) once a .sfz file is loaded, so a
        mixer track appears automatically as soon as the SFZ-Player has
        something to play. */
    void drawSfz2Row      (juce::Graphics&, int rowY) const;
    /** Per-zone sub-row under the SFZ-Player row, for zones with
        Slice::showInMixer set on sliceManager2 — mirrors drawSf2ChannelRow
        (GAIN/PAN only) but reads/writes the zone's own slice fields instead
        of a FluidSynth channel strip. */
    void drawSfz2ChannelRow (juce::Graphics&, int ry, int zoneIdx) const;
    void drawKnobInRow (juce::Graphics&, int cx, int cy, float norm,
                        bool locked, bool isMaster = false,
                        bool isGain = false) const;
    void drawMuteBadge (juce::Graphics&, int cx, int cy,
                        int muteGroup, bool locked, bool dimmed) const;
    void drawChroBadge (juce::Graphics&, int cx, int cy, int channel, bool locked) const;
    void drawMeter     (juce::Graphics&, int x, int y, int w, int h,
                        float peakL, float peakR, juce::Colour tint, int sliceIdx) const;

    juce::String fmtGain (float db)      const;
    juce::String fmtPan  (float pan)     const;
    juce::String fmtFcut (float hz)      const;
    juce::String fmtPres (float res)     const;
    juce::String fmtOut  (int bus)       const;
    juce::String fmtMute (int mg)        const;

    float toNormGain (float db)   const;
    float toNormPan  (float pan)  const;
    float toNormFcut (float hz)   const;
    float toNormPres (float res)  const;
    float toNormOut  (int bus)    const;

    float fromNormFcut (float n) const;

    int   colX        (Col col) const;
    int   rowY        (int sliceIdx) const;   // top Y of a slice row in the scroll area
    int   masterRowY  () const;
    int   sf2RowY     () const;
    int   sf2ChRowY   (int chRowIdx) const;   // top Y of a per-channel sub-row (0-based index)
    int   sf2TotalH   () const;               // kSf2RowH + N * kSf2ChRowH
    int   sfz2RowY    () const;               // top Y of the sfzPlayer2 row, sits below the sf2 section
    int   sfz2ChRowY  (int chRowIdx) const;   // top Y of a per-zone sub-row (0-based index into visible zones)
    int   sfz2TotalH  () const;               // kSf2RowH (+ N * kSf2ChRowH for visible zones) when sfzPlayer2 has a file loaded, else 0
    Cell  hitTest     (juce::Point<int> pos) const;

    // ── Drag state ────────────────────────────────────────────────────────
    struct DragState {
        bool   active    { false };
        bool   isMaster  { false };
        bool   isSf2     { false };
        bool   isSf2Ch   { false };
        bool   isSfz2    { false };
        bool   isSfz2Ch  { false };
        int    sf2Channel{ -1 };
        int    sliceIdx  { -1 };
        int    sfz2ZoneIdx{ -1 };
        Col    col       { ColGain };
        int    startY    { 0 };
        float  startVal  { 0.f };
    } drag;

    // ── Peak-hold for phosphor meter (UI-side, decays in timerCallback) ──
    static constexpr int kMaxHoldSlices = 128;  // matches DysektProcessor::kMaxMeterSlices
    mutable std::array<float, kMaxHoldSlices> holdL {};
    mutable std::array<float, kMaxHoldSlices> holdR {};

    // ── Scroll ────────────────────────────────────────────────────────────
    int   scrollPixels { 0 };   // vertical scroll offset in pixels

    // ── State cache ───────────────────────────────────────────────────────
    uint32_t cachedVersion { 0xFFFFFFFF };
    int      cachedNumSlices { -1 };

    // ── SF2 preset→channel map ────────────────────────────────────────────
    // Populated by setActiveChannels(); drives per-channel sub-rows.
    // Each entry: { presetInfo, fluidChannel }
    struct Sf2ChEntry { Sf2PresetInfo preset; int channel; };
    std::vector<Sf2ChEntry> sf2Channels;

    // ── Text editor for double-click ──────────────────────────────────────
    std::unique_ptr<juce::TextEditor> textEditor;

    DysektProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};
