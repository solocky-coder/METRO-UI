#pragma once

#include "../PluginProcessor.h"
#include "../network/MetroNetworkAudio.h"
#include "../network/NetworkAudioRecorder.h"
#include "../network/NetworkTrackMeters.h"
#include "../sequencer/AudioClipPlayer.h"
#include <array>
#include <atomic>
#include <utility>
#include <vector>

class NetworkAudioProcessor final : public DysektProcessor
{
public:
    NetworkAudioProcessor() { activeProcessor.store (this, std::memory_order_release); }
    ~NetworkAudioProcessor() override
    {
        for (auto& slot : trackRecorders)   // finalize any take still being written
            slot.recorder.stop();
        if (activeProcessor.load (std::memory_order_acquire) == this)
            activeProcessor.store (nullptr, std::memory_order_release);
    }

    static void setActiveNetworkAudio (MetroNetworkAudio* audio) noexcept
    {
        activeNetworkAudio.store (audio, std::memory_order_release);
        applyPreparedFormat (audio);   // the device may have been prepared before this object existed
    }
    static int createActiveNetworkAudioTrack (int64_t sourceKey, int32_t sourceId, int sourceChannel, const juce::String& sourceName, const juce::String& userName = {}) noexcept
    {
        auto* processor = activeProcessor.load (std::memory_order_acquire);
        if (processor == nullptr) return -1;
        return processor->sequencer.addNetworkAudioTrack (sourceKey, sourceId, juce::jmax (0, sourceChannel), sourceName, userName);
    }
    // True if any Audio track already routes this source (any channel selection).
    // Message-thread use; reads the sequencer's published track snapshot.
    static bool hasActiveNetworkAudioTrack (int64_t sourceKey) noexcept
    {
        auto* processor = activeProcessor.load (std::memory_order_acquire);
        if (processor == nullptr) return false;

        const int numTracks = processor->sequencer.getNumTracks();
        for (int i = 0; i < numTracks; ++i)
        {
            int64_t key = 0;
            int32_t id = 0;
            int channel = 0;
            if (processor->sequencer.getNetworkAudioRoute (i, key, id, channel) && key == sourceKey)
                return true;
        }
        return false;
    }
    static void setActiveNetworkSource (int64_t sourceKey, int sourceChannel) noexcept
    {
        activeSourceKey.store (sourceKey, std::memory_order_release);
        activeSourceChannel.store (juce::jmax (0, sourceChannel), std::memory_order_release);
    }
    void setNetworkAudio (MetroNetworkAudio* audio) noexcept { networkAudio = audio; setActiveNetworkAudio (audio); }
    // Called from the message thread (see ArrangeView::timerCallback()).
    // processBlock() finalizes each track's recorder and publishes the result
    // in that track's slot on the audio thread; this just moves
    // that already-captured data into the timeline, so it does no recording
    // work of its own and touches no realtime state.
    //
    // Every routed Audio track records its own take, so several can finish in
    // the same frame. Returns true if at least one clip was committed.
    bool commitLastRecordedClipToTimeline()
    {
        bool committedAny = false;

        for (int trackIndex = 0; trackIndex < kMaxRecordTracks; ++trackIndex)
        {
            auto& slot = trackRecorders[(size_t) trackIndex];

            // The audio thread only writes `published` while the flag is clear,
            // and this thread only touches it while the flag is set.
            if (! slot.publishedReady.load (std::memory_order_acquire))
                continue;

            const auto clip = slot.published;
            slot.published = {};
            slot.publishedReady.store (false, std::memory_order_release);

            if (! clip.isValid())
                continue;

            committedAny |= sequencer.addRecordedAudioClip (trackIndex, clip.file, clip.sampleRate, clip.channels,
                                                            clip.startTick, clip.lengthSamples);
        }

        return committedAny;
    }

