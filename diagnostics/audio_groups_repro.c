// Minimal, isolated repro for the SF2-PLAYER experimental-multi-group crash
// investigation (see docs/EXPERIMENTAL_MULTI_GROUP_TEST.md and the Aug 24
// crash logs). Tests ONLY whether constructing + destroying a FluidSynth
// instance with synth.audio-groups=16 crashes on its own -- no render call,
// no note played, no soundfont loaded, no JUCE/plugin code involved.
//
// This mirrors what the actual crash logs showed: every crashing session's
// sf2_player_debug.log had ZERO MIDI events processed before the crash, so
// the render path and note-on path are already ruled out as the trigger --
// only construction (SfzPlayer.cpp ~1514-1587) and destruction
// (SfzPlayer.cpp ~76-96) of an audio-groups=16 instance ran in every one
// of those sessions. This test isolates exactly that.
//
// No soundfont file is loaded, deliberately -- that step isn't needed to
// reproduce (per the same log evidence) and would require a file that
// doesn't exist on a CI runner. If this test DOES crash, that's about as
// narrow as the repro can get without stepping into FluidSynth's own
// source: it proves the bug needs nothing from this codebase at all.
//
// If this DOESN'T crash, the trigger needs something more than bare
// construct/destroy (e.g. sfload specifically, or something concurrent) --
// which redirects the investigation back into SfzPlayer.cpp/
// SoundFontLoader.cpp instead of pointing at FluidSynth itself.

#include <fluidsynth.h>
#include <stdio.h>

int main (void)
{
    fflush (stdout);   // make sure each line is visible in the CI log even
                        // if this crashes immediately after printing it

    printf ("1. new_fluid_settings()\n"); fflush (stdout);
    fluid_settings_t* settings = new_fluid_settings();

    printf ("2. fluid_settings_setint(audio-groups, 16)\n"); fflush (stdout);
    fluid_settings_setint (settings, "synth.audio-groups", 16);

    printf ("3. new_fluid_synth()\n"); fflush (stdout);
    fluid_synth_t* synth = new_fluid_synth (settings);
    printf ("   synth = %p\n", (void*) synth); fflush (stdout);

    printf ("4. delete_fluid_synth()\n"); fflush (stdout);
    delete_fluid_synth (synth);

    printf ("5. delete_fluid_settings()\n"); fflush (stdout);
    delete_fluid_settings (settings);

    printf ("DONE - no crash.\n"); fflush (stdout);
    return 0;
}
