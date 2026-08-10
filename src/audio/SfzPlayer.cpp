// =============================================================================
//  SfzPlayer.cpp  —  Real-time SF2/SFZ playback engine
//                    SF2 → FluidSynth backend
//                    SFZ → sfizz backend
// =============================================================================
#include "SfzPlayer.h"
#include <cmath>   // std::log2, std::log10 — ADSR unit conversion (see applyFluidAdsrFromUi)
#include "FluidSynthGlobalLock.h"

#if DYSEKT_HAS_SFIZZ
  // Include sfizz C API via path relative to the project root.
  // This avoids relying on target_include_directories propagation.
  #include "../../sfizz/src/sfizz.h"
#endif

// ── TEMPORARY diagnostic logging for the "SF2-PLAYER produces no audio"
// investigation. Self-contained (SfzPlayer has no back-reference to
// DysektProcessor, so it can't use processor.crashLogger) — writes to its
// own file in the same log folder as dysekt_crash.log. Only called on
// note-on (rare), never per-block, so it won't glitch playback. Remove once
// the root cause is confirmed and fixed.
static void sf2DebugLog (const juce::String& msg)
{
    static juce::CriticalSection sf2DebugLogLock;
    juce::ScopedLock sl (sf2DebugLogLock);

    static std::unique_ptr<juce::FileLogger> sf2Logger;
    if (sf2Logger == nullptr)
    {
      #if JUCE_WINDOWS
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("DunSoft/DYSEKT-SF");
      #elif JUCE_MAC
        auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/Logs/DunSoft/DYSEKT-SF");
      #else
        auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile (".config/DunSoft/DYSEKT-SF");
      #endif
        dir.createDirectory();
        sf2Logger = std::make_unique<juce::FileLogger> (
            dir.getChildFile ("sf2_player_debug.log"), "SF2-PLAYER audio debug log");
    }
    sf2Logger->logMessage (msg);
}

// =============================================================================
//  Constructor / destructor
// =============================================================================

SfzPlayer::SfzPlayer()
{
    // Initialise all channel slots to "no pending change"
    for (auto& slot : pendingChannelAssignment)
        slot.store (-1, std::memory_order_relaxed);

#if DYSEKT_HAS_FLUIDSYNTH
    // Prevent FluidSynth from spawning its own audio driver thread.
    // We only use it as an offline render engine inside processBlock.
    const char* drv[] { nullptr };
    fluid_audio_driver_register (drv);
#endif
}

SfzPlayer::~SfzPlayer()
{
    // Drain any pending load so we don't leak.
    delete pendingLoad.exchange (nullptr, std::memory_order_acq_rel);

    // Drain any pending preset list.
    delete freshPresets.exchange (nullptr, std::memory_order_acq_rel);

#if DYSEKT_HAS_FLUIDSYNTH
    if (synth != nullptr || settings != nullptr)
    {
        FLUIDSYNTH_LIFECYCLE_LOCK();
        if (synth    != nullptr) { delete_fluid_synth    (synth);    synth    = nullptr; }
        if (settings != nullptr) { delete_fluid_settings (settings); settings = nullptr; }
    }
#endif

#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth != nullptr) { sfizz_free (sfizzSynth); sfizzSynth = nullptr; }
#endif
}

// =============================================================================
//  UI-thread API
// =============================================================================

void SfzPlayer::loadFile (const juce::File& f, juce::ThreadPool& pool)
{
    juce::ignoreUnused (pool);
    pendingFilePath  = f.getFullPathName();   // record immediately for UI queries
    auto* pkg        = new PendingLoad();
    pkg->file        = f;
    pkg->shouldUnload = false;
    delete pendingLoad.exchange (pkg, std::memory_order_acq_rel);
}

void SfzPlayer::unload()
{
    auto* pkg        = new PendingLoad();
    pkg->shouldUnload = true;
    delete pendingLoad.exchange (pkg, std::memory_order_acq_rel);
}

void SfzPlayer::setVolume      (float g) { volume.store (g, std::memory_order_relaxed); }
void SfzPlayer::setTranspose   (int s)   { transpose.store (s, std::memory_order_relaxed); }
void SfzPlayer::setPitchShift  (float s) { pitchShift.store (juce::jlimit (-24.0f, 24.0f, s),
                                                               std::memory_order_relaxed); }
void SfzPlayer::setMidiChannel (int c)   { midiChannel.store (c, std::memory_order_relaxed); }

// ── SFZ ADSR setters ──────────────────────────────────────────────────────────

void SfzPlayer::setSfzAttack  (float s) noexcept
{
    sfzAttackSec .store (juce::jlimit (0.0f,  30.0f, s), std::memory_order_relaxed);
    sfzAdsrDirty .store (true, std::memory_order_release);
}
void SfzPlayer::setSfzDecay   (float s) noexcept
{
    sfzDecaySec  .store (juce::jlimit (0.0f,  30.0f, s), std::memory_order_relaxed);
    sfzAdsrDirty .store (true, std::memory_order_release);
}
void SfzPlayer::setSfzSustain (float p) noexcept
{
    sfzSustainPct.store (juce::jlimit (0.0f, 100.0f, p), std::memory_order_relaxed);
    sfzAdsrDirty .store (true, std::memory_order_release);
}
void SfzPlayer::setSfzRelease (float s) noexcept
{
    sfzReleaseSec.store (juce::jlimit (0.0f,  60.0f, s), std::memory_order_relaxed);
    sfzAdsrDirty .store (true, std::memory_order_release);
}

// ── JUCE ADSR setter (Option B — post-render envelope) ───────────────────────

void SfzPlayer::setJuceAdsr (float attackSec, float decaySec,
                              float sustainLvl, float releaseSec) noexcept
{
    juceAdsrAttack .store (juce::jlimit (0.0f, 30.0f,  attackSec),  std::memory_order_relaxed);
    juceAdsrDecay  .store (juce::jlimit (0.0f, 30.0f,  decaySec),   std::memory_order_relaxed);
    juceAdsrSustain.store (juce::jlimit (0.0f,  1.0f,  sustainLvl), std::memory_order_relaxed);
    juceAdsrRelease.store (juce::jlimit (0.0f, 60.0f,  releaseSec), std::memory_order_relaxed);
    juceAdsrParamsDirty.store (true, std::memory_order_release);
}

// ── Per-zone vol/pan (SFZ only — sfizz OSC) ──────────────────────────────────

void SfzPlayer::setZoneVolume (int regionIndex, float volDb) noexcept
{
#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth == nullptr || !isSfzFile) return;
    const int numRegions = sfizz_get_num_regions (sfizzSynth);
    if (regionIndex < 0 || regionIndex >= numRegions) return;

    char path[64];
    snprintf (path, sizeof (path), "/region%d/volume", regionIndex);
    sfizz_arg_t arg;
    arg.f = juce::jlimit (-144.0f, 6.0f, volDb);
    sfizz_send_message (sfizzSynth, nullptr, 0, path, "f", &arg);
#else
    juce::ignoreUnused (regionIndex, volDb);
#endif
}

void SfzPlayer::setZonePan (int regionIndex, float pan) noexcept
{
#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth == nullptr || !isSfzFile) return;
    const int numRegions = sfizz_get_num_regions (sfizzSynth);
    if (regionIndex < 0 || regionIndex >= numRegions) return;

    // SFZ pan opcode is in percent: -100 (L) .. 0 (C) .. +100 (R)
    char path[64];
    snprintf (path, sizeof (path), "/region%d/pan", regionIndex);
    sfizz_arg_t arg;
    arg.f = juce::jlimit (-100.0f, 100.0f, pan * 100.0f);
    sfizz_send_message (sfizzSynth, nullptr, 0, path, "f", &arg);
#else
    juce::ignoreUnused (regionIndex, pan);
#endif
}

void SfzPlayer::setZoneTune (int regionIndex, float cents) noexcept
{
#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth == nullptr || !isSfzFile) return;
    const int numRegions = sfizz_get_num_regions (sfizzSynth);
    if (regionIndex < 0 || regionIndex >= numRegions) return;

    char path[64];
    snprintf (path, sizeof (path), "/region%d/tune", regionIndex);
    sfizz_arg_t arg;
    arg.f = juce::jlimit (-100.0f, 100.0f, cents);
    sfizz_send_message (sfizzSynth, nullptr, 0, path, "f", &arg);
#else
    juce::ignoreUnused (regionIndex, cents);
#endif
}

