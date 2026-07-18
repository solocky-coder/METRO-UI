#pragma once
#include "MidiClip.h"
#include "../audio/SfzPlayer.h"
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>

//==============================================================================
//  ClipSlot  —  a MidiClip with a start position on the timeline.
//
//  Always lives behind a shared_ptr once it's been published into a track's
//  clip list (see SequencerTrack below), so its address is stable for as
//  long as anyone — audio thread or message thread — is holding a
//  reference to it.  startTick is atomic so an interactive drag can update
//  it in place without requiring the containing clip list to be rebuilt.
//==============================================================================
struct ClipSlot
{
    std::atomic<int64_t> startTick { 0 };   // position on the track timeline
    MidiClip clip;                          // owns note data + length (own internal lock)

    ClipSlot() = default;

    ClipSlot (int64_t start, int64_t lengthTicks)
        : startTick (start)
    {
        clip.setLengthTicks (lengthTicks);
    }

    JUCE_DECLARE_NON_COPYABLE (ClipSlot)

    int64_t getStartTick() const noexcept { return startTick.load (std::memory_order_relaxed); }
    int64_t endTick()      const noexcept { return getStartTick() + clip.getLengthTicks(); }

    void writeToStream (juce::MemoryOutputStream& s) const
    {
        s.writeInt64 (getStartTick());
        clip.writeToStream (s);
    }

    bool readFromStream (juce::MemoryInputStream& s)
    {
        startTick.store (s.readInt64(), std::memory_order_relaxed);
        return clip.readFromStream (s);
    }
};

//==============================================================================
//  SequencerTrack
//
//  One track in the multi-track linear sequencer.  Three types:
//
//    MainSlice      — triggers slices by their pad MIDI note (ch 1 by default).
//                     Always present, cannot be deleted.
//
//    ChromaticSlice — one track per slice with chromatic mode + sequencer
//                     toggle enabled. Fires notes on s.chromaticChannel (0-15).
//
//    SfPlayer       — one track per SF2 instrument group. Fires notes on
//                     MIDI channel 16 (index 15), prepends a program change
//                     at playback start.
//
//  Lifetime model: a SequencerTrack, once created, is always held behind a
//  shared_ptr<SequencerTrack> inside a SequencerEngine track-list snapshot
//  (see SequencerEngine::Impl::TrackList). It is never copied or moved —
//  fields that can change after publication (enabled, midiChannel) are
//  atomics mutated in place; the clip list is copy-on-write and atomically
//  swapped, so the audio thread never takes a lock to read any of it.
//==============================================================================
enum class TrackType { MainSlice, ChromaticSlice, SfPlayer };

struct SequencerTrack
{
    using ClipList = std::vector<std::shared_ptr<ClipSlot>>;

    SequencerTrack() = default;
    JUCE_DECLARE_NON_COPYABLE (SequencerTrack)

    //==========================================================================
    TrackType         type        = TrackType::MainSlice;   // immutable after construction
    std::atomic<bool> enabled     { true };                 // mutated in place — no rebuild needed

    // Display — set once at construction, read-only afterwards.
    juce::String name;
    juce::Colour colour      = juce::Colour (0xFF3A6080);

    // ChromaticSlice fields — immutable after construction.
    int              sliceIdx    = -1;     // which slice this track belongs to
    std::atomic<int> midiChannel { 0 };    // 0-15; mutated in place by addOrUpdateSfTrackOnChannel

    // SfPlayer fields — immutable after construction.
    Sf2PresetInfo preset;              // bank + program + name

    //==========================================================================
    //  Clip list  (copy-on-write, atomically swapped)
    //==========================================================================

    /** One atomic load — safe from any thread, never blocks. */
    std::shared_ptr<const ClipList> getClips() const noexcept
    {
        return clipsSnapshot.load (std::memory_order_acquire);
    }

    int getNumClips() const noexcept { return (int) getClips()->size(); }

    std::shared_ptr<ClipSlot> getClipSlot (int i) const
    {
        auto snap = getClips();
        return juce::isPositiveAndBelow (i, (int) snap->size()) ? (*snap)[(size_t) i] : nullptr;
    }

    //==========================================================================
    //  Clip helpers (message thread)
    //==========================================================================

    /** Add a new empty clip at startTick with given length. Returns the new index. */
    int addClip (int64_t startTick, int64_t lengthTicks = MidiClip::kPPQ * 4 * 4)
    {
        auto newSlot = std::make_shared<ClipSlot> (startTick, lengthTicks);
        auto next    = std::make_shared<ClipList> (*getClips());
        next->push_back (newSlot);
        sortAndPublish (std::move (next));

        auto published = getClips();
        for (size_t i = 0; i < published->size(); ++i)
            if ((*published)[i] == newSlot) return (int) i;
        return (int) published->size() - 1;
    }

    void removeClip (int index)
    {
        auto current = getClips();
        if (! juce::isPositiveAndBelow (index, (int) current->size())) return;
        auto next = std::make_shared<ClipList> (*current);
        next->erase (next->begin() + index);
        publish (std::move (next));
    }

