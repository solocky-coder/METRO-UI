#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>
#include <utility>

class NetworkAudioRecorder final
{
public:
    struct Clip
    {
        juce::File file;
        double sampleRate = 0.0;
        int channels = 0;
        int64_t startTick = 0;
        int64_t lengthSamples = 0;
        bool isValid() const noexcept { return file.existsAsFile() && channels > 0 && sampleRate > 0.0 && lengthSamples > 0; }
    };

    // Snapshot of an in-progress recording, for drawing a growing "recording
    // now" clip + live waveform before the take is committed as a real
    // AudioClip. peaks is a plain copy — safe to hand across threads and to
    // hold onto after the call, unlike the live data it was copied from.
    struct LiveSnapshot
    {
        bool isRecording = false;
        juce::File file;
        double sampleRate = 0.0;
        int channels = 0;
        int64_t startTick = 0;
        int64_t recordedSamples = 0;
        std::vector<std::pair<float, float>> peaks;
    };

    NetworkAudioRecorder() = default;
    NetworkAudioRecorder (const NetworkAudioRecorder&) = delete;
    NetworkAudioRecorder& operator= (const NetworkAudioRecorder&) = delete;
    ~NetworkAudioRecorder() { stop(); }

    bool start (const juce::File& destination, double sampleRate, int channels, int64_t startTick = 0)
    {
        stop();
        if (destination == juce::File() || sampleRate <= 0.0 || channels <= 0) return false;
        destination.getParentDirectory().createDirectory();
        auto stream = std::unique_ptr<juce::FileOutputStream> (destination.createOutputStream());
        if (stream == nullptr) return false;
        juce::WavAudioFormat format;
        auto* rawWriter = format.createWriterFor (stream.release(), sampleRate,
                                                   static_cast<unsigned int> (channels), 24, {}, 0);
        if (rawWriter == nullptr) return false;
        writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>
            (rawWriter, backgroundThread, juce::jmax (1024, static_cast<int> (sampleRate * 2.0)));
        if (! backgroundThread.isThreadRunning()) backgroundThread.startThread (juce::Thread::Priority::low);
        {
            // Guards these fields against a concurrent getLiveSnapshot()
            // call from the message thread — start()/stop()/push() all run
            // on the audio thread, so they never race each other, only the
            // UI's read of the same fields.
            const juce::ScopedLock sl (liveLock);
            file = destination;
            currentSampleRate = sampleRate;
            currentChannels = channels;
            currentStartTick = startTick;
            livePeaks.clear();
            livePeaks.reserve (8192); // ~6 min at the default bucket size before it needs to grow
        }
        livePartialMin = 0.0f;
        livePartialMax = 0.0f;
        livePartialCount = 0;
        recordedSamples.store (0, std::memory_order_relaxed);
        recording.store (true, std::memory_order_release);
        return true;
    }

    void push (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        if (! recording.load (std::memory_order_acquire) || writer == nullptr || numSamples <= 0) return;
        const int samples = juce::jmin (numSamples, buffer.getNumSamples());
        const int channels = juce::jmin (currentChannels, buffer.getNumChannels());
        if (samples <= 0 || channels <= 0) return;
        if (! writer->write (buffer.getArrayOfReadPointers(), samples)) return;
        recordedSamples.fetch_add (samples, std::memory_order_relaxed);

        // Live waveform peaks for the in-progress take — see LiveSnapshot /
        // NetworkAudioProcessor::getLiveRecordingSnapshot() / ArrangeView's
        // "recording now" rect. livePartial* below is touched only from
        // this thread (push() calls are already serialized by the audio
        // thread itself), so only the finished-bucket append needs the lock.
        int pos = 0;
        int remaining = samples;
        while (remaining > 0)
        {
            const int room = kLiveBucketSamples - livePartialCount;
            const int take = juce::jmin (room, remaining);
            for (int ch = 0; ch < channels; ++ch)
            {
                const float* d = buffer.getReadPointer (ch) + pos;
                for (int s = 0; s < take; ++s)
                {
                    livePartialMin = juce::jmin (livePartialMin, d[s]);
                    livePartialMax = juce::jmax (livePartialMax, d[s]);
                }
            }
            livePartialCount += take;
            pos += take;
            remaining -= take;

            if (livePartialCount >= kLiveBucketSamples)
            {
                const juce::ScopedLock sl (liveLock);
                livePeaks.push_back ({ livePartialMin, livePartialMax });
                livePartialMin = 0.0f;
                livePartialMax = 0.0f;
                livePartialCount = 0;
            }
        }
    }

    Clip stop()
    {
        recording.store (false, std::memory_order_release);
        writer.reset();
        Clip result;
        const juce::ScopedLock sl (liveLock);
        result.file = file;
        result.sampleRate = currentSampleRate;
        result.channels = currentChannels;
        result.startTick = currentStartTick;
        result.lengthSamples = recordedSamples.load (std::memory_order_relaxed);
        if (! result.isValid()) result = {};
        file = {};
        currentSampleRate = 0.0;
        currentChannels = 0;
        currentStartTick = 0;
        recordedSamples.store (0, std::memory_order_relaxed);
        livePeaks.clear();
        return result;
    }

    bool isRecording() const noexcept { return recording.load (std::memory_order_acquire); }
    int64_t getRecordedSamples() const noexcept { return recordedSamples.load (std::memory_order_relaxed); }

    // Message-thread-safe read of the in-progress recording, for live
    // clip/waveform drawing. Cheap when nothing is recording (returns
    // immediately without touching the lock); otherwise copies the peaks
    // collected so far — fine to call once per UI frame.
    LiveSnapshot getLiveSnapshot() const
    {
        LiveSnapshot snap;
        snap.isRecording = recording.load (std::memory_order_acquire);
        if (! snap.isRecording) return snap;

        const juce::ScopedLock sl (liveLock);
        snap.file            = file;
        snap.sampleRate      = currentSampleRate;
        snap.channels        = currentChannels;
        snap.startTick       = currentStartTick;
        snap.recordedSamples = recordedSamples.load (std::memory_order_relaxed);
        snap.peaks           = livePeaks;
        return snap;
    }

private:
    static constexpr int kLiveBucketSamples = 2048;

    juce::TimeSliceThread backgroundThread { "METRO Network Audio Recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
    std::atomic<bool> recording { false };
    std::atomic<int64_t> recordedSamples { 0 };

    mutable juce::CriticalSection liveLock;
    juce::File file;
    double currentSampleRate = 0.0;
    int currentChannels = 0;
    int64_t currentStartTick = 0;
    std::vector<std::pair<float, float>> livePeaks;   // guarded by liveLock

    // Running min/max for the not-yet-finished bucket. Audio-thread-only —
    // never touched from push()'s caller (getLiveSnapshot()), so no lock
    // needed for these three.
    float livePartialMin = 0.0f;
    float livePartialMax = 0.0f;
    int   livePartialCount = 0;
};