void SfzPlayer::sendAdsrToSfizz()
{
#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth == nullptr) return;
    const int numRegions = sfizz_get_num_regions (sfizzSynth);
    if (numRegions <= 0) return;

    const float attack  = sfzAttackSec .load (std::memory_order_relaxed);
    const float decay   = sfzDecaySec  .load (std::memory_order_relaxed);
    const float sustain = sfzSustainPct.load (std::memory_order_relaxed);
    const float release = sfzReleaseSec.load (std::memory_order_relaxed);

    auto sendFloat = [&] (int region, const char* opcode, float value)
    {
        char path[64];
        snprintf (path, sizeof (path), "/region%d/%s", region, opcode);
        sfizz_arg_t arg;
        arg.f = value;
        sfizz_send_message (sfizzSynth, nullptr, 0, path, "f", &arg);
    };

    for (int i = 0; i < numRegions; ++i)
    {
        sendFloat (i, "ampeg_attack",  attack);
        sendFloat (i, "ampeg_decay",   decay);
        sendFloat (i, "ampeg_sustain", sustain);
        sendFloat (i, "ampeg_release", release);
    }
#endif
}

void SfzPlayer::setPan (float p)
{
    pan.store (juce::jlimit (-1.0f, 1.0f, p), std::memory_order_relaxed);
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth != nullptr && !isSfzFile)
    {
        // CC10 pan: 0 = hard L, 64 = centre, 127 = hard R
        const int cc10 = juce::jlimit (0, 127,
            juce::roundToInt ((p + 1.0f) * 0.5f * 127.0f));
        // Channels 0/1 (MIDI ch 1/2) are reserved for the Slicer/SFZ-Player —
        // never write CC data there, even though no note ever routes to them.
        for (int ch = 2; ch < 16; ++ch)
            fluid_synth_cc (synth, ch, 10, cc10);
    }
#endif
#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth != nullptr && isSfzFile)
    {
        const int cc10 = juce::jlimit (0, 127,
            juce::roundToInt ((p + 1.0f) * 0.5f * 127.0f));
        sfizz_send_cc (sfizzSynth, 0, 10, cc10);
    }
#endif
}

void SfzPlayer::setFineTune (float cents)
{
    fineTune.store (juce::jlimit (-100.0f, 100.0f, cents), std::memory_order_relaxed);
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth != nullptr && !isSfzFile)
        // Channels 0/1 (MIDI ch 1/2) are reserved for the Slicer/SFZ-Player —
        // never write generator data there, even though no note ever routes to them.
        for (int ch = 2; ch < 16; ++ch)
            fluid_synth_set_gen (synth, ch, GEN_FINETUNE, cents);
#endif
    // sfizz fine-tune is applied via pitch-bend offset on next note — no direct API
}

void SfzPlayer::setReverb (float level)
{
    reverb.store (juce::jlimit (0.0f, 1.0f, level), std::memory_order_relaxed);
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth != nullptr && !isSfzFile)
        fluid_synth_set_reverb (synth,
            0.6,           // room size  (0.0–1.0)
            0.5,           // damping    (0.0–1.0)
            0.5,           // width      (0.0–100.0)
            (double) level); // level    (0.0–1.0)
#endif
    // sfizz: no reverb API — SFZ files define reverb via opcodes internally
}

void SfzPlayer::setChorus (float level)
{
    chorus.store (juce::jlimit (0.0f, 1.0f, level), std::memory_order_relaxed);
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth != nullptr && !isSfzFile)
        fluid_synth_set_chorus (synth,
            3,                        // voice count (1–99)
            (double) level * 10.0,    // level       (0.0–10.0)
            0.3,                      // speed Hz    (0.29–5.0)
            8.0,                      // depth ms    (0.0–21.0)
            FLUID_CHORUS_MOD_SINE);
#endif
    // sfizz: no chorus API
}

// ── Post-processing Reverb EFX setters ───────────────────────────────────────

void SfzPlayer::setReverbSize (float pct) noexcept
{
    reverbSize.store (juce::jlimit (0.0f, 100.0f, pct), std::memory_order_relaxed);
}

void SfzPlayer::setReverbDamp (float pct) noexcept
{
    reverbDamp.store (juce::jlimit (0.0f, 100.0f, pct), std::memory_order_relaxed);
}

void SfzPlayer::setReverbWidth (float pct) noexcept
{
    reverbWidth.store (juce::jlimit (0.0f, 100.0f, pct), std::memory_order_relaxed);
}

void SfzPlayer::setReverbMix (float pct) noexcept
{
    reverbMix.store (juce::jlimit (0.0f, 100.0f, pct), std::memory_order_relaxed);
}

void SfzPlayer::setReverbFreeze (bool on) noexcept
{
    reverbFreeze.store (on, std::memory_order_relaxed);
}

// ── SF2 filter setters ───────────────────────────────────────────────────────

void SfzPlayer::setSf2FilterCutoff (float hz) noexcept
{
    sf2FilterCutoffHz.store (juce::jlimit (kSf2FilterCutoffMinHz, kSf2FilterCutoffMaxHz, hz),
                              std::memory_order_relaxed);
}

void SfzPlayer::setSf2FilterResonance (float pct) noexcept
{
    sf2FilterResonancePct.store (juce::jlimit (kSf2FilterResonanceMinPct, kSf2FilterResonanceMaxPct, pct),
                                  std::memory_order_relaxed);
}

void SfzPlayer::updateReverbParams()
{
    juce::dsp::Reverb::Parameters p;
    p.roomSize   = reverbSize  .load (std::memory_order_relaxed) * 0.01f;  // 0–1
    p.damping    = reverbDamp  .load (std::memory_order_relaxed) * 0.01f;  // 0–1
    p.width      = reverbWidth .load (std::memory_order_relaxed) * 0.01f;  // 0–1
    p.wetLevel   = reverbMix   .load (std::memory_order_relaxed) * 0.01f;  // 0–1
    p.dryLevel   = 1.0f - p.wetLevel;
    p.freezeMode = reverbFreeze.load (std::memory_order_relaxed) ? 1.0f : 0.0f;
    dspReverb.setParameters (p);
}

void SfzPlayer::setPresetByIndex (int idx)
{
    presetIndex.store (idx, std::memory_order_relaxed);

    // Cache the bank/program for applyProgramChange so it doesn't need to
    // re-enumerate the sfont iterator (whose order may not match postPresetList).
    {
        // Use the UI-side cached list — safe to read here since setPresetByIndex
        // is always called from the UI thread and cachedPresets is UI-thread-only.
        if (idx >= 0 && idx < (int) cachedPresets.size())
        {
            pendingBank   .store (cachedPresets[(size_t) idx].bank,   std::memory_order_relaxed);
            pendingProgram.store (cachedPresets[(size_t) idx].preset, std::memory_order_relaxed);
        }
    }

    programChangePending.store (true, std::memory_order_release);
}

void SfzPlayer::setPresetOnChannel (int channel, int bank, int preset)
{
    // Channels 0/1 (MIDI ch 1/2) are permanently reserved for the
    // Slicer/SFZ-Player. Every current caller already clamps to [2, 15],
    // but that was caller discipline only — nothing here actually stopped
    // a future call site from loading a preset onto a reserved channel.
    // Guard it here too, same as previewPreset()'s isSfzFile check.
    if (channel < 2 || channel > 15) return;
    // Pack bank + preset into a single int. Bank fits in upper 16 bits (max 16383),
    // preset in lower 16 bits (max 127 for GM, up to 16383 for extended SF2).
    const int packed = (juce::jlimit (0, 0x7FFF, bank) << 16)
                     |  juce::jlimit (0, 0xFFFF, preset);
    pendingChannelAssignment[channel].store (packed, std::memory_order_relaxed);
    anyChannelDirty.store (true, std::memory_order_release);

    // Keep liveInputChannelMask in sync so live controller MIDI (arriving on
    // ch 1) is fanned out to every FluidSynth channel that has a preset loaded.
    // Without this, assigning a preset via the grid would load it into
    // FluidSynth but incoming MIDI would never be routed there — resulting in
    // "MIDI indicator blinks but no sound."
    const uint16_t bit = uint16_t(1) << channel;
    const uint16_t mask = liveInputChannelMask.load (std::memory_order_relaxed);
    liveInputChannelMask.store (mask | bit, std::memory_order_relaxed);
}

// =============================================================================
//  Per-channel mixer strip — UI-thread setters + audio-thread applicator
// =============================================================================

// Channels 0/1 (MIDI ch 1/2) are permanently reserved for the Slicer/SFZ-Player
// throughout this file — every accessor below rejects them, not just callers.

