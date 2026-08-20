#pragma once
// =============================================================================
//  SfzPlayer.h  —  Real-time SF2/SFZ playback engine
//                  SF2 → FluidSynth backend
//                  SFZ → sfizz backend
// =============================================================================
//  Owned by DysektProcessor.  prepareToPlay / processBlock / loadFile are
//  called from the audio thread (processBlock) or UI thread (load/param set).
//
//  Thread safety:
//    loadFile()           — UI thread; hands the file off to the given
//                           juce::ThreadPool, which snapshot-copies it to a
//                           private temp file before a PendingLoad (pointing
//                           at that copy) is posted via atomic. This is what
//                           keeps applyPendingLoad()'s read (below) from ever
//                           racing a caller that keeps rewriting the same
//                           source path (e.g. MultisamplerEditor's debounced
//                           re-export) — see PendingLoad::isTempCopy.
//    setVolume/Trans()    — UI thread; stored as std::atomic<float>
//    setPresetByIndex()   — UI thread; sets atomics + programChangePending flag
//    prepare()            — audio thread (prepareToPlay)
//    process()            — audio thread (processBlock); applies pending loads
//                           and program changes at the top of each block.
//                           NOTE: applyPendingLoad() still calls
//                           sfizz_load_file()/fluid_synth_sfload() (parsing,
//                           not just the now-race-free disk read) directly on
//                           this thread — moving that off the audio thread
//                           too is a larger change than this fix; the private
//                           snapshot at least guarantees it never parses a
//                           file that's being overwritten out from under it.
//
//  Preset list handoff (audio → UI):
//    After a successful sfont load the audio thread allocates a new
//    std::vector<Sf2PresetInfo>* and stores it in freshPresets.
//    getPresetList() (UI thread) swaps it out and caches it.
// =============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <vector>

#include "Sf2ChannelMixer.h"

#if DYSEKT_HAS_FLUIDSYNTH
  #include <fluidsynth.h>
#endif

#if DYSEKT_HAS_SFIZZ
  #include "../../sfizz/src/sfizz.h"
#endif

// -----------------------------------------------------------------------------
//  Preset descriptor — used by the UI to populate the preset picker
// -----------------------------------------------------------------------------
struct Sf2PresetInfo
{
    int          bank   { 0 };
    int          preset { 0 };
    juce::String name;
};

// =============================================================================
class SfzPlayer
{
public:
    SfzPlayer();
    ~SfzPlayer();

    // ── Called from UI thread ─────────────────────────────────────────────────

    /** Queue a new SF2 file for loading. Returns immediately. */
    void loadFile (const juce::File& f, juce::ThreadPool& pool);

    /** Unload current instrument (silent output). */
    void unload();

#if DYSEKT_SF2_EXPERIMENTAL_MULTI_GROUP
    /** Runtime on/off switch for the audio-groups=16 / reverb+chorus-off
     *  experimental load path (see the long comment at the top of
     *  SfzPlayer.cpp). Only compiled in when the DYSEKT_SF2_EXPERIMENTAL_
     *  MULTI_GROUP CMake option is ON — that option remains the hard
     *  kill switch for release builds; this atomic is the convenience
     *  toggle on top of it for supervised debug-build testing. UI thread
     *  writes, audio thread reads at the next file (re)load — it does NOT
     *  retroactively change a synth instance that's already loaded, since
     *  the settings it gates (audio-groups, reverb.active, chorus.active)
     *  are fixed at fluid_synth creation time. Reload the file after
     *  flipping this to pick up the change. */
    void setExperimentalMultiGroupEnabled (bool shouldBeEnabled) noexcept
    {
        experimentalMultiGroupEnabled.store (shouldBeEnabled, std::memory_order_relaxed);
    }

    bool getExperimentalMultiGroupEnabled() const noexcept
    {
        return experimentalMultiGroupEnabled.load (std::memory_order_relaxed);
    }
#endif

    void setVolume      (float gainLinear);   ///< 0..2
    void setTranspose   (int semitones);      ///< SF2 only — MIDI note shift; SFZ uses setPitchShift
    void setPitchShift  (float semitones);    ///< SFZ audio-rate pitch shift, -24..+24 semitones
    void setMidiChannel (int ch);             ///< 0 = omni, 1-16 = specific
    void setPan         (float centred);      ///< -1.0 (L) .. 0.0 (C) .. +1.0 (R)
    void setFineTune    (float cents);        ///< -100 .. +100 cents
    void setReverb      (float level);        ///< 0..1 wet level
    void setChorus      (float level);        ///< 0..1 wet level

