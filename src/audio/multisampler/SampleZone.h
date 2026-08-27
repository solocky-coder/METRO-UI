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

    bool hasCustomColour = false;
    juce::uint32 customColourArgb = 0xFF000000;

    std::vector<std::pair<juce::String, juce::String>> extraOpcodes;

    bool hasMissingSample() const { return sampleFile == juce::File() || ! sampleFile.existsAsFile(); }
    bool keyInRange (int midiNote) const noexcept { return midiNote >= lowKey && midiNote <= highKey; }
    bool velocityInRange (int velocity) const noexcept { return velocity >= lowVelocity && velocity <= highVelocity; }
    bool matches (int midiNote, int velocity) const noexcept { return enabled && keyInRange (midiNote) && velocityInRange (velocity); }
};