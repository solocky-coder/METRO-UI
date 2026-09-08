#pragma once

#include "NetworkAudioInput.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

//==============================================================================
// NetworkAudioTrackRouter
//
// Message-thread owned routing model with an immutable audio-thread snapshot.
// It is deliberately separate from AOO: the network backend supplies samples,
// while this class decides which source/channel feeds which DAW track.
//
// This is the bridge used by the next SequencerEngine audio-track integration;
// keeping the route table independent of AOO prevents transport details from
// leaking into arranger/mixer code.
//==============================================================================
class NetworkAudioTrackRouter
{
public:
    using Route = NetworkAudioInput;

    NetworkAudioTrackRouter() = default;

    int64_t addRoute (int32_t sourceId, int sourceChannel, int targetTrackIndex,
                      const juce::String& sourceName = {},
                      const juce::String& userName = {})
    {
        std::lock_guard<std::mutex> lock (mutex);
        Route route;
        route.routeId = nextRouteId++;
        route.sourceId = sourceId;
        route.sourceChannel = sourceChannel;
        route.targetTrackIndex = targetTrackIndex;
        route.sourceName = sourceName;
        route.userName = userName;
        routes.push_back (std::make_shared<Route> (std::move (route)));
        publishSnapshotLocked();
        return routes.back()->routeId;
    }

    bool removeRoute (int64_t routeId)
    {
        std::lock_guard<std::mutex> lock (mutex);
        const auto oldSize = routes.size();
        routes.erase (std::remove_if (routes.begin(), routes.end(),
                                      [routeId] (const auto& r) { return r->routeId == routeId; }),
                      routes.end());
        if (routes.size() == oldSize) return false;
        publishSnapshotLocked();
        return true;
    }

    bool setTargetTrack (int64_t routeId, int trackIndex)
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto& route : routes)
            if (route->routeId == routeId)
            {
                route->targetTrackIndex = trackIndex;
                publishSnapshotLocked();
                return true;
            }
        return false;
    }

    std::shared_ptr<const std::vector<std::shared_ptr<Route>>> getSnapshot() const noexcept
    {
        return snapshot.load (std::memory_order_acquire);
    }

    std::vector<NetworkAudioInputRoute> getRouteInfo() const
    {
        std::vector<NetworkAudioInputRoute> result;
        auto snap = getSnapshot();
        result.reserve (snap->size());
        for (const auto& route : *snap)
        {
            NetworkAudioInputRoute info;
            info.routeId = route->routeId;
            info.sourceId = route->sourceId;
            info.sourceChannel = route->sourceChannel;
            info.targetTrackIndex = route->targetTrackIndex;
            info.enabled = route->enabled.load (std::memory_order_relaxed);
            info.recordArm = route->recordArm.load (std::memory_order_relaxed);
            info.monitor = route->monitor.load (std::memory_order_relaxed);
            info.mute = route->mute.load (std::memory_order_relaxed);
            info.solo = route->solo.load (std::memory_order_relaxed);
            info.gainDb = route->gainDb.load (std::memory_order_relaxed);
            info.pan = route->pan.load (std::memory_order_relaxed);
            result.push_back (info);
        }
        return result;
    }

    bool setRecordArm (int64_t routeId, bool armed) noexcept
    {
        auto snap = getSnapshot();
        for (const auto& route : *snap)
            if (route->routeId == routeId)
            {
                route->recordArm.store (armed, std::memory_order_relaxed);
                return true;
            }
        return false;
    }

    bool setMonitor (int64_t routeId, bool enabled) noexcept
    {
        auto snap = getSnapshot();
        for (const auto& route : *snap)
            if (route->routeId == routeId)
            {
                route->monitor.store (enabled, std::memory_order_relaxed);
                return true;
            }
        return false;
    }

    bool setGainDb (int64_t routeId, float db) noexcept
    {
        auto snap = getSnapshot();
        for (const auto& route : *snap)
            if (route->routeId == routeId)
            {
                route->gainDb.store (juce::jlimit (-60.0f, 12.0f, db), std::memory_order_relaxed);
                return true;
            }
        return false;
    }

    bool setPan (int64_t routeId, float value) noexcept
    {
        auto snap = getSnapshot();
        for (const auto& route : *snap)
            if (route->routeId == routeId)
            {
                route->pan.store (juce::jlimit (-1.0f, 1.0f, value), std::memory_order_relaxed);
                return true;
            }
        return false;
    }

private:
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Route>> routes;
    int64_t nextRouteId = 1;
    std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<Route>>>> snapshot
    {
        std::make_shared<const std::vector<std::shared_ptr<Route>>>()
    };

    void publishSnapshotLocked()
    {
        auto next = std::make_shared<std::vector<std::shared_ptr<Route>>> (routes);
        snapshot.store (std::move (next), std::memory_order_release);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioTrackRouter)
};
