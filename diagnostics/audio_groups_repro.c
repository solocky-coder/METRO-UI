// Minimal, isolated repro for the SF2-PLAYER experimental-multi-group
// crash investigation (see docs/EXPERIMENTAL_MULTI_GROUP_TEST.md and the
// Aug 24 crash logs).
//
// v1 of this test (construct -> destroy, no render call) came back clean
// in CI -- ruled that out as the trigger.
//
// v2 (this version) adds the piece v1 was missing: SfzPlayer.cpp's render
// path calls fluid_synth_process() with nout=32 on EVERY processed block,
// even ones with zero MIDI events (see the unconditional renderSegment()
// call after the per-event loop, SfzPlayer.cpp ~1183) -- so every crashing
// session's silent blocks still ran this call repeatedly, many times,
// before the crash. This test now does the same: construct with
// audio-groups=16, call fluid_synth_process() with 32 output buffers
// (silence in, matching a block with no notes) a few hundred times to
// mirror a short real session, then destroy. Still no soundfont loaded --
// per the crash logs, sfload of the real synth (not the preview synth)
// completing is not required to observe the crash, only the render calls
// that follow it are.
//
// If this crashes, it narrows the cause to the render call itself (most
// likely fluid_synth_process() with nout=32 touching something in
// FluidSynth's internal state that a plain nout=2 call doesn't). If it
// STILL doesn't crash, the remaining candidates are: sfload() specifically
// completing on the main synth, or something concurrent with the preview
// synth's own rendering (SoundFontLoader.cpp's discoverActiveNotesFs, on
// its own thread) -- which would need the two synths running side by side
// to reproduce, not just one in isolation.

#include <fluidsynth.h>
#include <stdio.h>
#include <string.h>

int main (void)
{
    printf ("1. new_fluid_settings()\n"); fflush (stdout);
    fluid_settings_t* settings = new_fluid_settings();

    printf ("2. fluid_settings_setint(audio-groups, 16)\n"); fflush (stdout);
    fluid_settings_setint (settings, "synth.audio-groups", 16);

    printf ("3. new_fluid_synth()\n"); fflush (stdout);
    fluid_synth_t* synth = new_fluid_synth (settings);
    printf ("   synth = %p\n", (void*) synth); fflush (stdout);

    // Mirror SfzPlayer.cpp's buffer setup: 32 buffers (16 groups x L/R),
    // sized for a typical block (matches the 441-sample blocks seen in
    // the real crash logs at a 44.1kHz-ish rate/44.1kHz-derived buffer).
    enum { kBlockSize = 512, kNumGroups = 16, kNumBuffers = kNumGroups * 2 };
    static float groupBuffers[kNumBuffers][kBlockSize];
    float* groupPtrs[kNumBuffers];
    for (int i = 0; i < kNumBuffers; ++i)
        groupPtrs[i] = groupBuffers[i];

    printf ("4. fluid_synth_process() x 500 (nout=32, silence, no notes)\n");
    fflush (stdout);
    for (int block = 0; block < 500; ++block)
    {
        for (int i = 0; i < kNumBuffers; ++i)
            memset (groupBuffers[i], 0, sizeof (float) * kBlockSize);

        int rc = fluid_synth_process (synth, kBlockSize, 0, NULL, kNumBuffers, groupPtrs);
        if (rc != 0)
        {
            printf ("   block %d: fluid_synth_process rc=%d (FLUID_FAILED)\n", block, rc);
            fflush (stdout);
        }
        if (block == 0 || block == 499)
        {
            printf ("   block %d: rc=%d\n", block, rc);
            fflush (stdout);
        }
    }
    printf ("   500 render calls completed.\n"); fflush (stdout);

    printf ("5. delete_fluid_synth()\n"); fflush (stdout);
    delete_fluid_synth (synth);

    printf ("6. delete_fluid_settings()\n"); fflush (stdout);
    delete_fluid_settings (settings);

    printf ("DONE - no crash.\n"); fflush (stdout);
    return 0;
}

