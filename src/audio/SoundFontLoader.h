#pragma once
// =============================================================================
//  SoundFontLoader.h  —  SF2 / SFZ → DYSEKT-SF sample engine  (sfizz backend)
//  ─────────────────────────────────────────────────────────────────────────
//  Requires: sfizz linked in CMakeLists.txt and DYSEKT_HAS_SFIZZ=1 defined.
//
//  What it does
//  ─────────────
//  1. Opens the SF2/SFZ with sfizz on a background thread.
//  2. Discovers which MIDI notes have audio (fast probe pass).
//  3. Renders each active note (sustain + release tail) into its own buffer.
//  4. Silence-trims both ends of every note render.
//  5. Concatenates all note renders into one stereo AudioBuffer with small
//     silence gaps between notes.
//  6. Posts the buffer via the processor's completedLoadData atomic for
//     SoundFontLoadTarget::Slicer (same path as a normal WAV load), or via
//     completedLoadData2 for SoundFontLoadTarget::SfzPlayer2 (a separate,
//     visual-only preview buffer decoupled from the Slicer engine).
//  7. For SoundFontLoadTarget::Slicer, also posts matching slice positions +
//     MIDI notes via the pendingSfzSlices atomic so processBlock can create
//     them after apply. For SoundFontLoadTarget::SfzPlayer2, the same
//     per-note descriptors are instead posted via the pendingPreviewZones2
//     atomic into a SfzPreviewZoneStore — a read-only "preview zones"
//     overlay so the SFZ-PLAYER's waveform can show the same colored
//     per-note zone bands as the Slicer, without ever touching sliceManager.
//
//  Thread safety
//  ─────────────
//  Everything is posted through the same atomics the WAV loader uses, so no
//  extra synchronisation is needed.  The processor's processBlock already
//  polls completedLoadData every callback.
// =============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <atomic>
#include "SampleData.h"   // for INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
#include "SfzZoneColours.h"

#if DYSEKT_HAS_SFIZZ
  #include "../../sfizz/src/sfizz.h"
#endif

// Forward declaration — full definition is in PluginProcessor.h
class DysektProcessor;

// =============================================================================
//  SoundFontLoadTarget — which preview buffer a load() call should populate.
//  ─────────────────────────────────────────────────────────────────────────
//  Slicer     — posts to the processor's completedLoadData / pendingSfzSlices
//                atomics, same as a normal WAV load. This is the Slicer
//                engine's actual sample buffer (sampleData), used for
//                real-time playback/slicing.
//  SfzPlayer2 — posts to the processor's completedLoadData2 atomic for the
//                visual-only preview buffer (sampleData2) used by the
//                SFZ-PLAYER tab; it is never touched by any audio engine
//                (sfzPlayer2 has its own internal sfizz state for actual
//                playback). Also posts the same per-note descriptors via
//                pendingPreviewZones2 into a SfzPreviewZoneStore, so the
//                waveform can draw a read-only zone overlay — no slices
//                are ever created in sliceManager for this target.
//  SfPlayer   — posts to the processor's completedLoadData3 atomic for the
//                visual-only preview buffer (sampleData3) used by the
//                SF2-PLAYER tab. Mirrors SfzPlayer2 exactly, including the
//                pendingPreviewZones3 zone overlay. Note this still renders
//                via sfizz (which can load .sf2 files directly) rather than
//                the real FluidSynth engine sfzPlayer uses for live playback
//                — a deliberate display-accuracy tradeoff, not a live-audio
//                one; the waveform shown may not be bit-identical to what
//                FluidSynth actually plays.
// =============================================================================
enum class SoundFontLoadTarget { Slicer = 0, SfzPlayer2 = 1, SfPlayer = 2 };

// =============================================================================
class SoundFontLoader
{
public:
    explicit SoundFontLoader (DysektProcessor& p) : processor (p) {}

