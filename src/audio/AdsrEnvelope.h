#pragma once
#include <cmath>

// ============================================================
//  AdsrEnvelope  –  AHDSR with one-shot drum mode
//
//  Stages (in order):
//    Attack  : ramp from 0 → 1 (exponential one-pole)
//    Hold    : hold at 1 for holdSec (frame counter)
//    Decay   : ramp from 1 → target (exponential one-pole)
//              target = 0.0  when oneShotMode == true   (drum / one-shot)
//              target = sustainLvl  when oneShotMode == false (gate mode)
//    Sustain : hold at sustainLvl until noteOff() (gate mode only)
//    Release : ramp to silence on noteOff() (gate mode only)
//              in one-shot mode noteOff() is a no-op — the decay drives
//              the envelope all the way to Done with no external trigger.
//
//  Curve shape: all ramps use the classic one-pole exponential filter
//  ( level += coeff * (target - level) ) which gives a natural
//  "analog-style" curve — fast at the start, tapering at the end.
//  This matches TAL-Drum's "analog modelled non-linear envelopes".
// ============================================================

class AdsrEnvelope
{
public:
    enum State { Attack, Hold, Decay, Sustain, Release, Done };

    // --------------------------------------------------------
    //  noteOn
    //
    //  holdSec      — time to hold at peak before decay starts (0 = no hold)
    //  oneShotMode  — true: decay goes to silence, noteOff() ignored
    //                 false: normal gate ADSR behaviour
    // --------------------------------------------------------
    void noteOn (float attackSec, float decaySec, float sustain,
                 float releaseSec, double sr,
                 float holdSec     = 0.0f,
                 bool  oneShotMode = false)
    {
        sampleRate    = sr;
        attackCoeff   = makeCoeff (attackSec);
        decayCoeff    = makeCoeff (decaySec);
        sustainLvl    = jlimit (0.0f, 1.0f, sustain);
        releaseCoeff  = makeCoeff (releaseSec);
        oneShot       = oneShotMode;

        // Hold length in samples (rounded; minimum 0)
        holdSamplesLeft = (holdSec > 0.0001f)
                            ? (int)(holdSec * (float)sr + 0.5f)
                            : 0;

        state = Attack;
        level = 0.0f;
    }

    // --------------------------------------------------------
    //  noteOff  — ignored in one-shot mode
    // --------------------------------------------------------
    void noteOff()
    {
        if (oneShot)
            return;                   // one-shot: decay drives envelope to Done

        if (state != Done)
            state = Release;
    }

    // --------------------------------------------------------
    //  forceRelease  — always triggers, regardless of mode
    //  (used internally when the sample data runs out)
    // --------------------------------------------------------
    void forceRelease (float timeSec, double sr)
    {
        sampleRate   = sr;
        releaseCoeff = makeCoeff (timeSec);
        state        = Release;
    }

    // --------------------------------------------------------
    //  processSample  — call once per output sample
    // --------------------------------------------------------
    float processSample()
    {
        switch (state)
        {
            // ── Attack ──────────────────────────────────────
            case Attack:
                // Target slightly above 1.0 so the curve reaches 1.0
                // within the specified time rather than asymptotically.
                level += attackCoeff * (1.01f - level);
                if (level >= 1.0f)
                {
                    level = 1.0f;
                    state = (holdSamplesLeft > 0) ? Hold : Decay;
                }
                break;

            // ── Hold ─────────────────────────────────────────
            case Hold:
                level = 1.0f;
                if (--holdSamplesLeft <= 0)
                    state = Decay;
                break;

            // ── Decay ────────────────────────────────────────
            case Decay:
            {
                // In one-shot mode decay target is silence (0).
                // In gate mode decay target is the sustain level.
                const float target = oneShot ? 0.0f : sustainLvl;
                level += decayCoeff * (target - level);

                // Threshold: ~-76 dB
                if (std::abs (level - target) < 0.00015f)
                {
                    level = target;
                    state = oneShot ? Done : Sustain;
                }
                break;
            }

            // ── Sustain (gate mode only) ─────────────────────
            case Sustain:
                level = sustainLvl;
                break;

            // ── Release ──────────────────────────────────────
            case Release:
                // Multiply by per-sample factor = exp(-5 / (releaseTime * sr))
                level *= (1.0f - releaseCoeff);
                if (level < 0.00015f)
                {
                    level = 0.0f;
                    state = Done;
                }
                break;

            // ── Done ─────────────────────────────────────────
            case Done:
                level = 0.0f;
                break;
        }

        return level;
    }

    bool  isDone()   const { return state == Done; }
    State getState() const { return state; }
    float getLevel() const { return level; }

private:
    // One-pole coefficient so that after timeSec the envelope is
    // ~99.3 % toward its target — gives the natural exponential curve.
    float makeCoeff (float timeSec) const
    {
        if (timeSec < 0.0001f || sampleRate < 1.0)
            return 1.0f;
        return 1.0f - std::exp (-5.0f / (timeSec * (float)sampleRate));
    }

    // Clamp helper (avoids pulling in a full JUCE header here)
    static float jlimit (float lo, float hi, float v)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    State  state           = Done;
    float  level           = 0.0f;
    float  attackCoeff     = 1.0f;
    float  decayCoeff      = 1.0f;
    float  sustainLvl      = 1.0f;
    float  releaseCoeff    = 1.0f;
    int    holdSamplesLeft = 0;
    bool   oneShot         = false;
    double sampleRate      = 44100.0;
};