    /** Select a preset by its index in the list returned by getPresetList().
     *  Single-preset mode: assigns the chosen preset to FluidSynth channel 0.
     *  For multi-timbral use, prefer setPresetOnChannel() instead. */
    void setPresetByIndex (int idx);

    /** Multi-timbral SF2: assign a specific bank/preset to a FluidSynth channel (0-15).
     *  Each sequencer track calls this with its own channel so FluidSynth plays
     *  multiple programs simultaneously.  No-op for SFZ files. */
    void setPresetOnChannel (int channel, int bank, int preset);

    /** Clear all pending channel-preset assignments (e.g. on SF2 unload). */
    void clearChannelPresets();

    /** Preview mode: load bank/preset onto the dedicated preview channel and
     *  arm a one-shot Middle-C demo-note request.  Call from the UI thread.
     *  A second call with the same bank/preset clears the preview (toggle).
     *
     *  This never touches MIDI channel ownership, Arranger track selection,
     *  or the live keyboard channel — PluginProcessor injects the demo note
     *  as real MIDI on getPreviewMidiChannel() (see
     *  takePendingPreviewNoteOn()/takePendingPreviewNoteOff()). */
    void previewPreset (int bank, int preset);

    /** Stop any active preview: silence the preview channel and request a
     *  note-off for the demo note if it's still sounding. */
    void clearPreview();

    /** MIDI channel (1-based) permanently reserved for preset preview/audition.
     *  PluginProcessor injects the demo note-on/off requested by
     *  previewPreset()/clearPreview() on this channel and nothing else. */
    static constexpr int getPreviewMidiChannel() noexcept { return 16; }

    /** Consumed once per block by PluginProcessor: if a demo note-on is
     *  pending, returns true and writes the note number (0-127) to noteOut,
     *  clearing the request. Call from the audio thread. */
    bool takePendingPreviewNoteOn (int& noteOut) noexcept
    {
        if (! previewNoteOnPending.exchange (false, std::memory_order_acq_rel))
            return false;
        noteOut = previewNoteNumber.load (std::memory_order_relaxed);
        return true;
    }

    /** Same as takePendingPreviewNoteOn(), for the matching note-off request
     *  fired by clearPreview()/a superseding previewPreset() call. */
    bool takePendingPreviewNoteOff (int& noteOut) noexcept
    {
        if (! previewNoteOffPending.exchange (false, std::memory_order_acq_rel))
            return false;
        noteOut = previewNoteOffNumber.load (std::memory_order_relaxed);
        return true;
    }

    /** Set which FluidSynth channels (bitmask, bit 0 = ch 0 … bit 15 = ch 15)
     *  should receive live controller input that arrives on MIDI channel 1.
     *  Call from the UI/message thread whenever the user selects or deselects
     *  SF2 tracks.  0 = no fan-out (controller input is silenced for SF2). */
    void setLiveInputChannelMask (uint16_t mask) noexcept
    {
        liveInputChannelMask.store (mask, std::memory_order_relaxed);
    }

    uint16_t getLiveInputChannelMask() const noexcept
    {
        return liveInputChannelMask.load (std::memory_order_relaxed);
    }

    float      getVolume()      const noexcept { return volume.load(); }
    int        getTranspose()   const noexcept { return transpose.load(); }
    float      getPitchShift()  const noexcept { return pitchShift.load (std::memory_order_relaxed); }
    int        getMidiChannel() const noexcept { return midiChannel.load(); }
    float      getPan()         const noexcept { return pan.load(); }
    float      getFineTune()    const noexcept { return fineTune.load(); }
    float      getReverb()      const noexcept { return reverb.load(); }
    float      getChorus()      const noexcept { return chorus.load(); }
    int        getCurrentPresetIndex() const noexcept { return presetIndex.load(); }

