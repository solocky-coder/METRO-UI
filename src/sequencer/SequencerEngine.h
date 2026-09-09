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
    int64_t networkRouteId = 0; int64_t networkSourceKey = 0; int32_t networkSourceId = 0; int networkSourceChannel = 0;
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

    int addNetworkAudioTrack(int64_t routeId, int64_t sourceKey, int32_t sourceId, int sourceChannel,
                             const juce::String& sourceName, const juce::String& userName = {});
    bool setNetworkAudioTrackRoute(int trackIndex, int64_t routeId, int64_t sourceKey, int32_t sourceId, int sourceChannel);
    bool isNetworkAudioTrack(int trackIndex) const noexcept;
    bool getNetworkAudioRoute(int trackIndex, int64_t& routeId, int64_t& sourceKey, int32_t& sourceId, int& sourceChannel) const noexcept;

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
