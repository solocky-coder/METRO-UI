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
    bool                 justStarted = false;
    std::atomic<int64_t> playheadTick { 0 };

    struct ActiveNote { int trackIdx; int clipIdx; int note; int channel; };
    juce::Array<ActiveNote> activeNotes;

    std::atomic<bool>    playing      { false };
    std::atomic<bool>    recording    { false };
    std::atomic<bool>    looping      { true  };
    std::atomic<bool>    pendingPlay  { false };
    std::atomic<bool>    pendingStop  { false };
    std::atomic<bool>    pendingRewind{ false };
    std::atomic<bool>    pendingSeek  { false };
    std::atomic<int64_t> pendingSeekTick { 0 };
    std::atomic<float>   internalBpm  { 120.f };
    std::atomic<float>   hostBpm      { 120.f };
    std::atomic<bool>    syncToHost   { false };
    float                lastAppliedBpm = 0.f;

    AbletonLink* abletonLink = nullptr;
    SfzPlayer*   sfzPlayer   = nullptr;

    std::atomic<bool> midiActivityFlags[SequencerEngine::kActivityFlagCount] = {};
    std::atomic<int>  selectedLiveChannel  { 0 };  // 1-based; 0 = disabled
    std::atomic<int>  recordingTrackIndex  { -1 }; // which track receives recorded MIDI (-1 = none)

    //==========================================================================
    //  Recorded-note FIFO  (audio thread writes, message thread drains)
    //
    //  Same shape as DysektProcessor's commandFifo, just flowing the other
    //  direction: audio thread -> message thread instead of message thread ->
    //  audio thread. Each entry already carries a clip-relative tick, resolved
    //  at the point of capture, so the drain never has to re-derive it (and
    //  can't get it wrong if the clip has since moved).
    //==========================================================================
    struct RecordedNoteEvent
    {
        int     trackIndex = -1;
        int     note       = 60;
        int     velocity   = 100;
        bool    isNoteOn   = true;
        int64_t localTick  = 0;   // already clip-relative
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

    //==========================================================================
    //  Audio-thread note rendering — processes one ClipSlot
    //==========================================================================
    void processClipSlot (juce::MidiBuffer& outMidi, SequencerTrack& track,
                          int trackIdx, int clipIdx, const ClipSlot& slot,
                          double playheadStart, double playheadEnd,
                          int numSamples, double ticksPerSample, bool doLoop,
                          int64_t masterLen)
    {
        const int ch = midiChannelForTrack (track);
        const double clipStart = (double) slot.getStartTick();
        const double clipEnd   = (double) slot.endTick();

        double localStart = playheadStart;
        double localEnd   = playheadEnd;

        if (doLoop && masterLen > 0)
        {
            localStart = std::fmod (localStart, (double) masterLen);
            localEnd   = localStart + (playheadEnd - playheadStart);
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
int64_t SequencerEngine::getPlayheadTick()  const noexcept { return impl->playheadTick.load (std::memory_order_relaxed); }
double  SequencerEngine::getPlayheadBeats() const noexcept { return (double) getPlayheadTick() / (double) MidiClip::kPPQ; }
float   SequencerEngine::getBpm()           const noexcept { return impl->internalBpm.load (std::memory_order_relaxed); }
bool    SequencerEngine::getSyncToHost()    const noexcept { return impl->syncToHost.load (std::memory_order_relaxed); }

int64_t SequencerEngine::getLengthTicks() const noexcept
{
    auto snap = impl->getTracks();
    return Impl::computeMasterLenFor (*snap);
}

//==============================================================================
void SequencerEngine::play()
{
    if (impl->abletonLink != nullptr && impl->abletonLink->isEnabled())
        impl->abletonLink->requestBeatAlignedStart (4.0);
    impl->pendingPlay.store (true, std::memory_order_relaxed);
}

void SequencerEngine::stop()
{
    if (impl->abletonLink != nullptr && impl->abletonLink->isEnabled())
        impl->abletonLink->notifyStop();
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

void SequencerEngine::setRecording  (bool v) { impl->recording.store  (v, std::memory_order_relaxed); }
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
    return { t.type, t.enabled.load (std::memory_order_relaxed), t.name, t.colour, t.sliceIdx,
             t.midiChannel.load (std::memory_order_relaxed), t.preset, t.getNumClips() };
}

void SequencerEngine::setTrackEnabled (int i, bool enabled)
{
    auto snap = impl->getTracks();
    if (juce::isPositiveAndBelow (i, (int) snap->size()))
        (*snap)[(size_t) i]->enabled.store (enabled, std::memory_order_relaxed);
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
    int sfCh = 0;
    for (auto& t : *current)
        if (t->type == TrackType::SfPlayer)
            sfCh = juce::jmax (sfCh, t->midiChannel.load (std::memory_order_relaxed) + 1);
    sfCh = juce::jmin (sfCh, 15);

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

    for (int i = 0; i < (int) presets.size(); ++i)
    {
        const juce::Colour col = paletteSize > 0
            ? palette[i % paletteSize] : juce::Colour (0xFF406080);
        auto track = SequencerTrack::makeSfPlayer (presets[i], col);
        track->midiChannel.store (juce::jmin (i, 15), std::memory_order_relaxed);   // sequential FluidSynth channels
        next->push_back (track);
    }

    impl->publishTracks (next);

    if (impl->sfzPlayer != nullptr)
        for (int i = 0; i < (int) presets.size(); ++i)
            impl->sfzPlayer->setPresetOnChannel (juce::jmin (i, 15),
                                                  presets[i].bank,
                                                  presets[i].preset);
}

void SequencerEngine::addOrUpdateSfTrackOnChannel (const Sf2PresetInfo& preset,
                                                    int midiChannel0Based,
                                                    juce::Colour colour)
{
    const int ch = juce::jlimit (0, 15, midiChannel0Based);
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

    // Remove any existing SfPlayer track with the same name (previous SFZ load).
    auto next = std::make_shared<Impl::TrackList>();
    next->reserve (current->size() + 1);
    for (auto& t : *current)
        if (! (t->type == TrackType::SfPlayer && t->name == name))
            next->push_back (t);

    Sf2PresetInfo sfzPreset;
    sfzPreset.name   = name;
    sfzPreset.bank   = 0;
    sfzPreset.preset = 0;

    auto track = SequencerTrack::makeSfPlayer (sfzPreset, colour);
    track->midiChannel.store (ch, std::memory_order_relaxed);
    next->push_back (track);

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
void SequencerEngine::setAbletonLink (AbletonLink* l) noexcept { impl->abletonLink = l; }
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
            mask |= static_cast<uint16_t> (1u << (t->midiChannel.load (std::memory_order_relaxed) & 0xF));
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

void SequencerEngine::setRecordingTrack (int trackIndex) noexcept
{
    impl->recordingTrackIndex.store (trackIndex, std::memory_order_relaxed);
    impl->openRecNotes.clearQuick();  // discard any held notes from previous selection
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

    auto applyEvent = [&] (const Impl::RecordedNoteEvent& ev)
    {
        if (! juce::isPositiveAndBelow (ev.trackIndex, (int) snap->size())) return;
        auto slot = (*snap)[(size_t) ev.trackIndex]->getClipSlot (0);
        if (slot == nullptr) return;

        auto&         clip   = slot->clip;
        const int64_t clipLen = clip.getLengthTicks();

        if (ev.isNoteOn)
        {
            MidiNote n;
            n.note         = ev.note;
            n.velocity     = ev.velocity;
            n.startTick    = ev.localTick;
            n.durationTick = MidiClip::kPPQ / 4;   // placeholder; corrected on note-off
            const int idx  = clip.addNote (n);
            impl->openRecNotes.add ({ ev.trackIndex, n.note, ev.localTick, idx });
        }
        else
        {
            for (int i = impl->openRecNotes.size() - 1; i >= 0; --i)
            {
                auto& orn = impl->openRecNotes.getReference (i);
                if (orn.trackIndex == ev.trackIndex && orn.note == ev.note)
                {
                    // Compute real duration, clamped to loop length.
                    int64_t dur = ev.localTick - orn.startTick;
                    if (dur <= 0) dur += clipLen;   // wrapped loop
                    if (dur <= 0) dur  = MidiClip::kPPQ / 4;
                    clip.setNoteDuration (orn.noteIndexInClip, dur);
                    impl->openRecNotes.remove (i);
                    break;
                }
            }
        }
    };

    for (int i = 0; i < scope.blockSize1; ++i)
        applyEvent (impl->recordBuffer[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i)
        applyEvent (impl->recordBuffer[(size_t) (scope.startIndex2 + i)]);
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
        impl->justStarted = true;
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
        impl->justStarted = true;
    }

    if (! impl->playing.load (std::memory_order_relaxed)) return;
    if (bpm < 1.f || sampleRate < 1.0) return;

    const double ticksPerSample = (bpm / 60.0) * (double) MidiClip::kPPQ / sampleRate;
    const bool   doLoop         = impl->looping.load (std::memory_order_relaxed);
    const double blockEndTick   = impl->currentTick + ticksPerSample * numSamples;

    // One atomic load for the whole block — zero locks in the hot path.
    auto tracksSnap = impl->getTracks();
    const int64_t masterLen = Impl::computeMasterLenFor (*tracksSnap);

    for (int ti = 0; ti < (int) tracksSnap->size(); ++ti)
    {
        auto& track = *(*tracksSnap)[(size_t) ti];
        if (! track.enabled.load (std::memory_order_relaxed)) continue;

        const int priorSize = outMidi.getNumEvents();
        auto clipsSnap = track.getClips();

        for (int ci = 0; ci < (int) clipsSnap->size(); ++ci)
        {
            impl->processClipSlot (outMidi, track, ti, ci, *(*clipsSnap)[(size_t) ci],
                                   impl->currentTick, blockEndTick,
                                   numSamples, ticksPerSample, doLoop, masterLen);
        }

        if (outMidi.getNumEvents() > priorSize
            && juce::isPositiveAndBelow (ti, kActivityFlagCount))
            impl->midiActivityFlags[ti].store (true, std::memory_order_relaxed);
    }

    impl->justStarted = false;

    // ── MIDI input recording ──────────────────────────────────────────────────
    // Cubase-style: the selected (record-armed) track captures all incoming
    // MIDI regardless of channel. This never touches MidiClip on this thread
    // — it resolves each event to a tick relative to the target clip's start
    // (fixes the earlier absolute-vs-clip-relative bug) and pushes a small
    // event into the lock-free FIFO. drainRecordedEvents(), called from the
    // message thread, is what actually calls addNote()/setNoteDuration().
    const int recTi = impl->recordingTrackIndex.load (std::memory_order_relaxed);
    if (impl->recording.load (std::memory_order_relaxed)
        && impl->playing.load (std::memory_order_relaxed)
        && juce::isPositiveAndBelow (recTi, (int) tracksSnap->size()))
    {
        auto& track = *(*tracksSnap)[(size_t) recTi];
        auto  slot0 = track.getClipSlot (0);

        if (slot0 != nullptr)
        {
            const int64_t clipStartTick = slot0->getStartTick();
            const int64_t clipLenTicks  = slot0->clip.getLengthTicks();
            const double  ticksPerSmp   = (bpm / 60.0) * (double) MidiClip::kPPQ / sampleRate;
            const bool    haveTargetClip = clipLenTicks > 0;   // guards against modulo-by-zero

            for (const auto meta : inMidi)
            {
                const auto msg = meta.getMessage();
                if (msg.getChannel() == 16) continue;   // skip SFZ-internal channel
                if (! haveTargetClip)       continue;

                const double evTickGlobal = impl->currentTick + meta.samplePosition * ticksPerSmp;

                // Resolve to a tick local to the target clip, in the half-open
                // range from 0 up to (but not including) clipLenTicks.
                int64_t localTick = (int64_t) std::fmod (evTickGlobal - (double) clipStartTick,
                                                          (double) clipLenTicks);
                if (localTick < 0) localTick += clipLenTicks;

                if (msg.isNoteOn (true))        // true = treat velocity-0 as note-off
                {
                    Impl::RecordedNoteEvent ev;
                    ev.trackIndex = recTi;
                    ev.note       = msg.getNoteNumber();
                    ev.velocity   = msg.getVelocity();
                    ev.isNoteOn   = true;
                    ev.localTick  = localTick;
                    impl->pushRecordedEvent (ev);
                }
                else if (msg.isNoteOff (true))
                {
                    Impl::RecordedNoteEvent ev;
                    ev.trackIndex = recTi;
                    ev.note       = msg.getNoteNumber();
                    ev.velocity   = 0;
                    ev.isNoteOn   = false;
                    ev.localTick  = localTick;
                    impl->pushRecordedEvent (ev);
                }
            }
        }
    }

    if (doLoop && masterLen > 0 && blockEndTick >= (double) masterLen)
    {
        const int loopSample = juce::jlimit (0, numSamples - 1,
            (int)(((double) masterLen - impl->currentTick) / ticksPerSample));
        impl->flushAllActiveNotes (outMidi, loopSample);
        impl->currentTick = std::fmod (blockEndTick, (double) masterLen);
        impl->justStarted = true;
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
    s.writeInt   (kStreamVersion2);
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
    // Peek at first int — kStreamVersion2 means multi-clip format,
    // otherwise treat the bytes as a legacy float BPM (version 1).
    const auto startPos = s.getPosition();
    const int firstInt  = s.readInt();

    float bpm;
    bool  loop, sync;
    int   n;
    const bool isV2 = (firstInt == kStreamVersion2);

    if (isV2)
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
        bool ok = isV2 ? t->readFromStream (s) : t->readFromStreamV1 (s);
        if (! ok) return false;
        loaded->push_back (t);
    }

    impl->internalBpm.store (bpm,  std::memory_order_relaxed);
    impl->looping    .store (loop, std::memory_order_relaxed);
    impl->syncToHost .store (sync, std::memory_order_relaxed);

    impl->publishTracks (std::move (loaded));

    return true;
}