    // ── Public API (call from UI thread) ─────────────────────────────────────
    // Queues a background job; returns immediately.
    //
    // presetBank/presetProgram (SfPlayer target only): when presetProgram >= 0,
    // the job sends a bank-select (CC0/CC32) + program-change to the sfizz
    // synth right after sfizz_load_file(), BEFORE probing/rendering — so the
    // resulting sampleData3/previewZones3 reflect that specific preset's
    // regions rather than whatever preset sfizz defaults to on load. Leave
    // both at -1 (the default) to render the file's default preset, as before.
    void load (const juce::File& file, SoundFontLoadTarget target = SoundFontLoadTarget::Slicer,
               int presetBank = -1, int presetProgram = -1);

private:
    DysektProcessor& processor;

#if DYSEKT_HAS_SFIZZ
    // ── Background job ────────────────────────────────────────────────────────
    class LoadJob;
#endif
};

// =============================================================================
//  Per-note slice descriptor — carried through to processBlock
// =============================================================================
struct SfzSliceDescriptor
{
    int startSample = 0;
    int endSample   = 0;
    int midiNote    = 36;
    int loopStart   = -1;   // -1 = no loop; sample offset within the concatenated buffer
    int loopEnd     = -1;

    // ARGB colour of the <region>/zone this note belongs to (SfzPlayer2
    // target only; 0 = "unset", meaning the consumer should fall back to
    // its own default). Populated in finishAndPost() via
    // SfzZoneColours::zoneColourArgb(), matched by MIDI key range against
    // parseSfzAllRegionKeyRanges(). Stored as a raw ARGB uint32 rather than
    // juce::Colour so this POD struct stays trivially copyable across the
    // background-thread → processBlock hand-off.
    juce::uint32 zoneColourArgb = 0;
};

// Heap-allocated payload posted via pendingSfzSlices atomic.
// processBlock takes ownership, creates slices, then deletes it.
struct SfzSlicePayload
{
    std::vector<SfzSliceDescriptor> slices;
};

// Heap-allocated payload for a failed Slicer-target load, posted via
// ProcessorHandle::completedLoadFailure (see below). Defined here at
// top-level, rather than nested inside DysektProcessor as it originally
// was, because ProcessorHandle needs it and is itself built before
// DysektProcessor is a complete type. `kind` holds the underlying int
// value of DysektProcessor::LoadKind (LoadKindReplace/Relink/Trim) --
// comparisons against those enumerators still work via the usual
// enum<->int implicit conversion.
struct FailedLoadResult
{
    int        token { 0 };
    int        kind  { 0 };
    juce::File file;
};

// =============================================================================
//  SfzPreviewZoneStore — read-only "preview zones" for the SFZ-PLAYER tab.
//  ─────────────────────────────────────────────────────────────────────────
//  Holds the same per-note descriptors as SfzSlicePayload, but for display
//  only — there is no sliceManager involvement, no editing, no playback
//  binding. The UI thread reads a snapshot every paint(); processBlock
//  publishes a new one whenever a SfzPlayer2-target load completes.
//
//  Uses the same atomic-shared-ptr snapshot idiom as SampleData (see
//  INTERSECT_HAS_STD_ATOMIC_SHARED_PTR in SampleData.h) so reads from the UI
//  thread never race with a concurrent publish from processBlock.
// =============================================================================
class SfzPreviewZoneStore
{
public:
    using ZoneList    = std::vector<SfzSliceDescriptor>;
    using SnapshotPtr = std::shared_ptr<const ZoneList>;

    /** Publish a new zone list (UI/audio thread — called from processBlock
     *  after consuming pendingPreviewZones2). Takes ownership. */
    void set (std::unique_ptr<ZoneList> zones)
    {
        SnapshotPtr view = std::move (zones);

#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
        snapshot.store (view, std::memory_order_release);
#else
        std::atomic_store_explicit (&snapshot, view, std::memory_order_release);
#endif
    }

    /** Read the current zone list (UI thread, called from paint()). May be
     *  empty (but never null) if no SfzPlayer2-target load has completed yet. */
    SnapshotPtr get() const
    {
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
        auto view = snapshot.load (std::memory_order_acquire);
#else
        auto view = std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
#endif
        if (view == nullptr)
            view = std::make_shared<const ZoneList>();
        return view;
    }

private:
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
    std::atomic<SnapshotPtr> snapshot;
#else
    SnapshotPtr snapshot;
#endif
};

