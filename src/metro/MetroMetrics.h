/*
    DYSEKT 2
    Metro UI

    MetroMetrics.h

    Shared, grid-based layout measurements for the Metro standalone UI.
*/
#pragma once

namespace dysekt::metro
{
/**
    The single source of truth for Metro UI geometry.

    All values are expressed in logical JUCE pixels. Components should derive
    their dimensions from this 8 px grid rather than introducing local magic
    numbers, so the standalone retains consistent rhythm as it grows.
*/
struct MetroMetrics final
{
    // Base rhythm
    static constexpr int grid                 = 8;
    static constexpr int halfGrid             = grid / 2;
    static constexpr int quarterGrid          = grid / 4;
    static constexpr int separatorThickness   = 1;

    // Spacing
    static constexpr int compactPadding       = grid;
    static constexpr int panelPadding         = grid * 2;
    static constexpr int sectionGap           = grid * 2;
    static constexpr int largeGap             = grid * 3;

    // Standard controls
    static constexpr int compactControlHeight = grid * 3;
    static constexpr int controlHeight        = grid * 4;
    static constexpr int largeControlHeight   = grid * 5;
    static constexpr int iconButtonSize       = grid * 4;
    static constexpr int transportButtonWidth = grid * 10;
    static constexpr int tempoFieldWidth      = grid * 14;

    // Main standalone regions
    static constexpr int transportHeight      = grid * 7;
    static constexpr int sidebarWidth         = grid * 28;
    static constexpr int inspectorHeight      = grid * 18;
    static constexpr int minimumWindowWidth   = grid * 96;
    static constexpr int minimumWindowHeight  = grid * 72;
    static constexpr int defaultWindowWidth   = grid * 150;
    static constexpr int defaultWindowHeight  = grid * 100;

    // Arrangement workspace
    static constexpr int timelineHeight       = grid * 3;
    static constexpr int trackHeight          = grid * 7;
    static constexpr int minimumTrackHeight   = grid * 5;
    static constexpr int timelineBeatWidth    = grid * 8;
    static constexpr int clipInset            = grid;

    // Common visual geometry
    static constexpr float controlCornerRadius = 2.0f;
    static constexpr float panelCornerRadius   = 3.0f;
    static constexpr float focusRingThickness  = 1.0f;
};

} // namespace dysekt::metro
