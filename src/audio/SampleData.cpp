#include "SampleData.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <memory>

namespace
{
// Checked-division size guard: tests whether `frames * channels * bytesPerSample`
// would exceed `maxBytes` without ever forming that product. A naive
// multiply-then-compare can itself overflow before the comparison runs; doing
// the check as a division against the budget avoids that entirely.
bool exceedsByteBudget (size_t frames, size_t channels, size_t bytesPerSample, size_t maxBytes)
{
    const size_t perFrame = channels * bytesPerSample;
    if (perFrame == 0)
        return true; // degenerate — treat as unsafe rather than divide by zero
    return frames > maxBytes / perFrame;
}

void buildMipmapsForBuffer (const juce::AudioBuffer<float>& src,
                             std::array<SampleData::PeakMipmap, SampleData::kNumMipmapLevels>& outMipmaps)
{
    int numFrames = src.getNumSamples();
    if (numFrames <= 0 || src.getNumChannels() < 1)
    {
        for (auto& m : outMipmaps)
        {
            m.samplesPerPeak = 0;
            m.maxPeaks.clear();
            m.minPeaks.clear();
        }
        return;
    }

    const float* dataL = src.getReadPointer (0);
    const float* dataR = src.getNumChannels() > 1 ? src.getReadPointer (1) : dataL;
    if (dataL == nullptr)
        return;

    static constexpr int kBlockSizes[SampleData::kNumMipmapLevels] = { 64, 512, 4096 };

    for (int level = 0; level < SampleData::kNumMipmapLevels; ++level)
    {
        auto& m = outMipmaps[(size_t) level];
        m.samplesPerPeak = kBlockSizes[level];
        int numPeaks = (numFrames + m.samplesPerPeak - 1) / m.samplesPerPeak;
        m.maxPeaks.resize ((size_t) numPeaks);
        m.minPeaks.resize ((size_t) numPeaks);

        for (int i = 0; i < numPeaks; ++i)
        {
            int start = i * m.samplesPerPeak;
            int end   = std::min (start + m.samplesPerPeak, numFrames);
            float hi  = -1.0f;
            float lo  =  1.0f;
            for (int s = start; s < end; ++s)
            {
                float val = (dataL[s] + dataR[s]) * 0.5f;
                if (val > hi) hi = val;
                if (val < lo) lo = val;
            }
            m.maxPeaks[(size_t) i] = hi;
            m.minPeaks[(size_t) i] = lo;
        }
    }
}
} // namespace

SampleData::SampleData() = default;