// Heap-allocated payload posted via pendingPreviewZones2/pendingPreviewZones3
// atomics. processBlock takes ownership: for pendingPreviewZones2 (SFZ-PLAYER),
// each descriptor becomes a real slice in sliceManager2 (see Slice::nextSliceIdx
// for the loop-region two-slice split); for pendingPreviewZones3 (SF2-PLAYER),
// it's folded into the previewZones3 read-only display overlay. Either way the
// unique_ptr is destroyed once consumed — no manual delete needed, unlike
// SfzSlicePayload (which predates this and still uses raw new/delete).
struct SfzPreviewZonePayload
{
    std::vector<SfzSliceDescriptor> slices;

    // Which preset these zones/sampleData3 belong to (SfPlayer target only).
    // -1/-1 means "the file's default preset" (SfzPlayer2 target, or an
    // SfPlayer load that didn't request a specific preset). Consumed in
    // processBlock to update DysektProcessor::sf2PreviewRenderedBank/Program.
    int presetBank    = -1;
    int presetProgram = -1;
};

// =============================================================================
//  ProcessorHandle — shared, heap-allocated state that outlives DysektProcessor.
//  ─────────────────────────────────────────────────────────────────────────
//  Root cause this exists to fix: SoundFontLoader::LoadJob used to hold a raw
//  `DysektProcessor&` and write through it from a background ThreadPoolJob.
//  fluid_synth_sfload()/sfizz_load_file() are blocking, uncancellable disk-I/O
//  calls -- runJob() has no way to check shouldExit() while inside one. If the
//  plugin/editor is torn down while a load is still stuck in one of those
//  calls, ~DysektProcessor()'s fileLoadPool.removeAllJobs(true, 5000) can time
//  out after 5s while the job is still running; DysektProcessor and everything
//  it owns is then destroyed while the job, unaware, resumes and writes
//  through `processor` into freed memory. That's a delayed use-after-free --
//  it doesn't necessarily crash on the write itself, it typically surfaces a
//  beat later at an unrelated alloc/free, which matches the "stops dead after
//  2 log lines, no crash-handler entry" pattern this was diagnosed from.
//
//  Fix, mirroring SfzPlayer::BuildState (see SfzPlayer.h, which had the
//  identical bug in SynthBuildJob and was already fixed this way): everything
//  LoadJob needs to post its results lives in this struct instead, owned via
//  shared_ptr. DysektProcessor holds one reference (see soundFontProcessorHandle
//  in PluginProcessor.h, whose fields are then aliased back onto same-named
//  reference members so every existing "processor.mainLoadInFlight" style call
//  site elsewhere in the codebase -- SliceWaveformLcd, FileBrowserPanel,
//  WaveformView, Sf2WaveformLcd, PluginProcessor.cpp itself -- keeps compiling
//  and working unchanged). Every LoadJob holds its own independent shared_ptr,
//  captured by value at construction (never a pointer back to DysektProcessor).
//  DysektProcessor can now be destroyed at any time: an orphaned job simply
//  keeps this struct alive via its own reference until it finishes, then drops
//  it, and the struct is freed then -- nothing it does after that point ever
//  dereferences the (by-then-gone) DysektProcessor.
//
//  Scope note: LoadJob's runJobSfizz() also calls processor.sfzPlayer/
//  sfzPlayer2.setLoopPoints() directly (Step 3b) -- those write into small
//  atomics owned by the live SfzPlayer engine objects themselves, not by
//  DysektProcessor, and those objects are NOT moved into this handle (they're
//  full audio-engine instances referenced throughout MIDI routing/mixing, well
//  outside this fix's blast radius). processorAlive below is a best-effort
//  mitigation for just those call sites, not a full fix -- see its comment.
// =============================================================================
struct ProcessorHandle
{
    // ── Result atomics -- identical storage processBlock already polls ──────
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
    std::atomic<SampleData::SnapshotPtr> completedLoadData;
    std::atomic<SampleData::SnapshotPtr> completedLoadData2;
    std::atomic<SampleData::SnapshotPtr> completedLoadData3;
#else
    SampleData::SnapshotPtr              completedLoadData;
    SampleData::SnapshotPtr              completedLoadData2;
    SampleData::SnapshotPtr              completedLoadData3;
#endif

    std::atomic<FailedLoadResult*>      completedLoadFailure { nullptr };
    std::atomic<SfzSlicePayload*>       pendingSfzSlices      { nullptr };
    std::atomic<SfzPreviewZonePayload*> pendingPreviewZones2  { nullptr };
    std::atomic<SfzPreviewZonePayload*> pendingPreviewZones3  { nullptr };

