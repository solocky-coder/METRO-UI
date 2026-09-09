// SequencerEngine.cpp
// tracktion Engine/Edit/Transport layer has been removed.
// All note scheduling is done by DYSEKT-SF's own tick scheduler in processBlock().
// MidiClip.cpp retains its tracktion hook (attachMidiList) for future use.
//
// Threading model (audio-thread hot path):
//   - The track list (which tracks exist, in what order) is an immutable,
//     atomically-swapped snapshot: Impl::TrackList behind
//     std::atomic<std::shared_ptr<const TrackList>>. Structural edits
//     (add/remove/reorder track) build a new vector and publish it with one
//     atomic store; processBlock() does one atomic load per block and works
//     from that local snapshot for the whole block. No lock is taken.
//   - Each SequencerTrack's clip list is the same pattern one level down
//     (see SequencerTrack::getClips()/addClip()/removeClip()).
//   - Recording never touches MidiClip from the audio thread. It resolves a
//     clip-relative tick and pushes a small POD event into a lock-free FIFO;
//     drainRecordedEvents(), called from the message thread (ArrangeView's
//     Timer), is the only place that still calls MidiClip::addNote()/
//     setNoteDuration() for recording.

#include "SequencerEngine.h"
#include "../audio/SfzPlayer.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <array>
#include <vector>
#include <memory>

// Stream version tags
static constexpr int kStreamVersion1 = 1;  // legacy single-clip
static constexpr int kStreamVersion2 = 2;  // multi-clip
static constexpr int kStreamVersion3 = 3;  // + per-track solo/volumeDb/pan
static constexpr int kStreamVersion4 = 4;  // + stable network source key

//==============================================================================
struct SequencerEngine::Impl
{
    //==========================================================================
    //  Track list — copy-on-write, atomically swapped (see file header).
    //==========================================================================
    using TrackList = std::vector<std::shared_ptr<SequencerTrack>>;

    std::atomic<std::shared_ptr<const TrackList>> currentTracks
        { std::make_shared<const TrackList>() };

    std::shared_ptr<const TrackList> getTracks() const noexcept
    {
        return currentTracks.load (std::memory_order_acquire);
    }

    void publishTracks (std::shared_ptr<const TrackList> next)
    {
        currentTracks.store (std::move (next), std::memory_order_release);
    }

    double               currentTick = 0.0;
    std::atomic<int64_t> playheadTick { 0 };
    std::atomic<int64_t> loopStartTick { 0 };
    // 0 means use the current project end, preserving the legacy full-song loop.
    std::atomic<int64_t> loopEndTick   { 0 };

    struct ActiveNote { int trackIdx; int clipIdx; int note; int channel; };
    juce::Array<ActiveNote> activeNotes;

    std::atomic<bool>    playing      { false };
    std::atomic<bool>    recording    { false };
    // Recording is armed immediately, but actual capture waits for the
    // configured count-in. The audio thread owns the countdown state.
    std::atomic<int>     countInBars { 1 };
    std::atomic<bool>    countInActive { false };
    std::atomic<int64_t> countInRemainingTicks { 0 };
    int64_t countInElapsedTicks = 0;
    int     countInNextBeat = 0;
    std::atomic<bool>    looping      { true  };
    std::atomic<bool>    pendingPlay  { false };
    std::atomic<bool>    pendingStop  { false };
    std::atomic<bool>    pendingRewind{ false };
    std::atomic<bool>    pendingSeek  { false };
    std::atomic<int64_t> pendingSeekTick { 0 };

    // Set by play() instead of pendingPlay when Link is enabled, so the
    // audio thread can hold off flipping to "playing" until the requested
    // beat-aligned quantum boundary actually arrives (see processBlock()).
    // Only ever touched on the audio thread once set, aside from the
    // message-thread writes in play()/stop() below.
    std::atomic<bool>    awaitingLinkStart { false };
    double               linkPhasePrev     = 0.0;
    bool                 linkPhasePrimed   = false;
    // Last-observed Link session play state, used purely to edge-detect a
    // remote peer's Play/Stop in processBlock() (see isSessionPlaying()).
    bool                 lastLinkSessionPlaying = false;
    // Gates the remote-peer-reaction block below. Independent of
    // AbletonLink::isEnabled() (tempo sync) — see
    // SequencerEngine::setLinkFollowsTransport().
    std::atomic<bool>    linkFollowsTransport { false };
    // Set (message thread) whenever setLinkFollowsTransport() is called, so
    // processBlock() (audio thread) can re-baseline lastLinkSessionPlaying
    // itself instead of that plain bool being written cross-thread directly.
    std::atomic<bool>    linkFollowBaselineDirty { false };
    std::atomic<float>   internalBpm  { 120.f };
    std::atomic<float>   hostBpm      { 120.f };
    std::atomic<bool>    syncToHost   { false };
    float                lastAppliedBpm = 0.f;

    AbletonLink* abletonLink = nullptr;
    SfzPlayer*   sfzPlayer   = nullptr;

    std::atomic<bool> midiActivityFlags[SequencerEngine::kActivityFlagCount] = {};
    std::atomic<int>  selectedLiveChannel  { 0 };  // 1-based; 0 = disabled
    std::atomic<SelectedLiveTarget> selectedLiveTarget {}; // player + channel for the selected track
    std::atomic<int>  recordingTrackIndex  { -1 }; // which track receives recorded MIDI (-1 = none)
    std::atomic<SequencerEngine::RecordMode> recordMode { SequencerEngine::RecordMode::Overdub };

    //==========================================================================
    //  Recorded-note FIFO  (audio thread writes, message thread drains)
    //
    //  Same shape as DysektProcessor's commandFifo, just flowing the other
    //  direction: audio thread -> message thread instead of message thread ->
    //  audio thread. Each entry carries an absolute (global) tick — the
    //  audio thread no longer needs a target clip to exist at all, since
    //  drainRecordedEvents() now creates/grows the destination clip itself
    //  (Cubase-style: a clip is drawn the moment the first note-on arrives
    //  on an armed+playing track, and grows to follow the playhead from
    //  there). Resolving to clip-relative coordinates happens on the
    //  message thread, once the target clip is known.
    //==========================================================================
    struct RecordedNoteEvent
    {
        int     trackIndex = -1;
        int     note       = 60;
        int     velocity   = 100;
        bool    isNoteOn   = true;
        int64_t tick       = 0;   // absolute/global tick, resolved at capture time
    };

    static constexpr int kRecordFifoSize = 1024;
    juce::AbstractFifo recordFifo { kRecordFifoSize };
    std::array<RecordedNoteEvent, kRecordFifoSize> recordBuffer;
    std::atomic<int> droppedRecordEventCount { 0 };  // diagnostic only

    /** Audio-thread only. */
    void pushRecordedEvent (const RecordedNoteEvent& ev)
    {
        const auto scope = recordFifo.write (1);
        if (scope.blockSize1 > 0)
            recordBuffer[(size_t) scope.startIndex1] = ev;
        else if (scope.blockSize2 > 0)
            recordBuffer[(size_t) scope.startIndex2] = ev;
        else
            droppedRecordEventCount.fetch_add (1, std::memory_order_relaxed);  // FIFO full — dropped
    }

    // Tracks notes currently held during recording so we can set their real
    // duration when the note-off arrives. MESSAGE-THREAD ONLY — used
    // exclusively by SequencerEngine::drainRecordedEvents().
    struct OpenRecNote { int trackIndex; int note; int64_t startTick; int noteIndexInClip; };
    juce::Array<OpenRecNote> openRecNotes;