std::unique_ptr<SampleData::DecodedSample> SampleData::decodeFromFile (const juce::File& file,
                                                                         double projectSampleRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
        return nullptr;

    // ── Guard: defend against bad metadata from JUCE's MP3/VBR reader ─────────
    // dr_mp3 can return sampleRate=0 or lengthInSamples=0 for certain MP3 files.
    // Any of these conditions would crash in AudioBuffer allocation or division.
    if (reader->lengthInSamples <= 0)
        return nullptr;
    if (reader->sampleRate <= 0.0 || ! std::isfinite (reader->sampleRate))
        return nullptr;
    if (reader->numChannels <= 0 || reader->numChannels > 64)
        return nullptr;

    // Checked-narrowing: lengthInSamples is a 64-bit value from the reader.
    // Casting straight to int can silently wrap (a corrupt/adversarial header
    // claiming e.g. 2^31+N samples wraps to a small or negative int), which
    // would defeat the <= 0 check above since it runs on the wrapped value.
    // Reject anything that can't be represented exactly as an int first.
    if (reader->lengthInSamples > (juce::int64) std::numeric_limits<int>::max())
        return nullptr;

    auto numFrames        = (int) reader->lengthInSamples;
    auto numChannels      = (int) reader->numChannels;
    auto sourceSampleRate = reader->sampleRate;

    // ── Guard: duration cap, independent of whether the reported sample rate
    // is honest ────────────────────────────────────────────────────────────
    // The previous cap (30 min @ 48 kHz) only looked at numFrames, implicitly
    // assuming the worst case rate. A header can instead report a low sample
    // rate alongside a large frame count and imply a far longer duration while
    // still passing a frame-count-only check. Compute the duration these two
    // untrusted fields imply together and reject it directly.
    constexpr double kMaxDecodedSeconds = 5.0 * 60.0; // five minutes
    const double impliedDurationSeconds = (double) numFrames / sourceSampleRate;
    if (! std::isfinite (impliedDurationSeconds) || impliedDurationSeconds > kMaxDecodedSeconds)
        return nullptr;

    // ── Decode buffer padding ─────────────────────────────────────────────────
    // Two sources of over-read past the logical numFrames allocation:
    //
    // 1. MP3 / VBR pad (1152 * 4 = 4608 samples):
    //    JUCE's dr_mp3 wrapper trusts the Xing/VBRI header for lengthInSamples,
    //    but some VBR encoders write inaccurate frame counts and deliver more
    //    decoded samples than reported. Four MPEG frames covers the worst-case
    //    encoder drift observed in the wild.
    //
    // 2. LagrangeInterpolator look-ahead (4 samples):
    //    JUCE's LagrangeInterpolator uses a 4-point kernel. On the final output
    //    sample it reads src[i+1..i+3] — 3 samples past the end of its input
    //    pointer. setSize(..., avoidReallocating=true) below trims the *logical*
    //    size back to numFrames, but getReadPointer() still points into the
    //    physical allocation, so we must ensure that allocation extends at least
    //    4 samples beyond numFrames for any file that may be resampled.
    //    Applying it unconditionally is safe and simpler than predicting whether
    //    a rate-conversion will be needed.
    const bool isMp3           = file.hasFileExtension ("mp3|MP3");
    static constexpr int kMp3VbrPad       = 4 * 1152;  // 4 MPEG frames
    static constexpr int kInterpolatorPad = 4;          // Lagrange look-ahead
    const int  allocFrames = numFrames + (isMp3 ? kMp3VbrPad : 0) + kInterpolatorPad;

    // ── Guard: byte-based ceiling on the padded source allocation ─────────────
    // Computed here, before sourceBuffer is allocated below, using allocFrames
    // (the pad-inclusive count) rather than the nominal numFrames — the
    // ceiling must bound what we actually allocate. A duration cap alone isn't
    // sufficient: it doesn't account for channel count or the pad, and a
    // byte-based limit is what actually protects host memory.
    constexpr size_t kMaxSourceBytes = 256u * 1024u * 1024u; // 256 MiB
    if (allocFrames <= 0
        || exceedsByteBudget ((size_t) allocFrames, (size_t) numChannels, sizeof (float), kMaxSourceBytes))
        return nullptr;

    juce::AudioBuffer<float> sourceBuffer (numChannels, allocFrames);
    sourceBuffer.clear();
    reader->read (&sourceBuffer, 0, numFrames, 0, true, true);

    // ── Scrub non-finite samples ──────────────────────────────────────────────
    // A corrupt MP3 frame, a truncated file, or a partial dr_mp3 decode can
    // deposit NaN or ±Inf into the buffer. A single non-finite sample that
    // reaches the DAW's output buffer is enough to crash most hosts. Zero them
    // here before anything else (resampler, mipmap builder, voice pool) can
    // touch the data. Only the decoded region [0, numFrames) needs scanning;
    // the pad beyond is already zero-filled by sourceBuffer.clear() above.
    //
    // Patching a handful of stray non-finite samples is a reasonable recovery
    // for an otherwise-good file. But if a large fraction of the buffer is
    // corrupt, that's not a file worth quietly accepting with holes zeroed
    // into it — treat it as a failed decode instead.
    size_t repairedCount = 0;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* p = sourceBuffer.getWritePointer (ch);
        for (int i = 0; i < numFrames; ++i)
        {
            if (! std::isfinite (p[i]))
            {
                p[i] = 0.0f;
                ++repairedCount;
            }
        }
    }

    constexpr double kMaxRepairedFraction = 0.01; // 1% of decoded samples
    const size_t totalSamples = (size_t) numFrames * (size_t) numChannels;
    if (totalSamples > 0 && (double) repairedCount > kMaxRepairedFraction * (double) totalSamples)
        return nullptr;

    // Trim reported size back to numFrames — avoidReallocating keeps the larger
    // allocation so the pad stays zero-filled; downstream code sees numFrames only.
    sourceBuffer.setSize (numChannels, numFrames, /*keepExisting=*/true,
                          /*clearExtra=*/false, /*avoidReallocating=*/true);

    // ── Resample if needed ────────────────────────────────────────────────────
    if (std::abs (sourceSampleRate - projectSampleRate) > 0.01)
    {
        double ratio = sourceSampleRate / projectSampleRate;

        // Guard: a ratio outside [0.1, 10.0] means the reader returned garbage
        // metadata — reject rather than allocating a gigantic / zero-size buffer.
        if (ratio < 0.1 || ratio > 10.0)
            return nullptr;

        int resampledLen = (int) std::ceil ((double) numFrames / ratio);
        if (resampledLen <= 0
            || exceedsByteBudget ((size_t) resampledLen, (size_t) numChannels, sizeof (float), kMaxSourceBytes))
            return nullptr;

        juce::AudioBuffer<float> resampledBuffer (numChannels, resampledLen);
        resampledBuffer.clear();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process (ratio,
                                  sourceBuffer.getReadPointer (ch),
                                  resampledBuffer.getWritePointer (ch),
                                  resampledLen);
        }

        // The interpolator operates on already-scrubbed input, but extreme
        // ratios or edge samples can still coax non-finite values out of the
        // Lagrange kernel. Sanitize the resampled output the same way the
        // decoded source was sanitized above, before anything downstream
        // touches it.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float* p = resampledBuffer.getWritePointer (ch);
            for (int i = 0; i < resampledLen; ++i)
                if (! std::isfinite (p[i])) p[i] = 0.0f;
        }

        sourceBuffer = std::move (resampledBuffer);
        numFrames    = resampledLen;
    }

    // ── Up-mix to stereo ──────────────────────────────────────────────────────
    juce::AudioBuffer<float> newBuffer (2, numFrames);

    if (numChannels >= 2)
    {
        newBuffer.copyFrom (0, 0, sourceBuffer, 0, 0, numFrames);
        newBuffer.copyFrom (1, 0, sourceBuffer, 1, 0, numFrames);
    }
    else
    {
        newBuffer.copyFrom (0, 0, sourceBuffer, 0, 0, numFrames);
        newBuffer.copyFrom (1, 0, sourceBuffer, 0, 0, numFrames);
    }

    auto decoded          = std::make_unique<DecodedSample>();
    decoded->buffer       = std::move (newBuffer);
    decoded->fileName     = file.getFileName();
    decoded->filePath     = file.getFullPathName();
    buildMipmapsForBuffer (decoded->buffer, decoded->peakMipmaps);
    return decoded;
}

