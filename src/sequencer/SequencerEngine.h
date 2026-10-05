#pragma once
#include "SequencerTrack.h"
#include "AbletonLink.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

enum class LiveTargetPlayer { none, slicer, sf2, sfz, network };
struct SelectedLiveTarget { LiveTargetPlayer player = LiveTargetPlayer::none; int midiChannel = 0; };

struct SequencerTrackInfo
{
    TrackType type = TrackType::MainSlice;
    bool enabled = true; bool solo = false; bool audioRecordArm = true; bool monitor = true; float volumeDb = 0.0f; float pan = 0.0f;
    juce::String name; juce::Colour colour = juce::Colour (0xFF3A6080);
    int sliceIdx = -1; int midiChannel = 0; Sf2PresetInfo preset; int numClips = 0;
    bool isSfzInstrument = false;
    int64_t networkRouteId = 0; int32_t networkSourceId = 0; int networkSourceChannel = 0;
    int linkedMidiDeviceId=0; int networkMidiDeviceId=0; bool networkMidiIsChild=false; juce::String networkMidiPeer;
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
    // Audio record arm: per track, multi-arm. setAll* touches Audio tracks only.
    void setAudioRecordArm(int i, bool armed); void setAllAudioRecordArm(bool armed);
    void setTrackVolumeDb(int i, float volumeDb); void setTrackPan(int i, float pan); void setTrackColour(int i, juce::Colour colour);
    int findSfTrackForChannel(int midiChannel0Based) const;
    void addMainTrack(); void addChromaticTrack(int sliceIdx, int chromaticChannel, const juce::String& name, juce::Colour colour); void removeChromaticTrack(int sliceIdx);
    void addSfTrack(const Sf2PresetInfo& preset, juce::Colour colour); void removeSfTrack(int trackIndex); void rebuildSfTracks(const std::vector<Sf2PresetInfo>& presets, const juce::Colour* palette, int paletteSize);
    void addOrUpdateSfTrackOnChannel(const Sf2PresetInfo& preset, int midiChannel0Based, juce::Colour colour);
    void addSfzTrack(const juce::String& name, int midiChannel0Based, juce::Colour colour); void removeSfzTrack();

    int addNetworkAudioTrack(int64_t routeId, int32_t sourceId, int sourceChannel, const juce::String& sourceName, const juce::String& userName = {});
    bool setNetworkAudioTrackRoute(int trackIndex, int64_t routeId, int32_t sourceId, int sourceChannel);
    bool isNetworkAudioTrack(int trackIndex) const noexcept;
    bool getNetworkAudioRoute(int trackIndex, int64_t& routeId, int32_t& sourceId, int& sourceChannel) const noexcept;
    // Renames every Audio track routed to sourceKey whose name still starts with
    // "<oldLabel> | " (the auto-generated name) so it starts with newLabel instead.
    // Returns the number of tracks renamed. Message thread.
    int renameNetworkAudioTracks(int64_t sourceKey, const juce::String& oldLabel, const juce::String& newLabel);

    // Per-device (per network source) controls. A device is identified by its sourceKey
    // and acts on every Audio track routed from it. Message thread.
    struct NetworkSourceState
    {
        int numTracks = 0;          // tracks routed from this source (0 = nothing to control yet)
        float gainDb = 0.0f;        // gain of the first track (they are normally kept equal)
        bool muted = false;         // every track muted
        bool solo = false;          // any track soloed
        bool recordArmed = false;   // every track armed
        bool monitor = true;        // every track audible on the output
    };
    NetworkSourceState getNetworkSourceState(int64_t sourceKey) const;
    void setNetworkSourceGainDb(int64_t sourceKey, float gainDb);
    void setNetworkSourceMuted(int64_t sourceKey, bool muted);
    void setNetworkSourceSolo(int64_t sourceKey, bool solo);
    void setNetworkSourceRecordArm(int64_t sourceKey, bool armed);
    void setNetworkSourceMonitor(int64_t sourceKey, bool monitor);

    static constexpr int kMaxNetworkMidiChildren=16;
    using NetworkMidiSink=void(*)(void*,int,const juce::MidiMessage&);
    void setNetworkMidiSink(NetworkMidiSink,void*) noexcept;
    /** Lets the host tell the engine which IP a network audio source lives at, so MIDI
        children of that audio track start at the audio device's own address. */
    using AudioPeerResolver=juce::String(*)(void*,int32_t sourceId);
    void setNetworkAudioPeerResolver(AudioPeerResolver,void*) noexcept;
    void sendNetworkMidi(int,const juce::MidiMessage&) const noexcept;
    int addNetworkMidiTrack(const juce::String&,const juce::String&);
    int addNetworkMidiChild(int,int=-1);
    bool removeNetworkMidiTrack(int);
    bool setNetworkMidiChannel(int,int);
    bool setNetworkMidiPeer(int,const juce::String&);
    juce::String getNetworkMidiPeer(int) const;
    bool isNetworkMidiTrack(int) const noexcept;
    int getNetworkMidiChildCount(int) const;
    int findNetworkMidiDevice(const juce::String&) const;
    /** Network audio track = parent. Adds ONE Network MIDI child track on that audio
        device (lowest free MIDI channel; the channel can be changed 1-16 in the child's
        inspector). Returns the new child's track index, or -1. */
    int addNetworkMidiChildForAudio(int audioTrackIndex, const juce::String& defaultPeer);
    /** Number of MIDI children on the device linked to this audio track (0 if unlinked). */
    int getLinkedNetworkMidiChildCount(int audioTrackIndex) const;
    /** Per-track nesting for the track list. For a MIDI child of a network audio track:
        parentIndex = that audio track's index, deviceId = the shared device id. For an
        audio parent: deviceId = its linked device id, childCount = number of children.
        Everything else is parentIndex -1, deviceId 0, childCount 0. */
    struct TrackNest { int parentIndex = -1; int deviceId = 0; int childCount = 0; };
    std::vector<TrackNest> getTrackNesting() const;
    void setNetworkMidiLinkState(int,int) const noexcept;
    int getNetworkMidiLinkState(int) const noexcept;
    int getSelectedNetworkMidiDevice() const noexcept;

    // True while at least one track (of any type) is soloed.
    bool isAnyTrackSoloed() const noexcept;

    //==========================================================================
    // Audio clips
    int getNumAudioClips(int trackIndex) const;
    AudioClip getAudioClip(int trackIndex, int clipIndex) const;
    int addAudioClip(int trackIndex, const AudioClip& clip);
    bool removeAudioClip(int trackIndex, int clipIndex);
    bool setAudioClipStartTick(int trackIndex, int clipIndex, int64_t newStartTick);

    // Attach a completed network recording to its destination Audio Track.
    bool addRecordedAudioClip(int trackIndex,
                              const juce::File& file,
                              double sampleRate,
                              int channels,
                              int64_t startTick,
                              int64_t lengthSamples);

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