SfzPlayer::ChannelStrip SfzPlayer::getChannelStrip (int ch) const noexcept
{
    if (ch < 2 || ch >= 16) return {};
    const auto& s = channelStrips[ch];
    ChannelStrip out;
    out.volume     = s.volume    .load (std::memory_order_relaxed);
    out.pan        = s.pan       .load (std::memory_order_relaxed);
    out.reverbSend = s.reverbSend.load (std::memory_order_relaxed);
    out.preMuteVol = s.preMuteVol.load (std::memory_order_relaxed);
    out.muted      = s.muted     .load (std::memory_order_relaxed);
    return out;
}

void SfzPlayer::setChannelVolume (int ch, float v) noexcept
{
    if (ch < 2 || ch >= 16) return;
    channelStrips[ch].volume .store (juce::jlimit (0.f, 1.f, v), std::memory_order_relaxed);
    channelStrips[ch].dirty  .store (true,                        std::memory_order_relaxed);
    anyStripDirty            .store (true,                        std::memory_order_release);
}

void SfzPlayer::setChannelPan (int ch, float p) noexcept
{
    if (ch < 2 || ch >= 16) return;
    channelStrips[ch].pan   .store (juce::jlimit (-1.f, 1.f, p), std::memory_order_relaxed);
    channelStrips[ch].dirty .store (true,                         std::memory_order_relaxed);
    anyStripDirty           .store (true,                         std::memory_order_release);
}

void SfzPlayer::setChannelReverbSend (int ch, float s) noexcept
{
    if (ch < 2 || ch >= 16) return;
    channelStrips[ch].reverbSend.store (juce::jlimit (0.f, 1.f, s), std::memory_order_relaxed);
    channelStrips[ch].dirty     .store (true,                        std::memory_order_relaxed);
    anyStripDirty               .store (true,                        std::memory_order_release);
}

void SfzPlayer::setChannelMuted (int ch, bool mute) noexcept
{
    if (ch < 2 || ch >= 16) return;
    auto& s = channelStrips[ch];
    if (mute && ! s.muted.load (std::memory_order_relaxed))
        s.preMuteVol.store (s.volume.load (std::memory_order_relaxed), std::memory_order_relaxed);
    s.muted.store (mute, std::memory_order_relaxed);
    s.dirty.store (true, std::memory_order_relaxed);
    anyStripDirty.store (true, std::memory_order_release);
}

void SfzPlayer::soloChannel (int ch) noexcept
{
    if (ch < 2 || ch >= 16) return;
    // i starts at 2 — channels 0/1 (MIDI ch 1/2) are reserved for the
    // Slicer/SFZ-Player and must never be muted/unmuted by SF2 solo logic.
    for (int i = 2; i < 16; ++i)
    {
        const bool shouldMute = (i != ch);
        auto& s = channelStrips[i];
        if (shouldMute && ! s.muted.load (std::memory_order_relaxed))
            s.preMuteVol.store (s.volume.load (std::memory_order_relaxed), std::memory_order_relaxed);
        s.muted.store (shouldMute, std::memory_order_relaxed);
        s.dirty.store (true,       std::memory_order_relaxed);
    }
    anyStripDirty.store (true, std::memory_order_release);
}

void SfzPlayer::clearSolo() noexcept
{
    for (int i = 2; i < 16; ++i)
    {
        auto& s = channelStrips[i];
        if (s.muted.load (std::memory_order_relaxed))
        {
            // Restore pre-mute volume then unmute
            s.volume.store (s.preMuteVol.load (std::memory_order_relaxed), std::memory_order_relaxed);
            s.muted.store (false, std::memory_order_relaxed);
            s.dirty.store (true,  std::memory_order_relaxed);
        }
    }
    anyStripDirty.store (true, std::memory_order_release);
}

void SfzPlayer::applyDirtyStrips()
{
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth == nullptr) return;
    if (! anyStripDirty.load (std::memory_order_acquire)) return;
    anyStripDirty.store (false, std::memory_order_relaxed);

    // Channels 0/1 (MIDI ch 1/2) are reserved for the Slicer/SFZ-Player.
    // clearChannelPresets() marks every strip 0-15 dirty as part of its
    // reset sweep, so this loop must exclude them explicitly rather than
    // relying on the dirty flag alone.
    for (int ch = 2; ch < 16; ++ch)
    {
        auto& s = channelStrips[ch];
        if (! s.dirty.load (std::memory_order_relaxed)) continue;
        s.dirty.store (false, std::memory_order_relaxed);

        const bool  muted = s.muted      .load (std::memory_order_relaxed);
        const float vol   = muted ? 0.f : s.volume.load (std::memory_order_relaxed);
        const float pan   = s.pan        .load (std::memory_order_relaxed);
        const float rev   = s.reverbSend .load (std::memory_order_relaxed);

        // CC7 volume  0..127
        fluid_synth_cc (synth, ch, 7,  juce::roundToInt (vol * 127.f));
        // CC10 pan    0 = L, 64 = C, 127 = R
        fluid_synth_cc (synth, ch, 10, juce::roundToInt ((pan + 1.0f) * 63.5f));
        // CC91 reverb send 0..127
        fluid_synth_cc (synth, ch, 91, juce::roundToInt (rev * 127.f));
    }
#endif
}

void SfzPlayer::clearChannelPresets()
{
    for (auto& slot : pendingChannelAssignment)
        slot.store (-1, std::memory_order_relaxed);
    anyChannelDirty.store (false, std::memory_order_relaxed);
    liveInputChannelMask.store (0, std::memory_order_relaxed);

    // Reset per-channel mixer strips to defaults
    for (auto& s : channelStrips)
    {
        s.volume    .store (1.0f,  std::memory_order_relaxed);
        s.pan       .store (0.0f,  std::memory_order_relaxed);
        s.reverbSend.store (0.0f,  std::memory_order_relaxed);
        s.preMuteVol.store (1.0f,  std::memory_order_relaxed);
        s.muted     .store (false, std::memory_order_relaxed);
        s.dirty     .store (true,  std::memory_order_relaxed);  // push defaults to FluidSynth
    }
    anyStripDirty.store (true, std::memory_order_release);
}

// =============================================================================
//  Preview (audition) — channel 15 scratch slot, no MIDI-channel ownership
// =============================================================================
static constexpr int kPreviewChannel = 15;   // 0-based FluidSynth channel (MIDI ch 16)

void SfzPlayer::previewPreset (int bank, int preset)
{
    // Channel 15 is only a free scratch slot when an SF2 file is loaded.
    // SFZ playback owns channel 15 (sfizz); calling this while an SFZ is
    // active would corrupt that channel.  The UI gates this already, but
    // guard here too so a future refactor can't silently break it.
    jassert (! isSfzFile);
    if (isSfzFile) return;

    // Load onto the preview channel only.  Do NOT touch channel 0 — it may
    // hold a real user-assigned preset in multi-timbral mode, and stomping
    // it here was the root cause of presets "going silent" after previewing.
    setPresetOnChannel (kPreviewChannel, bank, preset);

    // Arm a fixed Middle-C (60) demo-note request. PluginProcessor injects
    // this as a real MIDI note-on on getPreviewMidiChannel() next block and
    // follows up with the matching note-off shortly after — no channel
    // ownership, chromatic-collision guard, Arranger selection, or keyboard
    // channel involved at any point in this path.
    previewNoteNumber.store (60, std::memory_order_relaxed);
    previewNoteOnPending.store (true, std::memory_order_release);
}

void SfzPlayer::clearPreview()
{
    // Reset the preview channel only — leave ch0 untouched so any
    // user-assigned preset on channel 1 (FluidSynth ch 0) is preserved.
    setPresetOnChannel (kPreviewChannel, 0, 0);

    // Cancel any note-on that hasn't been injected yet, and request a
    // note-off for the last-armed note in case it's still sounding.
    previewNoteOnPending.store (false, std::memory_order_relaxed);
    previewNoteOffNumber.store (previewNoteNumber.load (std::memory_order_relaxed),
                                 std::memory_order_relaxed);
    previewNoteOffPending.store (true, std::memory_order_release);
}

juce::File SfzPlayer::getLoadedFile() const
{
    // activeFile is only written on the audio thread; a torn read is harmless
    // here because we only use it for display.
    return activeFile;
}

std::vector<Sf2PresetInfo> SfzPlayer::getPresetList() const
{
    // Swap in any freshly-posted list from the audio thread.
    if (auto* p = freshPresets.exchange (nullptr, std::memory_order_acq_rel))
    {
        cachedPresets = std::move (*p);
        delete p;
    }
    return cachedPresets;
}

// =============================================================================
//  Audio-thread API
// =============================================================================

