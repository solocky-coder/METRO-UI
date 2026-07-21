#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <array>
#include <memory>
#include <vector>

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#define INTERSECT_HAS_STD_ATOMIC_SHARED_PTR 1
#else
#define INTERSECT_HAS_STD_ATOMIC_SHARED_PTR 0
#endif

class SampleData
{
public:
    struct PeakMipmap
    {
        int samplesPerPeak = 0;
        std::vector<float> maxPeaks;
        std::vector<float> minPeaks;
    };

    static constexpr int kNumMipmapLevels = 3;

    struct DecodedSample
    {
        juce::AudioBuffer<float> buffer;  // always stereo
        std::array<PeakMipmap, kNumMipmapLevels> peakMipmaps;
        juce::String fileName;
        juce::String filePath;
    };

    using SnapshotPtr = std::shared_ptr<const DecodedSample>;

    SampleData();

    static std::unique_ptr<DecodedSample> decodeFromFile (const juce::File& file,
                                                           double projectSampleRate);

    /** Publish an already-fully-built, immutable sample as the live sample.
     *
     *  IMPORTANT (real-time safety): this must do no more than a pointer /
     *  refcount swap. It is called directly from processBlock() for the
     *  primary load pipeline, so it must never allocate or copy the PCM
     *  buffer here -- the DecodedSample this SnapshotPtr points to must
     *  already be complete (buffer + mipmaps built) before it arrives.
     *  A std::unique_ptr<DecodedSample> converts implicitly, but doing so
     *  allocates a shared_ptr control block at the call site -- callers on
     *  the audio thread should instead pass a SnapshotPtr that was already
     *  constructed on a worker thread (see PluginProcessor::requestSampleLoad). */
    void applyDecodedSample (SnapshotPtr decoded);

    /** Build peak mipmaps for a DecodedSample whose buffer has already been
     *  filled.  Call this before posting a DecodedSample to completedLoadData
     *  whenever the audio was assembled outside of decodeFromFile() (e.g. the
     *  SF2/SFZ render path in SoundFontLoader). */
    static void buildPeakMipmaps (DecodedSample& ds);
    bool loadFromFile (const juce::File& file, double projectSampleRate);
    void clear();

    /** Create a new DecodedSample containing only the audio from [trimIn, trimOut).
        Returns nullptr if the range is invalid or the source has no audio. */
    static std::unique_ptr<DecodedSample> createTrimmed (const DecodedSample& src,
                                                          int trimIn, int trimOut);
    SnapshotPtr getSnapshot() const;

    float getInterpolatedSample (double pos, int channel) const;

    int getNumFrames() const;
    bool isLoaded() const { return loaded; }

    const juce::AudioBuffer<float>& getBuffer() const;

    const juce::String& getFileName() const { return loadedFileName; }
    void setFileName (const juce::String& name) { loadedFileName = name; }

    const juce::String& getFilePath() const { return loadedFilePath; }
    void setFilePath (const juce::String& path) { loadedFilePath = path; }

private:
    // Non-owning cache of the currently-live sample, always kept alive by
    // `snapshot` below. Written only by applyDecodedSample()/clear(), both of
    // which -- for the primary load pipeline -- run on the audio thread but
    // only ever perform a pointer assignment here, never a buffer copy.
    // Real-time reads (getInterpolatedSample/getBuffer/getNumFrames) go
    // through this raw pointer rather than through the atomic `snapshot`, so
    // per-sample voice rendering costs a plain pointer dereference instead of
    // an atomic shared_ptr load.
    const DecodedSample* liveView = nullptr;
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<const DecodedSample>> snapshot;
#else
    std::shared_ptr<const DecodedSample> snapshot;
#endif
    juce::String loadedFileName;
    juce::String loadedFilePath;
    bool loaded = false;
};
