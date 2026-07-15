#pragma once
#include <juce_core/juce_core.h>

// Manages state during the trim workflow.
// Created when the user selects YES in TrimDialog; destroyed after trim is
// applied or cancelled.  Non-copyable, movable only.
struct TrimSession
{
    juce::File file;          // the audio file being trimmed
    bool       active = false; // true while waveform is displayed in trim mode

    TrimSession() = default;
    TrimSession (TrimSession&&) noexcept = default;
    TrimSession& operator= (TrimSession&&) noexcept = default;

    TrimSession (const TrimSession&) = delete;
    TrimSession& operator= (const TrimSession&) = delete;
};
