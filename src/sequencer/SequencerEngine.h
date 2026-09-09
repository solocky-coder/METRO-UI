#pragma once
#include "SequencerTrack.h"
#include "AbletonLink.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

enum class LiveTargetPlayer { none, slicer, sf2, sfz };
struct SelectedLiveTarget { LiveTargetPlayer player = LiveTargetPlayer::none; int midiChannel = 0; };

struct SequencerTrackInfo
{
    TrackType type = TrackType::MainSlice;
    bool enabled = true; bool solo = false; float volumeDb = 0.0f; float pan = 0.0f;
    juce::String name; juce::Colour colour = juce::Colour (0xFF3A6080);
    int sliceIdx = -1; int midiChannel = 0; Sf2PresetInfo preset; int numClips = 0;
    bool isSfzInstrument = false;
    int64_t networkRouteId = 0; int32_t networkSourceId = 0; int networkSourceChannel = 0;
};

struct SequencerClipInfo
{
    int64_t startTick = 0; int64_t lengthTicks = MidiClip::kPPQ * 4 * 4;
    int64_t endTick() const noexcept { return startTick + lengthTicks; }
};

class SequencerEngine
{
public:
    SequencerEngine(); ~SequencerEngine();
    bool isPlaying() const noexcept; bool isLooping() const noexcept; bool isRecording() const noexcept;
    int getCountInBars() const noexcept; void setCountInBars (int bars) noexcept;
    int64_t getPlayheadTick() const noexcept; double getPlayheadBeats() const noexcept; float getBpm() const noexcept; bool getSyncToHost() const noexcept;
    int64_t getLengthTicks() const noexcept; int64_t getLoopStartTick() const noexcept; int64_t getLoopEndTick() const noexcept;
    void play(); void stop(); void rewind(); void setLooping (bool v); void setRecording (bool v);
    enum class RecordMode { Overdub, Add }; void setRecordMode (RecordMode m) noexcept; RecordMode getRecordMode() const noexcept;
    void setSyncToHost(bool v); void setBpm(float b); void setHostBpm(float b); void seekToTick(int64_t tick); void setLoopRange(int64_t startTick, int64_t endTick); void resetLoopRangeToDefault(); void setLengthTicks(int64_t ticks);

    int getNumTracks() const; SequencerTrackInfo getTrackInfo(int i) const; void setTrackEnabled(int i, bool enabled); void setTrackSolo(int i, bool solo);
    void setTrackVolumeDb(int i, float volumeDb); void setTrackPan(int i, float pan); void setTrackColour(int i, juce::Colour colour);
    int findSfTrackForChannel(int midiChannel0Based) const;
    void addMainTrack(); void addChromaticTrack(int sliceIdx, int chromaticChannel, const juce::String& name, juce::Colour colour); void removeChromaticTrack(int sliceIdx);
    void addSfTrack(const Sf2PresetInfo& preset, juce::Colour colour); void removeSfTrack(int trackIndex); void rebuildSfTracks(const std::vector<Sf2PresetInfo>& presets, const juce::Colour* palette, int paletteSize);
    void addOrUpdateSfTrackOnChannel(const Sf2PresetInfo& preset, int midiChannel0Based, juce::Colour colour);
    void addSfzTrack(const juce::String& name, int midiChannel0Based, juce::Colour colour); void removeSfzTrack();

    int addNetworkAudioTrack(int64_t routeId, int32_t sourceId, int sourceChannel,
                             const juce::String& sourceName, const juce::String& userName = {});
    bool setNetworkAudioTrackRoute(int trackIndex, int64_t routeId, int32_t sourceId, int sourceChannel);
    bool isNetworkAudioTrack(int trackIndex) const noexcept;
    bool getNetworkAudioRoute(int trackIndex, int64_t& routeId, int32_t& sourceId, int& sourceChannel) const noexcept;

    int getNumClips(int trackIndex) const; SequencerClipInfo getClipInfo(int trackIndex, int clipIndex) const; MidiClip* getClip(int trackIndex, int clipIndex = 0); MidiClip& getClip();
    int addClip(int trackIndex, int64_t startTick, int64_t lengthTicks = MidiClip::kPPQ * 4 * 4); void removeClip(int trackIndex, int clipIndex); void setClipStartTick(int trackIndex, int clipIndex, int64_t newStartTick); void setClipLengthTicks(int trackIndex, int clipIndex, int64_t newLength);
    int64_t getTrackLengthTicks(int trackIndex) const noexcept; void setTrackLengthTicks(int trackIndex, int64_t ticks);

