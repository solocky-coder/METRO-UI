#pragma once
#include "MidiClip.h"
#include "../audio/SfzPlayer.h"
#include "../network/NetworkAudioInput.h"
#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>

enum class TrackType { MainSlice, ChromaticSlice, SfPlayer, Audio };

struct ClipSlot
{
    std::atomic<int64_t> startTick { 0 };
    MidiClip clip;
    ClipSlot() = default;
    ClipSlot (int64_t start, int64_t lengthTicks) : startTick (start) { clip.setLengthTicks (lengthTicks); }
    JUCE_DECLARE_NON_COPYABLE (ClipSlot)
    int64_t getStartTick() const noexcept { return startTick.load (std::memory_order_relaxed); }
    int64_t endTick() const noexcept { return getStartTick() + clip.getLengthTicks(); }
    void writeToStream (juce::MemoryOutputStream& s) const { s.writeInt64 (getStartTick()); clip.writeToStream (s); }
    bool readFromStream (juce::MemoryInputStream& s) { startTick.store (s.readInt64(), std::memory_order_relaxed); return clip.readFromStream (s); }
};

struct SequencerTrack
{
    using ClipList = std::vector<std::shared_ptr<ClipSlot>>;
    SequencerTrack() = default;
    JUCE_DECLARE_NON_COPYABLE (SequencerTrack)

    TrackType type = TrackType::MainSlice;
    std::atomic<bool> enabled { true };
    std::atomic<bool> solo { false };
    std::atomic<float> volumeDb { 0.0f };
    std::atomic<float> pan { 0.0f };
    juce::String name;
    juce::Colour colour = juce::Colour (0xFF3A6080);
    int sliceIdx = -1;
    std::atomic<int> midiChannel { 0 };
    Sf2PresetInfo preset;
    bool isSfzInstrument = false;

    int64_t networkRouteId = 0;
    int64_t networkSourceKey = 0;
    int32_t networkSourceId = 0;
    int networkSourceChannel = 0;

    std::shared_ptr<const ClipList> getClips() const noexcept { return clipsSnapshot.load (std::memory_order_acquire); }
    int getNumClips() const noexcept { return (int) getClips()->size(); }
    std::shared_ptr<ClipSlot> getClipSlot (int i) const
    {
        auto snap = getClips();
        return juce::isPositiveAndBelow (i, (int) snap->size()) ? (*snap)[(size_t) i] : nullptr;
    }
    int addClip (int64_t startTick, int64_t lengthTicks = MidiClip::kPPQ * 4 * 4)
    {
        auto newSlot = std::make_shared<ClipSlot> (startTick, lengthTicks);
        auto next = std::make_shared<ClipList> (*getClips()); next->push_back (newSlot); sortAndPublish (std::move (next));
        auto published = getClips();
        for (size_t i = 0; i < published->size(); ++i) if ((*published)[i] == newSlot) return (int) i;
        return (int) published->size() - 1;
    }
    void removeClip (int index)
    {
        auto current = getClips(); if (! juce::isPositiveAndBelow (index, (int) current->size())) return;
        auto next = std::make_shared<ClipList> (*current); next->erase (next->begin() + index); publish (std::move (next));
    }
    void sortClips() { auto next = std::make_shared<ClipList> (*getClips()); sortAndPublish (std::move (next)); }

