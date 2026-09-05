#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>   // juce::AsyncUpdater, used by requestBpm()
#include <atomic>

// ─────────────────────────────────────────────────────────────────────────────
//  AbletonLink
//
//  Wraps the Ableton Link SDK (link/include/ableton/Link.hpp).
//  Conditionally compiled — without DYSEKT_HAS_LINK the class is a no-op
//  stub so the rest of the codebase needs zero #ifdefs.
//
//  Setup (one-time):
//    git submodule add https://github.com/Ableton/link.git link
//    git submodule update --init
//  CMakeLists.txt detects the submodule and sets DYSEKT_HAS_LINK=1.
//
//  Audio-thread use in SequencerEngine::processBlock():
//    float bpm = abletonLink.getBpm (internalBpm);   // Link BPM or fallback
// ─────────────────────────────────────────────────────────────────────────────

#if DYSEKT_HAS_LINK
  #include <ableton/Link.hpp>
#endif

class AbletonLink : private juce::AsyncUpdater
{
public:
    AbletonLink()
    {
#if DYSEKT_HAS_LINK
        link = std::make_unique<ableton::Link> (120.0);
        link->enable (false);
        link->setTempoCallback ([this] (double bpm)
        {
            cachedBpm.store ((float) bpm, std::memory_order_relaxed);
        });
        link->setNumPeersCallback ([this] (std::size_t n)
        {
            numPeers.store ((int) n, std::memory_order_relaxed);
        });
#endif
    }

    ~AbletonLink()
    {
#if DYSEKT_HAS_LINK
        if (link) link->enable (false);
#endif
    }

    // ── Enable / disable  (message thread) ───────────────────────────────────
    void setEnabled (bool v)
    {
#if DYSEKT_HAS_LINK
        if (link) link->enable (v);
#endif
        enabled.store (v, std::memory_order_relaxed);
    }

    bool isEnabled()   const noexcept { return enabled .load (std::memory_order_relaxed); }
    int  getPeerCount()const noexcept { return numPeers.load (std::memory_order_relaxed); }

    // ── Propagate local BPM change to Link session ────────────────────────────
    // Message-thread only: captureAppSessionState()/commitAppSessionState() take
    // an internal Link lock and must never be called from the audio thread. Call
    // this directly when you're already on the message thread (e.g. from a UI
    // BPM edit); call requestBpm() instead if the request may originate from the
    // audio thread (e.g. following the host's transport tempo in processBlock()).
    void setBpm (double bpm)
    {
#if DYSEKT_HAS_LINK
        if (link && isEnabled())
        {
            auto s = link->captureAppSessionState();
            s.setTempo (bpm, ableton::Link::Clock::now());
            link->commitAppSessionState (s);
        }
#endif
        cachedBpm.store ((float) bpm, std::memory_order_relaxed);
    }

    // ── Realtime-safe request to propagate a BPM change ───────────────────────
    // Safe to call from ANY thread, including the audio thread: this only does
    // an atomic store plus AsyncUpdater::triggerAsyncUpdate() (a lock-free flag
    // set + message post), never touches Link's AppSessionState directly. The
    // actual captureAppSessionState()/commitAppSessionState() call is deferred to
    // handleAsyncUpdate(), which JUCE guarantees runs on the message thread.
    void requestBpm (double bpm) noexcept
    {
        pendingBpm.store ((float) bpm, std::memory_order_relaxed);
        cachedBpm.store  ((float) bpm, std::memory_order_relaxed);
        triggerAsyncUpdate();
    }

    // ── Audio-thread accessors (no allocation, no locks) ─────────────────────

    /** Returns Link BPM when enabled, or fallback otherwise. */
    float getBpm (float fallback = 120.f) const noexcept
    {
        return isEnabled() ? cachedBpm.load (std::memory_order_relaxed) : fallback;
    }

    /** Phase in beats within the current quantum (0 .. quantum).
     *  quantum=4 = bar-level alignment. */
    double getPhase (double quantum = 4.0) const noexcept
    {
#if DYSEKT_HAS_LINK
        if (link && isEnabled())
        {
            const auto s = link->captureAudioSessionState();
            return s.phaseAtTime (ableton::Link::Clock::now(), quantum);
        }
#endif
        juce::ignoreUnused (quantum);
        return 0.0;
    }

    /** Request play aligned to the next quantum boundary — call on Play press. */
    void requestBeatAlignedStart (double quantum = 4.0)
    {
#if DYSEKT_HAS_LINK
        if (link && isEnabled())
        {
            auto s = link->captureAppSessionState();
            s.requestBeatAtStartPlayingTime (0.0, quantum);
            s.setIsPlaying (true, ableton::Link::Clock::now());
            link->commitAppSessionState (s);
        }
#endif
        juce::ignoreUnused (quantum);
    }

    void notifyStop()
    {
#if DYSEKT_HAS_LINK
        if (link && isEnabled())
        {
            auto s = link->captureAppSessionState();
            s.setIsPlaying (false, ableton::Link::Clock::now());
            link->commitAppSessionState (s);
        }
#endif
    }

private:
    // Runs on the message thread in response to requestBpm()'s
    // triggerAsyncUpdate(); safe to touch AppSessionState here.
    void handleAsyncUpdate() override
    {
#if DYSEKT_HAS_LINK
        if (link && isEnabled())
        {
            auto s = link->captureAppSessionState();
            s.setTempo ((double) pendingBpm.load (std::memory_order_relaxed),
                        ableton::Link::Clock::now());
            link->commitAppSessionState (s);
        }
#endif
    }

#if DYSEKT_HAS_LINK
    std::unique_ptr<ableton::Link> link;
#endif
    std::atomic<bool>  enabled  { false };
    std::atomic<float> cachedBpm{ 120.f };
    std::atomic<int>   numPeers { 0     };
    std::atomic<float> pendingBpm{ 120.f }; // last value passed to requestBpm()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AbletonLink)
};