    /** UI-display-only "currently selected" preset in the multitimbral program
     *  grid — the last preset the user previewed or selected for per-channel FX
     *  editing. Purely cosmetic: unlike setPresetByIndex()/presetIndex, setting
     *  this never sends a program change on any MIDI channel. */
    void       setDisplayPresetIndex (int idx) noexcept { displayPresetIndex.store (idx); }
    int        getDisplayPresetIndex() const noexcept { return displayPresetIndex.load(); }

    juce::File getLoadedFile()  const;

    /** Returns the file most recently passed to loadFile(), even if the async
     *  load hasn't finished yet.  Safe to call on the UI thread.
     *  Returns an empty File if nothing has ever been queued. */
    juce::File getPendingFilePath() const { return juce::File (pendingFilePath); }
    bool       isLoaded()       const noexcept { return loaded.load(); }

    // ── SFZ ADSR (applied via sfizz OSC messages per region) ──────────────────
    //  Values are stored as atomics and flushed to sfizz at the start of each
    //  processBlock() call when dirty.  Call from any thread; sfizz update is RT.
    void  setSfzAttack  (float sec)  noexcept;   ///< 0-30 s
    void  setSfzDecay   (float sec)  noexcept;   ///< 0-30 s
    void  setSfzSustain (float pct)  noexcept;   ///< 0-100 %
    void  setSfzRelease (float sec)  noexcept;   ///< 0-60 s

    /** Set per-region volume and pan for SFZ files (sfizz OSC, real-time safe).
     *  regionIndex is the 0-based zone/region index from the parsed Keyzone list.
     *  No-op for SF2 files. */
    void setZoneVolume (int regionIndex, float volDb)    noexcept;
    void setZonePan    (int regionIndex, float pan)      noexcept;  ///< pan: -1..+1
    void setZoneTune   (int regionIndex, float cents)    noexcept;  ///< cents: -100..+100

    float getSfzAttack()  const noexcept { return sfzAttackSec .load (std::memory_order_relaxed); }
    float getSfzDecay()   const noexcept { return sfzDecaySec  .load (std::memory_order_relaxed); }
    float getSfzSustain() const noexcept { return sfzSustainPct.load (std::memory_order_relaxed); }
    float getSfzRelease() const noexcept { return sfzReleaseSec.load (std::memory_order_relaxed); }

    /** Loop point in samples within the sfizz-rendered preview buffer.
     *  Returns -1 if no loop is defined. */
    int  getLoopStartSample() const noexcept { return sfzLoopStartSample.load (std::memory_order_relaxed); }
    int  getLoopEndSample()   const noexcept { return sfzLoopEndSample  .load (std::memory_order_relaxed); }

    /** Called by SoundFontLoader after rendering to store loop metadata. */
    void setLoopPoints (int loopStart, int loopEnd) noexcept
    {
        sfzLoopStartSample.store (loopStart, std::memory_order_relaxed);
        sfzLoopEndSample  .store (loopEnd,   std::memory_order_relaxed);
    }

    // ── Post-processing Reverb EFX (JUCE DSP — works for both SF2 & SFZ) ──
    void setReverbSize   (float pct) noexcept;   ///< 0–100 %
    void setReverbDamp   (float pct) noexcept;   ///< 0–100 %
    void setReverbWidth  (float pct) noexcept;   ///< 0–100 %
    void setReverbMix    (float pct) noexcept;   ///< 0–100 %
    void setReverbFreeze (bool  on)  noexcept;   ///< infinite sustain

    float getReverbSize()   const noexcept { return reverbSize  .load (std::memory_order_relaxed); }
    float getReverbDamp()   const noexcept { return reverbDamp  .load (std::memory_order_relaxed); }
    float getReverbWidth()  const noexcept { return reverbWidth .load (std::memory_order_relaxed); }
    float getReverbMix()    const noexcept { return reverbMix   .load (std::memory_order_relaxed); }
    bool  getReverbFreeze() const noexcept { return reverbFreeze.load (std::memory_order_relaxed); }