    // The clip currently being drawn by live MIDI recording, if any.
    // MESSAGE-THREAD ONLY — created, grown, and torn down exclusively by
    // SequencerEngine::drainRecordedEvents(). Cubase-style: nothing exists
    // until the first note-on arrives on an armed+playing track, at which
    // point a clip starts right at that tick and drainRecordedEvents()
    // stretches its length every call to keep its end following the
    // playhead, until recording stops or the armed track changes.
    struct LiveRecordClip
    {
        bool active         = false;
        int  trackIndex     = -1;
        std::shared_ptr<ClipSlot> slot;
    } liveRecordClip;

    //==========================================================================
    Impl()
    {
        openRecNotes.ensureStorageAllocated (64);
    }
    ~Impl() = default;

    //==========================================================================
    static int midiChannelForTrack (const SequencerTrack& t) noexcept
    {
        switch (t.type)
        {
            case TrackType::MainSlice:      return 1;
            case TrackType::ChromaticSlice: return t.midiChannel.load (std::memory_order_relaxed) + 1;
            case TrackType::SfPlayer:       return t.midiChannel.load (std::memory_order_relaxed) + 1;
        }
        return 1;
    }

    /** Compute the global end tick = rightmost clip end across all tracks. */
    static int64_t computeMasterLenFor (const TrackList& tracks)
    {
        int64_t masterLen = MidiClip::kPPQ * 4 * 4;
        for (auto& track : tracks)
            for (auto& slot : *track->getClips())
                masterLen = juce::jmax (masterLen, slot->endTick());
        return masterLen;
    }

    // Pre-create the live-record clip before the audio thread reaches the
    // first block of a recording pass. This is important at tick 0: if the
    // first note creates the clip only from drainRecordedEvents(), that
    // message-thread callback runs after the audio block containing tick 0,
    // so playback would never see the note at the start boundary until the
    // next loop.
    void beginLiveRecordClip (int trackIndex)
    {
        if (liveRecordClip.active
            || ! juce::isPositiveAndBelow (trackIndex, (int) currentTracks.load()->size()))
            return;

        auto tracks = getTracks();
        if (! juce::isPositiveAndBelow (trackIndex, (int) tracks->size())) return;

        auto& track = *(*tracks)[(size_t) trackIndex];
        const int64_t globalTick = (int64_t) currentTick;
        const auto mode = recordMode.load (std::memory_order_relaxed);

        if (mode == SequencerEngine::RecordMode::Overdub)
        {
            auto clipsSnap = track.getClips();
            for (auto& existing : *clipsSnap)
            {
                if (globalTick >= existing->getStartTick()
                    && globalTick < existing->endTick())
                {
                    liveRecordClip = { true, trackIndex, existing };
                    return;
                }
            }
        }

        const int newIdx = track.addClip (globalTick, MidiClip::kPPQ / 4);
        auto newSlot = track.getClipSlot (newIdx);
        liveRecordClip = { true, trackIndex, newSlot };
    }

    //==========================================================================
    //  Audio-thread note rendering — processes one ClipSlot
    //==========================================================================
    void processClipSlot (juce::MidiBuffer& outMidi, SequencerTrack& track,
                          int trackIdx, int clipIdx, const ClipSlot& slot,
                          double playheadStart, double playheadEnd,
                          int numSamples, double ticksPerSample, bool doLoop,
                          int64_t loopStart, int64_t loopEnd)
    {
        const int ch = midiChannelForTrack (track);
        const double clipStart = (double) slot.getStartTick();
        const double clipEnd   = (double) slot.endTick();

        double localStart = playheadStart;
        double localEnd   = playheadEnd;

        if (doLoop && loopEnd > loopStart)
        {
            const double loopLength = (double) (loopEnd - loopStart);
            localStart = (double) loopStart + std::fmod (localStart - (double) loopStart, loopLength);
            if (localStart < (double) loopStart) localStart += loopLength;
            localEnd = localStart + (playheadEnd - playheadStart);
        }

        if (localEnd <= clipStart || localStart >= clipEnd)
            return;

        const double winStart = juce::jmax (localStart, clipStart);
        const double winEnd   = juce::jmin (localEnd,   clipEnd);

        const double clipLocalStart = winStart - clipStart;
        const double clipLocalEnd   = winEnd   - clipStart;

        const juce::ScopedReadLock cl (slot.clip.getLock());
        for (const auto& n : slot.clip.getNotes())
        {
            const double nStart = (double) n.startTick;
            const double nEnd   = (double) n.endTick();

            if (nStart >= clipLocalStart && nStart < clipLocalEnd)
            {
                const int sp = juce::jlimit (0, numSamples - 1,
                    (int)((nStart - clipLocalStart + (winStart - localStart)) / ticksPerSample));
                outMidi.addEvent (
                    juce::MidiMessage::noteOn (ch, n.note, (juce::uint8) n.velocity), sp);
                activeNotes.add ({ trackIdx, clipIdx, n.note, ch });
            }

            if (nEnd > clipLocalStart && nEnd <= clipLocalEnd)
            {
                const int sp = juce::jlimit (0, numSamples - 1,
                    (int)((nEnd - clipLocalStart + (winStart - localStart)) / ticksPerSample));
                outMidi.addEvent (juce::MidiMessage::noteOff (ch, n.note), sp);
                for (int i = activeNotes.size() - 1; i >= 0; --i)
                    if (activeNotes[i].trackIdx == trackIdx
                        && activeNotes[i].clipIdx == clipIdx
                        && activeNotes[i].note == n.note)
                        { activeNotes.remove (i); break; }
            }
        }
    }

    void flushAllActiveNotes (juce::MidiBuffer& outMidi, int samplePos)
    {
        for (const auto& an : activeNotes)
            outMidi.addEvent (juce::MidiMessage::noteOff (an.channel, an.note), samplePos);
        activeNotes.clear();
    }
};

//==============================================================================
SequencerEngine::SequencerEngine()  : impl (std::make_unique<Impl>()) {}
SequencerEngine::~SequencerEngine() = default;

//==============================================================================
bool    SequencerEngine::isPlaying()        const noexcept { return impl->playing.load   (std::memory_order_relaxed); }
bool    SequencerEngine::isLooping()        const noexcept { return impl->looping.load   (std::memory_order_relaxed); }
bool    SequencerEngine::isRecording()      const noexcept { return impl->recording.load (std::memory_order_relaxed); }
int     SequencerEngine::getCountInBars()   const noexcept { return impl->countInBars.load (std::memory_order_relaxed); }
void    SequencerEngine::setCountInBars (int bars) noexcept
{
    impl->countInBars.store (juce::jlimit (1, 2, bars), std::memory_order_relaxed);
}
int64_t SequencerEngine::getPlayheadTick()  const noexcept { return impl->playheadTick.load (std::memory_order_relaxed); }
double  SequencerEngine::getPlayheadBeats() const noexcept { return (double) getPlayheadTick() / (double) MidiClip::kPPQ; }
float   SequencerEngine::getBpm()           const noexcept { return impl->internalBpm.load (std::memory_order_relaxed); }
bool    SequencerEngine::getSyncToHost()    const noexcept { return impl->syncToHost.load (std::memory_order_relaxed); }

int64_t SequencerEngine::getLengthTicks() const noexcept
{
    auto snap = impl->getTracks();
    return Impl::computeMasterLenFor (*snap);
}

int64_t SequencerEngine::getLoopStartTick() const noexcept
{
    return impl->loopStartTick.load (std::memory_order_relaxed);
}

int64_t SequencerEngine::getLoopEndTick() const noexcept
{
    const auto configured = impl->loopEndTick.load (std::memory_order_relaxed);
    return configured > 0 ? configured : getLengthTicks();
}

