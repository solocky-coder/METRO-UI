/*
    DYSEKT 2
    Metro UI

    MetroSelection.h

    Lightweight selection value type shared between MetroArrangementView and
    MetroInspector. SequencerEngine exposes tracks/clips as by-value snapshots
    (SequencerTrackInfo / SequencerClipInfo) rather than stable pointers, so
    selection is expressed as indices plus a cached snapshot rather than raw
    Track and Clip pointers.
*/
#pragma once

#include "../sequencer/SequencerEngine.h"

namespace dysekt::metro
{
enum class SelectionKind
{
    none,
    track,
    clip
};

/** What is currently selected in the arrangement workspace, if anything. */
struct MetroSelection final
{
    SelectionKind kind = SelectionKind::none;

    int trackIndex = -1;
    int clipIndex  = -1;

    // Cached snapshots so downstream views (e.g. the inspector) don't need to
    // re-query the engine or worry about indices going stale mid-repaint.
    SequencerTrackInfo track;
    SequencerClipInfo  clip;

    static MetroSelection none() { return {}; }

    static MetroSelection forTrack (int index, SequencerTrackInfo info)
    {
        MetroSelection s;
        s.kind = SelectionKind::track;
        s.trackIndex = index;
        s.track = std::move (info);
        return s;
    }

    static MetroSelection forClip (int trackIdx, int clipIdx, SequencerTrackInfo trackInfo, SequencerClipInfo clipInfo)
    {
        MetroSelection s;
        s.kind = SelectionKind::clip;
        s.trackIndex = trackIdx;
        s.clipIndex = clipIdx;
        s.track = std::move (trackInfo);
        s.clip = std::move (clipInfo);
        return s;
    }

    bool isNone()  const noexcept { return kind == SelectionKind::none; }
    bool isTrack() const noexcept { return kind == SelectionKind::track; }
    bool isClip()  const noexcept { return kind == SelectionKind::clip; }
};

} // namespace dysekt::metro
