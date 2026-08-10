#pragma once
// =============================================================================
//  FluidSynthGlobalLock.h
// =============================================================================
//  This app creates FluidSynth instances from two independent places on two
//  independent threads that are never otherwise coordinated:
//
//   - SfzPlayer's live playback synth, (re)built inside applyPendingLoad(),
//     called from SfzPlayer::process() on the AUDIO thread.
//   - SoundFontLoader::LoadJob::runJobFluidSynth()'s throwaway preview synth,
//     built fresh per preset-browse click, on a background juce::ThreadPool
//     worker thread.
//
//  Each fluid_synth_t instance is independent and FluidSynth's per-instance
//  rendering (fluid_synth_process, noteon/noteoff, etc.) on two separate
//  instances from two separate threads is fine. The hazard is specifically
//  around *instance construction/destruction and soundfont loading*
//  (new_fluid_settings / new_fluid_synth / fluid_synth_sfload /
//  delete_fluid_synth / delete_fluid_settings): some FluidSynth builds do
//  lazy, unguarded one-time initialisation of process-global state (DSP
//  interpolation tables, sfloader registration, etc.) the first time it's
//  touched, and/or share global bookkeeping across instances during
//  load/unload. Two threads racing through that isn't guaranteed safe, and
//  observed symptom of exactly this kind of race is a silent heap corruption
//  that only crashes later, at an unrelated allocation/free (matches the
//  observed "crashes right after clean shutdown" pattern).
//
//  Fix: serialize just the lifecycle calls (creation, sfload, destruction)
//  across every fluid_synth_t instance in the process with one shared lock.
//  Steady-state rendering is deliberately NOT covered by this lock — only
//  construction/load/destruction, to keep the critical section as small as
//  possible and avoid adding contention to the actual render path.
// =============================================================================

#if DYSEKT_HAS_FLUIDSYNTH

#include <mutex>

/** Process-wide mutex guarding FluidSynth instance lifecycle calls. Meyers
 *  singleton — safe, lazy, thread-safe initialisation guaranteed by the
 *  compiler (C++11 magic statics), no separate init-order step needed.
 */
inline std::mutex& fluidSynthGlobalLifecycleLock()
{
    static std::mutex m;
    return m;
}

/** RAII helper: place at the top of any block that calls
 *  new_fluid_settings / new_fluid_synth / fluid_synth_sfload /
 *  delete_fluid_synth / delete_fluid_settings, so the whole
 *  create-or-destroy sequence for that instance happens under the lock.
 */
#define FLUIDSYNTH_LIFECYCLE_LOCK() \
    std::lock_guard<std::mutex> fluidSynthLifecycleLockGuard_ (fluidSynthGlobalLifecycleLock())

#endif // DYSEKT_HAS_FLUIDSYNTH
