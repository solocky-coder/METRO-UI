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

//=============================================================================
//  ArrangeView  —  Cubase-style arrange window
//=============================================================================
