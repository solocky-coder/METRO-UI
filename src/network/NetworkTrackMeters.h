#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

// Per-track peak levels for Network Audio tracks, handed from the audio thread
// to the UI thread without locks or allocation.
//
//  Audio thread (NetworkAudioProcessor::processBlock):
//      NetworkTrackMeters::push (trackIndex, peakL, peakR, isStereo);
//    Called once per block for every Audio track that actually rendered a
//    source. The value written is the post-fader/pan sample peak for the block
//    (linear gain, 1.0 == 0 dBFS). Peaks accumulate (max) until the UI reads
//    them, so a short transient between two UI polls is never missed.
//
//  UI thread (TrackHeaderStrip timer, ~30 Hz):
//      auto r = NetworkTrackMeters::consume (trackIndex);
//    Returns the peak since the previous consume() and resets it to zero.
//    `blocks` is a running count of blocks rendered for that track; the UI
//    compares it with the previous value to tell "source is delivering audio"
//    (even silence) from "source offline / track muted".
//
// Single writer (audio thread) and, per reader, a single reader (message thread).
//
// Two independent read cursors exist so two UI components can meter the same
// track without stealing each other's peaks (consume() resets what it reads):
//      reader 0 (default) -> TrackHeaderStrip (Arranger)
//      reader 1           -> MixerPanel
// push() feeds both from the one audio-thread call; there is no second metering path.
class NetworkTrackMeters
{
public:
    // Matches SequencerEngine::kActivityFlagCount / TrackHeaderStrip::kMaxTracks.
    static constexpr int kMaxTracks = 64;
    static constexpr int kNumReaders = 2;
    static constexpr int kReaderArranger = 0;
    static constexpr int kReaderMixer    = 1;

    struct Reading
    {
        float    left   = 0.0f;   // linear peak since last consume()
        float    right  = 0.0f;
        bool     stereo = false;  // true for a stereo (Ch 1+2) route, false for mono
        uint32_t blocks = 0;      // monotonically increasing while the track renders
    };

    // Audio thread. Real-time safe: lock-free, no allocation.
    static void push (int trackIndex, float peakL, float peakR, bool stereo) noexcept
    {
        if (trackIndex < 0 || trackIndex >= kMaxTracks)
            return;

        auto& s = slots()[trackIndex];
        for (int r = 0; r < kNumReaders; ++r)
        {
            atomicMax (s.left[r],  peakL);
            atomicMax (s.right[r], peakR);
        }
        s.stereo.store (stereo, std::memory_order_relaxed);
        s.blocks.fetch_add (1u, std::memory_order_relaxed);
    }

    // Message thread.
    static Reading consume (int trackIndex, int reader = kReaderArranger) noexcept
    {
        Reading r;
        if (trackIndex < 0 || trackIndex >= kMaxTracks || reader < 0 || reader >= kNumReaders)
            return r;

        auto& s = slots()[trackIndex];
        r.left   = s.left [reader].exchange (0.0f, std::memory_order_relaxed);
        r.right  = s.right[reader].exchange (0.0f, std::memory_order_relaxed);
        r.stereo = s.stereo.load (std::memory_order_relaxed);
        r.blocks = s.blocks.load (std::memory_order_relaxed);
        return r;
    }

private:
    struct alignas (64) Slot   // one cache line each: no false sharing between tracks
    {
        std::atomic<float>    left[kNumReaders]  {};
        std::atomic<float>    right[kNumReaders] {};
        std::atomic<bool>     stereo { false };
        std::atomic<uint32_t> blocks { 0u };
    };

    static Slot* slots() noexcept
    {
        static Slot table[kMaxTracks];
        return table;
    }

    static void atomicMax (std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }
};