//==============================================================================
void SequencerEngine::play()
{

    if (impl->abletonLink != nullptr && impl->abletonLink->isEnabled())
    {
        // Ask Link peers to start on the next bar, and actually wait for
        // that same boundary ourselves instead of starting immediately —
        // processBlock() flips pendingPlay only once the phase wraps.
        impl->abletonLink->requestBeatAlignedStart (4.0);
        impl->linkPhasePrimed = false;
        // release: pairs with the acquire load in processBlock() below, so the
        // audio thread is guaranteed to see the linkPhasePrimed write above
        // before it observes awaitingLinkStart == true. Without this pairing,
        // linkPhasePrimed/linkPhasePrev are plain (non-atomic) fields written
        // on this (message) thread and read/written on the audio thread with
        // no synchronizes-with relationship between them — a data race.
        impl->awaitingLinkStart.store (true, std::memory_order_release);
    }
    else
    {
        impl->pendingPlay.store (true, std::memory_order_relaxed);
    }
}

void SequencerEngine::stop()
{
    impl->countInActive.store (false, std::memory_order_relaxed);
    impl->countInRemainingTicks.store (0, std::memory_order_relaxed);
    if (impl->abletonLink != nullptr && impl->abletonLink->isEnabled())
        impl->abletonLink->notifyStop();
    impl->awaitingLinkStart.store (false, std::memory_order_relaxed);
    impl->pendingStop.store (true, std::memory_order_relaxed);
}

void SequencerEngine::rewind()
{
    impl->pendingRewind.store (true, std::memory_order_relaxed);
}

void SequencerEngine::setLooping (bool v)
{
    impl->looping.store (v, std::memory_order_relaxed);
}

void SequencerEngine::setRecording (bool v)
{
    impl->recording.store (v, std::memory_order_relaxed);

    // When recording is armed while transport is already running, establish
    // the destination clip immediately so a note landing exactly on the
    // current playhead (especially bar 1 / tick 0) exists before that audio
    // block is rendered. If transport starts after arming, play() performs
    // the same pre-creation step.
    if (! v)
    {
        impl->countInActive.store (false, std::memory_order_relaxed);
        impl->countInRemainingTicks.store (0, std::memory_order_relaxed);
        impl->liveRecordClip = {};
    }
}

void SequencerEngine::setRecordMode (RecordMode m) noexcept { impl->recordMode.store (m, std::memory_order_relaxed); }
SequencerEngine::RecordMode SequencerEngine::getRecordMode() const noexcept { return impl->recordMode.load (std::memory_order_relaxed); }
void SequencerEngine::setSyncToHost (bool v) { impl->syncToHost.store (v, std::memory_order_relaxed); }

void SequencerEngine::setBpm (float b)
{
    const float clamped = juce::jlimit (20.f, 999.f, b);
    impl->internalBpm.store (clamped, std::memory_order_relaxed);
    if (impl->abletonLink != nullptr && impl->abletonLink->isEnabled())
        impl->abletonLink->setBpm ((double) clamped);
}

void SequencerEngine::setHostBpm (float b)
{
    impl->hostBpm.store (juce::jlimit (20.f, 999.f, b), std::memory_order_relaxed);
}

void SequencerEngine::seekToTick (int64_t tick)
{
    impl->pendingSeekTick.store (tick, std::memory_order_relaxed);
    impl->pendingSeek    .store (true, std::memory_order_relaxed);
}

void SequencerEngine::setLoopRange (int64_t startTick, int64_t endTick)
{
    const auto start = juce::jmax ((int64_t) 0, startTick);
    const auto end = juce::jmax (start + (int64_t) MidiClip::kPPQ, endTick);
    impl->loopStartTick.store (start, std::memory_order_relaxed);
    impl->loopEndTick.store (end, std::memory_order_relaxed);
}

void SequencerEngine::resetLoopRangeToDefault()
{
    // 0 is the documented sentinel for "always track current project end"
    // (see getLoopEndTick()) — store it directly rather than routing through
    // setLoopRange(), which clamps endTick to a concrete captured length and
    // would freeze the loop at whatever the arrangement length happened to
    // be at reset time.
    impl->loopStartTick.store (0, std::memory_order_relaxed);
    impl->loopEndTick.store   (0, std::memory_order_relaxed);
}

void SequencerEngine::setLengthTicks (int64_t ticks)
{
    const int64_t clamped = juce::jmax ((int64_t) MidiClip::kPPQ, ticks);
    auto snap = impl->getTracks();
    for (auto& t : *snap)
    {
        auto slot = t->getClipSlot (0);
        if (slot != nullptr)
            slot->clip.setLengthTicks (clamped);
    }
}

//==============================================================================
//  Track management
//==============================================================================
int SequencerEngine::getNumTracks() const
{
    return (int) impl->getTracks()->size();
}

SequencerTrackInfo SequencerEngine::getTrackInfo (int i) const
{
    auto snap = impl->getTracks();
    if (! juce::isPositiveAndBelow (i, (int) snap->size())) return {};
    const auto& t = *(*snap)[(size_t) i];
    SequencerTrackInfo info;
    info.type        = t.type;
    info.enabled     = t.enabled.load (std::memory_order_relaxed);
    info.solo        = t.solo.load (std::memory_order_relaxed);
    info.volumeDb    = t.volumeDb.load (std::memory_order_relaxed);
    info.pan         = t.pan.load (std::memory_order_relaxed);
    info.name        = t.name;
    info.colour      = t.colour;
    info.sliceIdx    = t.sliceIdx;
    info.midiChannel = t.midiChannel.load (std::memory_order_relaxed);
    info.preset      = t.preset;
    info.numClips    = t.getNumClips();
    info.isSfzInstrument = t.isSfzInstrument;
    info.networkRouteId = t.networkRouteId;
    info.networkSourceKey = t.networkSourceKey;
    info.networkSourceId = t.networkSourceId;
    info.networkSourceChannel = t.networkSourceChannel;
    return info;
}

void SequencerEngine::setTrackEnabled (int i, bool enabled)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (i, (int) snap->size()))
        (*snap)[(size_t) i]->enabled.store (enabled, std::memory_order_relaxed);
}

int SequencerEngine::findSfTrackForChannel (int midiChannel0Based) const
{
    if (midiChannel0Based < 0 || midiChannel0Based > 15) return -1;
    auto snap = impl->getTracks();
    for (size_t i = 0; i < snap->size(); ++i)
    {
        const auto& t = *(*snap)[i];
        if (t.type == TrackType::SfPlayer && ! t.isSfzInstrument
            && t.midiChannel.load (std::memory_order_relaxed) == midiChannel0Based)
            return (int) i;
    }
    return -1;
}

void SequencerEngine::setTrackSolo (int i, bool solo)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (i, (int) snap->size()))
        (*snap)[(size_t) i]->solo.store (solo, std::memory_order_relaxed);
}

void SequencerEngine::setTrackVolumeDb (int i, float volumeDb)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (i, (int) snap->size()))
        (*snap)[(size_t) i]->volumeDb.store (volumeDb, std::memory_order_relaxed);
}

void SequencerEngine::setTrackPan (int i, float pan)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (i, (int) snap->size()))
        (*snap)[(size_t) i]->pan.store (pan, std::memory_order_relaxed);
}

void SequencerEngine::setTrackColour (int i, juce::Colour colour)
{
    // Plain assignment, not .store() — colour is a non-atomic juce::Colour
    // field, safe to mutate here only because this method (like every
    // other reader/writer of it) runs on the message thread exclusively.
    // See SequencerTrack::colour's declaration comment.
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (i, (int) snap->size()))
        (*snap)[(size_t) i]->colour = colour;
}

void SequencerEngine::addMainTrack()
{
    auto current = impl->getTracks();
    if (! current->empty()) return;
    auto next = std::make_shared<Impl::TrackList> (*current);
    next->push_back (SequencerTrack::makeMain());
    impl->publishTracks (std::move (next));
}

void SequencerEngine::addChromaticTrack (int sliceIdx, int chromaticChannel,
                                          const juce::String& name, juce::Colour colour)
{
    auto current = impl->getTracks();
    for (auto& t : *current)
        if (t->type == TrackType::ChromaticSlice && t->sliceIdx == sliceIdx) return;

    auto next = std::make_shared<Impl::TrackList> (*current);
    next->push_back (SequencerTrack::makeChromatic (sliceIdx, chromaticChannel, name, colour));
    impl->publishTracks (std::move (next));
}