    // ── SF2 filter (FluidSynth GEN_FILTERFC / GEN_FILTERQ) — UI-driven, SF2 only ──
    //  Same UI-atomic / private-apply-helper / reapply-on-reset pattern as the
    //  ADSR controls above; see applyFluidFilterFromUi() doc comment.
    static constexpr float kSf2FilterCutoffMinHz     = 20.0f;
    static constexpr float kSf2FilterCutoffMaxHz     = 20000.0f;
    static constexpr float kSf2FilterCutoffDefaultHz = 20000.0f; ///< fully open — neutral on first load
    static constexpr float kSf2FilterResonanceMinPct     = 0.0f;
    static constexpr float kSf2FilterResonanceMaxPct     = 100.0f;
    static constexpr float kSf2FilterResonanceDefaultPct = 0.0f; ///< no resonance emphasis — neutral on first load

    void setSf2FilterCutoff    (float hz)  noexcept;  ///< clamped to 20 Hz .. 20 kHz
    void setSf2FilterResonance (float pct) noexcept;  ///< clamped to 0-100 %

    float getSf2FilterCutoff()    const noexcept { return sf2FilterCutoffHz    .load (std::memory_order_relaxed); }
    float getSf2FilterResonance() const noexcept { return sf2FilterResonancePct.load (std::memory_order_relaxed); }

    /**
     * Returns the cached preset list for the currently loaded SF2.
     * If the audio thread has posted new data since the last call,
     * the cache is updated first (wait-free on both sides).
     * Safe to call from any thread except the audio thread.
     */
    std::vector<Sf2PresetInfo> getPresetList() const;

    // ── Per-channel mixer strip (SF2 multi-timbral) ───────────────────────────

    /** Snapshot of one channel's mixer state — safe to copy on the UI thread. */
    struct ChannelStrip
    {
        float volume     { 1.0f };   ///< normalised 0..1  (maps to CC7 0..127)
        float pan        { 0.0f };   ///< -1..+1           (maps to CC10 0..127)
        float reverbSend { 0.0f };   ///< normalised 0..1  (maps to CC91 0..127)
        float preMuteVol { 1.0f };   ///< volume saved before mute
        bool  muted      { false };
    };

    /** Read a channel's current strip state.  Safe to call on any thread. */
    ChannelStrip getChannelStrip (int channel) const noexcept;

    void setChannelVolume     (int channel, float normVol)   noexcept; ///< 0..1
    void setChannelPan        (int channel, float pan)       noexcept; ///< -1..+1
    void setChannelReverbSend (int channel, float normSend)  noexcept; ///< 0..1
    void setChannelMuted      (int channel, bool muted)      noexcept;
    void soloChannel          (int channel)                  noexcept;
    void clearSolo            ()                             noexcept;

    // ── Called from audio thread ──────────────────────────────────────────────

    void prepare (double sampleRate, int maxBlockSize);

    /**
     * Process one block. MIDI events from @p midiIn whose channel matches
     * midiChannel (0 = all) are forwarded to FluidSynth.  Rendered stereo
     * audio is mixed additively into @p outL / @p outR.
     */
    void process (const juce::MidiBuffer& midiIn,
                  float* outL, float* outR, int numSamples);

    // ── JUCE ADSR (applied post-render — SFZ/sfizz branch only) ──────────────
    //  The envelope is owned here so it lives on the audio thread.
    //  UI thread sets parameters via setJuceAdsr(); noteOn/Off are signalled via
    //  atomics so the audio thread fires the envelope at the right moment.
    //
    //  SF2/FluidSynth branch no longer uses this shared envelope (a single
    //  post-mix ADSR gated every voice at once, causing one note's release to
    //  fade every other currently-sounding note — see
    //  SF2_PLAYER_POLYPHONY_AND_NOTE_CUTOFF_FIXES.md). Instead the same UI
    //  A/D/S/R values are converted to FluidSynth generator units and written
    //  per-channel via applyFluidAdsrFromUi(), so FluidSynth's own per-voice
    //  envelopes shape each note independently while still tracking the UI
    //  controls (Option B from that doc, not Option A — the amp-envelope
    //  graph/LCD stay live instead of going inert).

    /** Update ADSR parameters.  Safe to call from any thread. */
    void setJuceAdsr (float attackSec, float decaySec,
                      float sustainLvl, float releaseSec) noexcept;

    /** Signal a note-on to the JUCE envelope (called from UI when key is pressed). */
    void juceAdsrNoteOn (int noteNumber = -1) noexcept
    {
        juceAdsrNoteOnPending.store (true, std::memory_order_relaxed);
        if (noteNumber >= 0)
            pendingTriggeredNote.store (noteNumber, std::memory_order_relaxed);
    }

