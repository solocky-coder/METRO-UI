#include "SequencerEngine.h"

int SequencerEngine::addNetworkAudioTrack (int64_t routeId, int32_t sourceId, int sourceChannel,
                                           const juce::String& sourceName, const juce::String& userName)
{
    if (routeId == 0 || sourceChannel < 0)
        return -1;

    auto current = impl->getTracks();
    for (size_t i = 0; i < current->size(); ++i)
    {
        const auto& track = *(*current)[i];
        if (track.type == TrackType::Audio
            && track.networkRouteId == routeId
            && track.networkSourceChannel == sourceChannel)
            return (int) i;
    }

    NetworkAudioInput input;
    input.routeId = routeId;
    input.sourceKey = routeId;
    input.sourceId = sourceId;
    input.sourceChannel = sourceChannel;
    input.sourceName = sourceName;
    input.userName = userName;

    auto track = SequencerTrack::makeAudio (input);
    if (userName.isNotEmpty() && sourceName.isNotEmpty())
        track->name = userName + " | " + sourceName + " Ch " + juce::String (sourceChannel + 1);
    else if (sourceName.isNotEmpty())
        track->name = sourceName + " Ch " + juce::String (sourceChannel + 1);

    const int newTrackIndex = (int) current->size();
    auto next = std::make_shared<Impl::TrackList> (*current);
    next->push_back (std::move (track));
    impl->publishTracks (std::move (next));
    return newTrackIndex;
}

bool SequencerEngine::setNetworkAudioTrackRoute (int trackIndex, int64_t routeId,
                                                  int32_t sourceId, int sourceChannel)
{
    auto snap = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) snap->size())
        || (*snap)[(size_t) trackIndex]->type != TrackType::Audio
        || routeId == 0 || sourceChannel < 0)
        return false;

    auto& track = *(*snap)[(size_t) trackIndex];
    track.networkRouteId = routeId;
    track.networkSourceId = sourceId;
    track.networkSourceChannel = sourceChannel;
    return true;
}

bool SequencerEngine::isNetworkAudioTrack (int trackIndex) const noexcept
{
    auto snap = impl->getTracks();
    return juce::isPositiveAndBelow (trackIndex, (int) snap->size())
        && (*snap)[(size_t) trackIndex]->type == TrackType::Audio;
}

bool SequencerEngine::getNetworkAudioRoute (int trackIndex, int64_t& routeId,
                                             int32_t& sourceId, int& sourceChannel) const noexcept
{
    auto snap = impl->getTracks();
    if (! juce::isPositiveAndBelow (trackIndex, (int) snap->size())
        || (*snap)[(size_t) trackIndex]->type != TrackType::Audio)
        return false;

    const auto& track = *(*snap)[(size_t) trackIndex];
    routeId = track.networkRouteId;
    sourceId = track.networkSourceId;
    sourceChannel = track.networkSourceChannel;
    return routeId != 0;
}