    // Live "recording in progress" info for ArrangeView to draw a growing
    // clip + waveform before the take is committed above. Same
    // message-thread-from-audio-thread-state pattern as
    // commitLastRecordedClipToTimeline(): each recorder's live state is read
    // through NetworkAudioRecorder::getLiveSnapshot(), which takes its own lock.
    struct LiveRecordingInfo
    {
        NetworkAudioRecorder::LiveSnapshot snapshot;
        int trackIndex = -1;
    };
    // One entry per track that is recording right now; empty (and cheap)
    // when nothing is.
    std::vector<LiveRecordingInfo> getLiveRecordingSnapshots() const
    {
        std::vector<LiveRecordingInfo> result;
        for (int trackIndex = 0; trackIndex < kMaxRecordTracks; ++trackIndex)
        {
            auto snapshot = trackRecorders[(size_t) trackIndex].recorder.getLiveSnapshot();
            if (! snapshot.isRecording)
                continue;

            LiveRecordingInfo info;
            info.snapshot = std::move (snapshot);
            info.trackIndex = trackIndex;
            result.push_back (std::move (info));
        }
        return result;
    }

    // Rebuilds the AudioClip reader cache from the current arrangement.
    // Called from the message thread — see ArrangeView::timerCallback(),
    // which polls this every frame alongside commitLastRecordedClipToTimeline()
    // so a clip that was just recorded, moved, or deleted is reflected in
    // playback promptly without doing any of that bookkeeping on the audio
    // thread itself.
    void syncAudioClipPlayback() { audioClipPlayer.syncAllTracks (sequencer); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override
    {
        DysektProcessor::prepareToPlay (sampleRate, samplesPerBlock);
        networkSampleRate = sampleRate;

        // Tell the AOO sinks the host's block size (message thread, not the
        // audio callback). Remembered so a MetroNetworkAudio attached later
        // is configured too - see applyPreparedFormat().
        preparedSampleRate.store (sampleRate, std::memory_order_release);
        preparedBlockSize.store (samplesPerBlock, std::memory_order_release);
        applyPreparedFormat (networkAudio != nullptr ? networkAudio
                                                     : activeNetworkAudio.load (std::memory_order_acquire));
        const int capacity = juce::jmax (4096, samplesPerBlock > 0 ? samplesPerBlock : 512);
        networkBuffer.setSize (2, capacity, false, true, true);
        audioClipScratch.setSize (2, capacity, false, true, true);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        // Diagnostics only: measures the whole callback (including the base
        // DysektProcessor::processBlock) and reports it to MetroNetworkAudio,
        // which logs calls/sec and avg/max duration once a second.
        struct BlockTimer
        {
            MetroNetworkAudio* audio;
            juce::AudioBuffer<float>* out;
            int numSamples;
            juce::int64 startTicks = juce::Time::getHighResolutionTicks();
            ~BlockTimer()
            {
                if (audio != nullptr)
                    audio->noteHostBlock (numSamples,
                                          (int64_t) (juce::Time::highResolutionTicksToSeconds (
                                              juce::Time::getHighResolutionTicks() - startTicks) * 1.0e6),
                                          out != nullptr && numSamples > 0 ? out->getMagnitude (0, numSamples) : 0.0f);
            }
        } blockTimer { networkAudio != nullptr ? networkAudio : activeNetworkAudio.load (std::memory_order_acquire),
                       &buffer, buffer.getNumSamples() };

        DysektProcessor::processBlock (buffer, midi);

        // Recorded AudioClip playback. Deliberately runs independent of the
        // live network-audio system below — a project with committed clips
        // should still hear them during playback even with no network
        // source connected at all, whereas everything from here down to the
        // early `audio == nullptr` return only concerns *live* network
        // audio (routing, recording, monitoring fallback).
        {
            const int clipNumSamples = buffer.getNumSamples();
            if (clipNumSamples > 0 && clipNumSamples <= audioClipScratch.getNumSamples() && sequencer.isPlaying())
            {
                const double clipSampleRate = networkSampleRate > 0.0 ? networkSampleRate : 44100.0;
                const int64_t clipPlayheadTick = sequencer.getPlayheadTick();
                const double bpm = sequencer.getBpm();
                const int numTracksForClips = sequencer.getNumTracks();
                for (int trackIndex = 0; trackIndex < numTracksForClips; ++trackIndex)
                {
                    const auto clipTrackInfo = sequencer.getTrackInfo (trackIndex);
                    if (clipTrackInfo.type != TrackType::Audio || ! clipTrackInfo.enabled) continue;

                    const float gain = juce::Decibels::decibelsToGain (clipTrackInfo.volumeDb);
                    const float pan = juce::jlimit (-1.0f, 1.0f, clipTrackInfo.pan);
                    const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
                    const float leftGain = gain * std::cos (angle);
                    const float rightGain = gain * std::sin (angle);

                    audioClipPlayer.render (trackIndex, buffer, audioClipScratch, clipNumSamples,
                                            clipPlayheadTick, bpm, clipSampleRate, leftGain, rightGain);
                }
            }
        }

        MetroNetworkAudio* audio = networkAudio;
        if (audio == nullptr) audio = activeNetworkAudio.load (std::memory_order_acquire);
        if (audio == nullptr || ! audio->isRunning()) return;

        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0 || numSamples > networkBuffer.getNumSamples()) return;

        bool renderedTrack = false;
        const int numTracks = sequencer.getNumTracks();
        const bool transportRecording = sequencer.isRecording() && sequencer.isPlaying();
        const int64_t playheadTick = sequencer.getPlayheadTick();

        // Every enabled Audio track that has a network route records its own
        // take while the transport records, each into its own file and clip.
        // (There is no audio arm control yet, so routed means armed.)
        for (int trackIndex = 0; trackIndex < numTracks; ++trackIndex)
        {
            const auto info = sequencer.getTrackInfo (trackIndex);
            if (info.type != TrackType::Audio || ! info.enabled) continue;

            int64_t sourceKey = 0;
            int32_t sourceId = 0;
            int sourceChannel = 0;
            if (! sequencer.getNetworkAudioRoute (trackIndex, sourceKey, sourceId, sourceChannel)) continue;
            sourceChannel = juce::jmax (0, sourceChannel);

            TrackRecorder* slot = trackIndex < kMaxRecordTracks ? &trackRecorders[(size_t) trackIndex] : nullptr;
            if (slot != nullptr) slot->seenThisBlock = true;

            networkBuffer.clear();
            if (! audio->processSourceChannel (networkBuffer, numSamples, networkSampleRate, sourceKey, sourceChannel))
            {
                // Source not delivering this block. If this track is already
                // recording, write silence so its clip stays aligned with the
                // other tracks' clips instead of drifting early.
                if (slot != nullptr && transportRecording && slot->recorder.isRecording())
                    slot->recorder.push (networkBuffer, numSamples);
                continue;
            }

            // The sequencer holds the playhead at tick 0 during count-in. Start
            // capture only after the first real post-count-in tick is observable.
            // A track starts with its first delivered audio, and the clip's start
            // tick records exactly where on the timeline that was.
            if (slot != nullptr && transportRecording && playheadTick > 0 && ! slot->recorder.isRecording())
                startTrackRecording (*slot, info.name, playheadTick);

            const float gain = juce::Decibels::decibelsToGain (info.volumeDb);
            const float pan = juce::jlimit (-1.0f, 1.0f, info.pan);
            float leftGain, rightGain;
            if (MetroNetworkAudio::isStereoRoute (sourceChannel))
            {
                // True stereo: the pan control acts as a balance, unity at
                // centre. (Constant-power pan would drop both sides ~3 dB.)
                leftGain = gain * juce::jmin (1.0f, 1.0f - pan);
                rightGain = gain * juce::jmin (1.0f, 1.0f + pan);
            }
            else
            {
                const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
                leftGain = gain * std::cos (angle);
                rightGain = gain * std::sin (angle);
            }
            if (buffer.getNumChannels() > 0) buffer.addFrom (0, 0, networkBuffer, 0, 0, numSamples, leftGain);
            if (buffer.getNumChannels() > 1) buffer.addFrom (1, 0, networkBuffer, 1, 0, numSamples, rightGain);
            // Post-fader/pan sample peaks for this track's horizontal header meter
            // (read by TrackHeaderStrip on the UI thread). Lock-free, no allocation.
            {
                const float peakL = networkBuffer.getMagnitude (0, 0, numSamples) * leftGain;
                const float peakR = networkBuffer.getNumChannels() > 1
                                        ? networkBuffer.getMagnitude (1, 0, numSamples) * rightGain
                                        : peakL;
                NetworkTrackMeters::push (trackIndex, peakL, peakR,
                                          MetroNetworkAudio::isStereoRoute (sourceChannel));
            }
            renderedTrack = true;

            // Push this track's route before the scratch buffer is reused for
            // the next Audio track. No allocation/copy is performed on the
            // audio thread.
            if (slot != nullptr && transportRecording && slot->recorder.isRecording())
                slot->recorder.push (networkBuffer, numSamples);
        }

        finishRecordings (transportRecording);

        // Settings/source-list fallback is playback-only and can never be
        // accidentally captured when no Audio track exists.
        if (! renderedTrack)
        {
            const auto sourceKey = activeSourceKey.load (std::memory_order_acquire);
            const auto sourceChannel = activeSourceChannel.load (std::memory_order_acquire);
            if (sourceKey != 0)
            {
                networkBuffer.clear();
                if (audio->processSourceChannel (networkBuffer, numSamples, networkSampleRate, sourceKey, sourceChannel))
                {
                    if (buffer.getNumChannels() > 0) buffer.addFrom (0, 0, networkBuffer, 0, 0, numSamples);
                    if (buffer.getNumChannels() > 1) buffer.addFrom (1, 0, networkBuffer, 1, 0, numSamples);
                }
            }
        }
    }

private:
    // Upper bound on tracks that can record network audio at once (indexed by
    // track number). A fixed array keeps all of this allocation-free on the
    // audio thread.
    static constexpr int kMaxRecordTracks = 64;