void SequencerEngine::removeChromaticTrack (int sliceIdx)
{
    auto current = impl->getTracks();
    auto next = std::make_shared<Impl::TrackList> (*current);
    next->erase (std::remove_if (next->begin(), next->end(),
                    [sliceIdx] (const std::shared_ptr<SequencerTrack>& t)
                    { return t->type == TrackType::ChromaticSlice && t->sliceIdx == sliceIdx; }),
                 next->end());
    impl->publishTracks (std::move (next));
}

void SequencerEngine::addSfTrack (const Sf2PresetInfo& preset, juce::Colour colour)
{
    auto current = impl->getTracks();
    for (auto& t : *current)
        if (t->type == TrackType::SfPlayer
            && t->preset.bank   == preset.bank
            && t->preset.preset == preset.preset) return;

    // Assign the next available FluidSynth channel (0-15) to this track.
    // 0-based channels 0 and 1 (MIDI channels 1 and 2) are reserved for the
    // Slicer and SFZ-Player respectively — SF2 tracks must start at 0-based 2.
    int sfCh = 2;
    for (auto& t : *current)
        if (t->type == TrackType::SfPlayer)
            sfCh = juce::jmax (sfCh, t->midiChannel.load (std::memory_order_relaxed) + 1);
    sfCh = juce::jlimit (2, 15, sfCh);

    auto track = SequencerTrack::makeSfPlayer (preset, colour);
    track->midiChannel.store (sfCh, std::memory_order_relaxed);

    auto next = std::make_shared<Impl::TrackList> (*current);
    next->push_back (track);
    impl->publishTracks (std::move (next));

    if (impl->sfzPlayer != nullptr)
        impl->sfzPlayer->setPresetOnChannel (sfCh, preset.bank, preset.preset);
}

void SequencerEngine::removeSfTrack (int trackIndex)
{
    auto current = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) current->size())
        || (*current)[(size_t) trackIndex]->type != TrackType::SfPlayer)
        return;

    auto next = std::make_shared<Impl::TrackList> (*current);
    next->erase (next->begin() + trackIndex);
    impl->publishTracks (std::move (next));
}

void SequencerEngine::rebuildSfTracks (const std::vector<Sf2PresetInfo>& presets,
                                        const juce::Colour* palette, int paletteSize)
{
    auto current = impl->getTracks();
    auto next = std::make_shared<Impl::TrackList>();
    next->reserve (current->size() + presets.size());
    for (auto& t : *current)
        if (t->type != TrackType::SfPlayer)
            next->push_back (t);

    // Channels already claimed by ChromaticSlice tracks must never be handed
    // out to a new SF2 preset track — otherwise a chromatic slice's live and
    // recorded MIDI ends up sharing a channel with (and triggering) an SF2
    // preset. midiChannel on a ChromaticSlice track is stored 0-based.
    uint32_t chromaOccupied = 0u;
    for (auto& t : *current)
        if (t->type == TrackType::ChromaticSlice)
        {
            const int ch0 = t->midiChannel.load (std::memory_order_relaxed) & 0xF;
            chromaOccupied |= (1u << ch0);
        }

    // 0-based channels 0/1 (MIDI ch 1/2) are reserved for Slicer/SFZ-Player;
    // sequential FluidSynth channels for SF2 tracks start at 0-based 2 and
    // skip any channel already owned by a chromatic slice.
    std::vector<int> assignedChannels;
    assignedChannels.reserve (presets.size());
    int nextCh = 2;
    for (int i = 0; i < (int) presets.size(); ++i)
    {
        while (nextCh <= 15 && (chromaOccupied & (1u << nextCh)))
            ++nextCh;
        const int ch = juce::jmin (nextCh, 15);
        assignedChannels.push_back (ch);
        if (nextCh <= 15)
            ++nextCh;

        const juce::Colour col = paletteSize > 0
            ? palette[i % paletteSize] : juce::Colour (0xFF406080);
        auto track = SequencerTrack::makeSfPlayer (presets[i], col);
        track->midiChannel.store (ch, std::memory_order_relaxed);
        next->push_back (track);
    }

    impl->publishTracks (next);

    if (impl->sfzPlayer != nullptr)
        for (int i = 0; i < (int) presets.size(); ++i)
            impl->sfzPlayer->setPresetOnChannel (assignedChannels[(size_t) i],
                                                  presets[i].bank,
                                                  presets[i].preset);
}

void SequencerEngine::addOrUpdateSfTrackOnChannel (const Sf2PresetInfo& preset,
                                                    int midiChannel0Based,
                                                    juce::Colour colour)
{
    // 0-based channels 0/1 (MIDI ch 1/2) are reserved for Slicer/SFZ-Player;
    // SF2 tracks may only occupy 0-based channels 2-15.
    const int ch = juce::jlimit (2, 15, midiChannel0Based);
    bool needsPlayerUpdate = false;

    auto current = impl->getTracks();

    // If a track already exists for this preset, just update its channel —
    // mutated in place (atomic), no snapshot rebuild needed.
    for (auto& t : *current)
    {
        if (t->type == TrackType::SfPlayer
            && t->preset.bank   == preset.bank
            && t->preset.preset == preset.preset)
        {
            t->midiChannel.store (ch, std::memory_order_relaxed);
            needsPlayerUpdate = true;
            break;
        }
    }

    if (! needsPlayerUpdate)
    {
        // New track for this preset on the chosen channel.
        auto track = SequencerTrack::makeSfPlayer (preset, colour);
        track->midiChannel.store (ch, std::memory_order_relaxed);

        auto next = std::make_shared<Impl::TrackList> (*current);
        next->push_back (track);
        impl->publishTracks (std::move (next));
        needsPlayerUpdate = true;
    }

    if (needsPlayerUpdate && impl->sfzPlayer != nullptr)
        impl->sfzPlayer->setPresetOnChannel (ch, preset.bank, preset.preset);
}

void SequencerEngine::addSfzTrack (const juce::String& name, int midiChannel0Based,
                                    juce::Colour colour)
{
    const int ch = juce::jlimit (0, 15, midiChannel0Based);
    auto current = impl->getTracks();

    // Only one SFZ-instrument track ever exists. If it's already there,
    // update it in place — same shared_ptr, same track-list index — so a
    // channel change, loading a different .sfz file, or an Add Zone edit
    // to the currently-loaded MULTISAMPLER instrument doesn't disturb the
    // current selection. See the "sanctioned exception" note on
    // SequencerTrack::name/colour, and makeSfzInstrument()'s doc comment
    // for why both the browser-load and Add Zone workflows share this one
    // track instead of getting one each.
    for (auto& t : *current)
        if (t->type == TrackType::SfPlayer && t->isSfzInstrument)
        {
            t->name   = name;
            t->colour = colour;
            t->midiChannel.store (ch, std::memory_order_relaxed);
            return;
        }

    // Nothing to replace yet — either the first SFZ load this session, or
    // the MULTISAMPLER instrument just gained its first zone. Append fresh.
    auto track = SequencerTrack::makeSfzInstrument (name, colour);
    track->midiChannel.store (ch, std::memory_order_relaxed);

    auto next = std::make_shared<Impl::TrackList> (*current);
    next->push_back (track);
    impl->publishTracks (std::move (next));
}

void SequencerEngine::removeSfzTrack()
{
    auto current = impl->getTracks();
    auto next = std::make_shared<Impl::TrackList>();
    next->reserve (current->size());
    bool removedAny = false;
    for (auto& t : *current)
    {
        if (t->type == TrackType::SfPlayer && t->isSfzInstrument) { removedAny = true; continue; }
        next->push_back (t);
    }
    if (removedAny)
        impl->publishTracks (std::move (next));
}

