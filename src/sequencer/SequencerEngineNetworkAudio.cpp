#include "SequencerEngine.h"
#include "../network/NetworkAudioInput.h"

namespace
{
    static bool isValidTrackIndex (const std::shared_ptr<const std::vector<std::shared_ptr<SequencerTrack>>>& tracks,
                                   int index) noexcept
    {
        return tracks != nullptr && juce::isPositiveAndBelow (index, (int) tracks->size());
    }
}

int SequencerEngine::addNetworkAudioTrack (int64_t routeId, int32_t sourceId, int sourceChannel,
                                            const juce::String& sourceName, const juce::String& userName)
{
    // Structural track edits use the same copy-on-write publication model as
    // the existing MIDI/SF tracks. The route itself is owned by the network
    // audio router; this track stores only the stable route/source identity.
    NetworkAudioInput input;
    input.routeId = routeId;
    input.sourceId = sourceId;
    input.sourceChannel = juce::jmax (0, sourceChannel);
    input.sourceName = sourceName;
    input.userName = userName;

    auto current = impl->getTracks();
    auto next = std::make_shared<std::vector<std::shared_ptr<SequencerTrack>>> (*current);
    next->push_back (SequencerTrack::makeAudio (input));

    const int index = (int) next->size() - 1;
    impl->publishTracks (std::move (next));
    return index;
}

bool SequencerEngine::setNetworkAudioTrackRoute (int trackIndex, int64_t routeId,
                                                  int32_t sourceId, int sourceChannel)
{
    auto current = impl->getTracks();
    if (! isValidTrackIndex (current, trackIndex))
        return false;

    auto& track = *(*current)[(size_t) trackIndex];
    if (track.type != TrackType::Audio)
        return false;

    track.networkRouteId = routeId;
    track.networkSourceId = sourceId;
    track.networkSourceChannel = juce::jmax (0, sourceChannel);
    return true;
}

bool SequencerEngine::isNetworkAudioTrack (int trackIndex) const noexcept
{
    auto current = impl->getTracks();
    return isValidTrackIndex (current, trackIndex)
        && (*current)[(size_t) trackIndex]->type == TrackType::Audio;
}

bool SequencerEngine::getNetworkAudioRoute (int trackIndex, int64_t& routeId,
                                             int32_t& sourceId, int& sourceChannel) const noexcept
{
    auto current = impl->getTracks();
    if (! isValidTrackIndex (current, trackIndex))
        return false;

    const auto& track = *(*current)[(size_t) trackIndex];
    if (track.type != TrackType::Audio)
        return false;

    routeId = track.networkRouteId;
    sourceId = track.networkSourceId;
    sourceChannel = track.networkSourceChannel;
    return true;
}
