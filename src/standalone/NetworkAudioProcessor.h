#pragma once

#include "../PluginProcessor.h"
#include "../network/MetroNetworkAudio.h"
#include "../network/NetworkAudioRecorder.h"
#include "../sequencer/AudioClipPlayer.h"
#include <atomic>

class NetworkAudioProcessor final : public DysektProcessor
{
public:
    NetworkAudioProcessor() { activeProcessor.store (this, std::memory_order_release); }
    ~NetworkAudioProcessor() override
    {
        stopNetworkRecording();
        if (activeProcessor.load (std::memory_order_acquire) == this)
            activeProcessor.store (nullptr, std::memory_order_release);
    }

    static void setActiveNetworkAudio (MetroNetworkAudio* audio) noexcept { activeNetworkAudio.store (audio, std::memory_order_release); }
    static int createActiveNetworkAudioTrack (int64_t sourceKey, int32_t sourceId, int sourceChannel, const juce::String& sourceName, const juce::String& userName = {}) noexcept
    {
        auto* processor = activeProcessor.load (std::memory_order_acquire);
        if (processor == nullptr) return -1;
        return processor->sequencer.addNetworkAudioTrack (sourceKey, sourceId, juce::jmax (0, sourceChannel), sourceName, userName);
    }
    static void setActiveNetworkSource (int64_t sourceKey, int sourceChannel) noexcept
    {
        activeSourceKey.store (sourceKey, std::memory_order_release);
        activeSourceChannel.store (juce::jmax (0, sourceChannel), std::memory_order_release);
    }
    static bool startActiveNetworkRecording (const juce::File& file, int channels = 2) noexcept
    {
        auto* processor = activeProcessor.load (std::memory_order_acquire);
        return processor != nullptr && processor->startNetworkRecording (file, channels, processor->sequencer.getPlayheadTick());
    }
    static void stopActiveNetworkRecording() noexcept { if (auto* p = activeProcessor.load (std::memory_order_acquire)) p->stopNetworkRecording(); }
    static bool isActiveNetworkRecording() noexcept { auto* p = activeProcessor.load (std::memory_order_acquire); return p != nullptr && p->recorder.isRecording(); }

    void setNetworkAudio (MetroNetworkAudio* audio) noexcept { networkAudio = audio; setActiveNetworkAudio (audio); }
    bool startNetworkRecording (const juce::File& file, int channels = 2, int64_t startTick = 0) noexcept
    {
        const double rate = networkSampleRate > 0.0 ? networkSampleRate : 44100.0;
        return recorder.start (file, rate, juce::jlimit (1, 64, channels), startTick);
    }
    void stopNetworkRecording() noexcept { lastRecordedClip = recorder.stop(); }
    NetworkAudioRecorder::Clip getLastRecordedNetworkClip() const { return lastRecordedClip; }

    // Called from the message thread (see ArrangeView::timerCallback()).
    // processBlock() finalizes the recorder and stashes the result in
    // lastRecordedClip/recordTrackArmed on the audio thread; this just moves
    // that already-captured data into the timeline, so it does no recording
    // work of its own and touches no realtime state.
    bool commitLastRecordedClipToTimeline()
    {
        if (! lastRecordedClip.isValid())
            return false;

        const int trackIndex = recordTrackArmed;
        if (trackIndex < 0)
            return false;

        const auto clip = lastRecordedClip;
        lastRecordedClip = {};

        const bool committed = sequencer.addRecordedAudioClip(
            trackIndex,
            clip.file,
            clip.sampleRate,
            clip.channels,
            clip.startTick,
            clip.lengthSamples);

        return committed;
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
        const int capacity = juce::jmax (4096, samplesPerBlock > 0 ? samplesPerBlock : 512);
        networkBuffer.setSize (2, capacity, false, true, true);
        audioClipScratch.setSize (2, capacity, false, true, true);
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
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
        bool capturedTrack = false;
        const int numTracks = sequencer.getNumTracks();
        const bool transportRecording = sequencer.isRecording() && sequencer.isPlaying();
        const int64_t playheadTick = sequencer.getPlayheadTick();

        // Audio record-arm is deliberately separate from the MIDI recording
        // track. Until an audio-arm control exists, the first enabled Audio
        // track is the active capture route.
        int recordTrackIndex = -1;
        for (int i = 0; i < numTracks; ++i)
        {
            const auto info = sequencer.getTrackInfo (i);
            if (info.type == TrackType::Audio && info.enabled) { recordTrackIndex = i; break; }
        }

        // The sequencer holds the playhead at tick 0 during count-in. Start
        // capture only after the first real post-count-in tick is observable.
        if (transportRecording && recordTrackIndex >= 0 && playheadTick > 0 && ! recorder.isRecording())
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("DYSEKT-SF Recordings");
            const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");
            auto safeName = sequencer.getTrackInfo (recordTrackIndex).name;
            safeName = safeName.replaceCharacters ("\\/:*?\"<>|", "_________");
            const auto file = dir.getNonexistentChildFile (safeName.isEmpty() ? "AOO" : safeName, "_" + stamp + ".wav");
            recordTrackArmed = recordTrackIndex;
            recorder.start (file, networkSampleRate, 2, playheadTick);
        }

        if ((! transportRecording || recordTrackIndex < 0) && recorder.isRecording())
            lastRecordedClip = recorder.stop();

        for (int trackIndex = 0; trackIndex < numTracks; ++trackIndex)
        {
            const auto info = sequencer.getTrackInfo (trackIndex);
            if (info.type != TrackType::Audio || ! info.enabled) continue;

            int64_t sourceKey = 0;
            int32_t sourceId = 0;
            int sourceChannel = 0;
            if (! sequencer.getNetworkAudioRoute (trackIndex, sourceKey, sourceId, sourceChannel)) continue;
            sourceChannel = juce::jmax (0, sourceChannel);

            networkBuffer.clear();
            if (! audio->processSourceChannel (networkBuffer, numSamples, networkSampleRate, sourceKey, sourceChannel)) continue;

            const float gain = juce::Decibels::decibelsToGain (info.volumeDb);
            const float pan = juce::jlimit (-1.0f, 1.0f, info.pan);
            const float angle = (pan + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
            const float leftGain = gain * std::cos (angle);
            const float rightGain = gain * std::sin (angle);
            if (buffer.getNumChannels() > 0) buffer.addFrom (0, 0, networkBuffer, 0, 0, numSamples, leftGain);
            if (buffer.getNumChannels() > 1) buffer.addFrom (1, 0, networkBuffer, 1, 0, numSamples, rightGain);
            renderedTrack = true;

            // Push the selected route before the scratch buffer is reused for
            // another Audio track. No allocation/copy is performed on the
            // audio thread.
            if (recorder.isRecording() && trackIndex == recordTrackArmed && ! capturedTrack)
            {
                recorder.push (networkBuffer, numSamples);
                capturedTrack = true;
            }
        }

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
    inline static std::atomic<MetroNetworkAudio*> activeNetworkAudio { nullptr };
    inline static std::atomic<NetworkAudioProcessor*> activeProcessor { nullptr };
    inline static std::atomic<int64_t> activeSourceKey { 0 };
    inline static std::atomic<int> activeSourceChannel { 0 };
    MetroNetworkAudio* networkAudio = nullptr;
    double networkSampleRate = 44100.0;
    int recordTrackArmed = -1;
    juce::AudioBuffer<float> networkBuffer;
    NetworkAudioRecorder recorder;
    NetworkAudioRecorder::Clip lastRecordedClip;
    AudioClipPlayer audioClipPlayer;
    juce::AudioBuffer<float> audioClipScratch;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioProcessor)
};