void SampleData::applyDecodedSample (SnapshotPtr decoded)
{
    if (decoded == nullptr)
        return;

    // Pointer-only assignment: liveView is a raw, non-owning cache used by
    // the real-time accessors below. No buffer/mipmap data is copied here --
    // `decoded` must already be the complete, immutable payload built by the
    // loader (SampleData::decodeFromFile + PluginProcessor's worker-thread
    // packaging). This is what makes it safe to call from processBlock().
    liveView       = decoded.get();
    loadedFileName = decoded->fileName;   // juce::String copy is ref-counted, not a heap copy
    loadedFilePath = decoded->filePath;

#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
    snapshot.store (std::move (decoded), std::memory_order_release);
#else
    std::atomic_store_explicit (&snapshot, std::move (decoded), std::memory_order_release);
#endif
    loaded = true;
}

// static
void SampleData::buildPeakMipmaps (DecodedSample& ds)
{
    buildMipmapsForBuffer (ds.buffer, ds.peakMipmaps);
}

bool SampleData::loadFromFile (const juce::File& file, double projectSampleRate)
{
    auto decoded = decodeFromFile (file, projectSampleRate);
    if (decoded == nullptr)
        return false;
    applyDecodedSample (std::move (decoded));
    return true;
}

void SampleData::clear()
{
    liveView = nullptr;
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
    snapshot.store (std::shared_ptr<const DecodedSample>{}, std::memory_order_release);
#else
    std::atomic_store_explicit (&snapshot, std::shared_ptr<const DecodedSample>{},
                                std::memory_order_release);
#endif
    loadedFileName.clear();
    loadedFilePath.clear();
    loaded = false;
}

SampleData::SnapshotPtr SampleData::getSnapshot() const
{
#if INTERSECT_HAS_STD_ATOMIC_SHARED_PTR
    return snapshot.load (std::memory_order_acquire);
#else
    return std::atomic_load_explicit (&snapshot, std::memory_order_acquire);
#endif
}

const juce::AudioBuffer<float>& SampleData::getBuffer() const
{
    static const juce::AudioBuffer<float> kEmptyBuffer;
    return liveView != nullptr ? liveView->buffer : kEmptyBuffer;
}

int SampleData::getNumFrames() const
{
    return liveView != nullptr ? liveView->buffer.getNumSamples() : 0;
}

float SampleData::getInterpolatedSample (double pos, int channel) const
{
    if (! loaded || liveView == nullptr || channel < 0 || channel > 1)
        return 0.0f;

    const auto& buf = liveView->buffer;

    int   ipos = (int) pos;
    float frac = (float) (pos - ipos);

    if (ipos < 0 || ipos >= buf.getNumSamples() - 1)
        return 0.0f;

    auto* data = buf.getReadPointer (channel);
    if (data == nullptr)
        return 0.0f;

    return data[ipos] + (data[ipos + 1] - data[ipos]) * frac;
}

std::unique_ptr<SampleData::DecodedSample> SampleData::createTrimmed (
    const DecodedSample& src, int trimIn, int trimOut)
{
    const int numFrames = src.buffer.getNumSamples();
    trimIn  = juce::jlimit (0, numFrames, trimIn);
    trimOut = juce::jlimit (trimIn + 1, numFrames, trimOut);
    const int trimLen = trimOut - trimIn;
    if (trimLen <= 0)
        return nullptr;

    auto result        = std::make_unique<DecodedSample>();
    result->fileName   = src.fileName;
    result->filePath   = src.filePath;

    const int numCh = src.buffer.getNumChannels();
    result->buffer.setSize (numCh, trimLen);
    for (int ch = 0; ch < numCh; ++ch)
        result->buffer.copyFrom (ch, 0, src.buffer, ch, trimIn, trimLen);

    buildMipmapsForBuffer (result->buffer, result->peakMipmaps);
    return result;
}