    static std::shared_ptr<SequencerTrack> makeMain()
    { auto t = std::make_shared<SequencerTrack>(); t->type = TrackType::MainSlice; t->name = "MAIN"; t->colour = juce::Colour (0xFF25D9D9); return t; }
    static std::shared_ptr<SequencerTrack> makeChromatic (int sliceIdxIn, int chromaticChannel, const juce::String& sliceName, juce::Colour sliceColour)
    { auto t = std::make_shared<SequencerTrack>(); t->type = TrackType::ChromaticSlice; t->sliceIdx = sliceIdxIn; t->midiChannel.store (chromaticChannel - 1); t->name = sliceName.isEmpty() ? ("CHROM " + juce::String (sliceIdxIn + 1)) : sliceName; t->colour = sliceColour; return t; }
    static std::shared_ptr<SequencerTrack> makeSfPlayer (const Sf2PresetInfo& p, juce::Colour c)
    { auto t = std::make_shared<SequencerTrack>(); t->type = TrackType::SfPlayer; t->preset = p; t->midiChannel.store (15); t->name = p.name; t->colour = c; return t; }
    static std::shared_ptr<SequencerTrack> makeSfzInstrument (const juce::String& n, juce::Colour c)
    { Sf2PresetInfo p; p.name = n; p.bank = 0; p.preset = 0; auto t = std::make_shared<SequencerTrack>(); t->type = TrackType::SfPlayer; t->preset = p; t->isSfzInstrument = true; t->midiChannel.store (15); t->name = n; t->colour = c; return t; }
    static std::shared_ptr<SequencerTrack> makeAudio (const NetworkAudioInput& input, juce::Colour c = juce::Colour (0xFF406080))
    {
        auto t = std::make_shared<SequencerTrack>(); t->type = TrackType::Audio;
        t->networkRouteId = input.routeId; t->networkSourceKey = input.sourceKey; t->networkSourceId = input.sourceId; t->networkSourceChannel = input.sourceChannel;
        t->name = input.sourceName.isNotEmpty() ? input.sourceName : ("NETWORK " + juce::String (input.sourceId) + "." + juce::String (input.sourceChannel + 1));
        t->colour = c; t->volumeDb.store (input.gainDb.load()); t->pan.store (input.pan.load()); return t;
    }

    void writeToStream (juce::MemoryOutputStream& s) const
    {
        s.writeInt ((int) type); s.writeBool (enabled.load()); s.writeString (name); s.writeInt ((int) colour.getARGB()); s.writeInt (sliceIdx); s.writeInt (midiChannel.load());
        s.writeInt (preset.bank); s.writeInt (preset.preset); s.writeString (preset.name);
        auto snap = getClips(); s.writeInt ((int) snap->size()); for (auto& slot : *snap) slot->writeToStream (s);
        s.writeBool (solo.load()); s.writeFloat (volumeDb.load()); s.writeFloat (pan.load());
        if (type == TrackType::Audio) { s.writeInt64 (networkRouteId); s.writeInt64 (networkSourceKey); s.writeInt (networkSourceId); s.writeInt (networkSourceChannel); }
    }
    bool readFromStream (juce::MemoryInputStream& s, bool hasExtendedFields = true)
    {
        type = (TrackType) s.readInt(); enabled.store (s.readBool()); name = s.readString(); colour = juce::Colour ((juce::uint32) s.readInt()); sliceIdx = s.readInt(); midiChannel.store (s.readInt());
        preset.bank = s.readInt(); preset.preset = s.readInt(); preset.name = s.readString();
        const int n = s.readInt(); if (n < 0 || n > 1024) return false; auto next = std::make_shared<ClipList>(); next->reserve ((size_t) n);
        for (int i = 0; i < n; ++i) { auto slot = std::make_shared<ClipSlot>(); if (! slot->readFromStream (s)) return false; next->push_back (std::move (slot)); }
        sortAndPublish (std::move (next));
        if (hasExtendedFields) { solo.store (s.readBool()); volumeDb.store (s.readFloat()); pan.store (s.readFloat()); }
        if (type == TrackType::Audio) { networkRouteId = s.readInt64(); networkSourceKey = s.readInt64(); networkSourceId = s.readInt(); networkSourceChannel = s.readInt(); }
        return true;
    }
    bool readFromStreamV1 (juce::MemoryInputStream& s)
    {
        type = (TrackType) s.readInt(); enabled.store (s.readBool()); name = s.readString(); colour = juce::Colour ((juce::uint32) s.readInt()); sliceIdx = s.readInt(); midiChannel.store (s.readInt());
        preset.bank = s.readInt(); preset.preset = s.readInt(); preset.name = s.readString(); auto slot = std::make_shared<ClipSlot>(); slot->startTick.store (0); if (! slot->clip.readFromStream (s)) return false;
        auto next = std::make_shared<ClipList>(); next->push_back (std::move (slot)); publish (std::move (next)); return true;
    }
private:
    std::atomic<std::shared_ptr<const ClipList>> clipsSnapshot { std::make_shared<const ClipList>() };
    void publish (std::shared_ptr<const ClipList> next) { clipsSnapshot.store (std::move (next), std::memory_order_release); }
    void sortAndPublish (std::shared_ptr<ClipList> next) { std::sort (next->begin(), next->end(), [] (const auto& a, const auto& b) { return a->getStartTick() < b->getStartTick(); }); publish (std::move (next)); }
};