void SfzPlayer::prepare (double sampleRate, int maxBlockSize)
{
    currentSR    = sampleRate;
    currentBlock = maxBlockSize;

    scratchL.assign ((size_t) maxBlockSize, 0.0f);
    scratchR.assign ((size_t) maxBlockSize, 0.0f);

    for (auto& buf : groupBuffers)
        buf.assign ((size_t) maxBlockSize, 0.0f);

    // ── Prepare post-processing reverb ────────────────────────────────────────
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (maxBlockSize);
        spec.numChannels      = 2;
        dspReverb.prepare (spec);
        updateReverbParams();
    }

#if DYSEKT_HAS_FLUIDSYNTH
    // Keep the existing synth in sync with the new sample rate.
    if (synth != nullptr)
        fluid_synth_set_sample_rate (synth, (float) sampleRate);
#endif

#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth != nullptr)
    {
        sfizz_set_sample_rate       (sfizzSynth, (float) sampleRate);
        sfizz_set_samples_per_block (sfizzSynth, maxBlockSize);
    }
#endif

    // Prepare JUCE ADSR with current sample rate.
    juceAdsr.setSampleRate (sampleRate);
    juceAdsrParams = { juceAdsrAttack .load (std::memory_order_relaxed),
                       juceAdsrDecay  .load (std::memory_order_relaxed),
                       juceAdsrSustain.load (std::memory_order_relaxed),
                       juceAdsrRelease.load (std::memory_order_relaxed) };
    juceAdsr.setParameters (juceAdsrParams);
}