    /** Signal a note-off to the JUCE envelope. */
    void juceAdsrNoteOff() noexcept { juceAdsrNoteOffPending.store (true, std::memory_order_relaxed); }

    /** True when the JUCE ADSR is active (envelope not idle). */
    bool juceAdsrIsActive() const noexcept { return juceAdsrActive.load (std::memory_order_relaxed); }

    /** Sample position within the rendered preview buffer (processor.sampleData2)
     *  of the most recently triggered note, advancing each block while a note
     *  is active.  Mirrors VoicePool::voicePositions semantics: 0 = not
     *  playing/idle.  Resets to 0 on note-on, freezes when the envelope goes
     *  idle (after release tail completes) so the UI playhead disappears the
     *  same way the Slicer's does.  Safe to read from the UI/message thread. */
    int getPreviewPositionSample() const noexcept
    {
        return previewPositionSample.load (std::memory_order_relaxed);
    }

    /** MIDI note number of the most recently triggered note-on (sfizz or
     *  FluidSynth branch, MIDI or UI-keyboard injection — all sites that
     *  reset previewPositionSample also record this). -1 if no note has
     *  fired yet. Combined with previewPositionSample by the UI layer
     *  (which knows the note->region mapping via previewZones3) to find
     *  the absolute buffer position of the playhead, since SfzPlayer
     *  itself has no knowledge of region/zone boundaries. */
    int getLastTriggeredNote() const noexcept
    {
        return lastTriggeredNote.load (std::memory_order_relaxed);
    }

    // ── Per-channel peak meters (public — read by MixerPanel timer) ──────────
    // Written on audio thread after each process() block; read on UI thread.
    std::atomic<float> channelPeakL[16] {};
    std::atomic<float> channelPeakR[16] {};

private:
    // ── Pending load (UI → audio thread handoff) ──────────────────────────────
    struct PendingLoad
    {
        // Path actually handed to sfizz_load_file()/fluid_synth_sfload() —
        // either a private snapshot copy loadFile() made on the background
        // pool, or (if that copy failed) the caller's original path
        // unchanged. See loadFile()'s doc comment.
        juce::File file;

        // The caller's real, user-facing path — always the original file
        // loadFile() was given, regardless of whether `file` above ended up
        // being a temp copy. activeFile/getLoadedFile() must be set to
        // *this*, not `file`, or every caller that displays or re-reads
        // "the loaded file" (filenames in the LCD, reloadZones(), the
        // browser's default directory, etc.) would end up pointing at a
        // throwaway temp path that applyPendingLoad() deletes right after
        // using it.
        juce::File originalFile;

        bool       shouldUnload { false };

        // True when `file` is a private snapshot copy that loadFile() made
        // on the background pool, rather than `originalFile` itself.
        // applyPendingLoad() schedules its deletion (via loadPool, off the
        // audio thread) once it's done reading it.
        bool       isTempCopy   { false };
    };
    std::atomic<PendingLoad*> pendingLoad { nullptr };

    // Background pool passed to the most recent loadFile() call — used both
    // to make the snapshot copy in loadFile() and, later, to delete it again
    // from applyPendingLoad(). Written on the UI thread immediately before
    // each pendingLoad publish; safely visible on the audio thread via the
    // same release/acquire pair that publishes pendingLoad itself (the same
    // guarantee owner->file/owner->shouldUnload already rely on). Raw,
    // non-owning: SfzPlayer never outlives the DysektProcessor that owns
    // both it and fileLoadPool.
    juce::ThreadPool* loadPool { nullptr };

    // Stores the path of the most recently queued file (set by loadFile() on
    // the UI thread; safe to read via getPendingFilePath() at any time).
    juce::String pendingFilePath;

    // ── Pending preset list (audio → UI handoff) ──────────────────────────────
    mutable std::atomic<std::vector<Sf2PresetInfo>*> freshPresets { nullptr };
    mutable std::vector<Sf2PresetInfo>               cachedPresets;

    // ── Audio-thread FluidSynth state (SF2) ───────────────────────────────────
#if DYSEKT_HAS_FLUIDSYNTH
    fluid_settings_t* settings { nullptr };
    fluid_synth_t*    synth    { nullptr };
    int               sfontId  { -1 };
#endif