    void processBlock(juce::MidiBuffer& outMidi, const juce::MidiBuffer& inMidi, int numSamples, double sampleRate);
    void writeToStream(juce::MemoryOutputStream& s) const; bool readFromStream(juce::MemoryInputStream& s);
    void setAbletonLink(AbletonLink* l) noexcept; void setLinkFollowsTransport(bool shouldFollow) noexcept; bool getLinkFollowsTransport() const noexcept;
    void setSfzPlayer(class SfzPlayer* player) noexcept; void setSelectedSfLiveChannels(uint16_t channelMask) noexcept; uint16_t getAllSfPlayerChannelMask() const noexcept;
    void setSelectedLiveChannel(int ch1Based) noexcept; int getSelectedLiveChannel() const noexcept; void setSelectedTrack(int trackIndex) noexcept; SelectedLiveTarget getSelectedLiveTarget() const noexcept; uint16_t getSfzInstrumentChannelMask() const noexcept;
    void setRecordingTrack(int trackIndex) noexcept; int getRecordingTrackIndex() const noexcept; void drainRecordedEvents();
    static constexpr int kActivityFlagCount = 64; bool getMidiActivityAndClear(int trackIndex) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerEngine)
};

inline int SequencerEngine::addNetworkAudioTrack (int64_t routeId, int32_t sourceId, int sourceChannel,
                                                   const juce::String& sourceName, const juce::String& userName)
{
    if (routeId == 0 || sourceChannel < 0)
        return -1;

    auto current = impl->getTracks();
    for (size_t i = 0; i < current->size(); ++i)
    {
        const auto& track = *(*current)[i];
        if (track.type == TrackType::Audio
            && track.networkRouteId == routeId
            && track.networkSourceChannel == sourceChannel)
            return (int) i;
    }

    NetworkAudioInput input;
    input.routeId = routeId;
    input.sourceKey = routeId;
    input.sourceId = sourceId;
    input.sourceChannel = sourceChannel;
    input.sourceName = sourceName;
    input.userName = userName;

    auto track = SequencerTrack::makeAudio (input);
    if (userName.isNotEmpty() && sourceName.isNotEmpty())
        track->name = userName + " | " + sourceName + " Ch " + juce::String (sourceChannel + 1);
    else if (sourceName.isNotEmpty())
        track->name = sourceName + " Ch " + juce::String (sourceChannel + 1);

    const int newTrackIndex = (int) current->size();
    auto next = std::make_shared<Impl::TrackList> (*current);
    next->push_back (std::move (track));
    impl->publishTracks (std::move (next));
    return newTrackIndex;
}

inline bool SequencerEngine::setNetworkAudioTrackRoute (int trackIndex, int64_t routeId,
                                                          int32_t sourceId, int sourceChannel)
{
    auto snap = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) snap->size())
        || (*snap)[(size_t) trackIndex]->type != TrackType::Audio
        || routeId == 0 || sourceChannel < 0)
        return false;

    auto& track = *(*snap)[(size_t) trackIndex];
    track.networkRouteId = routeId;
    track.networkSourceId = sourceId;
    track.networkSourceChannel = sourceChannel;
    return true;
}

inline bool SequencerEngine::isNetworkAudioTrack (int trackIndex) const noexcept
{
    auto snap = impl->getTracks();
    return juce::isPositiveAndBelow (trackIndex, (int) snap->size())
        && (*snap)[(size_t) trackIndex]->type == TrackType::Audio;
}

inline bool SequencerEngine::getNetworkAudioRoute (int trackIndex, int64_t& routeId,
                                                     int32_t& sourceId, int& sourceChannel) const noexcept
{
    auto snap = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) snap->size())
        || (*snap)[(size_t) trackIndex]->type != TrackType::Audio)
        return false;

    const auto& track = *(*snap)[(size_t) trackIndex];
    routeId = track.networkRouteId;
    sourceId = track.networkSourceId;
    sourceChannel = track.networkSourceChannel;
    return routeId != 0;
}