void SfzPlayer::process (const juce::MidiBuffer& midiIn,
                          float* outL, float* outR, int numSamples)
{
    // TEMP diagnostic — unconditional, no gating whatsoever. If this line
    // never appears in sf2_player_debug.log, process() itself is not being
    // invoked by the binary being tested (stale build / plugin caching /
    // wrong instance), full stop — nothing past this point matters yet.
    {
        static bool loggedEntryOnce = false;
        if (! loggedEntryOnce)
        {
            loggedEntryOnce = true;
            sf2DebugLog ("process() ENTRY — first call reached. numSamples=" + juce::String (numSamples)
                + " midiIn.getNumEvents()=" + juce::String (midiIn.getNumEvents()));
        }
    }

    // TEMP diagnostic — NOT gated to "once": the block above only ever
    // catches the very first process() call of the session (typically
    // silent, before any note is played), so it can't show us what happens
    // on an actual note-on. This logs every block that actually carries
    // MIDI, for as long as the session runs.
    if (! midiIn.isEmpty())
        sf2DebugLog ("process(): non-empty block — " + juce::String (midiIn.getNumEvents())
            + " event(s), loaded=" + juce::String ((int) loaded.load (std::memory_order_relaxed))
            + " isSfzFile=" + juce::String ((int) isSfzFile)
            + " sfizzSynth=" + juce::String (sfizzSynth != nullptr ? "non-null" : "NULL")
            + " midiChannel(filterCh)=" + juce::String (midiChannel.load (std::memory_order_relaxed)));

    applyPendingLoad();

    if (! loaded.load (std::memory_order_relaxed))
    {
        if (! midiIn.isEmpty())
            sf2DebugLog ("process(): loaded==false — bailing out at the very top with "
                + juce::String (midiIn.getNumEvents()) + " MIDI event(s) discarded this block.");
        return;
    }

    const int filterCh = midiChannel.load (std::memory_order_relaxed);
    const int trans    = transpose.load   (std::memory_order_relaxed);
    const float vol    = volume.load      (std::memory_order_relaxed);

    // Ensure scratch buffers are large enough
    if ((int) scratchL.size() < numSamples)
    {
        scratchL.assign ((size_t) numSamples, 0.0f);
        scratchR.assign ((size_t) numSamples, 0.0f);
    }

    // Same growth guarantee for the 32 per-channel group buffers — numSamples
    // can exceed the maxBlockSize given to prepare() (host buffer-size change
    // without a fresh prepareToPlay, etc.), and fluid_synth_process() will
    // write numSamples frames into whatever pointers it's given regardless.
    for (auto& buf : groupBuffers)
        if ((int) buf.size() < numSamples)
            buf.assign ((size_t) numSamples, 0.0f);

#if DYSEKT_HAS_SFIZZ
    if (isSfzFile && sfizzSynth != nullptr)
    {
        // ── Flush pending ADSR changes via sfizz OSC messages ────────────────
        if (sfzAdsrDirty.exchange (false, std::memory_order_acquire))
            sendAdsrToSfizz();

        // ── Forward MIDI to sfizz ─────────────────────────────────────────────
        for (const auto meta : midiIn)
        {
            const auto msg = meta.getMessage();
            const int  ch  = msg.getChannel();   // 1-16

            if (msg.isNoteOn() || msg.isNoteOff())
                sf2DebugLog (juce::String ("  [sfizz] msg: ch=") + juce::String (ch)
                    + " filterCh=" + juce::String (filterCh)
                    + " note=" + juce::String (msg.getNoteNumber())
                    + (msg.isNoteOn() ? " NOTE-ON" : " NOTE-OFF")
                    + ((filterCh != 0 && ch != filterCh) ? "  -> DROPPED (channel mismatch)" : "  -> forwarded to sfizz"));

            if (filterCh != 0 && ch != filterCh)
                continue;

            if (msg.isNoteOn())
            {
                // SFZ: no MIDI transpose — key zones are fixed, pitch is shifted audio-rate
                sfizz_send_note_on (sfizzSynth, meta.samplePosition,
                                    msg.getNoteNumber(), msg.getVelocity());
                // Drive the JUCE ADSR so it doesn't stay idle and multiply output by 0.
                // (juceAdsrNoteOn() is only called for UI-keyboard injections; external
                //  MIDI must trigger it here on the audio thread directly.)
                juceAdsr.noteOn();
                previewPositionSample.store (0, std::memory_order_relaxed);
                lastTriggeredNote.store (msg.getNoteNumber(), std::memory_order_relaxed);
            }
            else if (msg.isNoteOff())
            {
                sfizz_send_note_off (sfizzSynth, meta.samplePosition,
                                     msg.getNoteNumber(), msg.getVelocity());
                juceAdsr.noteOff();
            }
            else if (msg.isController())
            {
                sfizz_send_cc (sfizzSynth, meta.samplePosition,
                               msg.getControllerNumber(),
                               msg.getControllerValue());
            }
            else if (msg.isPitchWheel())
            {
                // sfizz expects -8192..+8191; JUCE provides 0..16383
                sfizz_send_pitch_wheel (sfizzSynth, meta.samplePosition,
                                        msg.getPitchWheelValue() - 8192);
            }
            else if (msg.isChannelPressure())
            {
                sfizz_send_channel_aftertouch (sfizzSynth, meta.samplePosition,
                                               msg.getChannelPressureValue());
            }
            else if (msg.isAftertouch())
            {
                sfizz_send_poly_aftertouch (sfizzSynth, meta.samplePosition,
                                            msg.getNoteNumber(),
                                            msg.getAfterTouchValue());
            }
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            {
                sfizz_all_sound_off (sfizzSynth);
            }
        }

        // ── Render sfizz with optional audio-rate pitch shift ─────────────────
        // If pitch shift is active, sfizz renders into pitchBufL/R at a larger or
        // smaller block size, then pitchShiftBlock() resamples into scratchL/R at
        // numSamples via linear interpolation.  At 0 semitones we skip the extra
        // buffer entirely.
        //
        // Ratio = 2^(semi/12).  Pitch up (ratio>1) → sfizz renders MORE frames
        // which we compress into numSamples.  Pitch down → fewer frames stretched.
        const float pitchSemi  = pitchShift.load (std::memory_order_relaxed);
        const float pitchRatio = std::pow (2.0f, pitchSemi / 12.0f);
        const int   srcLen     = (pitchSemi == 0.0f)
                                 ? numSamples
                                 : juce::jmax (1, (int) std::roundf ((float) numSamples * pitchRatio));

        if (pitchSemi == 0.0f)
        {
            // Fast path — no pitch shift, render straight into scratch buffers
            std::fill (scratchL.begin(), scratchL.begin() + numSamples, 0.0f);
            std::fill (scratchR.begin(), scratchR.begin() + numSamples, 0.0f);

            float* sfzPlanes[2] = { scratchL.data(), scratchR.data() };
            sfizz_render_block (sfizzSynth, sfzPlanes, 2, numSamples);
        }
        else
        {
            // Grow pitch buffers if needed
            if ((int) pitchBufL.size() < srcLen)
            {
                pitchBufL.assign ((size_t) srcLen, 0.0f);
                pitchBufR.assign ((size_t) srcLen, 0.0f);
            }
            std::fill (pitchBufL.begin(), pitchBufL.begin() + srcLen, 0.0f);
            std::fill (pitchBufR.begin(), pitchBufR.begin() + srcLen, 0.0f);

            float* srcPlanes[2] = { pitchBufL.data(), pitchBufR.data() };
            sfizz_render_block (sfizzSynth, srcPlanes, 2, srcLen);

            std::fill (scratchL.begin(), scratchL.begin() + numSamples, 0.0f);
            std::fill (scratchR.begin(), scratchR.begin() + numSamples, 0.0f);

            pitchShiftBlock (pitchBufL.data(), scratchL.data(), srcLen, numSamples);
            pitchShiftBlock (pitchBufR.data(), scratchR.data(), srcLen, numSamples);
        }

        // Apply volume
        for (int i = 0; i < numSamples; ++i)
        {
            scratchL[(size_t) i] *= vol;
            scratchR[(size_t) i] *= vol;
        }

        // Apply post-processing reverb
        updateReverbParams();
        {
            float* rvPlanes[2] = { scratchL.data(), scratchR.data() };
            juce::dsp::AudioBlock<float> block (rvPlanes, 2, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            dspReverb.process (ctx);
        }

        // ── Apply JUCE ADSR (Option B) ────────────────────────────────────────
        // Flush param changes first, then handle note-on/off atomics.
        if (juceAdsrParamsDirty.exchange (false, std::memory_order_acquire))
        {
            juceAdsrParams = { juceAdsrAttack .load (std::memory_order_relaxed),
                               juceAdsrDecay  .load (std::memory_order_relaxed),
                               juceAdsrSustain.load (std::memory_order_relaxed),
                               juceAdsrRelease.load (std::memory_order_relaxed) };
            juceAdsr.setParameters (juceAdsrParams);
        }
        if (juceAdsrNoteOnPending .exchange (false, std::memory_order_acquire))
        {
            juceAdsr.noteOn();
            previewPositionSample.store (0, std::memory_order_relaxed);
            const int n = pendingTriggeredNote.exchange (-1, std::memory_order_relaxed);
            if (n >= 0)
                lastTriggeredNote.store (n, std::memory_order_relaxed);
        }
        if (juceAdsrNoteOffPending.exchange (false, std::memory_order_acquire))
            juceAdsr.noteOff();

        {
            // Apply envelope to L and R using per-sample loop
            for (int i = 0; i < numSamples; ++i)
            {
                const float env = juceAdsr.getNextSample();
                scratchL[(size_t) i] *= env;
                scratchR[(size_t) i] *= env;
            }
        }
        juceAdsrActive.store (juceAdsr.isActive(), std::memory_order_relaxed);
        if (juceAdsr.isActive())
            previewPositionSample.fetch_add (numSamples, std::memory_order_relaxed);
        else
            previewPositionSample.store (0, std::memory_order_relaxed);

        // Mix into output
        for (int i = 0; i < numSamples; ++i)
        {
            outL[i] += scratchL[(size_t) i];
            outR[i] += scratchR[(size_t) i];
        }
        return;
    }
#endif

#if DYSEKT_HAS_FLUIDSYNTH
    if (synth == nullptr)
    {
        static bool loggedNullSynthOnce = false;
        if (! loggedNullSynthOnce)
        {
            loggedNullSynthOnce = true;
            sf2DebugLog ("process(): synth == nullptr — bailing out before any FluidSynth "
                         "call. isSfzFile=" + juce::String ((int) isSfzFile)
                         + " loaded=" + juce::String ((int) loaded.load (std::memory_order_relaxed)));
        }
        return;
    }

    // Apply any pending preset assignments — multi-timbral first, then single-preset legacy.
    applyDirtyStrips();

    if (anyChannelDirty.load (std::memory_order_acquire))
        applyPendingChannelChanges();

    if (programChangePending.load (std::memory_order_acquire))
        applyProgramChange();

    // Unconditional per-block reapply. fluid_synth_set_gen() is a plain array
    // write with no allocation, so this is cheap enough to call every block
    // rather than tracking a dirty flag — which matters because
    // fluid_synth_program_select()/_program_change() (just above, and also
    // possible mid-block below on an incoming program-change message) both
    // reset a channel's generators back to the SoundFont's own defaults,
    // silently reverting the UI-driven envelope if not reapplied.
    juceAdsrParamsDirty.exchange (false, std::memory_order_acquire);   // clear; consumed here instead of the old post-mix block
    applyFluidAdsrFromUi();
    applyFluidFilterFromUi();

    // ── Forward MIDI to FluidSynth ────────────────────────────────────────────
    // Multi-timbral mode: route each message to its own FluidSynth channel (0-based).
    // Channels 1 and 2 are reserved (Slicer / SFZ-Player respectively) and are
    // filtered out below before any note/CC/etc. processing occurs. Every other
    // channel (3-16) passes through so each sequencer track fires on the channel
    // its preset was assigned to via setPresetOnChannel().
    //
    // NOTE: the previous "ch-1 fan-out" behaviour (remapping ch-1 messages to
    // every channel in liveInputChannelMask) has been removed — it directly
    // contradicted the channel-1 reservation. liveMask is retained only for
    // debug logging below.

    const uint16_t liveMask = liveInputChannelMask.load (std::memory_order_relaxed);
    bool sf2DebugHadNoteOnThisBlock = false;   // TEMP diagnostic — see note below

    if (! midiIn.isEmpty())
        sf2DebugLog ("process(): FluidSynth branch reached with " + juce::String (midiIn.getNumEvents())
            + " MIDI event(s) this block. loaded=" + juce::String ((int) loaded.load (std::memory_order_relaxed))
            + " sfontId=" + juce::String (sfontId)
            + " liveMask=0x" + juce::String::toHexString ((int) liveMask));

    // ── Segmented rendering (Phase 2: sample-accurate MIDI timing) ───────────
    // fluid_synth_process() has no notion of "apply this event at sample N
    // within the block" — timing has to be achieved by rendering in pieces,
    // advancing a cursor to each event's sample position before dispatching
    // it. See SF2_PLAYER_PHASE2_SAMPLE_ACCURATE_TIMING.md for the full design.
    //
    // fluid_synth_process ACCUMULATES — must zero before the first call.
    // Render now targets the 32 per-channel group buffers instead of
    // scratchL/R directly (real per-channel audio, not a summed pair) — all
    // 32 must be zeroed, not just 2.
    for (auto& buf : groupBuffers)
        std::fill (buf.begin(), buf.begin() + numSamples, 0.0f);

    int renderCursor  = 0;
    int lastProcessRc = 0;   // last segment's rc, kept only for the debug log below

    auto renderSegment = [&] (int uptoSample)
    {
        const int span = uptoSample - renderCursor;
        if (span <= 0)
            return;

        // 16 groups × L/R, each pointer offset by renderCursor so this
        // segment lands at the right position within every group buffer —
        // matching the offset the old 2-buffer version applied to scratchL/R.
        float* groupSeg[32];
        for (int g = 0; g < 32; ++g)
            groupSeg[g] = groupBuffers[g].data() + renderCursor;

        lastProcessRc = fluid_synth_process (synth, span, 0, nullptr, 32, groupSeg);
        renderCursor = uptoSample;
    };

    for (const auto meta : midiIn)
    {
        const auto msg    = meta.getMessage();
        const int  midiCh = msg.getChannel();   // 1-16

        // Hard reservation boundary: MIDI channel 1 is owned by the Slicer and
        // channel 2 is owned by the SFZ-Player. The SF2/FluidSynth player must
        // never react to either channel, regardless of mask/UI state upstream.
        if (midiCh < 3 || midiCh > 16)
            continue;

        // Render up to this event's position BEFORE applying it, so FluidSynth
        // hears the event at the correct offset instead of at the top of the
        // block. Clamp defensively — a host may (rarely, due to rounding) hand
        // us an event timestamped at or past numSamples.
        renderSegment (juce::jlimit (0, numSamples, meta.samplePosition));

        // TEMP diagnostic: log every message unconditionally, before any
        // channel/velocity filtering, so we can catch cases where isNoteOn()
        // or the targetMask computation silently drops something.
        {
            const uint16_t previewTargetMask = (uint16_t) (1u << (midiCh - 1));
            sf2DebugLog ("  msg: ch=" + juce::String (midiCh)
                + " desc=\"" + msg.getDescription() + "\""
                + " isNoteOn(false)=" + juce::String ((int) msg.isNoteOn (false))
                + " isNoteOn(true)=" + juce::String ((int) msg.isNoteOn (true))
                + " vel=" + juce::String (msg.getVelocity())
                + " targetMask=0x" + juce::String::toHexString ((int) previewTargetMask));
        }

        if (msg.isNoteOn())
        {
            sf2DebugHadNoteOnThisBlock = true;
            previewPositionSample.store (0, std::memory_order_relaxed);
            lastTriggeredNote.store (juce::jlimit (0, 127, msg.getNoteNumber() + trans),
                                     std::memory_order_relaxed);
            // FluidSynth shapes this voice's envelope itself now (see
            // applyFluidAdsrFromUi) — no shared envelope to drive here.
            // sf2ActiveNoteCount just tracks whether any note is currently
            // held, for the UI playhead (see field doc comment in the header).
            sf2ActiveNoteCount.fetch_add (1, std::memory_order_relaxed);
        }
        else if (msg.isNoteOff())
        {
            // Clamp at 0 defensively — a stray note-off with no matching
            // note-on (e.g. host transport jump) shouldn't go negative and
            // leave the playhead permanently "stuck active".
            int prev = sf2ActiveNoteCount.load (std::memory_order_relaxed);
            while (prev > 0 && ! sf2ActiveNoteCount.compare_exchange_weak (
                       prev, prev - 1, std::memory_order_relaxed))
            {}
        }

        // Determine the set of FluidSynth channels to address.
        // Channels 1 and 2 are filtered out above (reserved for Slicer / SFZ-Player),
        // so every message reaching this point targets its own channel directly —
        // no fan-out. (liveInputChannelMask-based ch-1 fan-out has been removed:
        // it directly contradicted the channel-1/2 reservation.)
        const uint16_t targetMask = (uint16_t) (1u << (midiCh - 1));

        for (int fch = 0; fch < 16; ++fch)
        {
            if (! (targetMask & (1u << fch))) continue;

            if (msg.isNoteOn())
            {
                const int note = juce::jlimit (0, 127, msg.getNoteNumber() + trans);
                const int rc = fluid_synth_noteon (synth, fch, note, msg.getVelocity());
                sf2DebugLog ("noteOn: incomingCh=" + juce::String (midiCh)
                    + " fch=" + juce::String (fch)
                    + " note=" + juce::String (note)
                    + " vel=" + juce::String (msg.getVelocity())
                    + " targetMask=0x" + juce::String::toHexString ((int) targetMask)
                    + " liveMask=0x" + juce::String::toHexString ((int) liveMask)
                    + " fluid_synth_noteon rc=" + juce::String (rc)
                    + " (0=FLUID_OK, -1=FLUID_FAILED)");
            }
            else if (msg.isNoteOff())
            {
                const int note = juce::jlimit (0, 127, msg.getNoteNumber() + trans);
                fluid_synth_noteoff (synth, fch, note);
            }
            else if (msg.isController())
            {
                fluid_synth_cc (synth, fch,
                                msg.getControllerNumber(),
                                msg.getControllerValue());
            }
            else if (msg.isPitchWheel())
            {
                fluid_synth_pitch_bend (synth, fch, msg.getPitchWheelValue());
            }
            else if (msg.isChannelPressure())
            {
                fluid_synth_channel_pressure (synth, fch,
                                              msg.getChannelPressureValue());
            }
            else if (msg.isAftertouch())
            {
                fluid_synth_key_pressure (synth, fch,
                                          msg.getNoteNumber(),
                                          msg.getAfterTouchValue());
            }
            else if (msg.isProgramChange())
            {
                fluid_synth_program_change (synth, fch,
                                            msg.getProgramChangeNumber());
                // Program change resets this channel's generators to the
                // SoundFont's own defaults — reapply immediately so a
                // note-on later in this same block still gets the UI's
                // envelope/filter shape rather than the raw SoundFont one.
                applyFluidAdsrFromUi();
                applyFluidFilterFromUi();
            }
            else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            {
                fluid_synth_all_notes_off (synth, fch);
            }
        }

        // SysEx is not channel-specific — send once regardless of fan-out
        if (msg.isSysEx())
        {
            fluid_synth_sysex (synth,
                               reinterpret_cast<const char*> (msg.getSysExData()),
                               msg.getSysExDataSize(),
                               nullptr, nullptr, nullptr, 0);
        }
    }

    // ── Render FluidSynth (remainder) ─────────────────────────────────────────
    // Every event above already rendered its preceding segment via
    // renderSegment() at the correct sample offset; this call renders
    // whatever's left between the last event and the end of the block. With
    // an empty/all-filtered midiIn this is the only call made, and it renders
    // the full block in one go — same as the old single-call path.
    renderSegment (numSamples);

    // ── Scope B-Internal: per-channel mixing stage sums groups to master ─────
    // Same net effect as Scope A's inline sum (plugin output is still one
    // summed stereo pair — everything downstream of this point is unchanged),
    // but now goes through Sf2ChannelMixer, which is where a future
    // per-channel insert would plug in. No inserts are installed today, so
    // this is bit-identical to a plain sum.
    channelMixer.sumToMaster (groupBuffers, numSamples, scratchL.data(), scratchR.data());

    if (sf2DebugHadNoteOnThisBlock)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
            peak = std::max (peak, std::abs (scratchL[(size_t) i]));
        sf2DebugLog ("post-render (note-on this block): fluid_synth_process rc=" + juce::String (lastProcessRc)
            + " outputPeakL=" + juce::String (peak, 6)
            + " sfontId=" + juce::String (sfontId)
            + " gain=" + juce::String (fluid_synth_get_gain (synth), 3));
    }

    // Measure per-channel peaks from active voices before volume scaling
    measureChannelPeaks (numSamples);

    // Apply volume
    for (int i = 0; i < numSamples; ++i)
    {
        scratchL[(size_t) i] *= vol;
        scratchR[(size_t) i] *= vol;
    }

    // Apply post-processing reverb
    updateReverbParams();
    {
        float* planes[2] = { scratchL.data(), scratchR.data() };
        juce::dsp::AudioBlock<float> block (planes, 2, (size_t) numSamples);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        dspReverb.process (ctx);
    }

    // ── Playhead tracking (replaces the old post-mix shared-envelope block) ──
    // FluidSynth has already shaped each voice's envelope internally via the
    // per-channel generators written above/in applyFluidAdsrFromUi() — no
    // post-mix gain stage needed here. Just advance the UI playhead position
    // while any note is held.
    //
    // fluid_synth_get_active_voice_count() is used here instead of
    // sf2ActiveNoteCount: the note-on/off counter alone hits 0 the instant the
    // *last note-off message* is seen, with no idea whether that voice is
    // still ringing out its release tail. fluid_synth_get_active_voice_count()
    // reports true voice lifetime — it stays > 0 for a voice that received a
    // note-off but hasn't finished its release (or is held by sustain/
    // sostenuto) — so the playhead now keeps animating through the tail,
    // matching the old juceAdsr.isActive()-driven behaviour exactly. Calling
    // it here, immediately after this block's render, satisfies FluidSynth's
    // documented requirement that the count be read synchronously with audio
    // synthesis for an accurate value. sf2ActiveNoteCount is retained (see its
    // doc comment) for other bookkeeping that only cares about note-on/off,
    // not full voice lifetime.
    if (fluid_synth_get_active_voice_count (synth) > 0)
        previewPositionSample.fetch_add (numSamples, std::memory_order_relaxed);
    else
        previewPositionSample.store (0, std::memory_order_relaxed);

    // Mix into output
    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] += scratchL[(size_t) i];
        outR[i] += scratchR[(size_t) i];
    }
