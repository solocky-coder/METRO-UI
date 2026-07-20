#pragma once
#include "SequencerTrack.h"
#include "AbletonLink.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
//  SequencerTrackInfo  —  lightweight metadata snapshot of a SequencerTrack.
//==============================================================================
struct SequencerTrackInfo
{
    TrackType     type        = TrackType::MainSlice;
    bool          enabled     = true;
    bool          solo        = false;
    float         volumeDb    = 0.0f;   // -60..+6, not yet applied to audio output
    float         pan         = 0.0f;   // -1..+1, not yet applied to audio output
    juce::String  name;
    juce::Colour  colour      = juce::Colour (0xFF3A6080);
    int           sliceIdx    = -1;
    int           midiChannel = 0;
    Sf2PresetInfo preset;
    int           numClips    = 0;
    // See SequencerTrack::isSfzInstrument — only meaningful when
    // type == TrackType::SfPlayer; distinguishes a real .sfz-file
    // instrument track from an SF2 preset track.
    bool          isSfzInstrument = false;
};

//==============================================================================
//  SelectedLiveTarget  —  the single authoritative description of where a
//  normal channel-1 live-MIDI performance stream should go right now.
//
//  This is the "one shared source of truth" for the selected-track live
//  routing workflow (see docs/selected-track-live-midi-workflow.md): the
//  selected arranger track owns live channel-1 input, regardless of which
//  of the three playable engines (slicer, SF2/FluidSynth, SFZ) it is.
//  Track MIDI-channel assignments (Track Inspector) remain the actual
//  routing/output mechanism; this struct just reports, authoritatively,
//  which one currently applies to a plain channel-1 keyboard.
//==============================================================================
enum class LiveTargetPlayer { none, slicer, sf2, sfz };

struct SelectedLiveTarget
{
    LiveTargetPlayer player      = LiveTargetPlayer::none;
    int              midiChannel = 0;   // 1-16; 0 means no live target
    int              trackIndex  = -1;  // stable track index, for diagnostics/UI (-1 = none)
};

//==============================================================================
//  SequencerClipInfo  —  lightweight snapshot of a single ClipSlot.
//==============================================================================
struct SequencerClipInfo
{
    int64_t startTick  = 0;
    int64_t lengthTicks = MidiClip::kPPQ * 4 * 4;
    int64_t endTick() const noexcept { return startTick + lengthTicks; }
};

class SequencerEngine
{
public:
    SequencerEngine();
    ~SequencerEngine();

    //==========================================================================
    //  Transport
    //==========================================================================
    bool    isPlaying()         const noexcept;
    bool    isLooping()         const noexcept;
    bool    isRecording()       const noexcept;
    int64_t getPlayheadTick()   const noexcept;
    double  getPlayheadBeats()  const noexcept;
    float   getBpm()            const noexcept;
    bool    getSyncToHost()     const noexcept;

    /** Global length = end of last clip across all tracks (governs playhead wrap). */
    int64_t getLengthTicks()    const noexcept;

    void play();
    void stop();
    void rewind();

    void setLooping   (bool v);
    void setRecording (bool v);
    void setSyncToHost(bool v);
    void setBpm       (float b);
    void setHostBpm   (float b);
    void seekToTick   (int64_t tick);

    /** Backward-compat: sets clip 0 length on all tracks. */
    void setLengthTicks (int64_t ticks);

    //==========================================================================
    //  Track management  (message thread)
    //==========================================================================
    int                getNumTracks()       const;
    SequencerTrackInfo getTrackInfo (int i) const;

    void setTrackEnabled (int i, bool enabled);

    /** Solo follows standard mixer convention: while any track is soloed,
     *  only soloed tracks are audible, regardless of their own enabled/mute
     *  state. Clearing solo on all tracks restores normal enabled-based
     *  gating. */
    void setTrackSolo (int i, bool solo);

    /** Stored on the track and persisted; not yet applied to audio output. */
    void setTrackVolumeDb (int i, float volumeDb);
    /** Stored on the track and persisted; not yet applied to audio output. -1..+1 */
    void setTrackPan (int i, float pan);

    /** Change an SFZ-instrument SfPlayer track's assigned MIDI channel
     *  (0-based) directly — no FluidSynth preset call involved, unlike
     *  addOrUpdateSfTrackOnChannel() which is SF2-preset-specific. No-op if
     *  the track isn't an SFZ-instrument SfPlayer track. */
    void setSfzTrackChannel (int trackIndex, int midiChannel0Based);

    void addMainTrack();
    void addChromaticTrack (int sliceIdx, int chromaticChannel,
                            const juce::String& name, juce::Colour colour);
    void removeChromaticTrack (int sliceIdx);
    void addSfTrack (const Sf2PresetInfo& preset, juce::Colour colour);
    void removeSfTrack (int trackIndex);
    void rebuildSfTracks (const std::vector<Sf2PresetInfo>& presets,
                          const juce::Colour* palette, int paletteSize);

    /** Add or update an SF2 track for a given preset on a specific MIDI channel (0-based).
     *  If a track already exists for this preset, its channel is updated.
     *  Also calls sfzPlayer->setPresetOnChannel() so audio follows immediately. */
    void addOrUpdateSfTrackOnChannel (const Sf2PresetInfo& preset, int midiChannel0Based,
                                      juce::Colour colour);

    /** Add a single SFZ track (one channel covers the whole instrument).
     *  If an SFZ track already exists (identified by name), it is replaced. */
    void addSfzTrack (const juce::String& name, int midiChannel0Based, juce::Colour colour);

    //==========================================================================
    //  Clip management  (message thread)
    //==========================================================================

    /** Returns the number of clips on a track. */
    int getNumClips (int trackIndex) const;

