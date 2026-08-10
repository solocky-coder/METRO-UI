#pragma once
// =============================================================================
//  Sf2ChannelMixer.h  —  Scope B-Internal: real internal per-channel mixing
// =============================================================================
//  Context (see sf2-per-channel-audio-plan.md):
//
//  Scope A gave SfzPlayer real per-channel audio in 32 FluidSynth audio-group
//  buffers (16 channels × L/R), immediately summed back into the master pair
//  inline inside SfzPlayer::process(). That's correct but leaves the "combine
//  per-channel audio into the master mix" step with no seam for anything
//  channel-specific to happen along the way.
//
//  Scope B-Internal is that seam: pulling the summation out into its own
//  stage with an explicit (currently unused) per-channel hook, so a future
//  per-channel insert (EQ/comp/etc. — NOT asked for yet, see the plan's "B1"
//  notes) has one obvious place to plug in, instead of requiring another trip
//  back into SfzPlayer::process()'s render loop.
//
//  What this deliberately does NOT do:
//   - It does NOT reimplement solo/mute. Solo/mute stays exactly as it is
//     today: fluid_synth_cc(..., CC7, 0) silences a channel at the synthesis
//     level, before any of these buffers are even written to. Reimplementing
//     that as a post-synthesis gain here was explicitly flagged in the plan
//     as not recommended, and isn't done.
//   - It does NOT change the plugin's I/O — output is still one summed
//     stereo pair. That's Scope B-External's job, and it's a separate,
//     much higher-risk change (new output buses, per-format DAW behaviour)
//     that hasn't been requested.
//   - It does NOT add any per-channel DSP. channelInsertProcessors below is
//     an empty extension point, not a feature — every slot defaults to
//     nullptr and is skipped, so behaviour is bit-identical to Scope A's
//     inline summation unless something is actually installed there later.
// =============================================================================

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

/** Sums FluidSynth's 16 per-channel audio-group buffers (32 = 16 × L/R,
 *  produced by SfzPlayer with synth.audio-groups=16) into a single master
 *  stereo pair, with an explicit per-channel processing seam.
 *
 *  Not thread-safe on its own — like the buffers it reads, this is only ever
 *  touched from the audio thread inside SfzPlayer::process().
 */
class Sf2ChannelMixer
{
public:
    static constexpr int kNumChannels = 16;

    /** Optional per-channel insert hook, indexed 0-15 (FluidSynth channel
     *  number). Called with that channel's own L/R buffer immediately
     *  before it's summed into the master pair, so an insert can freely
     *  modify the channel's samples in place. Every slot defaults to an
     *  empty std::function and is skipped — this is a future extension
     *  point, not active behaviour. Not populated anywhere today.
     */
    std::array<std::function<void (float* chL, float* chR, int numSamples)>, kNumChannels>
        channelInsertProcessors {};

    /** Sums groupBuffers[2*ch]/[2*ch+1] for ch in [0, kNumChannels) into
     *  outL/outR (which are zeroed first — this assigns, it does not
     *  accumulate on top of whatever outL/outR already held).
     *
     *  groupBuffers must contain at least 2*kNumChannels vectors, each at
     *  least numSamples long — SfzPlayer guarantees this the same way it
     *  guarantees scratchL/scratchR are long enough (see the growth check
     *  in SfzPlayer::process()).
     */
    void sumToMaster (std::vector<float> groupBuffers[], int numSamples,
                       float* outL, float* outR)
    {
        std::fill (outL, outL + numSamples, 0.0f);
        std::fill (outR, outR + numSamples, 0.0f);

        for (int ch = 0; ch < kNumChannels; ++ch)
        {
            float* chL = groupBuffers[2 * ch].data();
            float* chR = groupBuffers[2 * ch + 1].data();

            if (channelInsertProcessors[(size_t) ch])
                channelInsertProcessors[(size_t) ch] (chL, chR, numSamples);

            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] += chL[i];
                outR[i] += chR[i];
            }
        }
    }
};
