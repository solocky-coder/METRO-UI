#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../audio/SliceManager.h"

class DysektProcessor;
class WaveformView;

class SliceLane : public juce::Component
{
public:
    explicit SliceLane (DysektProcessor& p);
    void setWaveformView (WaveformView* view) { waveformView = view; }
    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;

    /// Invalidate the label-position cache so it is recomputed on the next paint.
    /// Call whenever the slice list, selection, or viewport changes.
    void invalidateLabelCache() noexcept { labelCacheDirty = true; }

private:
    DysektProcessor& processor;
    WaveformView* waveformView = nullptr;

    // ── Label position cache (Bug #3: prevents per-frame jitter / flicker) ───
    struct CachedLabel
    {
        int   sliceIdx = 0;
        int   x        = 0;  ///< Pixel x-coordinate of label left edge
    };

    std::array<CachedLabel, SliceManager::kMaxSlices> cachedLabels {};
    int cachedLabelCount = 0;

    // Key used to detect whether the cache needs rebuilding
    struct LabelCacheKey
    {
        int  numSlices     = 0;
        int  selectedSlice = -1;
        int  visStart      = 0;
        int  visLen        = 0;
        int  width         = 0;
        bool operator== (const LabelCacheKey& o) const
        {
            return numSlices == o.numSlices && selectedSlice == o.selectedSlice
                && visStart == o.visStart && visLen == o.visLen && width == o.width;
        }
    };

    LabelCacheKey prevLabelCacheKey {};
    bool          labelCacheDirty   = true;
};
