#pragma once

//==============================================================================
//  ZoneEnvelopeRanges — the ONE definition of the multisampler zone envelope
//  (ADSR) ranges. The waveform's A/D/S/R nodes, the control-bar cells, the zone
//  LCD knobs/readouts and the model clamps all use these, so the same zone value
//  always sits at the same place and reads the same everywhere. Time is mapped
//  linearly: position = seconds / max. Sustain is 0..1 (shown as 0..100 %).
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
}