//==============================================================================
//  Clip management
//==============================================================================
int SequencerEngine::getNumClips (int trackIndex) const
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
        return (*snap)[(size_t) trackIndex]->getNumClips();
    return 0;
}

SequencerClipInfo SequencerEngine::getClipInfo (int trackIndex, int clipIndex) const
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
    {
        auto slot = (*snap)[(size_t) trackIndex]->getClipSlot (clipIndex);
        if (slot != nullptr)
            return { slot->getStartTick(), slot->clip.getLengthTicks() };
    }
    return {};
}

MidiClip* SequencerEngine::getClip (int trackIndex, int clipIndex)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
    {
        auto slot = (*snap)[(size_t) trackIndex]->getClipSlot (clipIndex);
        if (slot != nullptr) return &slot->clip;
    }
    return nullptr;
}

MidiClip& SequencerEngine::getClip()
{
    auto snap = impl->getTracks();
    return (*snap)[0]->getClipSlot (0)->clip;
}

int SequencerEngine::addClip (int trackIndex, int64_t startTick, int64_t lengthTicks)
{
    auto snap = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) snap->size())) return -1;
    return (*snap)[(size_t) trackIndex]->addClip (startTick, lengthTicks);
}

void SequencerEngine::removeClip (int trackIndex, int clipIndex)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
        (*snap)[(size_t) trackIndex]->removeClip (clipIndex);
}

void SequencerEngine::setClipStartTick (int trackIndex, int clipIndex, int64_t newStartTick)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
    {
        auto& track = *(*snap)[(size_t) trackIndex];
        auto slot = track.getClipSlot (clipIndex);
        if (slot != nullptr)
        {
            slot->startTick.store (juce::jmax ((int64_t) 0, newStartTick), std::memory_order_relaxed);
            track.sortClips();
        }
    }
}

void SequencerEngine::setClipLengthTicks (int trackIndex, int clipIndex, int64_t newLength)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
    {
        auto slot = (*snap)[(size_t) trackIndex]->getClipSlot (clipIndex);
        if (slot != nullptr)
            slot->clip.setLengthTicks (juce::jmax ((int64_t) MidiClip::kPPQ, newLength));
    }
}

int64_t SequencerEngine::getTrackLengthTicks (int trackIndex) const noexcept
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
    {
        auto slot = (*snap)[(size_t) trackIndex]->getClipSlot (0);
        if (slot != nullptr) return slot->clip.getLengthTicks();
    }
    return MidiClip::kPPQ * 4 * 4;
}

void SequencerEngine::setTrackLengthTicks (int trackIndex, int64_t ticks)
{
    setClipLengthTicks (trackIndex, 0, ticks);
}

//==============================================================================
int SequencerEngine::addNetworkAudioTrack (int64_t routeId, int64_t sourceKey, int32_t sourceId, int sourceChannel,
                                                   const juce::String& sourceName, const juce::String& userName)
{
    auto current = impl->getTracks();
    auto input = NetworkAudioInput{};
    input.routeId = routeId;
    input.sourceKey = sourceKey;
    input.sourceId = sourceId;
    input.sourceChannel = sourceChannel;
    input.sourceName = sourceName;
    input.userName = userName;

    auto track = SequencerTrack::makeAudio (input);
    auto next = std::make_shared<Impl::TrackList> (*current);
    next->push_back (std::move (track));
    impl->publishTracks (std::move (next));
    return (int) current->size();
}

bool SequencerEngine::setNetworkAudioTrackRoute (int trackIndex, int64_t routeId, int64_t sourceKey, int32_t sourceId, int sourceChannel)
{
    auto current = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) current->size())) return false;
    auto& track = *(*current)[(size_t) trackIndex];
    if (track.type != TrackType::Audio) return false;
    // Route metadata is message-thread state, not structural track state. Keep
    // the immutable track-list snapshot intact and update only these fields.
    track.networkRouteId = routeId;
    track.networkSourceKey = sourceKey;
    track.networkSourceId = sourceId;
    track.networkSourceChannel = sourceChannel;
    return true;
}

bool SequencerEngine::isNetworkAudioTrack (int trackIndex) const noexcept
{
    auto current = impl->getTracks();
    return juce::isPositiveAndBelow (trackIndex, (int) current->size())
        && (*current)[(size_t) trackIndex]->type == TrackType::Audio;
}

bool SequencerEngine::getNetworkAudioRoute (int trackIndex, int64_t& routeId, int64_t& sourceKey, int32_t& sourceId, int& sourceChannel) const noexcept
{
    auto current = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) current->size())) return false;
    const auto& track = *(*current)[(size_t) trackIndex];
    if (track.type != TrackType::Audio) return false;
    routeId = track.networkRouteId; sourceKey = track.networkSourceKey; sourceId = track.networkSourceId; sourceChannel = track.networkSourceChannel;
    return true;
}

void SequencerEngine::setAbletonLink (AbletonLink* l) noexcept { impl->abletonLink = l; }

void SequencerEngine::setLinkFollowsTransport (bool shouldFollow) noexcept
{
    impl->linkFollowsTransport.store (shouldFollow, std::memory_order_relaxed);
    // Re-baseline on the audio thread (see processBlock()) rather than
    // touching the plain lastLinkSessionPlaying bool from here directly,
    // so toggling this doesn't immediately react to whatever the session's
    // play state already happens to be — only to a change from here on.
    impl->linkFollowBaselineDirty.store (true, std::memory_order_relaxed);
}

bool SequencerEngine::getLinkFollowsTransport() const noexcept
{
    return impl->linkFollowsTransport.load (std::memory_order_relaxed);
}
void SequencerEngine::setSfzPlayer   (SfzPlayer*   p) noexcept { impl->sfzPlayer   = p; }

void SequencerEngine::setSelectedSfLiveChannels (uint16_t channelMask) noexcept
{
    if (impl->sfzPlayer != nullptr)
        impl->sfzPlayer->setLiveInputChannelMask (channelMask);
}

uint16_t SequencerEngine::getAllSfPlayerChannelMask() const noexcept
{
    uint16_t mask = 0;
    auto snap = impl->getTracks();
    for (auto& t : *snap)
        if (t->type == TrackType::SfPlayer)
        {
            const int ch0 = t->midiChannel.load (std::memory_order_relaxed) & 0xF;
            if (ch0 < 2)   // 0-based ch 0/1 (MIDI ch 1/2) reserved — never emit their bits
                continue;
            mask |= static_cast<uint16_t> (1u << ch0);
        }
    return mask;
}

uint16_t SequencerEngine::getSfzInstrumentChannelMask() const noexcept
{
    uint16_t mask = 0;
    auto snap = impl->getTracks();
    for (auto& t : *snap)
        if (t->type == TrackType::SfPlayer && t->isSfzInstrument)
        {
            // t->midiChannel is stored 0-based, but this mask feeds
            // sfzPlayer2ChannelMask, which processMidi2() tests directly
            // against the 1-based MIDI channel number (no -1) — so the bit
            // index here must be the 1-based channel, not the 0-based one.
            const int ch1 = (t->midiChannel.load (std::memory_order_relaxed) & 0xF) + 1;
            mask |= static_cast<uint16_t> (1u << ch1);
        }
    return mask;
}

//==============================================================================
void SequencerEngine::setSelectedLiveChannel (int ch1Based) noexcept
{
    impl->selectedLiveChannel.store (ch1Based, std::memory_order_relaxed);
}

int SequencerEngine::getSelectedLiveChannel() const noexcept
{
    return impl->selectedLiveChannel.load (std::memory_order_relaxed);
}