    /** Ensure clips stay ordered by startTick. Call after moving a clip. */
    void sortClips()
    {
        auto next = std::make_shared<ClipList> (*getClips());
        sortAndPublish (std::move (next));
    }

    //==========================================================================
    //  Factory helpers — return a freshly-built track behind a shared_ptr,
    //  ready to be pushed into a TrackList snapshot.
    static std::shared_ptr<SequencerTrack> makeMain()
    {
        auto t = std::make_shared<SequencerTrack>();
        t->type    = TrackType::MainSlice;
        t->name    = "MAIN";
        t->colour  = juce::Colour (0xFF25D9D9);
        t->addClip (0);
        return t;
    }

    static std::shared_ptr<SequencerTrack> makeChromatic (int sliceIdx, int chromaticChannel,
                                                           const juce::String& sliceName,
                                                           juce::Colour sliceColour)
    {
        auto t = std::make_shared<SequencerTrack>();
        t->type        = TrackType::ChromaticSlice;
        t->sliceIdx    = sliceIdx;
        t->midiChannel.store (chromaticChannel - 1, std::memory_order_relaxed);
        t->name        = sliceName.isEmpty()
                            ? ("CHROM " + juce::String (sliceIdx + 1))
                            : sliceName;
        t->colour      = sliceColour;
        t->addClip (0);
        return t;
    }

    static std::shared_ptr<SequencerTrack> makeSfPlayer (const Sf2PresetInfo& p,
                                                          juce::Colour colour)
    {
        auto t = std::make_shared<SequencerTrack>();
        t->type        = TrackType::SfPlayer;
        t->preset      = p;
        t->midiChannel.store (15, std::memory_order_relaxed);
        t->name        = p.name;
        t->colour      = colour;
        t->addClip (0);
        return t;
    }

    //==========================================================================
    //  Serialisation  (v2 — multi-clip)
    void writeToStream (juce::MemoryOutputStream& s) const
    {
        s.writeInt  ((int) type);
        s.writeBool (enabled.load (std::memory_order_relaxed));
        s.writeString (name);
        s.writeInt  ((int) colour.getARGB());
        s.writeInt  (sliceIdx);
        s.writeInt  (midiChannel.load (std::memory_order_relaxed));
        s.writeInt  (preset.bank);
        s.writeInt  (preset.preset);
        s.writeString (preset.name);

        auto snap = getClips();
        s.writeInt ((int) snap->size());
        for (auto& slot : *snap)
            slot->writeToStream (s);
    }

    bool readFromStream (juce::MemoryInputStream& s)
    {
        type        = (TrackType) s.readInt();
        enabled.store (s.readBool(), std::memory_order_relaxed);
        name        = s.readString();
        colour      = juce::Colour ((juce::uint32) s.readInt());
        sliceIdx    = s.readInt();
        midiChannel.store (s.readInt(), std::memory_order_relaxed);
        preset.bank   = s.readInt();
        preset.preset = s.readInt();
        preset.name   = s.readString();

        const int n = s.readInt();
        if (n < 0 || n > 1024) return false;

        auto next = std::make_shared<ClipList>();
        next->reserve ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            auto slot = std::make_shared<ClipSlot>();
            if (! slot->readFromStream (s)) return false;
            next->push_back (std::move (slot));
        }
        sortAndPublish (std::move (next));
        return true;
    }

    /** Legacy v1 read: single MidiClip at startTick=0. */
    bool readFromStreamV1 (juce::MemoryInputStream& s)
    {
        type        = (TrackType) s.readInt();
        enabled.store (s.readBool(), std::memory_order_relaxed);
        name        = s.readString();
        colour      = juce::Colour ((juce::uint32) s.readInt());
        sliceIdx    = s.readInt();
        midiChannel.store (s.readInt(), std::memory_order_relaxed);
        preset.bank   = s.readInt();
        preset.preset = s.readInt();
        preset.name   = s.readString();

        auto slot = std::make_shared<ClipSlot>();
        slot->startTick.store (0, std::memory_order_relaxed);
        if (! slot->clip.readFromStream (s)) return false;

        auto next = std::make_shared<ClipList>();
        next->push_back (std::move (slot));
        publish (std::move (next));
        return true;
    }

private:
    // Immutable once published; add/remove/sort build a new vector and swap
    // it in with one atomic store — the audio thread only ever sees a
    // fully-formed list, never a half-mutated one.
    std::atomic<std::shared_ptr<const ClipList>> clipsSnapshot { std::make_shared<const ClipList>() };

    void publish (std::shared_ptr<const ClipList> next)
    {
        clipsSnapshot.store (std::move (next), std::memory_order_release);
    }

    void sortAndPublish (std::shared_ptr<ClipList> next)
    {
        std::sort (next->begin(), next->end(),
                   [] (const std::shared_ptr<ClipSlot>& a, const std::shared_ptr<ClipSlot>& b)
                   { return a->getStartTick() < b->getStartTick(); });
        publish (std::move (next));
    }
};