    // ── Audio-thread sfizz state (SFZ) ────────────────────────────────────────
#if DYSEKT_HAS_SFIZZ
    sfizz_synth_t*    sfizzSynth { nullptr };
#endif

    bool isSfzFile { false };   ///< true when the loaded file is .sfz
    double   currentSR    { 44100.0 };
    int      currentBlock { 256 };
    juce::File activeFile;

    // ── Shared params (atomic, UI-writable) ───────────────────────────────────
    std::atomic<float> volume      { 1.0f };
    std::atomic<int>   transpose   { 0 };
    std::atomic<float> pitchShift  { 0.0f };  ///< SFZ audio-rate pitch, -24..+24 semitones
    std::atomic<int>   midiChannel { 16 };   // 0 = omni, default 16 = DY-SFP dedicated channel
    std::atomic<float> pan         { 0.0f }; // -1..+1
    std::atomic<float> fineTune    { 0.0f }; // cents -100..+100
    std::atomic<float> reverb      { 0.4f }; // 0..1
    std::atomic<float> chorus      { 0.2f }; // 0..1
    std::atomic<int>   presetIndex   { 0 };  // index into cachedPresets (UI display)
    std::atomic<int>   displayPresetIndex { -1 }; // grid's "last selected" preset — UI-only, no engine effect
    std::atomic<int>   pendingBank   { 0 };  // bank number for applyProgramChange
    std::atomic<int>   pendingProgram{ 0 };  // program number for applyProgramChange
    std::atomic<bool>  loaded      { false };

#if DYSEKT_SF2_EXPERIMENTAL_MULTI_GROUP
    // Runtime companion to the DYSEKT_SF2_EXPERIMENTAL_MULTI_GROUP CMake
    // option — see setExperimentalMultiGroupEnabled() above and the load-site
    // comment in SfzPlayer.cpp. Defaults OFF even in a build compiled with
    // the option on, so enabling it is always an explicit action.
    std::atomic<bool> experimentalMultiGroupEnabled { false };
#endif

    // ── SFZ ADSR atomics (written from any thread, read on audio thread) ──────
    std::atomic<float> sfzAttackSec   { 0.005f };  ///< seconds (SFZ default ~0)
    std::atomic<float> sfzDecaySec    { 0.1f   };  ///< seconds
    std::atomic<float> sfzSustainPct  { 100.0f };  ///< percent 0-100
    std::atomic<float> sfzReleaseSec  { 0.05f  };  ///< seconds (SFZ default ~0)
    std::atomic<bool>  sfzAdsrDirty   { false  };

    // ── SFZ loop points (written by SoundFontLoader, read by Sf2WaveformLcd) ───
    std::atomic<int>   sfzLoopStartSample { -1 };   ///< -1 = no loop
    std::atomic<int>   sfzLoopEndSample   { -1 };  ///< set by setters, cleared in processBlock

    /** Set when presetIndex changes; audio thread picks it up in process(). */
    std::atomic<bool>  programChangePending { false };

    // ── Multi-timbral channel assignments (SF2 only) ──────────────────────────
    // pendingChannelAssignment[ch] holds a packed (bank << 16) | preset value,
    // or -1 if no change is pending on that channel.
    // Written from the UI thread via setPresetOnChannel(); read+cleared on audio thread.
    std::atomic<int>  pendingChannelAssignment[16];  // initialised to -1 in ctor
    std::atomic<bool> anyChannelDirty { false };

    // ── Live controller fan-out (SF2 multi-timbral) ───────────────────────────
    // Bitmask of FluidSynth channels (bit 0 = ch 0 … bit 15 = ch 15) that should
    // receive fan-out of incoming MIDI ch-1 controller input.
    // Written from UI thread via setLiveInputChannelMask(); read on audio thread.
    std::atomic<uint16_t> liveInputChannelMask { 0 };