void SequencerEngine::setSelectedTrack (int trackIndex) noexcept
{
    SelectedLiveTarget target;   // defaults to { LiveTargetPlayer::none, 0 }
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (trackIndex, (int) snap->size()))
    {
        auto& t = *(*snap)[(size_t) trackIndex];
        target.midiChannel = Impl::midiChannelForTrack (t);

        switch (t.type)
        {
            case TrackType::MainSlice:
            case TrackType::ChromaticSlice:
                target.player = LiveTargetPlayer::slicer;
                break;

            case TrackType::SfPlayer:
                target.player = t.isSfzInstrument ? LiveTargetPlayer::sfz
                                                   : LiveTargetPlayer::sf2;
                break;
        }
    }

    impl->selectedLiveTarget.store (target, std::memory_order_relaxed);
    // Keep the legacy channel-only accessor in sync for any remaining callers.
    impl->selectedLiveChannel.store (target.midiChannel, std::memory_order_relaxed);
}

SelectedLiveTarget SequencerEngine::getSelectedLiveTarget() const noexcept
{
    return impl->selectedLiveTarget.load (std::memory_order_relaxed);
}

void SequencerEngine::setRecordingTrack (int trackIndex) noexcept
{
    impl->recordingTrackIndex.store (trackIndex, std::memory_order_relaxed);
    impl->openRecNotes.clearQuick();  // discard any held notes from previous selection
    impl->liveRecordClip = {};

    // If the user arms/selects the recording track while an already-playing
    // recording pass is active, pre-create its destination immediately. This
    // keeps the first boundary note in the same path as later notes.
    if (impl->recording.load (std::memory_order_relaxed)
        && impl->playing.load (std::memory_order_relaxed)
        && ! impl->countInActive.load (std::memory_order_relaxed))
        impl->beginLiveRecordClip (trackIndex);
}

int SequencerEngine::getRecordingTrackIndex() const noexcept
{
    return impl->recordingTrackIndex.load (std::memory_order_relaxed);
}

