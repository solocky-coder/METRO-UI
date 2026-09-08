#pragma once
#include "NetworkAudioInput.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

class NetworkAudioTrackRouter
{
public:
    using Route = NetworkAudioInput;
    NetworkAudioTrackRouter() = default;

    int64_t addRoute (int64_t sourceKey, int32_t sourceId, int sourceChannel, int targetTrackIndex,
                      const juce::String& sourceName = {}, const juce::String& userName = {})
    {
        std::lock_guard<std::mutex> lock (mutex);
        auto route = std::make_shared<Route>();
        route->routeId = nextRouteId++;
        route->sourceKey = sourceKey; route->sourceId = sourceId; route->sourceChannel = sourceChannel;
        route->targetTrackIndex = targetTrackIndex; route->sourceName = sourceName; route->userName = userName;
        routes.push_back (route); publishSnapshotLocked(); return route->routeId;
    }

    // Compatibility overload for older callers that only know the raw AOO id.
    int64_t addRoute (int32_t sourceId, int sourceChannel, int targetTrackIndex,
                      const juce::String& sourceName = {}, const juce::String& userName = {})
    {
        return addRoute ((int64_t) sourceId, sourceId, sourceChannel, targetTrackIndex, sourceName, userName);
    }

    bool removeRoute (int64_t routeId)
    {
        std::lock_guard<std::mutex> lock (mutex);
        const auto oldSize = routes.size();
        routes.erase (std::remove_if (routes.begin(), routes.end(), [routeId] (const auto& r) { return r->routeId == routeId; }), routes.end());
        if (routes.size() == oldSize) return false;
        publishSnapshotLocked(); return true;
    }

    bool setTargetTrack (int64_t routeId, int trackIndex)
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto& route : routes)
            if (route->routeId == routeId) { route->targetTrackIndex = trackIndex; publishSnapshotLocked(); return true; }
        return false;
    }

    std::shared_ptr<const std::vector<std::shared_ptr<Route>>> getSnapshot() const noexcept
    {
        return snapshot.load (std::memory_order_acquire);
    }

    std::vector<NetworkAudioInputRoute> getRouteInfo() const
    {
        std::vector<NetworkAudioInputRoute> result;
        auto snap = getSnapshot(); result.reserve (snap->size());
        for (const auto& route : *snap)
        {
            NetworkAudioInputRoute info;
            info.routeId = route->routeId; info.sourceKey = route->sourceKey;
            info.sourceId = route->sourceId; info.sourceChannel = route->sourceChannel;
            info.targetTrackIndex = route->targetTrackIndex; info.enabled = route->enabled.load();
            info.recordArm = route->recordArm.load(); info.monitor = route->monitor.load();
            info.mute = route->mute.load(); info.solo = route->solo.load();
            info.gainDb = route->gainDb.load(); info.pan = route->pan.load();
            result.push_back (info);
        }
        return result;
    }

    bool setRecordArm (int64_t routeId, bool armed) noexcept { return setAtomic (routeId, [&] (auto& r) { r.recordArm.store (armed); }); }
    bool setMonitor   (int64_t routeId, bool enabled) noexcept { return setAtomic (routeId, [&] (auto& r) { r.monitor.store (enabled); }); }
    bool setMute      (int64_t routeId, bool muted) noexcept { return setAtomic (routeId, [&] (auto& r) { r.mute.store (muted); }); }
    bool setSolo      (int64_t routeId, bool solo) noexcept { return setAtomic (routeId, [&] (auto& r) { r.solo.store (solo); }); }
    bool setEnabled   (int64_t routeId, bool enabled) noexcept { return setAtomic (routeId, [&] (auto& r) { r.enabled.store (enabled); }); }
    bool setGainDb    (int64_t routeId, float db) noexcept { return setAtomic (routeId, [&] (auto& r) { r.gainDb.store (juce::jlimit (-60.0f, 12.0f, db)); }); }
    bool setPan       (int64_t routeId, float value) noexcept { return setAtomic (routeId, [&] (auto& r) { r.pan.store (juce::jlimit (-1.0f, 1.0f, value)); }); }

private:
    template <typename Fn>
    bool setAtomic (int64_t routeId, Fn&& fn) noexcept
    {
        auto snap = getSnapshot();
        for (const auto& route : *snap)
            if (route->routeId == routeId) { fn (*route); return true; }
        return false;
    }

    mutable std::mutex mutex;
    std::vector<std::shared_ptr<Route>> routes;
    int64_t nextRouteId = 1;
    std::atomic<std::shared_ptr<const std::vector<std::shared_ptr<Route>>>> snapshot { std::make_shared<const std::vector<std::shared_ptr<Route>>>() };

    void publishSnapshotLocked()
    {
        auto next = std::make_shared<std::vector<std::shared_ptr<Route>>> (routes);
        snapshot.store (std::move (next), std::memory_order_release);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioTrackRouter)
};
