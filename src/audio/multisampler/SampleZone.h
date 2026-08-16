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

    bool enabled = true;   ///< editor-only mute; excluded from SFZ export when false

    /** User-picked colour override, round-tripped through the same
        `dysekt_zone_color` custom opcode ZONES already writes/reads (see
        PluginEditor::setZoneBuilderZoneColour / SfzImporter's opcode
        table). When false, the zone's colour is purely derived from its
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