#else
    juce::ignoreUnused (midiIn, outL, outR, numSamples);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  measureChannelPeaks
//
//  Real per-channel peak metering. Each SF2 MIDI channel is rendered into its
//  own FluidSynth audio-group (synth.audio-groups=16, default channel%groups
//  mapping — a clean 1:1 with 16 channels), so groupBuffers[2*ch]/[2*ch+1]
//  are that channel's actual, already-panned L/R audio for this block. This
//  replaces the old voice-list-scanning proxy (peak_ch = max over active
//  voices on ch of velocity/127 * CC7_vol, with pan unmodelled / L≈R) with a
//  real max-abs-sample measurement, so pan (CC10) and reverb/chorus tails now
//  show up correctly per channel instead of being invisible or assumed L≈R.
//  A fast exponential decay is applied so the needle falls when voices stop —
//  same decay contract as before, so MixerPanel's meter drawing is unchanged.
// ─────────────────────────────────────────────────────────────────────────────
void SfzPlayer::measureChannelPeaks (int numSamples)
{
    if (synth == nullptr) return;

    static constexpr float kDecay = 0.85f;

    for (int ch = 0; ch < 16; ++ch)
    {
        const float* gl = groupBuffers[2 * ch].data();
        const float* gr = groupBuffers[2 * ch + 1].data();

        float pkL = 0.0f, pkR = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            pkL = std::max (pkL, std::abs (gl[i]));
            pkR = std::max (pkR, std::abs (gr[i]));
        }

        channelPeakL[ch].store (
            std::max (channelPeakL[ch].load (std::memory_order_relaxed) * kDecay, pkL),
            std::memory_order_relaxed);
        channelPeakR[ch].store (
            std::max (channelPeakR[ch].load (std::memory_order_relaxed) * kDecay, pkR),
            std::memory_order_relaxed);
    }
}