//==============================================================================
//  drainRecordedEvents()  —  MESSAGE THREAD ONLY.
//
//  The only remaining caller of MidiClip::addNote()/setNoteDuration() for
//  recording. Safe because it never runs on the audio thread.
//==============================================================================
void SequencerEngine::drainRecordedEvents()
{
    const auto scope = impl->recordFifo.read (impl->recordFifo.getNumReady());
    auto snap = impl->getTracks();

    // Finds (or lazily creates) the clip a recorded note-on should land in.
    // This only runs once per recording pass — the first branch below
    // reuses whatever was decided here for every event after that, so the
    // mode only matters at the very start of a take.
    const auto mode = impl->recordMode.load (std::memory_order_relaxed);
    auto findOrCreateSessionSlot = [&] (int trackIndex, int64_t globalTick) -> std::shared_ptr<ClipSlot>
    {
        if (impl->liveRecordClip.active && impl->liveRecordClip.trackIndex == trackIndex)
            return impl->liveRecordClip.slot;

        if (! juce::isPositiveAndBelow (trackIndex, (int) snap->size())) return nullptr;
        auto& track = *(*snap)[(size_t) trackIndex];

        // Overdub: if the armed track already has a clip sitting under the
        // playhead, merge new notes into it rather than starting a new one.
        // Add: skip this search entirely — always lay down a brand-new clip
        // below, so whatever was already there is left completely alone.
        if (mode == RecordMode::Overdub)
        {
            auto clipsSnap = track.getClips();
            for (auto& existing : *clipsSnap)
            {
                if (globalTick >= existing->getStartTick() && globalTick < existing->endTick())
                {
                    impl->liveRecordClip = { true, trackIndex, existing };
                    return existing;
                }
            }
        }

        // Nothing to merge into (Overdub with empty space) or Add mode —
        // draw a fresh clip starting at the playhead tick the first note-on
        // landed on. Length grows below (both here, as notes come in, and
        // once per drain call to keep pace with the playhead even between
        // notes). In Add mode this may end up overlapping an existing clip
        // on the same track — that's intentional, it's a separate take
        // layered on top, not a punch-in.
        const int newIdx = track.addClip (globalTick, MidiClip::kPPQ / 4);
        auto newSlot = track.getClipSlot (newIdx);
        impl->liveRecordClip = { true, trackIndex, newSlot };
        return newSlot;
    };

    auto applyEvent = [&] (const Impl::RecordedNoteEvent& ev)
    {
        if (! juce::isPositiveAndBelow (ev.trackIndex, (int) snap->size())) return;

        if (ev.isNoteOn)
        {
            auto slot = findOrCreateSessionSlot (ev.trackIndex, ev.tick);
            if (slot == nullptr) return;

            auto&         clip      = slot->clip;
            const int64_t localTick = juce::jmax ((int64_t) 0, ev.tick - slot->getStartTick());
            if (localTick >= clip.getLengthTicks())
                clip.setLengthTicks (localTick + 1);

            MidiNote n;
            n.note         = ev.note;
            n.velocity     = ev.velocity;
            n.startTick    = localTick;
            n.durationTick = MidiClip::kPPQ / 4;   // placeholder; corrected on note-off
            const int idx  = clip.addNote (n);
            impl->openRecNotes.add ({ ev.trackIndex, n.note, localTick, idx });
        }
        else
        {
            // A note-off can only belong to the currently open live-record
            // clip for this track — if there isn't one (e.g. recording was
            // stopped/re-armed between the note-on and note-off), just drop
            // it rather than guessing at a clip.
            if (! (impl->liveRecordClip.active && impl->liveRecordClip.trackIndex == ev.trackIndex))
                return;

            auto  slot = impl->liveRecordClip.slot;
            auto& clip = slot->clip;
            const int64_t localTick = juce::jmax ((int64_t) 0, ev.tick - slot->getStartTick());

            for (int i = impl->openRecNotes.size() - 1; i >= 0; --i)
            {
                auto& orn = impl->openRecNotes.getReference (i);
                if (orn.trackIndex == ev.trackIndex && orn.note == ev.note)
                {
                    int64_t dur = localTick - orn.startTick;
                    if (dur <= 0) dur = MidiClip::kPPQ / 4;   // clock jitter guard
                    clip.setNoteDuration (orn.noteIndexInClip, dur);
                    impl->openRecNotes.remove (i);
                    break;
                }
            }

            if (localTick >= clip.getLengthTicks())
                clip.setLengthTicks (localTick + 1);
        }
    };

    for (int i = 0; i < scope.blockSize1; ++i)
        applyEvent (impl->recordBuffer[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i)
        applyEvent (impl->recordBuffer[(size_t) (scope.startIndex2 + i)]);

    // Keep the live-record clip's end following the playhead every drain
    // tick, not just when a note happens to land — this is what makes the
    // clip visibly grow in the arranger while recording, Cubase-style.
    // Once recording stops (or the armed track changes), close the session
    // out so the next recording pass starts its own new clip.
    if (impl->liveRecordClip.active)
    {
        const int recTi = impl->recordingTrackIndex.load (std::memory_order_relaxed);
        const bool stillRecordingThisTrack =
            impl->recording.load (std::memory_order_relaxed)
            && impl->playing.load  (std::memory_order_relaxed)
            && recTi == impl->liveRecordClip.trackIndex;

        if (stillRecordingThisTrack)
        {
            auto&         clip    = impl->liveRecordClip.slot->clip;
            const int64_t nowTick = impl->playheadTick.load (std::memory_order_relaxed);
            const int64_t grownLen = nowTick - impl->liveRecordClip.slot->getStartTick();
            if (grownLen > clip.getLengthTicks())
                clip.setLengthTicks (grownLen);
        }
        else
        {
            impl->liveRecordClip = {};
        }
    }
}

//==============================================================================
bool SequencerEngine::getMidiActivityAndClear (int trackIndex) noexcept
{
    if (! juce::isPositiveAndBelow (trackIndex, kActivityFlagCount))
        return false;
    return impl->midiActivityFlags[trackIndex].exchange (false, std::memory_order_relaxed);
}

//==============================================================================
void SequencerEngine::processBlock (juce::MidiBuffer& outMidi, const juce::MidiBuffer& inMidi,
                                    int numSamples, double sampleRate)
{
    const float fallbackBpm = impl->syncToHost.load (std::memory_order_relaxed)
                                ? impl->hostBpm.load     (std::memory_order_relaxed)
                                : impl->internalBpm.load (std::memory_order_relaxed);
    const float bpm = (impl->abletonLink != nullptr && impl->abletonLink->isEnabled())
                        ? impl->abletonLink->getBpm (fallbackBpm)
                        : fallbackBpm;

    // Track effective BPM even though there is no Edit to push it to.
    if (bpm >= 20.f && bpm != impl->lastAppliedBpm)
        impl->lastAppliedBpm = bpm;

    // The track selected for live input is also the visual MIDI destination.
    // Mark it on *incoming* events even when transport is stopped: previously
    // activity was set only while adding sequencer playback events, so the
    // track LEDs misleadingly showed output/recorded MIDI but never a keyboard.
    const int inputTi = impl->recordingTrackIndex.load (std::memory_order_relaxed);
    if (! inMidi.isEmpty() && juce::isPositiveAndBelow (inputTi, kActivityFlagCount))
        impl->midiActivityFlags[inputTi].store (true, std::memory_order_relaxed);

    if (impl->pendingStop.exchange (false, std::memory_order_relaxed))
    {
        if (impl->playing.load (std::memory_order_relaxed))
        {
            impl->playing.store (false, std::memory_order_relaxed);
            impl->flushAllActiveNotes (outMidi, 0);
            impl->openRecNotes.clearQuick();
        }
    }
    if (impl->pendingRewind.exchange (false, std::memory_order_relaxed))
    {
        impl->flushAllActiveNotes (outMidi, 0);
        impl->openRecNotes.clearQuick();
        impl->currentTick = 0.0;
        impl->playheadTick.store (0, std::memory_order_relaxed);
    }
    if (impl->pendingSeek.exchange (false, std::memory_order_relaxed))
    {
        impl->flushAllActiveNotes (outMidi, 0);
        impl->currentTick = (double) impl->pendingSeekTick.load (std::memory_order_relaxed);
        impl->playheadTick.store ((int64_t) impl->currentTick, std::memory_order_relaxed);
    }
    if (impl->pendingPlay.exchange (false, std::memory_order_relaxed))
    {
        impl->playing.store (true, std::memory_order_relaxed);

        // A record press arms recording; Play starts the transport with a
        // musical count-in. The actual recording take starts exactly back at
        // bar 1 after 1 or 2 full bars, so the first recorded note can never
        // land in the transport's already-passed opening block.
        if (impl->recording.load (std::memory_order_relaxed))
        {
            const int bars = juce::jlimit (1, 2,
                impl->countInBars.load (std::memory_order_relaxed));
            impl->countInRemainingTicks.store (
                (int64_t) bars * MidiClip::kPPQ * 4, std::memory_order_relaxed);
            impl->countInElapsedTicks = 0;
            impl->countInNextBeat = 0;
            impl->countInActive.store (true, std::memory_order_relaxed);
            impl->liveRecordClip = {};
        }
    }
    // ── React to a remote Link peer's Play/Stop ───────────────────────────
    // Everything above only ever sends our own transport state to Link
    // (requestBeatAlignedStart()/notifyStop() in play()/stop()). Nothing
    // previously looked at isSessionPlaying(), so a peer pressing Play/Stop
    // in another Link-enabled app had no effect here at all. Edge-detect it
    // and either join in (beat-aligned, same path our own Play button uses)
    // or stop, without touching the local send-side logic above.
    if (impl->abletonLink != nullptr && impl->abletonLink->isEnabled()
         && impl->linkFollowsTransport.load (std::memory_order_relaxed))
    {
        if (impl->linkFollowBaselineDirty.exchange (false, std::memory_order_relaxed))
            impl->lastLinkSessionPlaying = impl->abletonLink->isSessionPlaying();

        const bool sessionPlaying = impl->abletonLink->isSessionPlaying();
        if (sessionPlaying != impl->lastLinkSessionPlaying)
        {
            if (sessionPlaying
                && ! impl->playing.load (std::memory_order_relaxed)
                && ! impl->awaitingLinkStart.load (std::memory_order_relaxed))
            {
                impl->awaitingLinkStart.store (true, std::memory_order_relaxed);
                impl->linkPhasePrimed = false;
            }
            else if (! sessionPlaying && impl->playing.load (std::memory_order_relaxed))
            {
                impl->playing.store (false, std::memory_order_relaxed);
                impl->awaitingLinkStart.store (false, std::memory_order_relaxed);
                impl->flushAllActiveNotes (outMidi, 0);
                impl->openRecNotes.clearQuick();
            }
            impl->lastLinkSessionPlaying = sessionPlaying;
        }
    }

    // acquire: pairs with the release store in play() above, so that if we
    // observe awaitingLinkStart == true here, the linkPhasePrimed = false
    // write play() did on the message thread is guaranteed visible before we
    // touch linkPhasePrimed/linkPhasePrev below.
    if (impl->awaitingLinkStart.load (std::memory_order_acquire))
    {
        // Waiting on the quantum boundary requested by requestBeatAlignedStart().
        // Detect arrival by watching the Link phase wrap back past zero —
        // block-granularity, consistent with the rest of this lock-free
        // transport, and needs no extra Link timing APIs beyond getPhase().
        if (impl->abletonLink != nullptr && impl->abletonLink->isEnabled())
        {
            const double quantum = 4.0;
            const double phase = impl->abletonLink->getPhase (quantum);
            if (! impl->linkPhasePrimed)
            {
                impl->linkPhasePrimed = true;
            }
            else if (phase < impl->linkPhasePrev)
            {
                impl->awaitingLinkStart.store (false, std::memory_order_relaxed);
                impl->playing.store (true, std::memory_order_relaxed);
            }
            impl->linkPhasePrev = phase;
        }
        else
        {
            // Link was disabled while we were waiting — fall back to starting now.
            impl->awaitingLinkStart.store (false, std::memory_order_relaxed);
            impl->playing.store (true, std::memory_order_relaxed);
        }
    }

    if (! impl->playing.load (std::memory_order_relaxed)) return;
    if (bpm < 1.f || sampleRate < 1.0) return;

    const double ticksPerSample = (bpm / 60.0) * (double) MidiClip::kPPQ / sampleRate;

    // Count-in is a real pre-roll: do not render arrangement clips or capture
    // MIDI while it is active. At the end, reset to tick 0 and create the live
    // recording clip before the first recording block is processed.
    if (impl->countInActive.load (std::memory_order_relaxed))
    {
        const int64_t blockTicks = juce::jmax<int64_t> (1,
            (int64_t) std::ceil (ticksPerSample * (double) numSamples));
        const int64_t remaining = impl->countInRemainingTicks.load (std::memory_order_relaxed);
        const int64_t blockStartElapsed = impl->countInElapsedTicks;
        const int64_t blockEndElapsed = blockStartElapsed + blockTicks;
        const int64_t beatTicks = MidiClip::kPPQ;

        // Emit one internal metronome trigger at every quarter-note boundary.
        // Channel 16 is reserved by the engine for this private transport
        // signal; PluginProcessor consumes it into an audio click and never
        // routes it to a musical destination.
        while ((int64_t) impl->countInNextBeat * beatTicks < blockEndElapsed
               && (int64_t) impl->countInNextBeat * beatTicks < (int64_t) (impl->countInRemainingTicks.load() + blockStartElapsed))
        {
            const int64_t clickTick = (int64_t) impl->countInNextBeat * beatTicks;
            if (clickTick >= blockStartElapsed)
            {
                const int sampleOffset = juce::jlimit (0, numSamples - 1,
                    (int) std::llround ((double) (clickTick - blockStartElapsed) / ticksPerSample));
                const int barBeat = impl->countInNextBeat % 4;
                const int velocity = (barBeat == 0) ? 120 : 92;
                outMidi.addEvent (juce::MidiMessage::noteOn (16, 37, (juce::uint8) velocity),
                                  sampleOffset);
            }
            ++impl->countInNextBeat;
        }

        impl->countInElapsedTicks = blockEndElapsed;
        if (remaining > blockTicks)
        {
            impl->countInRemainingTicks.store (remaining - blockTicks, std::memory_order_relaxed);
            return;
        }

        impl->countInRemainingTicks.store (0, std::memory_order_relaxed);
        impl->countInActive.store (false, std::memory_order_relaxed);
        impl->currentTick = 0.0;
        impl->playheadTick.store (0, std::memory_order_relaxed);

        if (impl->recording.load (std::memory_order_relaxed))
            impl->beginLiveRecordClip (impl->recordingTrackIndex.load (std::memory_order_relaxed));
    }

    const bool   doLoop         = impl->looping.load (std::memory_order_relaxed);
    const double blockEndTick   = impl->currentTick + ticksPerSample * numSamples;

    // One atomic load for the whole block — zero locks in the hot path.
    auto tracksSnap = impl->getTracks();
    const int64_t masterLen = Impl::computeMasterLenFor (*tracksSnap);
    const int64_t loopStart = impl->loopStartTick.load (std::memory_order_relaxed);
    const int64_t configuredLoopEnd = impl->loopEndTick.load (std::memory_order_relaxed);
    const int64_t loopEnd = configuredLoopEnd > loopStart ? configuredLoopEnd : masterLen;
    const int64_t loopLength = juce::jmax ((int64_t) MidiClip::kPPQ, loopEnd - loopStart);

    // Standard mixer convention: while any track is soloed, only soloed
    // tracks are audible, regardless of their own enabled/mute state.
    bool anySoloed = false;
    for (auto& t : *tracksSnap)
    {
        if (t->solo.load (std::memory_order_relaxed)) { anySoloed = true; break; }
    }

    for (int ti = 0; ti < (int) tracksSnap->size(); ++ti)
    {
        auto& track = *(*tracksSnap)[(size_t) ti];
        const bool audible = anySoloed ? track.solo.load (std::memory_order_relaxed)
                                       : track.enabled.load (std::memory_order_relaxed);
        if (! audible) continue;

        const int priorSize = outMidi.getNumEvents();
        auto clipsSnap = track.getClips();

        for (int ci = 0; ci < (int) clipsSnap->size(); ++ci)
        {
            impl->processClipSlot (outMidi, track, ti, ci, *(*clipsSnap)[(size_t) ci],
                                   impl->currentTick, blockEndTick,
                                   numSamples, ticksPerSample, doLoop, loopStart, loopEnd);
        }

        if (outMidi.getNumEvents() > priorSize
            && juce::isPositiveAndBelow (ti, kActivityFlagCount))
            impl->midiActivityFlags[ti].store (true, std::memory_order_relaxed);
    }

    // ── MIDI input recording ──────────────────────────────────────────────────
    // Cubase-style: the selected (record-armed) track captures all incoming
    // MIDI regardless of channel. This never touches MidiClip on this thread
    // — no target clip needs to exist yet at all. Each event is stamped with
    // its absolute/global tick and pushed into the lock-free FIFO;
    // drainRecordedEvents(), called from the message thread, is what
    // actually creates/grows the destination clip and calls
    // addNote()/setNoteDuration() on it.
    const int recTi = impl->recordingTrackIndex.load (std::memory_order_relaxed);
    if (impl->recording.load (std::memory_order_relaxed)
        && impl->playing.load (std::memory_order_relaxed)
        && juce::isPositiveAndBelow (recTi, (int) tracksSnap->size()))
    {
        for (const auto meta : inMidi)
        {
            const auto msg = meta.getMessage();
            if (msg.getChannel() == 16) continue;   // skip SFZ-internal channel

            const double evTickGlobal = impl->currentTick + meta.samplePosition * ticksPerSample;

            if (msg.isNoteOn (true))        // true = treat velocity-0 as note-off
            {
                Impl::RecordedNoteEvent ev;
                ev.trackIndex = recTi;
                ev.note       = msg.getNoteNumber();
                ev.velocity   = msg.getVelocity();
                ev.isNoteOn   = true;
                ev.tick       = (int64_t) evTickGlobal;
                impl->pushRecordedEvent (ev);
            }
            else if (msg.isNoteOff (true))
            {
                Impl::RecordedNoteEvent ev;
                ev.trackIndex = recTi;
                ev.note       = msg.getNoteNumber();
                ev.velocity   = 0;
                ev.isNoteOn   = false;
                ev.tick       = (int64_t) evTickGlobal;
                impl->pushRecordedEvent (ev);
            }
        }
    }

    if (doLoop && loopEnd > loopStart && blockEndTick >= (double) loopEnd)
    {
        const int loopSample = juce::jlimit (0, numSamples - 1,
            (int)(((double) loopEnd - impl->currentTick) / ticksPerSample));
        impl->flushAllActiveNotes (outMidi, loopSample);
        impl->currentTick = (double) loopStart
                          + std::fmod (blockEndTick - (double) loopStart, (double) loopLength);
    }
    else
    {
        impl->currentTick = blockEndTick;
    }

    impl->playheadTick.store ((int64_t) impl->currentTick, std::memory_order_relaxed);
}

//==============================================================================
void SequencerEngine::writeToStream (juce::MemoryOutputStream& s) const
{
    s.writeInt   (kStreamVersion4);
    s.writeFloat (impl->internalBpm.load (std::memory_order_relaxed));
    s.writeBool  (impl->looping    .load (std::memory_order_relaxed));
    s.writeBool  (impl->syncToHost .load (std::memory_order_relaxed));

    auto snap = impl->getTracks();
    s.writeInt ((int) snap->size());
    for (auto& t : *snap)
        t->writeToStream (s);
}

bool SequencerEngine::readFromStream (juce::MemoryInputStream& s)
{
    // Peek at first int — kStreamVersion2/3/4 mean multi-clip format,
    // otherwise treat the bytes as a legacy float BPM (version 1).
    const auto startPos = s.getPosition();
    const int firstInt  = s.readInt();

    float bpm;
    bool  loop, sync;
    int   n;
    const bool isV2 = (firstInt == kStreamVersion2);
    const bool isV3 = (firstInt == kStreamVersion3);
    const bool isV4 = (firstInt == kStreamVersion4);

    if (isV2 || isV3 || isV4)
    {
        bpm  = s.readFloat();
        loop = s.readBool();
        sync = s.readBool();
        n    = s.readInt();
    }
    else
    {
        // Legacy: firstInt was the raw bytes of a float BPM.
        s.setPosition (startPos);
        bpm  = s.readFloat();
        loop = s.readBool();
        sync = s.readBool();
        s.readInt64();   // legacy clipLengthTicks — discard
        n    = s.readInt();
    }

    if (bpm < 20.f || bpm > 999.f || n < 0 || n > 256) return false;

    auto loaded = std::make_shared<Impl::TrackList>();
    loaded->reserve ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        auto t  = std::make_shared<SequencerTrack>();
        bool ok = (isV2 || isV3 || isV4) ? t->readFromStream (s, isV3 || isV4, isV4) : t->readFromStreamV1 (s);
        if (! ok) return false;
        loaded->push_back (t);
    }

    impl->internalBpm.store (bpm,  std::memory_order_relaxed);
    impl->looping    .store (loop, std::memory_order_relaxed);
    impl->syncToHost .store (sync, std::memory_order_relaxed);

    impl->publishTracks (std::move (loaded));

    return true;
}