    // ── Preview (audition) demo-note request — see previewPreset()/clearPreview() ──
    // PluginProcessor polls these once per block via takePendingPreviewNoteOn()/
    // takePendingPreviewNoteOff() and injects a real MIDI note-on/off on
    // getPreviewMidiChannel(). No MIDI channel ownership, Arranger track
    // selection, or keyboard routing is consulted anywhere in this path.
    std::atomic<bool> previewNoteOnPending  { false };
    std::atomic<bool> previewNoteOffPending { false };
    std::atomic<int>  previewNoteNumber     { 60 };   // Middle C default
    std::atomic<int>  previewNoteOffNumber  { 60 };

    // ── Scratch buffer for FluidSynth interleaved → planar conversion ─────────
    std::vector<float> scratchL, scratchR;

    // ── Per-channel FluidSynth audio-group buffers (real per-channel audio) ───
    // 16 groups × L/R = 32 independently-growable buffers, one pair per SF2
    // MIDI channel (default FluidSynth channel→group mapping is channel %
    // audio_groups, and audio-groups==16 makes that a clean 1:1). Deliberately
    // std::vector, NOT a fixed-size C array: numSamples is not guaranteed to
    // stay <= the block size passed to prepare() (see the same growth check
    // on scratchL/R in process()), so these must be able to grow the same way
    // or a larger-than-expected block will write past a fixed buffer's end.
    std::vector<float> groupBuffers[32];

    // ── Scope B-Internal: per-channel mixing stage ────────────────────────────
    // Owns summing groupBuffers into the master pair, with a seam for future
    // per-channel insert processing. See Sf2ChannelMixer.h for what this is
    // and (importantly) is not. Solo/mute is unaffected — still CC7 at the
    // synthesis level, not touched by this stage.
    Sf2ChannelMixer channelMixer;

    // ── Pitch shift render buffer (SFZ only) ──────────────────────────────────
    // sfizz renders into pitchL/R at an oversampled or undersampled block size,
    // then a linear interpolating resampler writes the pitch-shifted result into
    // scratchL/R at the true block size.
    std::vector<float> pitchBufL, pitchBufR;

    /** Apply a semitone pitch shift to src (srcLen samples) into dst (dstLen
     *  samples) using linear interpolation.  srcLen/dstLen == ratio == 2^(semi/12). */
    static void pitchShiftBlock (const float* src, float* dst,
                                  int srcLen, int dstLen) noexcept;

    // ── Post-processing Reverb EFX (juce::dsp::Reverb) ───────────────────────
    juce::dsp::Reverb dspReverb;

    std::atomic<float> reverbSize   { 50.0f };   // 0–100
    std::atomic<float> reverbDamp   { 50.0f };   // 0–100
    std::atomic<float> reverbWidth  { 50.0f };   // 0–100
    std::atomic<float> reverbMix    {  0.0f };   // 0–100 (default dry)
    std::atomic<bool>  reverbFreeze { false };

    void updateReverbParams();  ///< maps atomics → juce::dsp::Reverb::Parameters

    // ── SF2 filter atomics (UI-written, audio-thread-read; see applyFluidFilterFromUi) ──
    std::atomic<float> sf2FilterCutoffHz     { kSf2FilterCutoffDefaultHz };
    std::atomic<float> sf2FilterResonancePct { kSf2FilterResonanceDefaultPct };

    // ── Per-channel mixer strip state ─────────────────────────────────────────
    // Written on UI thread via setChannel*(); read on audio thread in applyDirtyStrips().
    struct ChannelStripAtomics
    {
        std::atomic<float> volume     { 1.0f };
        std::atomic<float> pan        { 0.0f };
        std::atomic<float> reverbSend { 0.0f };
        std::atomic<float> preMuteVol { 1.0f };
        std::atomic<bool>  muted      { false };
        std::atomic<bool>  dirty      { false };
    };
    ChannelStripAtomics channelStrips[16];
    std::atomic<bool>   anyStripDirty { false };

    void applyDirtyStrips();   ///< called at top of FluidSynth process block

    void measureChannelPeaks (int numSamples);   ///< called at end of SF2 render block

