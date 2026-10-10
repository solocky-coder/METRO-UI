#pragma once
#include <algorithm>
#include <cmath>

//==============================================================================
//  ZoneEnvelopeRanges — the ONE definition of the multisampler zone envelope
//  (ADSR) ranges. The waveform's A/D/S/R nodes, the control-bar cells, the zone
//  LCD knobs/readouts and the model clamps all use these, so the same zone value
//  always sits at the same place and reads the same everywhere. On the waveform
//  graph, time is mapped linearly: position = seconds / max. Sustain is 0..1
//  (shown as 0..100 %).
//
//  KNOBS are the one exception: attack / decay / release knobs (SliceControlBar
//  and MultisamplerZoneLcd) use a cube taper — see timeToKnob()/knobToTime()
//  below — so the short times that matter for drums (a 5 ms attack is only
//  0.1 % of a 0-5 s linear sweep) get real knob travel. The knob is purely a
//  front-end for the same stored seconds, so the waveform nodes, the model
//  and saved state are unaffected.
//==============================================================================
namespace ZoneEnv
{
    constexpr float kMaxAttackSec  = 5.0f;
    constexpr float kMaxDecaySec   = 5.0f;
    constexpr float kMaxReleaseSec = 10.0f;

    // Seconds moved per pixel of vertical drag (a full sweep over ~500 px).
    constexpr float kAttackDragStep  = kMaxAttackSec  / 500.0f;
    constexpr float kDecayDragStep   = kMaxDecaySec   / 500.0f;
    constexpr float kReleaseDragStep = kMaxReleaseSec / 500.0f;

    // Knob travel per pixel of vertical drag for the tapered time knobs
    // (full sweep over ~500 px, same as the linear steps above).
    constexpr float kKnobNormPerPixel = 1.0f / 500.0f;

    // Cube taper: seconds <-> 0..1 knob position. knobToTime() is the exact
    // inverse of timeToKnob().
    inline float timeToKnob (float sec, float maxSec) noexcept
    {
        if (maxSec <= 0.0f) return 0.0f;
        return std::cbrt (std::clamp (sec / maxSec, 0.0f, 1.0f));
    }

    inline float knobToTime (float norm, float maxSec) noexcept
    {
        const float n = std::clamp (norm, 0.0f, 1.0f);
        return maxSec * n * n * n;
    }
}