    /** Snapshot of a clip's position+length. */
    SequencerClipInfo getClipInfo (int trackIndex, int clipIndex) const;

    /** Direct access to a MidiClip for editing (piano roll). Null if out of range. */
    MidiClip* getClip (int trackIndex, int clipIndex = 0);

    /** Legacy single-clip accessor — main track clip 0. */
    MidiClip& getClip();

    /** Create a new empty clip on the track. Returns the new clip index. */
    int addClip (int trackIndex, int64_t startTick,
                 int64_t lengthTicks = MidiClip::kPPQ * 4 * 4);

    /** Delete a clip by index. */
    void removeClip (int trackIndex, int clipIndex);

    /** Move a clip's start position. */
    void setClipStartTick (int trackIndex, int clipIndex, int64_t newStartTick);

    /** Resize a clip. */
    void setClipLengthTicks (int trackIndex, int clipIndex, int64_t newLength);

    /** Backward-compat per-track length helpers (operate on clip 0). */
    int64_t getTrackLengthTicks (int trackIndex) const noexcept;
    void    setTrackLengthTicks (int trackIndex, int64_t ticks);

    //==========================================================================
    //  Audio thread
    //==========================================================================
    void processBlock (juce::MidiBuffer& outMidi, const juce::MidiBuffer& inMidi,
                       int numSamples, double sampleRate);

    //==========================================================================
    //  Serialisation
    //==========================================================================
    void writeToStream (juce::MemoryOutputStream& s) const;
    bool readFromStream (juce::MemoryInputStream& s);

    //==========================================================================
    //  Ableton Link
    //==========================================================================
    void setAbletonLink (AbletonLink* l) noexcept;

    //==========================================================================
    //  SfzPlayer integration  (multi-timbral channel assignment)
    //==========================================================================
    /** Store a pointer to the SfzPlayer so that rebuildSfTracks() and
     *  addSfTrack() can call setPresetOnChannel() automatically.
     *  Call once from PluginProcessor constructor, before any SF2 is loaded. */
    void setSfzPlayer (class SfzPlayer* player) noexcept;

    /** Legacy/vestigial: sets SfzPlayer's own liveInputChannelMask, which is
     *  no longer consulted for actual message routing (SfzPlayer::process()
     *  now routes purely by each message's own channel — see SfzPlayer.cpp).
     *  Retained only for the plugin-tab-focus MidiRouteMode switch in
     *  PluginProcessor::setMidiRouteMode(), which is unrelated to arranger
     *  track selection. Selected-track live routing must use
     *  setSelectedTrackIndex() / getSelectedLiveTarget() below instead — do
     *  not wire any new behaviour through this mask. */
    void setSelectedSfLiveChannels (uint16_t channelMask) noexcept;

    /** Returns a bitmask (bit N = channel N+1) covering every SfPlayer track's
     *  assigned MIDI channel (both SF2-preset and SFZ-instrument tracks).
     *  Used to keep the slicer from seeing MIDI that belongs to an SF-player
     *  track, whether that MIDI came from live input or recorded playback. */
    uint16_t getAllSfPlayerChannelMask() const noexcept;

    /** Same as getAllSfPlayerChannelMask(), restricted to SF2-preset SfPlayer
     *  tracks (isSfzInstrument == false). Used to keep the SF2/FluidSynth
     *  engine's live/recorded MIDI isolated from SFZ-instrument channels. */
    uint16_t getSf2PresetChannelMask() const noexcept;

    /** Same as getAllSfPlayerChannelMask(), restricted to SFZ-instrument
     *  SfPlayer tracks (isSfzInstrument == true). */
    uint16_t getSfzInstrumentChannelMask() const noexcept;

    //==========================================================================
    //  Selected-track live MIDI routing — single source of truth
    //==========================================================================

    /** Set which track index is the "selected" arranger track for live-input
     *  purposes. Pass -1 for none. Called from ArrangeView::selectTrack on
     *  the message thread — atomic, safe to read from the audio thread.
     *
     *  This is the ONLY thing a caller needs to set when selection changes;
     *  getSelectedLiveTarget() derives the destination player and MIDI
     *  channel directly from the track's own type / isSfzInstrument /
     *  midiChannel fields, so that logic is never duplicated elsewhere. */
    void setSelectedTrackIndex (int trackIndex) noexcept;

    /** Returns the current selected track index, or -1 if none. */
    int  getSelectedTrackIndex() const noexcept;

    /** Returns the authoritative live-input routing target for whichever
     *  track is currently selected (see SelectedLiveTarget above). Safe to
     *  call from the audio thread once per block. */
    SelectedLiveTarget getSelectedLiveTarget() const noexcept;

    /** Tell the engine which track index receives recorded MIDI.
     *  Call alongside setSelectedTrackIndex when the user selects a track.
     *  Pass -1 to disable recording. */
    void setRecordingTrack (int trackIndex) noexcept;

    /** Drains recorded-note events captured by the audio thread during
     *  processBlock() and applies them to the target clip via
     *  MidiClip::addNote()/setNoteDuration().
     *
     *  MESSAGE-THREAD ONLY. The audio thread never touches MidiClip during
     *  recording — it resolves a clip-relative tick and pushes a small POD
     *  event into a lock-free FIFO; this is the only place that still calls
     *  the (audio-thread-illegal) MidiClip editing calls for recording.
     *  Call this periodically (e.g. from a UI Timer) while recording. */
    void drainRecordedEvents();

    //==========================================================================
    //  Per-track MIDI activity flags (for the receive indicator in TrackHeaderStrip)
    //==========================================================================

    static constexpr int kActivityFlagCount = 64;

    /** Returns true and clears the flag if the audio thread set it since last
     *  call.  Safe to call from the message thread (atomic exchange). */
    bool getMidiActivityAndClear (int trackIndex) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
