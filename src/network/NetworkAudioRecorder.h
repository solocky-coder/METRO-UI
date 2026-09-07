#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

//==============================================================================
// NetworkAudioRecorder
//
// Dedicated recorder for the Network Audio track.  The audio callback only
// hands planar sample pointers to JUCE's ThreadedWriter; file creation and all
// disk I/O happen outside the realtime callback.
//
// The resulting WAV file is an immutable audio-clip source for the arranger.
// A later arrange-layer can place this file at startTick without changing the
// network transport or recorder itself.
//==============================================================================
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

        bool isValid() const noexcept
        {
            return file.existsAsFile() && channels > 0 && sampleRate > 0.0 && lengthSamples > 0;
        }
    };

    NetworkAudioRecorder() = default;

    ~NetworkAudioRecorder()
    {
        stop();
    }

    NetworkAudioRecorder (const NetworkAudioRecorder&) = delete;
    NetworkAudioRecorder& operator= (const NetworkAudioRecorder&) = delete;

    bool start (const juce::File& destination, double sampleRate, int channels,
                int64_t startTick = 0)
    {
        stop();

        if (destination == juce::File() || sampleRate <= 0.0 || channels <= 0)
            return false;

        destination.getParentDirectory().createDirectory();

        auto stream = std::unique_ptr<juce::FileOutputStream> (destination.createOutputStream());
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat format;
        auto* rawWriter = format.createWriterFor (stream.release(), sampleRate,
                                                   static_cast<unsigned int> (channels),
                                                   24, {}, 0);
        if (rawWriter == nullptr)
            return false;

        // 2 seconds of safety buffering at the requested sample rate.  The
        // writer owns the AudioFormatWriter and its output stream.
        writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>
            (rawWriter, backgroundThread, juce::jmax (1024, static_cast<int> (sampleRate * 2.0)));

        if (! backgroundThread.isThreadRunning())
            backgroundThread.startThread (juce::Thread::Priority::low);

        file = destination;
        currentSampleRate = sampleRate;
        currentChannels = channels;
        currentStartTick = startTick;
        recordedSamples.store (0, std::memory_order_relaxed);
        recording.store (true, std::memory_order_release);
        return true;
    }

    void push (const juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        if (! recording.load (std::memory_order_acquire) || writer == nullptr || numSamples <= 0)
            return;

        const int samples = juce::jmin (numSamples, buffer.getNumSamples());
        const int channels = juce::jmin (currentChannels, buffer.getNumChannels());
        if (samples <= 0 || channels <= 0)
            return;

        // ThreadedWriter::write() is specifically designed for realtime input:
        // it copies the incoming channel pointers into its FIFO and leaves the
        // actual file writes to the TimeSliceThread.
        const bool accepted = writer->write (buffer.getArrayOfReadPointers(), samples);
        if (accepted)
            recordedSamples.fetch_add (samples, std::memory_order_relaxed);
    }

    Clip stop()
    {
        recording.store (false, std::memory_order_release);

        // Destroying ThreadedWriter flushes its FIFO before releasing the
        // underlying AudioFormatWriter, producing a valid WAV file.
        writer.reset();

        Clip result;
        result.file = file;
        result.sampleRate = currentSampleRate;
        result.channels = currentChannels;
        result.startTick = currentStartTick;
        result.lengthSamples = recordedSamples.load (std::memory_order_relaxed);

        if (! result.isValid())
            result = {};

        file = {};
        currentSampleRate = 0.0;
        currentChannels = 0;
        currentStartTick = 0;
        recordedSamples.store (0, std::memory_order_relaxed);
        return result;
    }

    bool isRecording() const noexcept
    {
        return recording.load (std::memory_order_acquire);
    }

    int64_t getRecordedSamples() const noexcept
    {
        return recordedSamples.load (std::memory_order_relaxed);
    }

private:
    juce::TimeSliceThread backgroundThread { "METRO Network Audio Recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;

    std::atomic<bool> recording { false };
    std::atomic<int64_t> recordedSamples { 0 };

    juce::File file;
    double currentSampleRate = 0.0;
    int currentChannels = 0;
    int64_t currentStartTick = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioRecorder)
};
