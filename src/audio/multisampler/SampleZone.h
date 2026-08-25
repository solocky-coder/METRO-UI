#pragma once
// =============================================================================
//  SampleZone.h  —  One mapped sample region in a MultisamplerInstrument
//  ─────────────────────────────────────────────────────────────────────────
//  This is the native, editable representation of a single <region>. It is
//  intentionally a plain-data struct (no logic) so it can be freely copied,
//  diffed for undo/redo, and serialized without any engine dependency.
//
//  Field ranges intentionally mirror the SFZ opcodes they round-trip with
//  (see SfzImporter.h / SfzExporter.h for the supported subset) so mapping
//  values never need lossy rescaling on import or export.
// =============================================================================

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <cstdint>
#include <vector>
#include <utility>

/** Mirrors the SFZ `loop_mode` opcode's four values exactly, so import and
    export never need to guess at an equivalence. */
enum class LoopMode
{
    noLoop,          ///< loop_mode=no_loop        (default)
    oneShot,         ///< loop_mode=one_shot
    loopContinuous,  ///< loop_mode=loop_continuous
    loopSustain      ///< loop_mode=loop_sustain
};

inline juce::String loopModeToOpcodeValue (LoopMode m)
{
    switch (m)
    {
        case LoopMode::oneShot:         return "one_shot";
        case LoopMode::loopContinuous:  return "loop_continuous";
        case LoopMode::loopSustain:     return "loop_sustain";
        case LoopMode::noLoop:
        default:                        return "no_loop";
    }
}

inline LoopMode loopModeFromOpcodeValue (const juce::String& v)
{
    const auto s = v.trim().toLowerCase();
    if (s == "one_shot")        return LoopMode::oneShot;
    if (s == "loop_continuous") return LoopMode::loopContinuous;
    if (s == "loop_sustain")    return LoopMode::loopSustain;
    return LoopMode::noLoop;
}

// -----------------------------------------------------------------------------
struct SampleZone
{
    /** Stable identity — selection, undo/redo, and sfizz region updates key
        off this rather than the zone's index in MultisamplerInstrument::zones,
        since that index shifts whenever an earlier zone is added or removed. */
    juce::Uuid id;

    /** Absolute path at authoring time. Bundles (.metrokit) additionally store
        a bundle-relative path in InstrumentSerializer; this field is the
        resolved, playable location on this machine (see relink handling in
        MultisamplerInstrument::relinkSample). */
    juce::File sampleFile;

    // ── Mapping ──────────────────────────────────────────────────────────
    int lowKey      = 0;
    int highKey     = 127;
    int rootKey     = 60;
    int lowVelocity = 1;
    int highVelocity = 127;

    // ── Tuning / level ───────────────────────────────────────────────────
    float tuneCents = 0.0f;   ///< -1200 .. +1200 (sfz `tune`, extended range)
    float gainDb    = 0.0f;   ///< sfz `volume`
    float pan       = 0.0f;   ///< -1 (L) .. +1 (R), sfz `pan` is -100..100

    // ── Sample region ────────────────────────────────────────────────────
    int64_t sampleStart = 0;
    int64_t sampleEnd   = -1;   ///< -1 == full sample length
    int64_t loopStart   = -1;
    int64_t loopEnd     = -1;
    LoopMode loopMode   = LoopMode::noLoop;

    // ── Amp envelope ─────────────────────────────────────────────────────
    float attackSeconds  = 0.005f;
    float decaySeconds   = 0.1f;
    float sustainLevel   = 1.0f;   ///< 0..1
    float releaseSeconds = 0.1f;

    // ── Filter ───────────────────────────────────────────────────────────
    float filterCutoffHz  = 20000.0f;
    float filterResonance = 0.0f;   ///< 0..1, exported as sfz `resonance` (dB, 0..40)

    // ── Voicing ──────────────────────────────────────────────────────────
    int group            = 0;   ///< sfz `group` — 0 means "no group"
    int offBy             = 0;   ///< sfz `off_by` — choke: silences notes in `group`
    int sequencePosition  = 0;   ///< sfz `seq_position`
    int sequenceLength    = 0;   ///< sfz `seq_length` — 0/1 means "no round robin"

