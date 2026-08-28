#pragma once
// =============================================================================
//  SampleZone.h  —  One mapped sample region in a MultisamplerInstrument
//  ─────────────────────────────────────────────────────────────────────────
//  Extended with native per-zone 3-Band Parametric EQ fields matching SFZ opcodes.
// =============================================================================

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <cstdint>
#include <vector>
#include <utility>

enum class LoopMode
{
    noLoop,
    oneShot,
    loopContinuous,
    loopSustain
};

struct SampleZone
{
    juce::Uuid id;
    juce::File sampleFile;

    // ── Mapping ──────────────────────────────────────────────────────────
    int lowKey       = 0;
    int highKey      = 127;
    int rootKey      = 60;
    int lowVelocity  = 1;
    int highVelocity = 127;

    // ── Tuning / level ───────────────────────────────────────────────────
    float tuneCents  = 0.0f;   ///< -1200 .. +1200 (sfz tune)
    float gainDb     = 0.0f;   ///< sfz volume
    float pan        = 0.0f;   ///< -1 (L) .. +1 (R), sfz pan is -100..100

    // ── Sample region ────────────────────────────────────────────────────
    int64_t sampleStart = 0;
    int64_t sampleEnd   = -1;   ///< -1 == full sample length
    int64_t loopStart   = -1;
    int64_t loopEnd     = -1;
    LoopMode loopMode   = LoopMode::noLoop;

    // ── Envelopes ────────────────────────────────────────────────────────
    float attackSeconds  = 0.001f;
    float decaySeconds   = 0.0f;
    float sustainLevel   = 1.0f;   ///< 0..1 linear
    float releaseSeconds = 0.05f;

    // ── Filter (Lowpass) ─────────────────────────────────────────────────
    float filterCutoffHz  = 20000.0f;
    float filterResonance = 0.0f;

    // ── Per-Zone 3-Band Parametric EQ (sfz eq1_*, eq2_*, eq3_*) ──────────
    bool  eqEnabled = true;
    float eq1Freq   = 80.0f;    ///< Low-shelf frequency (Hz): 20..1000, sfz eq1_freq
    float eq1Gain   = 0.0f;     ///< Low-shelf gain (dB): -24..+24, sfz eq1_gain
    float eq1Bw     = 1.0f;     ///< Low-shelf bandwidth/slope, sfz eq1_bw

    float eq2Freq   = 1000.0f;  ///< Mid peak frequency (Hz): 100..10000, sfz eq2_freq
    float eq2Gain   = 0.0f;     ///< Mid peak gain (dB): -24..+24, sfz eq2_gain
    float eq2Bw     = 1.0f;     ///< Mid peak bandwidth/Q (octaves): 0.1..8.0, sfz eq2_bw

    float eq3Freq   = 8000.0f;  ///< High-shelf frequency (Hz): 1000..20000, sfz eq3_freq
    float eq3Gain   = 0.0f;     ///< High-shelf gain (dB): -24..+24, sfz eq3_gain
    float eq3Bw     = 1.0f;     ///< High-shelf bandwidth/slope, sfz eq3_bw

    // ── Voice grouping & output routing ──────────────────────────────────
    int  group        = 0;
    int  offBy        = 0;
    int  outputBus    = 1;
    bool showInMixer  = true;
    bool reverse      = false;
    bool enabled      = true;

    // ── Round-robin sequencing (sfz seq_position / seq_length) ────────────
    // sequenceLength == 1 (the default) means "no round robin" — the zone
    // always plays. sequenceLength > 1 groups this zone with any other zone
    // sharing the same key/velocity range into a `sequenceLength`-deep
    // round-robin cycle, and sequencePosition (1-based) is this zone's slot
    // in that cycle. See MultisamplerInstrument::validate() for the range
    // check (sequencePosition must be within 1..sequenceLength).
    int  sequencePosition = 1;
    int  sequenceLength   = 1;

    bool hasCustomColour = false;
    juce::uint32 customColourArgb = 0xFF000000;

    std::vector<std::pair<juce::String, juce::String>> extraOpcodes;

    bool hasMissingSample() const { return sampleFile == juce::File() || ! sampleFile.existsAsFile(); }
    bool keyInRange (int midiNote) const noexcept { return midiNote >= lowKey && midiNote <= highKey; }
    bool velocityInRange (int velocity) const noexcept { return velocity >= lowVelocity && velocity <= highVelocity; }
    bool matches (int midiNote, int velocity) const noexcept { return enabled && keyInRange (midiNote) && velocityInRange (velocity); }
};

// =============================================================================
//  Sample-range resolution
//  ─────────────────────────────────────────────────────────────────────────
//  Shared by the UI (SliceLcdDisplay, MultisamplerWaveformLcd,
//  AddZoneTrimOverlay — trim/waveform display) and by anything reading a
//  zone's start/end against a decoded file's actual frame count.
//  SampleZone::sampleStart/sampleEnd (and, by the same convention, an
//  AddZoneTrimOverlay trim seed) use "-1 == full length" for the end point;
//  this normalises that, plus any out-of-range or reversed values, into a
//  concrete, always-valid [start, end) pair.
// =============================================================================

struct SampleRange
{
    int64_t start = 0;
    int64_t end   = 0;   ///< exclusive

    int64_t length() const noexcept { return end > start ? end - start : 0; }
};

/** Clamps `start`/`end` (SFZ convention: end < 0 means "unset" -> full
    sample length) into a valid [0, totalFrames) range. Reversed or
    degenerate input still yields at least one playable frame, as long as
    `totalFrames` allows it. `totalFrames <= 0` (e.g. a preview before the
    file has finished decoding) resolves to an empty range rather than
    asserting or dividing by zero. */
inline SampleRange resolveSampleRange (int64_t start, int64_t end, int64_t totalFrames) noexcept
{
    if (totalFrames <= 0)
        return {};

    if (end < 0)
        end = totalFrames;

    start = juce::jlimit ((int64_t) 0, totalFrames, start);
    end   = juce::jlimit ((int64_t) 0, totalFrames, end);

    if (end <= start)
    {
        if (start < totalFrames)
        {
            end = start + 1;
        }
        else if (start > 0)
        {
            start -= 1;
            end = start + 1;
        }
    }

    return { start, end };
}

/** Convenience overload: resolves a zone's own sampleStart/sampleEnd. */
inline SampleRange resolveSampleRange (const SampleZone& zone, int64_t totalFrames) noexcept
{
    return resolveSampleRange (zone.sampleStart, zone.sampleEnd, totalFrames);
}

// =============================================================================
//  LoopMode <-> SFZ loop_mode= opcode value
// =============================================================================

inline juce::String loopModeToOpcodeValue (LoopMode mode) noexcept
{
    switch (mode)
    {
        case LoopMode::oneShot:        return "one_shot";
        case LoopMode::loopContinuous: return "loop_continuous";
        case LoopMode::loopSustain:    return "loop_sustain";
        case LoopMode::noLoop:
        default:                       return "no_loop";
    }
}

inline LoopMode loopModeFromOpcodeValue (const juce::String& value) noexcept
{
    if (value == "one_shot")        return LoopMode::oneShot;
    if (value == "loop_continuous") return LoopMode::loopContinuous;
    if (value == "loop_sustain")    return LoopMode::loopSustain;
    return LoopMode::noLoop;
}