// ── ADSR unit conversion (seconds/percent UI values -> SF2 generator units) ──
//  FluidSynth/SoundFont generators use timecents for the time stages and
//  centibels of attenuation for sustain — not the seconds/percent the UI
//  works in, so every write needs converting.
//    time_sec = 2^(timecents/1200)  =>  timecents = 1200 * log2(time_sec)
//    FluidSynth's usable floor is -12000 timecents (~1 ms).
//    GEN_VOLENVSUSTAIN is attenuation in centibels: 0 = full level (0 dB),
//    1000 = silence (-100 dB) — inverse of the UI's 0-100% *level* meaning,
//    so it needs a sign flip as well as a unit conversion.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    float secondsToTimecents (float sec) noexcept
    {
        sec = juce::jmax (0.001f, sec);   // avoid log2(0); ~1 ms floor
        return juce::jlimit (-12000.0f, 8000.0f, 1200.0f * std::log2 (sec));
    }

    float levelPercentToSustainCentibels (float pct) noexcept
    {
        const float lvl = juce::jlimit (0.0001f, 1.0f, pct * 0.01f);
        return juce::jlimit (0.0f, 1000.0f, -200.0f * std::log10 (lvl));
    }
}

// ── Filter unit conversion (UI Hz/percent -> SF2 generator units) ────────────
//  GEN_FILTERFC is absolute cents referenced to 8.176 Hz (MIDI note 0):
//    hz = 8.176 * 2^(cents/1200)  =>  cents = 1200 * log2(hz / 8.176)
//  GEN_FILTERQ is resonance in centibels above the SoundFont spec's 0 dB
//  floor (no resonant peak); the spec's usable range runs 0-960 cB, but the
//  conversion here is bounded well short of that ceiling to keep the control
//  in a musically useful range rather than self-oscillating.
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
    float hzToAbsoluteCents (float hz) noexcept
    {
        hz = juce::jmax (1.0f, hz);   // guard log2() against zero/negative input
        return juce::jlimit (1500.0f, 13500.0f, 1200.0f * std::log2 (hz / 8.176f));
    }

    float resonancePercentToCentibels (float pct) noexcept
    {
        const float p = juce::jlimit (0.0f, 100.0f, pct);
        return juce::jlimit (0.0f, 720.0f, p * 7.2f);   // 0-100% -> 0-720 cB (~0-72 dB)
    }
}

// ── applyFluidAdsrFromUi ──────────────────────────────────────────────────────
//  Writes the current UI A/D/S/R values (juceAdsrAttack/Decay/Sustain/Release —
//  the same atomics the amp-envelope graph and LCD display read/write) into
//  FluidSynth's per-channel generators on channels 2-15 (0-based; channels 0/1
//  are reserved for the Slicer/SFZ-Player and are never touched). FluidSynth then
//  shapes each voice's envelope internally and independently, so releasing
//  one note no longer affects any other currently-sounding note the way the
//  old shared post-mix JUCE ADSR did. Called after load, after every
//  program/preset change (both reset a channel's generators back to the
//  SoundFont's own defaults), and whenever the UI values change.
// ─────────────────────────────────────────────────────────────────────────────
void SfzPlayer::applyFluidAdsrFromUi()
{
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth == nullptr) return;

    const float atkTc = secondsToTimecents (juceAdsrAttack .load (std::memory_order_relaxed));
    const float decTc = secondsToTimecents (juceAdsrDecay  .load (std::memory_order_relaxed));
    const float susCb = levelPercentToSustainCentibels (juceAdsrSustain.load (std::memory_order_relaxed) * 100.0f);
    const float relTc = secondsToTimecents (juceAdsrRelease.load (std::memory_order_relaxed));

    for (int ch = 2; ch < 16; ++ch)
    {
        fluid_synth_set_gen (synth, ch, GEN_VOLENVATTACK,  atkTc);
        fluid_synth_set_gen (synth, ch, GEN_VOLENVDECAY,   decTc);
        fluid_synth_set_gen (synth, ch, GEN_VOLENVSUSTAIN, susCb);
        fluid_synth_set_gen (synth, ch, GEN_VOLENVRELEASE, relTc);
    }
#endif
}

// ── applyFluidFilterFromUi ─────────────────────────────────────────────────────
//  Writes the current UI cutoff/resonance values (sf2FilterCutoffHz/
//  sf2FilterResonancePct — the same atomics the Column 2 FILTER tab reads/
//  writes) into FluidSynth's per-channel GEN_FILTERFC/GEN_FILTERQ generators
//  on channels 2-15 (0-based; channels 0/1 are reserved for the Slicer/
//  SFZ-Player and are never touched). Mirrors applyFluidAdsrFromUi() exactly:
//  called after load, after every program/preset change (both reset a
//  channel's generators back to the SoundFont's own defaults), and whenever
//  the UI values change.
// ─────────────────────────────────────────────────────────────────────────────
void SfzPlayer::applyFluidFilterFromUi()
{
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth == nullptr) return;

    const float cutoffCents = hzToAbsoluteCents (sf2FilterCutoffHz.load (std::memory_order_relaxed));
    const float resCb       = resonancePercentToCentibels (sf2FilterResonancePct.load (std::memory_order_relaxed));

    for (int ch = 2; ch < 16; ++ch)
    {
        fluid_synth_set_gen (synth, ch, GEN_FILTERFC, cutoffCents);
        fluid_synth_set_gen (synth, ch, GEN_FILTERQ,  resCb);
    }
#endif
}

// =============================================================================
//  Private helpers
// =============================================================================