    /** Output routing for this zone: 0 = Main, 1-15 = Aux N. Round-tripped
        through the `dysekt_output_bus` custom opcode (same pattern as
        `dysekt_zone_color` — see SfzImporter/SfzExporter) so it survives
        save/reload and MultisamplerEditor::performEngineSync()'s
        export/reimport cycle instead of only ever living on the derived
        sliceManager2 copy. Edited via MultisamplerZoneField::outputBus
        (MultisamplerZoneLcd's OUT cell) or MultisamplerEditor::
        autoAssignOutputBuses() (the drum-kit auto-routing prompt). */
    int outputBus = 0;

    /** Manual "pin to mixer" override, independent of outputBus. A zone
        routed to an Aux bus already gets its own MixerPanel row
        automatically (see PluginProcessor.cpp's PendingZonePin handling),
        but a zone left on Main has no way to get a row of its own without
        this — e.g. wanting to automate/monitor one drum-kit voice's gain
        without actually splitting it onto an Aux bus. Round-tripped through
        the `dysekt_show_in_mixer` custom opcode (same pattern as
        `dysekt_output_bus` — see SfzImporter/SfzExporter) and, on reimport,
        OR'd with the outputBus!=0 auto-pin rather than replacing it, so
        turning this off never hides a zone that's routed off Main. Edited
        via MultisamplerZoneField::showInMixer (MultisamplerZoneLcd's MIX
        cell) — mirrors SliceControlBar's own per-slice MIX toggle
        (Slice::showInMixer). */
    bool showInMixer = false;

    bool enabled = true;   ///< editor-only mute; excluded from SFZ export when false

    /** User-picked colour override, round-tripped through the same
        `dysekt_zone_color` custom opcode (see SfzImporter's opcode table
        and SfzExporter). When false, the zone's colour is purely derived from its
        palette index (SfzZoneColours::zoneColour) — see ZoneMapView::
        rebuildLayout() and MultisamplerEditor::toKeyzones(), which both
        prefer this field over the index-derived colour whenever it's set. */
    bool hasCustomColour = false;
    juce::uint32 customColourArgb = 0xFF000000;   ///< only meaningful when hasCustomColour

    /** Opcodes read on import that fall outside the documented supported
        subset (see SfzImporter). Preserved verbatim and re-emitted on export
        so a save/reload cycle doesn't silently discard authoring the user
        did in another editor. Never interpreted by METRO-UI itself. */
    std::vector<std::pair<juce::String, juce::String>> extraOpcodes;

    bool hasMissingSample() const
    {
        return sampleFile == juce::File() || ! sampleFile.existsAsFile();
    }

    bool keyInRange (int midiNote) const noexcept
    {
        return midiNote >= lowKey && midiNote <= highKey;
    }

    bool velocityInRange (int velocity) const noexcept
    {
        return velocity >= lowVelocity && velocity <= highVelocity;
    }

    /** True if this zone would ever sound for the given note/velocity pair. */
    bool matches (int midiNote, int velocity) const noexcept
    {
        return enabled && keyInRange (midiNote) && velocityInRange (velocity);
    }
};

/** A resolved, clamped [start, end) playback range against a known
    totalFrames — see SampleZone::sampleStart/sampleEnd's doc comments for
    the half-open convention this normalises to. Any display or engine code
    reading a zone's sample range should go through resolveSampleRange()
    rather than re-deriving this by hand, so a malformed/out-of-range import
    can never produce a negative-length or out-of-bounds interval anywhere
    a zone is shown or played (SliceWaveformLcd, SliceLcdDisplay,
    AddZoneTrimOverlay's preview-clamp on reopen, etc.) */
struct ResolvedSampleRange
{
    int64_t start = 0;
    int64_t end   = 0;   // exclusive

    int64_t length() const noexcept { return end - start; }
};

/** totalFrames <= 0 resolves to an empty {0, 0} range rather than asserting —
    callers (typically UI code reacting to a not-yet-decoded preview) are
    expected to treat length() == 0 as "nothing to show yet". */
inline ResolvedSampleRange resolveSampleRange (int64_t sampleStart, int64_t sampleEnd,
                                                int64_t totalFrames) noexcept
{
    if (totalFrames <= 0)
        return {};

    const int64_t resolvedEnd = sampleEnd < 0 ? totalFrames : sampleEnd;
    const int64_t start = juce::jlimit<int64_t> (0, totalFrames - 1, sampleStart);
    const int64_t end   = juce::jlimit<int64_t> (start + 1, totalFrames, resolvedEnd);
    return { start, end };
}

inline ResolvedSampleRange resolveSampleRange (const SampleZone& z, int64_t totalFrames) noexcept
{
    return resolveSampleRange (z.sampleStart, z.sampleEnd, totalFrames);
}