    // Staleness-check tokens LoadJob's finishAndPost() reads on the
    // background thread. (nextLoadToken/latestLoadToken/nextPreviewToken2/3
    // are only ever touched from SoundFontLoader::load() on the message
    // thread, so they stay put as ordinary DysektProcessor members -- no
    // background-thread access, no UAF risk, no need to move them here.)
    std::atomic<int> latestLoadKind      { 0 };   // DysektProcessor::LoadKindReplace
    std::atomic<int> latestPreviewToken2 { 0 };
    std::atomic<int> latestPreviewToken3 { 0 };

    std::atomic<bool> mainLoadInFlight            { false };
    std::atomic<bool> sf2PreviewRenderInFlight     { false };
    std::atomic<bool> sfzPlayer2RebuildInFlight    { false };
    std::atomic<bool> sfzPlayer2LcdRebuildInFlight { false };

    // Set false as the very first statement in ~DysektProcessor(), before
    // fileLoadPool.removeAllJobs() starts waiting. LoadJob checks this
    // immediately after each blocking, uncancellable call
    // (fluid_synth_sfload/sfizz_load_file) returns, before touching
    // sfzPlayer/sfzPlayer2 directly. This narrows what used to be a
    // routinely-hittable ~5-second window down to the handful of
    // instructions between the destructor's first line and its members
    // actually being torn down -- a large practical improvement, but NOT a
    // formal guarantee the way the atomics above are (those are now safe
    // unconditionally, regardless of timing, because LoadJob writes into
    // memory this handle itself keeps alive). A fully airtight fix for the
    // setLoopPoints() call sites would need sfzPlayer/sfzPlayer2's loop-point
    // storage moved into their own BuildState the same way this struct now
    // holds everything else.
    std::atomic<bool> processorAlive { true };

    // ── Diagnostic logging, independent of CrashLogger's lifetime ──────────
    // A second FileLogger instance appending to the same crash-log file
    // DysektProcessor::crashLogger already writes to (see initLogger()
    // below), so LoadJob's log() calls never depend on that member -- or
    // DysektProcessor itself -- surviving. Two FileLogger instances on the
    // same file can interleave a line at startup; harmless for diagnostics.
    std::unique_ptr<juce::FileLogger> logger;
    juce::CriticalSection             loggerLock;

    void initLogger (const juce::File& logFile)
    {
        const juce::ScopedLock sl (loggerLock);
        logger = std::make_unique<juce::FileLogger> (logFile, "SoundFontLoader job log", 0);
    }

    void log (const juce::String& message)
    {
        const juce::ScopedLock sl (loggerLock);
        if (logger != nullptr)
            logger->logMessage (message);
    }

    // ── Result-atomic exchange helpers ──────────────────────────────────────
    // Same logic as DysektProcessor::exchangeCompletedLoadData()/2()/3()
    // (which now just forward here), pulled onto ProcessorHandle itself so
    // LoadJob can call these without needing a live DysektProcessor at all.
    SampleData::SnapshotPtr exchangeCompletedLoadData (SampleData::SnapshotPtr newValue)
    {
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
        return completedLoadData.exchange (std::move (newValue), std::memory_order_acq_rel);
#else
        return std::atomic_exchange_explicit (&completedLoadData, std::move (newValue),
                                              std::memory_order_acq_rel);
#endif
    }

    SampleData::SnapshotPtr exchangeCompletedLoadData2 (SampleData::SnapshotPtr newValue)
    {
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
        return completedLoadData2.exchange (std::move (newValue), std::memory_order_acq_rel);
#else
        return std::atomic_exchange_explicit (&completedLoadData2, std::move (newValue),
                                              std::memory_order_acq_rel);
#endif
    }

    SampleData::SnapshotPtr exchangeCompletedLoadData3 (SampleData::SnapshotPtr newValue)
    {
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
        return completedLoadData3.exchange (std::move (newValue), std::memory_order_acq_rel);
#else
        return std::atomic_exchange_explicit (&completedLoadData3, std::move (newValue),
                                              std::memory_order_acq_rel);
#endif
    }
};
