#pragma once
#include "../ui/DysektLookAndFeel.h"

// Windows Metro / Fluent icon policy.
//
// The spec's VISUAL LANGUAGE section rules out skeuomorphic frames, and
// every button/pad/program-grid cell across the app already falls back to a
// flat, theme-coloured rounded rect whenever IconManager's sprite getters
// return null (that fallback path predates Metro — it exists for when the
// binary sprite assets fail to load). Metro reuses that existing fallback
// instead of shipping new flat icon art: IconManager::getButtonIdle/Hover/
// Active() call MetroIcons::suppressChromeSprites() and return null when
// it's true, which flattens every sprite-chrome button app-wide from this
// one place — see IconManager.cpp.
namespace MetroIcons
{
    inline bool suppressChromeSprites()
    {
        return getTheme().name == "metro";
    }
}