    struct TrackRecorder
    {
        NetworkAudioRecorder recorder;

        // Finished take, handed from the audio thread to the message thread.
        // `staged` is audio-thread-only. `published` is written by the audio
        // thread only while publishedReady is false and read/cleared by the
        // message thread only while it is true.
        NetworkAudioRecorder::Clip staged;
        NetworkAudioRecorder::Clip published;
        std::atomic<bool> publishedReady { false };

        bool seenThisBlock = false;   // audio thread only
    };

    void startTrackRecording (TrackRecorder& slot, const juce::String& trackName, int64_t startTick) noexcept
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("DYSEKT-SF Recordings");
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
        auto safeName = trackName.replaceCharacters ("\\/:*?\"<>|", "_________");
        const auto file = dir.getNonexistentChildFile (safeName.isEmpty() ? "AOO" : safeName, "_" + stamp + ".wav");
        slot.recorder.start (file, networkSampleRate, 2, startTick);
    }

    // End of each network callback: stop every recorder whose track is no
    // longer being fed (transport stopped, or the track was disabled, unrouted
    // or deleted), then publish finished takes to the message thread.
    void finishRecordings (bool transportRecording) noexcept
    {
        for (auto& slot : trackRecorders)
        {
            if (slot.recorder.isRecording() && (! transportRecording || ! slot.seenThisBlock))
                slot.staged = slot.recorder.stop();
            slot.seenThisBlock = false;

            // A finished take waits in `staged` until the message thread has
            // collected the previous one, so no take is ever overwritten.
            if (slot.staged.channels > 0 && ! slot.publishedReady.load (std::memory_order_acquire))
            {
                slot.published = std::move (slot.staged);
                slot.staged = {};
                slot.publishedReady.store (true, std::memory_order_release);
            }
        }
    }

    // Pushes the last prepareToPlay() format into a MetroNetworkAudio. Cheap and
    // idempotent (prepare() does nothing when the block size is unchanged).
    static void applyPreparedFormat (MetroNetworkAudio* audio) noexcept
    {
        const int block = preparedBlockSize.load (std::memory_order_acquire);
        if (audio == nullptr || block <= 0) return;
        try { audio->prepare (preparedSampleRate.load (std::memory_order_acquire), block); }
        catch (...) {}
    }

    inline static std::atomic<double> preparedSampleRate { 0.0 };
    inline static std::atomic<int> preparedBlockSize { 0 };
    inline static std::atomic<MetroNetworkAudio*> activeNetworkAudio { nullptr };
    inline static std::atomic<NetworkAudioProcessor*> activeProcessor { nullptr };
    inline static std::atomic<int64_t> activeSourceKey { 0 };
    inline static std::atomic<int> activeSourceChannel { 0 };
    MetroNetworkAudio* networkAudio = nullptr;
    double networkSampleRate = 44100.0;
    juce::AudioBuffer<float> networkBuffer;
    std::array<TrackRecorder, kMaxRecordTracks> trackRecorders;
    AudioClipPlayer audioClipPlayer;
    juce::AudioBuffer<float> audioClipScratch;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioProcessor)
};