    // ── JUCE ADSR private state (audio-thread owned) ─────────────────────────
    juce::ADSR                 juceAdsr;
    juce::ADSR::Parameters     juceAdsrParams { 0.005f, 0.1f, 1.0f, 0.05f };
    std::atomic<bool>          juceAdsrParamsDirty   { false };
    std::atomic<float>         juceAdsrAttack        { 0.005f };
    std::atomic<float>         juceAdsrDecay         { 0.1f   };
    std::atomic<float>         juceAdsrSustain        { 1.0f   };
    std::atomic<float>         juceAdsrRelease        { 0.05f  };
    std::atomic<bool>          juceAdsrNoteOnPending  { false  };
    std::atomic<bool>          juceAdsrNoteOffPending { false  };
    std::atomic<bool>          juceAdsrActive         { false  };

    // ── Preview playhead position (UI display only) ──────────────────────────
    // Tracks elapsed samples since the most recent note-on for the waveform
    // LCD's playhead cursor. 0 = idle/not playing. Reset to 0 on note-on,
    // advanced each block in process() while the envelope is active, frozen
    // once the envelope goes idle (matches VoicePool::voicePositions semantics
    // — see WaveformView::drawPlaybackCursors's `pos <= 0.0f` idle check).
    std::atomic<int>           previewPositionSample  { 0 };

    // Note number behind previewPositionSample's region lookup (see
    // getLastTriggeredNote doc comment in the header). -1 = none yet.
    std::atomic<int>           lastTriggeredNote      { -1 };

    // Staging slot for juceAdsrNoteOn(int): set by the caller (UI-injection
    // path), consumed into lastTriggeredNote when juceAdsrNoteOnPending is
    // processed in process(). -1 = no note number supplied for this trigger.
    // (SFZ/sfizz branch only — the FluidSynth branch tracks note-on/off
    // directly off real MIDI events, see sf2ActiveNoteCount below.)
    std::atomic<int>           pendingTriggeredNote   { -1 };

    // ── FluidSynth branch note-on/off bookkeeping ─────────────────────────────
    // Incremented on each note-on, decremented on each note-off, dispatched
    // to FluidSynth in the process() event loop. NOTE: previewPositionSample
    // (the UI playhead) no longer reads this directly — it uses
    // fluid_synth_get_active_voice_count(synth) instead, since that reports
    // true voice lifetime (including release tails and sustain/sostenuto
    // holds) rather than just note-on/off message counts. sf2ActiveNoteCount
    // is retained for anything that only needs to know "is a note currently
    // held down" rather than "is a voice still audibly sounding" (e.g.
    // lastTriggeredNote reset gating above).
    std::atomic<int>           sf2ActiveNoteCount     { 0 };

    /** Converts the current UI A/D/S/R atomics (juceAdsrAttack/Decay/Sustain/
     *  Release — same values the amp-envelope graph and LCD display) into
     *  FluidSynth generator units and writes them to all 16 channels, giving
     *  FluidSynth's own per-voice envelopes the UI-specified shape while
     *  keeping each voice's envelope independent of every other voice.
     *  Called after SF2 load and after any program/preset change (both reset
     *  a channel's generators back to the SoundFont's own defaults), and
     *  whenever the UI values themselves change. No-op for SFZ. */
    void applyFluidAdsrFromUi();

    /** Converts the current UI SF2 filter atomics (sf2FilterCutoffHz /
     *  sf2FilterResonancePct — the same values the Column 2 FILTER tab reads/
     *  writes) into FluidSynth generator units and writes them to GEN_FILTERFC
     *  / GEN_FILTERQ on channels 2-15, mirroring applyFluidAdsrFromUi() exactly
     *  (same channel scope, same reapplication points — program-select/
     *  program-change reset a channel's generators back to the SoundFont's
     *  own defaults for filter cutoff/resonance the same way they do for the
     *  envelope). Harmless no-op when FluidSynth is unavailable or no
     *  synthesizer instance exists. No-op for SFZ. */
    void applyFluidFilterFromUi();

    // ── Private helpers ───────────────────────────────────────────────────────
    void applyPendingLoad();             ///< called at top of process()
    void applyProgramChange();           ///< single-preset legacy (channel 0); called when programChangePending
    void applyPendingChannelChanges();   ///< multi-timbral; called when anyChannelDirty

    /** Send current ADSR atomics to sfizz via OSC messages (audio thread only). */
    void sendAdsrToSfizz();

    /** Build and post a fresh preset list after a successful sfont load.
     *  Called from the audio thread — no locks needed on write side. */
    void postPresetList();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SfzPlayer)
};