void SfzPlayer::applyPendingLoad()
{
    auto* pkg = pendingLoad.exchange (nullptr, std::memory_order_acq_rel);
    if (pkg == nullptr)
        return;

    std::unique_ptr<PendingLoad> owner (pkg);

    // ── Tear down whatever is currently loaded ────────────────────────────────
    loaded.store (false, std::memory_order_release);
    activeFile = juce::File();
    isSfzFile  = false;

#if DYSEKT_HAS_FLUIDSYNTH
    if (synth != nullptr || settings != nullptr)
    {
        FLUIDSYNTH_LIFECYCLE_LOCK();
        if (synth != nullptr)
        {
            fluid_synth_all_notes_off (synth, 0);
            delete_fluid_synth    (synth);    synth    = nullptr;
        }
        if (settings != nullptr)
        {
            delete_fluid_settings (settings); settings = nullptr;
        }
    }
    sfontId = -1;
#endif

#if DYSEKT_HAS_SFIZZ
    if (sfizzSynth != nullptr)
    {
        sfizz_all_sound_off (sfizzSynth);
        sfizz_free (sfizzSynth);
        sfizzSynth = nullptr;
    }
#endif

    // Post an empty preset list so the UI clears.
    delete freshPresets.exchange (new std::vector<Sf2PresetInfo>(),
                                  std::memory_order_acq_rel);

    // Clear any stale per-channel preset assignments from a previous load.
    clearChannelPresets();

    if (owner->shouldUnload)
        return;

    const auto ext = owner->file.getFileExtension().toLowerCase();

    // ── SFZ path (sfizz) ─────────────────────────────────────────────────────
#if DYSEKT_HAS_SFIZZ
    if (ext == ".sfz")
    {
        isSfzFile  = true;
        sfizzSynth = sfizz_create_synth();
        sfizz_set_sample_rate       (sfizzSynth, (float) currentSR);
        sfizz_set_samples_per_block (sfizzSynth, currentBlock);

        if (! sfizz_load_file (sfizzSynth, owner->file.getFullPathName().toRawUTF8()))
        {
            sfizz_free (sfizzSynth);
            sfizzSynth = nullptr;
            isSfzFile  = false;
            return;
        }

        activeFile = owner->file;
        loaded.store (true, std::memory_order_release);

        // Re-apply pan (sfizz responds to CC10)
        setPan (pan.load (std::memory_order_relaxed));

        // Post a single dummy preset entry so the UI shows "loaded"
        auto* list = new std::vector<Sf2PresetInfo>();
        list->push_back ({ 0, 0, owner->file.getFileNameWithoutExtension() });
        delete freshPresets.exchange (list, std::memory_order_acq_rel);

        return;
    }
#endif

    // ── SF2 path (FluidSynth) ─────────────────────────────────────────────────
#if DYSEKT_HAS_FLUIDSYNTH
    {
        FLUIDSYNTH_LIFECYCLE_LOCK();

        settings = new_fluid_settings();

#if JUCE_DEBUG
        fluid_settings_setint (settings, "synth.verbose", 0);
#endif

        fluid_settings_setint (settings, "synth.reverb.active", 1);
        fluid_settings_setint (settings, "synth.chorus.active", 1);

        // Real per-channel audio: one stereo audio-group per MIDI channel, using
        // FluidSynth's default channel->group mapping (channel % audio_groups),
        // which is a clean 1:1 with 16 channels / 16 groups. Must be set before
        // new_fluid_synth() — it cannot be changed on a live synth instance.
        fluid_settings_setint (settings, "synth.audio-groups", 16);

        synth = new_fluid_synth (settings);
        fluid_synth_set_sample_rate (synth, (float) currentSR);
        fluid_synth_set_gain        (synth, 2.0f);

        sfontId = fluid_synth_sfload (synth,
                      owner->file.getFullPathName().toRawUTF8(), 1);

        if (sfontId == FLUID_FAILED)
        {
            delete_fluid_synth    (synth);    synth    = nullptr;
            delete_fluid_settings (settings); settings = nullptr;
            return;
        }
    }

    activeFile = owner->file;
    loaded.store (true, std::memory_order_release);

    // Re-apply user params that FluidSynth loses when synth is recreated.
    setPan      (pan.load      (std::memory_order_relaxed));
    setFineTune (fineTune.load (std::memory_order_relaxed));
    setReverb   (reverb.load   (std::memory_order_relaxed));
    setChorus   (chorus.load   (std::memory_order_relaxed));

    presetIndex.store (0, std::memory_order_relaxed);
    postPresetList();

    // ── SF2 loop points are extracted by SoundFontLoader via SHDR binary parse ──
    // (see SoundFontLoader.cpp: parseSf2LoopPoints)
    // SfzPlayer::setLoopPoints() is called from there after the render job
    // completes so Sf2WaveformLcd can display the loop overlay.
    // Reset them here so a reload of a non-looping SF2 clears old markers.
    sfzLoopStartSample.store (-1, std::memory_order_relaxed);
    sfzLoopEndSample  .store (-1, std::memory_order_relaxed);

    // Seed bank/program for the initial program change (preset 0).
    // We can't use cachedPresets here (audio thread), so read directly from FluidSynth.
    {
        fluid_sfont_t* sfont = fluid_synth_get_sfont_by_id (synth, sfontId);
        if (sfont != nullptr)
        {
            fluid_sfont_iteration_start (sfont);
            fluid_preset_t* first = fluid_sfont_iteration_next (sfont);
            if (first != nullptr)
            {
                pendingBank   .store (fluid_preset_get_banknum (first), std::memory_order_relaxed);
                pendingProgram.store (fluid_preset_get_num     (first), std::memory_order_relaxed);
            }
        }
    }

    // Assign first preset (bank 0, preset 0) to channel 0 as default.
    // The sequencer will call setPresetOnChannel() to populate other channels.
    applyPendingChannelChanges();  // all slots are -1 at this point; no-op but clears dirty flag
    setPresetByIndex (0);          // triggers applyProgramChange() on next process() tick

    // Apply the current UI ADSR values as FluidSynth's own per-voice envelope,
    // so loaded notes are shaped independently instead of via a shared post-mix
    // envelope (see applyFluidAdsrFromUi doc comment).
    applyFluidAdsrFromUi();

    // Same reasoning for the SF2 filter controls — a fresh synth/sfont load
    // starts every channel at the SoundFont's own filter defaults, so reapply
    // the UI-owned cutoff/resonance immediately (see applyFluidFilterFromUi
    // doc comment).
    applyFluidFilterFromUi();

    // Switch to omni so all incoming MIDI reaches FluidSynth without needing
    // the host to route on a specific channel.  In VST3, processMidi() already
    // blocks the slicer/VoicePool when SF-Player mode is active, so omni is
    // safe in both standalone and plugin builds.
    midiChannel.store (0, std::memory_order_relaxed);

#else
    juce::ignoreUnused (owner);
#endif
}

void SfzPlayer::applyPendingChannelChanges()
{
    anyChannelDirty.store (false, std::memory_order_relaxed);

#if DYSEKT_HAS_FLUIDSYNTH
    if (synth == nullptr || sfontId == FLUID_FAILED)
        return;

    const int offset = fluid_synth_get_bank_offset (synth, sfontId);

    // Channels 0/1 (MIDI ch 1/2) are reserved for the Slicer/SFZ-Player.
    // setPresetOnChannel() already refuses to populate pendingChannelAssignment[0/1],
    // but starting this sweep at 2 keeps that invariant visible here too rather
    // than depending solely on the write-side guard.
    for (int ch = 2; ch < 16; ++ch)
    {
        const int packed = pendingChannelAssignment[ch].exchange (
                               -1, std::memory_order_acq_rel);

        if (packed == -1)
            continue;   // nothing pending on this channel

        const int bank   = (packed >> 16) & 0x7FFF;
        const int preset = packed & 0xFFFF;

        fluid_synth_program_select (synth, ch,
                                    static_cast<unsigned int> (sfontId),
                                    static_cast<unsigned int> (offset + bank),
                                    static_cast<unsigned int> (preset));
    }
#endif
}

void SfzPlayer::applyProgramChange()
{
    programChangePending.store (false, std::memory_order_release);

#if DYSEKT_HAS_FLUIDSYNTH
    if (synth == nullptr || sfontId == FLUID_FAILED)
        return;

    const int bank    = pendingBank   .load (std::memory_order_relaxed);
    const int program = pendingProgram.load (std::memory_order_relaxed);
    const int offset  = fluid_synth_get_bank_offset (synth, sfontId);

    fluid_synth_program_select (synth, 0, sfontId,
                                (unsigned int)(offset + bank),
                                (unsigned int) program);
#endif
}

void SfzPlayer::postPresetList()
{
#if DYSEKT_HAS_FLUIDSYNTH
    if (synth == nullptr || sfontId == FLUID_FAILED)
        return;

    fluid_sfont_t* sfont = fluid_synth_get_sfont_by_id (synth, sfontId);
    if (sfont == nullptr)
        return;

    auto* list = new std::vector<Sf2PresetInfo>();

    fluid_sfont_iteration_start (sfont);
    for (fluid_preset_t* p = fluid_sfont_iteration_next (sfont);
         p != nullptr;
         p = fluid_sfont_iteration_next (sfont))
    {
        Sf2PresetInfo info;
        info.bank   = fluid_preset_get_banknum (p);
        info.preset = fluid_preset_get_num     (p);
        info.name   = juce::String::fromUTF8   (fluid_preset_get_name (p));
        list->push_back (info);
    }

    // Discard any previous unread list and post the fresh one.
    delete freshPresets.exchange (list, std::memory_order_acq_rel);
#endif
}

// ── Pitch shift helper ────────────────────────────────────────────────────────

void SfzPlayer::pitchShiftBlock (const float* src, float* dst,
                                   int srcLen, int dstLen) noexcept
{
    // Linear interpolating resampler.
    // Maps dstLen output samples from srcLen input samples.
    // ratio = srcLen / dstLen:  >1 compresses (pitch up), <1 stretches (pitch down).
    //
    // For each output sample i, the corresponding source position is:
    //   srcPos = i * (srcLen - 1) / (dstLen - 1)
    // We split into integer index + fractional part and lerp.

    if (srcLen == 0 || dstLen == 0) return;

    if (srcLen == dstLen)
    {
        // Exact 1:1 — just copy
        for (int i = 0; i < dstLen; ++i)
            dst[i] = src[i];
        return;
    }

    const float step = (float)(srcLen - 1) / (float)(dstLen - 1);
    float pos = 0.0f;

    for (int i = 0; i < dstLen; ++i, pos += step)
    {
        const int   idx  = (int) pos;
        const float frac = pos - (float) idx;

        const int   idx1 = juce::jmin (idx,     srcLen - 1);
        const int   idx2 = juce::jmin (idx + 1, srcLen - 1);

        dst[i] = src[idx1] + frac * (src[idx2] - src[idx1]);
    }
}
