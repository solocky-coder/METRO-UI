#pragma once

#include "AudioClip.h"
#include "MidiClip.h"
#include "SequencerEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <unordered_map>
#include <vector>
#include <memory>
#include <iterator>
#include <cmath>

// Mixes recorded AudioClips (see AudioClip.h / SequencerTrack::AudioClipList)
// into an Audio track's output during playback.
//
// Readers are opened lazily and cached by file path, keyed per track, so a
// clip already open stays open across calls — sync() only opens files for
// clips it hasn't seen before and drops readers for clips that were removed
// or renamed. Call sync() from the message thread whenever the arrangement
// may have changed (ArrangeView::timerCallback() already polls once per
// frame for exactly this reason — see its call to
// NetworkAudioProcessor::syncAudioClipPlayback()).
//
// render() runs on the audio thread and mixes whichever clips overlap the
// current block. It takes a lock shared with sync() rather than a lock-free
// snapshot swap — the same trade SequencerEngine::processClipSlot() already
// makes for MidiClip note reads (see its ScopedReadLock), and here the
// contended section is short (a map lookup + a handful of comparisons).
// Note this still performs a disk read via AudioFormatReader::read() on the
// audio thread; for very large clips or slow storage a background-buffered
// reader (juce::BufferingAudioSource or similar) would be the more correct
// long-term fix, but every clip here is a short local network recording, so
// a direct read keeps this simple until that becomes an actual problem.
class AudioClipPlayer
{
public:
    AudioClipPlayer() { formatManager.registerBasicFormats(); }

    // Rebuilds the reader cache for every Audio track from the engine's
    // current AudioClipList. Cheap when nothing changed — existing readers
    // are matched by file path and reused rather than reopened.
    void syncAllTracks (SequencerEngine& engine)
    {
        const int numTracks = engine.getNumTracks();
        const juce::ScopedLock sl (lock);

        // Drop caches for tracks that no longer exist (track removed).
        for (auto it = tracks.begin(); it != tracks.end(); )
            it = (it->first >= numTracks) ? tracks.erase (it) : std::next (it);

        for (int t = 0; t < numTracks; ++t)
        {
            if (engine.getTrackInfo (t).type != TrackType::Audio)
            {
                tracks.erase (t);
                continue;
            }

            const int numClips = engine.getNumAudioClips (t);
            std::vector<OpenClip> next;
            next.reserve ((size_t) numClips);
            auto& existing = tracks[t];

            for (int c = 0; c < numClips; ++c)
            {
                const auto clip = engine.getAudioClip (t, c);
                if (! clip.isValid()) continue;

                std::shared_ptr<juce::AudioFormatReader> reader;
                for (auto& e : existing)
                    if (e.clip.filePath == clip.filePath) { reader = e.reader; break; }

                if (reader == nullptr)
                {
                    const juce::File f (clip.filePath);
                    reader = std::shared_ptr<juce::AudioFormatReader> (formatManager.createReaderFor (f));
                }
                if (reader != nullptr)
                    next.push_back ({ clip, reader });
            }
            existing = std::move (next);
        }
    }

    // Mixes any clip on `trackIndex` overlapping the current block into
    // `mixBuffer` (added, not replaced), applying `leftGain`/`rightGain`
    // (already derived from the track's volume/pan by the caller, matching
    // the live network-audio path this sits alongside). `scratch` is
    // caller-owned working storage reused across tracks/blocks so no
    // allocation happens here in the steady state. Returns true if anything
    // was mixed in.
    bool render (int trackIndex, juce::AudioBuffer<float>& mixBuffer, juce::AudioBuffer<float>& scratch,
                int numSamples, int64_t playheadTick, double bpm, double sampleRate,
                float leftGain, float rightGain)
    {
        if (numSamples <= 0 || bpm <= 0.0 || sampleRate <= 0.0) return false;

        const juce::ScopedLock sl (lock);
        auto it = tracks.find (trackIndex);
        if (it == tracks.end()) return false;

        const double samplesPerTick = (sampleRate * 60.0) / ((double) bpm * (double) MidiClip::kPPQ);
        bool rendered = false;

        for (auto& oc : it->second)
        {
            const auto& c = oc.clip;
            auto* reader = oc.reader.get();
            if (! c.isValid() || reader == nullptr || c.lengthSamples <= 0) continue;

            // Both clip.startTick and playheadTick are project ticks; both
            // sides of the subtraction use the same samplesPerTick, so this
            // holds even though the clip's own sampleRate (set when it was
            // recorded) may in principle differ from the engine's current
            // playback sampleRate — in practice they match, since recording
            // always captures at the engine's networkSampleRate.
            const int64_t clipStartAbsSample  = (int64_t) std::llround ((double) c.startTick * samplesPerTick);
            const int64_t blockStartAbsSample = (int64_t) std::llround ((double) playheadTick * samplesPerTick);
            const int64_t offsetIntoClip = blockStartAbsSample - clipStartAbsSample;

            if (offsetIntoClip + numSamples <= 0 || offsetIntoClip >= c.lengthSamples)
                continue; // this block doesn't overlap the clip at all

            const int64_t readStart  = juce::jmax<int64_t> (0, offsetIntoClip);
            const int     destStart  = (int) juce::jmax<int64_t> (0, -offsetIntoClip);
            const int     available  = (int) juce::jmin<int64_t> (numSamples - destStart, c.lengthSamples - readStart);
            if (available <= 0) continue;

            const int scratchChannels = juce::jmax (1, c.channels);
            scratch.setSize (scratchChannels, numSamples, false, false, true);
            scratch.clear();
            reader->read (&scratch, destStart, available, readStart, true, true);
            rendered = true;

            if (mixBuffer.getNumChannels() > 0)
                mixBuffer.addFrom (0, 0, scratch, 0, 0, numSamples, leftGain);
            if (mixBuffer.getNumChannels() > 1)
                mixBuffer.addFrom (1, 0, scratch, scratchChannels > 1 ? 1 : 0, 0, numSamples, rightGain);
        }
        return rendered;
    }

private:
    struct OpenClip
    {
        AudioClip clip;
        std::shared_ptr<juce::AudioFormatReader> reader;
    };

    juce::CriticalSection lock;
    juce::AudioFormatManager formatManager;
    std::unordered_map<int, std::vector<OpenClip>> tracks;
};
