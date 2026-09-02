/*
    DYSEKT 2
    Metro UI

    MetroColours.h
*/
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace dysekt::metro
{
using Colour = juce::Colour;

namespace Base {
inline const Colour Background{0xFF000000};   // matches MetroTheme.cpp's t.background
inline const Colour Surface{0xFF0A0A0C};      // matches MetroTheme.cpp's t.waveformBg
inline const Colour SurfaceAlt{0xFF121214};   // matches MetroTheme.cpp's t.darkBar
inline const Colour Elevated{0xFF26262A};     // matches MetroTheme.cpp's t.buttonHover
inline const Colour Border{0xFF48484F};       // brighter divider — was 0xFF232326, matches MetroTheme.cpp's t.separator
inline const Colour White{0xFFFFFFFF};
inline const Colour Black{0xFF000000};
inline const Colour Disabled{0xFF666666};
}

namespace Text {
inline const Colour Primary{0xFFFFFFFF};
inline const Colour Secondary{0xFF7E7E7E};
inline const Colour Muted{0xFF5F5F5F};
inline const Colour Disabled{0xFF4A4A4A};
}

namespace Accent {
inline const Colour Blue{0xFF0EA7D6};
inline const Colour Green{0xFF60C560};
inline const Colour Orange{0xFFF9B04D};
inline const Colour Yellow{0xFFD9C12B};
inline const Colour Purple{0xFF7663F5};
inline const Colour Pink{0xFFCC4C7B};
inline const Colour Red{0xFFEC6168};
inline const Colour Cyan{0xFF48D7FF};
}

namespace Transport {
inline const Colour Background=Base::Surface;
inline const Colour Border=Base::Border;
inline const Colour Play=Accent::Green;
inline const Colour Stop=Text::Primary;
inline const Colour Record=Accent::Red;
inline const Colour TimeDisplay=Text::Primary;
inline const Colour Tempo=Accent::Blue;
}

namespace Sidebar {
inline const Colour Background=Base::SurfaceAlt;
inline const Colour Icon=Text::Secondary;
inline const Colour Hover=Base::Elevated;
inline const Colour Selected=Accent::Blue;
inline const Colour SelectedText=Base::White;
}

namespace Workspace {
inline const Colour Background=Base::Background;
inline const Colour GridMajor{0xFF262629};
inline const Colour GridMinor{0xFF18181A};
inline const Colour Playhead=Base::White;
}

namespace Waveform {
inline const Colour Background=Base::Surface;
inline const Colour Wave=Accent::Cyan;
inline const Colour Slice=Base::White;
inline const Colour Selection=Accent::Blue;
}

namespace Inspector {
inline const Colour Background=Base::SurfaceAlt;
inline const Colour Border=Base::Border;
}

namespace Mixer {
inline const Colour Background=Base::SurfaceAlt;
inline const Colour Meter=Accent::Green;
inline const Colour Peak=Accent::Red;
inline const Colour Fader=Accent::Blue;
}

namespace Browser {
inline const Colour Background=Base::SurfaceAlt;
inline const Colour Folder=Accent::Blue;
inline const Colour Sample=Text::Primary;
}

namespace Clips {
inline const Colour Track1=Accent::Blue;
inline const Colour Track2=Accent::Orange;
inline const Colour Track3=Accent::Green;
inline const Colour Track4=Accent::Purple;
inline const Colour Track5=Accent::Pink;
inline const Colour Track6=Accent::Yellow;
inline const Colour Track7=Accent::Cyan;
inline const Colour Track8=Accent::Red;
}

namespace State {
inline const Colour Hover{0x22FFFFFF};
inline const Colour Pressed{0x44FFFFFF};
inline const Colour Selected=Accent::Blue;
inline const Colour Focus=Accent::Blue;
inline const Colour Disabled=Base::Disabled;
}

}