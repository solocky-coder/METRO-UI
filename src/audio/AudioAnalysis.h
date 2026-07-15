#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <iterator>
#include <array>

namespace AudioAnalysis
{

//==============================================================================
/** Sensitivity presets for detectTransientsHybrid(). */
enum class SensitivityMode
{
    Conservative = 0,   ///< High threshold, 200ms min spacing, only biggest hits
    Normal       = 1,   ///< Balanced detection, ~100ms min spacing
    Aggressive   = 2    ///< Low threshold, 50ms min spacing, catches subtle transients
};

inline int findNearestZeroCrossing (const juce::AudioBuffer<float>& buffer, int pos,
                                     int searchRange = 512)
{
    int numFrames = buffer.getNumSamples();
    if (numFrames == 0 || pos < 0 || pos >= numFrames)
        return pos;

    const float* L = buffer.getReadPointer (0);
    const float* R = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : L;

    auto mono = [&] (int i) -> float { return (L[i] + R[i]) * 0.5f; };

    int bestPos = pos;
    int bestDist = searchRange + 1;

    int lo = std::max (1, pos - searchRange);
    int hi = std::min (numFrames - 1, pos + searchRange);

    for (int i = lo; i <= hi; ++i)
    {
        float a = mono (i - 1);
        float b = mono (i);
        if ((a >= 0.0f && b < 0.0f) || (a < 0.0f && b >= 0.0f))
        {
            int dist = std::abs (i - pos);
            if (dist < bestDist)
            {
                bestDist = dist;
                bestPos = i;
            }
        }
    }

    return bestPos;
}

inline std::vector<int> detectTransients (const juce::AudioBuffer<float>& buffer,
                                           int start, int end,
                                           float sensitivity = 1.0f,
                                           double sampleRate = 44100.0)
{
    std::vector<int> onsets;

    int numFrames = buffer.getNumSamples();
    if (numFrames == 0 || start < 0 || end <= start || end > numFrames)
        return onsets;

    const float* L = buffer.getReadPointer (0);
    const float* R = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : L;

    constexpr int windowSize = 1024;
    constexpr int hopSize = 256;
    int minOnsetDist = (int) std::round (sampleRate * 0.1);  // 100ms
    minOnsetDist = std::max (1, minOnsetDist);
    constexpr int peakRadius = 3;       // local max must beat 3 neighbours each side

    // Step 1: Compute RMS energy per window
    std::vector<float> energy;
    for (int pos = start; pos + windowSize <= end; pos += hopSize)
    {
        float sum = 0.0f;
        for (int i = 0; i < windowSize; ++i)
        {
            int idx = pos + i;
            float m = (L[idx] + R[idx]) * 0.5f;
            sum += m * m;
        }
        energy.push_back (std::sqrt (sum / windowSize));
    }

    if (energy.size() < 5)
        return onsets;

    // Step 2: Compute onset detection function — ratio of energy increase
    // Uses log-ratio: log(energy[i] / energy[i-1]) when energy rises
    // This normalizes by current level, so a hit in a quiet section
    // scores the same as a hit in a loud section.
    std::vector<float> odf;
    odf.push_back (0.0f);
    for (size_t i = 1; i < energy.size(); ++i)
    {
        float prev = energy[i - 1];
        float curr = energy[i];
        if (curr > prev && prev > 1e-8f)
            odf.push_back (std::log (curr / prev));
        else if (curr > 1e-8f && prev <= 1e-8f)
            odf.push_back (10.0f);  // silence-to-sound: strong onset
        else
            odf.push_back (0.0f);
    }

    // Step 3: Compute global threshold from sorted ODF values
    // Sensitivity controls which percentile we pick as the cutoff.
    // sensitivity 1.0 -> low percentile (many detections)
    // sensitivity 0.0 -> high percentile (only biggest hits)
    std::vector<float> sorted;
    std::copy_if (odf.begin(), odf.end(), std::back_inserter (sorted),
                  [] (float v) { return v > 0.0f; });

    if (sorted.empty())
        return onsets;

    std::sort (sorted.begin(), sorted.end());

    float s = juce::jlimit (0.0f, 1.0f, sensitivity);
    // Map sensitivity to percentile: 1.0 -> 50th percentile, 0.0 -> 99.5th
    float percentile = 0.995f - s * 0.495f;
    size_t threshIdx = std::min ((size_t) (percentile * (float) sorted.size()),
                                  sorted.size() - 1);
    float threshold = sorted[threshIdx];

    // Step 4: Peak-pick — only accept positions where ODF is a local maximum
    // AND exceeds the threshold AND respects minimum onset distance
    int lastOnsetSample = start - minOnsetDist;

    for (size_t i = 1; i < odf.size() - 1; ++i)
    {
        if (odf[i] <= threshold)
            continue;

        // Check local maximum within peakRadius
        bool isLocalMax = true;
        for (int k = 1; k <= peakRadius && isLocalMax; ++k)
        {
            if (i >= (size_t) k && odf[i - (size_t) k] >= odf[i])
                isLocalMax = false;
            if (i + (size_t) k < odf.size() && odf[i + (size_t) k] >= odf[i])
                isLocalMax = false;
        }

        if (! isLocalMax)
            continue;

        // Offset by half window to compensate for analysis latency:
        // the energy window starting at pos integrates windowSize samples,
        // so the actual transient sits roughly half a window into it.
        int samplePos = start + (int) i * hopSize + windowSize / 2;
        samplePos = std::min (samplePos, end);
        if (samplePos - lastOnsetSample >= minOnsetDist && samplePos > start)
        {
            onsets.push_back (samplePos);
            lastOnsetSample = samplePos;
        }
    }

    return onsets;
}


//==============================================================================
/** Sub-sample onset refinement via parabolic interpolation.
    Given a peak at index @p i in @p odf, returns a fractional offset in
    [-0.5, +0.5] that refines the peak position to sub-sample accuracy. */
inline float sharpenOnset (const std::vector<float>& odf, size_t i)
{
    if (i == 0 || i + 1 >= odf.size())
        return 0.0f;
    float y0 = odf[i - 1];
    float y1 = odf[i];
    float y2 = odf[i + 1];
    float denom = y2 - 2.0f * y1 + y0;   // second difference
    if (std::abs (denom) < 1e-10f)
        return 0.0f;
    float offset = 0.5f * (y0 - y2) / denom;
    return juce::jlimit (-0.5f, 0.5f, offset);
}

//==============================================================================
/** Compute per-window spectral flux using first-difference energy as a
    high-frequency energy proxy.  Only positive increases are retained
    (half-wave rectification).  Returns one value per hop frame. */
inline std::vector<float> computeSpectralFlux (const float* L, const float* R,
                                                int start, int end,
                                                int windowSize, int hopSize)
{
    std::vector<float> flux;
    float prevHfE = 0.0f;
    bool first = true;

    for (int pos = start; pos + windowSize <= end; pos += hopSize)
    {
        float hfSum = 0.0f;
        for (int i = 1; i < windowSize; ++i)
        {
            int idx = pos + i;
            float m  = (L[idx]     + R[idx])     * 0.5f;
            float mp = (L[idx - 1] + R[idx - 1]) * 0.5f;
            float d  = m - mp;
            hfSum   += d * d;
        }
        float hfE = std::sqrt (hfSum / (float) (windowSize - 1));

        if (first)
        {
            flux.push_back (0.0f);
            first = false;
        }
        else
        {
            flux.push_back (std::max (0.0f, hfE - prevHfE));
        }
        prevHfE = hfE;
    }
    return flux;
}

//==============================================================================
/** Apply a first-order IIR high-pass filter to @p data in-place.
    @param cutoffHz  Cutoff frequency in Hz.
    @param sampleRate  Audio sample rate in Hz. */
inline void computeHighPassFiltered (float* data, int numSamples,
                                     float cutoffHz, double sampleRate)
{
    if (numSamples < 2)
        return;
    // First-order IIR HP filter: rc = 1/(2π·fc), dt = 1/sampleRate,
    // alpha = rc / (rc + dt) = sampleRate / (sampleRate + 2π·fc)
    float rc    = 1.0f / (2.0f * juce::MathConstants<float>::pi * cutoffHz);
    float dt    = 1.0f / (float) sampleRate;
    float alpha = rc / (rc + dt);
    float prevIn = data[0];
    float prevOut = 0.0f;
    data[0] = 0.0f;
    for (int i = 1; i < numSamples; ++i)
    {
        float in = data[i];
        data[i]  = alpha * (prevOut + in - prevIn);
        prevIn   = in;
        prevOut  = data[i];
    }
}

//==============================================================================
/** Hybrid transient detector combining RMS-energy and spectral-flux ODFs
    across three analysis scales (512, 1024, 2048 samples).

    @param buffer      Audio buffer to analyse.
    @param start       First sample of the region to analyse.
    @param end         One-past-last sample of the region.
    @param mode        Sensitivity preset (Conservative / Normal / Aggressive).
    @param sensitivity Fine-tune threshold density in [0, 1] (1 = most detections).
    @param sampleRate  Audio sample rate in Hz.
    @returns           Sample positions of detected transients. */
inline std::vector<int> detectTransientsHybrid (
    const juce::AudioBuffer<float>& buffer,
    int start, int end,
    SensitivityMode mode,
    float sensitivity = 0.5f,
    double sampleRate = 44100.0)
{
    std::vector<int> onsets;

    int numFrames = buffer.getNumSamples();
    if (numFrames == 0 || start < 0 || end <= start || end > numFrames)
        return onsets;

    const float* L = buffer.getReadPointer (0);
    const float* R = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : L;

    // ---- Mode-specific parameters ------------------------------------------------
    float energyWeight, fluxWeight;
    int   minOnsetDistMs;

    switch (mode)
    {
        case SensitivityMode::Conservative:
            minOnsetDistMs = 200;
            energyWeight   = 0.7f;
            fluxWeight     = 0.3f;
            sensitivity    = juce::jlimit (0.0f, 1.0f, sensitivity * 0.6f);
            break;
        case SensitivityMode::Aggressive:
            minOnsetDistMs = 50;
            energyWeight   = 0.4f;
            fluxWeight     = 0.6f;
            sensitivity    = juce::jlimit (0.0f, 1.0f, 0.4f + sensitivity * 0.6f);
            break;
        case SensitivityMode::Normal:
        default:
            minOnsetDistMs = 100;
            energyWeight   = 0.5f;
            fluxWeight     = 0.5f;
            break;
    }

    int minOnsetDist = (int) std::round (sampleRate * (double) minOnsetDistMs / 1000.0);
    minOnsetDist = std::max (1, minOnsetDist);

    constexpr int hopSize    = 256;
    constexpr int peakRadius = 3;

    // Multi-scale window sizes
    const std::array<int, 3> windowSizes = { 512, 1024, 2048 };

    // Preferred scale per mode gets a score boost
    const std::array<float, 3> scaleWeights = {
        (mode == SensitivityMode::Aggressive)  ? 1.5f : 0.8f,  // 512
        (mode == SensitivityMode::Normal)      ? 1.5f : 0.8f,  // 1024
        (mode == SensitivityMode::Conservative)? 1.5f : 0.8f   // 2048
    };

    // Accumulate (samplePos, score) candidates from all scales
    std::vector<std::pair<int, float>> candidates;

    for (int scaleIdx = 0; scaleIdx < 3; ++scaleIdx)
    {
        int ws = windowSizes[(size_t) scaleIdx];

        // ---- Step 1: RMS energy per window ----------------------------------
        std::vector<float> energy;
        for (int pos = start; pos + ws <= end; pos += hopSize)
        {
            float sumE = 0.0f;
            for (int i = 0; i < ws; ++i)
            {
                float m = (L[pos + i] + R[pos + i]) * 0.5f;
                sumE += m * m;
            }
            energy.push_back (std::sqrt (sumE / (float) ws));
        }

        // ---- Step 2: spectral flux ODF --------------------------------------
        std::vector<float> fluxVec = computeSpectralFlux (L, R, start, end, ws, hopSize);

        size_t nFrames = std::min (energy.size(), fluxVec.size());
        if (nFrames < 5)
            continue;

        // ---- Step 3: energy ODF (log-ratio) ---------------------------------
        std::vector<float> energyOdf (nFrames, 0.0f);
        for (size_t i = 1; i < nFrames; ++i)
        {
            float prev = energy[i - 1];
            float curr = energy[i];
            if (curr > prev && prev > 1e-8f)
                energyOdf[i] = std::log (curr / prev);
            else if (curr > 1e-8f && prev <= 1e-8f)
                energyOdf[i] = 10.0f;  // silence-to-sound: strong onset sentinel
        }

        // ---- Step 4: normalise flux ODF to energy ODF scale -----------------
        std::vector<float> fluxOdf (fluxVec.begin(),
                                    fluxVec.begin() + (std::ptrdiff_t) nFrames);
        float maxE = *std::max_element (energyOdf.begin(), energyOdf.end());
        float maxF = *std::max_element (fluxOdf.begin(),   fluxOdf.end());
        if (maxF > 1e-10f && maxE > 0.0f)
        {
            float scale = maxE / maxF;
            for (auto& v : fluxOdf)
                v *= scale;
        }

        // ---- Step 5: combined ODF -------------------------------------------
        std::vector<float> odf (nFrames);
        for (size_t i = 0; i < nFrames; ++i)
            odf[i] = energyWeight * energyOdf[i] + fluxWeight * fluxOdf[i];

        // ---- Step 6: threshold from ODF percentile --------------------------
        std::vector<float> sortedOdf;
        std::copy_if (odf.begin(), odf.end(), std::back_inserter (sortedOdf),
                      [] (float v) { return v > 0.0f; });
        if (sortedOdf.empty())
            continue;
        std::sort (sortedOdf.begin(), sortedOdf.end());

        float s = juce::jlimit (0.0f, 1.0f, sensitivity);
        // Map sensitivity [0,1] to percentile range [99.5%, 50%]:
        // sensitivity 1.0 -> 50th percentile (many detections)
        // sensitivity 0.0 -> 99.5th percentile (only biggest hits)
        float percentile = 0.995f - s * 0.495f;
        size_t threshIdx = std::min ((size_t) (percentile * (float) sortedOdf.size()),
                                     sortedOdf.size() - 1);
        float threshold = sortedOdf[threshIdx];

        // ---- Step 7: peak-pick + parabolic sub-sample refinement ------------
        for (size_t i = 1; i + 1 < odf.size(); ++i)
        {
            if (odf[i] <= threshold)
                continue;

            bool isLocalMax = true;
            for (int k = 1; k <= peakRadius && isLocalMax; ++k)
            {
                if (i >= (size_t) k && odf[i - (size_t) k] >= odf[i])
                    isLocalMax = false;
                if (i + (size_t) k < odf.size() && odf[i + (size_t) k] >= odf[i])
                    isLocalMax = false;
            }
            if (! isLocalMax)
                continue;

            float frac     = sharpenOnset (odf, i);
            int   samplePos = start
                              + (int) std::round (((float) i + frac) * (float) hopSize)
                              + ws / 2;
            samplePos = juce::jlimit (start, end, samplePos);
            candidates.push_back ({ samplePos, odf[i] * scaleWeights[(size_t) scaleIdx] });
        }
    }

    if (candidates.empty())
        return onsets;

    // ---- Merge candidates within 50 ms to remove duplicate detections -------
    std::sort (candidates.begin(), candidates.end(),
               [] (const auto& a, const auto& b) { return a.first < b.first; });

    int mergeWindow = (int) std::round (sampleRate * 50.0 / 1000.0); // 50 ms
    std::vector<std::pair<int, float>> merged;
    for (const auto& cand : candidates)
    {
        if (! merged.empty() && cand.first - merged.back().first < mergeWindow)
        {
            if (cand.second > merged.back().second)
                merged.back() = cand;
        }
        else
        {
            merged.push_back (cand);
        }
    }

    // ---- Apply minimum onset distance and collect results -------------------
    int lastOnset = start - minOnsetDist;
    for (const auto& [pos, score] : merged)
    {
        if (pos - lastOnset >= minOnsetDist && pos > start)
        {
            onsets.push_back (pos);
            lastOnset = pos;
        }
    }

    return onsets;
}

} // namespace AudioAnalysis
