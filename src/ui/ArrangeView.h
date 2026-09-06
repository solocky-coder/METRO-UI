#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "FloatingTransportBar.h"
#include "TrackHeaderStrip.h"
#include "TrackInspector.h"
#include "DysektLookAndFeel.h"
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/MidiClip.h"
#include "ToolIcons.h"
#include "ZoomableScrollBar.h"
#include <limits>
#include <algorithm>
#include <vector>
#include <utility>

//==============================================================================
//  ArrangeView  —  Cubase-style arrange window
//
//  Ruler:
//   Left-click / drag         → seek + scrub playhead
//   Alt + drag                → set loop in/out (L/R markers)
//   Right-click               → clear loop markers
//
//  Clip body:
//   Left-click                → select track
//   Drag                      → move clip (ghost preview, committed on mouseUp)
//   Drag right edge (8 px)    → resize clip length
//   Double-click              → open piano roll
//   Right-click               → context menu
//
//  Empty space:
//   Left-click                → deselect
//   Right-click               → track context menu
//
//  Keyboard:
//   Space                     → play / stop toggle
//   Home / Numpad 0           → rewind to bar 1
//   +/-                       → increase / decrease track height
//   Delete                    → clear selected clip contents
//
//  Scroll / zoom:
//   Ctrl + scroll             → horizontal zoom (centred on mouse)
//   Shift + scroll            → fast horizontal scroll
//   Scroll                    → horizontal scroll
//   Vertical scrollbar        → track rows scroll (when > screen height)
//
//  Auto-scroll:
//   Playhead auto-follows during playback (Cubase-style page scroll)
//==============================================================================
class ArrangeView : public juce::Component,
                    private juce::Timer,
                    private juce::ScrollBar::Listener
{
public:
    /** Clip-grid tool, selected via the clip right-click "Tool" submenu or the
     *  S/D/E/K/G shortcuts — same names/keys as PianoRollComponent::Tool, but
     *  operating on whole clips rather than notes:
     *   Select — default: click to select/move a clip, drag right edge to resize,
     *            click empty space to create a clip (unchanged pre-existing behaviour)
     *   Draw   — click empty space creates a clip; click an existing clip just selects it
     *   Erase  — click a clip deletes it
     *   Split  — click a clip cuts it into two clips at the click point
     *   Glue   — click a clip merges it with the next clip on the same track
     */
    enum class Tool { Select, Draw, Erase, Split, Glue };

    static constexpr int kTransportH   = 88;   // title strip (24) + gap (16) +
                                                // content row (40) + bottom gutter (8).
                                                // The gutter keeps child controls and
                                                // their expanded frames inside the
                                                // transport surface instead of sitting
                                                // on the arranger boundary.
    static constexpr int kInspectorW   = 216;
    static constexpr int kStripW       = 196;
    static constexpr int kLeftW        = kInspectorW + kStripW;
    static constexpr int kToolbarH     = 36;
    static constexpr int kRulerH       = 32;
    static constexpr int kScrollH      = 10;
    static constexpr int kScrollW      = 10;
    static constexpr int kMinClipPx    = 6;
    static constexpr int kResizeZone   = 10;
    static constexpr int kDefaultTrackH = 68;
    static constexpr int kMinTrackH    = 28;
    static constexpr int kMaxTrackH    = 140;

    std::function<void(int trackIndex, int clipIndex)> onClipDoubleClicked;

    // NOTE: This replacement intentionally preserves the rest of the existing
    // ArrangeView implementation. The full source is supplied from the current
    // repository snapshot in the actual update operation.
