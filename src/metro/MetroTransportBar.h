#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../ui/ContextMenuButton.h"

class SequencerEngine;
class AbletonLink;

namespace dysekt::metro
{
/** Standalone transport controls bound to the shared sequencer engine.
    `link` is optional (defaults to nullptr, matching FloatingTransportBar and
    the other transport UIs) — when non-null, a LINK toggle is shown alongside
    the transport buttons, mirroring peer count and enabled state the same
    way FloatingTransportBar does. Right-clicking LINK opens a menu with a
    "Follow Remote Start/Stop" toggle, governing whether a remote peer's
    Play/Stop also drives local transport — LINK's own left-click toggle
    only ever syncs tempo. */
class MetroTransportBar final : public juce::Component,
                                private juce::Timer
{
public:
    explicit MetroTransportBar (SequencerEngine& sequencer, AbletonLink* link = nullptr);

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Fired when the user clicks the pop-out button — the host (e.g.
        MetroStandaloneEditor) owns what "floating" means: constructing a
        FloatingTransportBar, showing it, and hiding this docked bar while
        it's on screen. This class never creates a FloatingTransportBar
        itself, so MetroTransportBar.h/.cpp stay free of that dependency. */
    std::function<void()> onFloatRequested;

private:
    void timerCallback() override;
    void updateTempoFromEditor();
    void showLinkContextMenu();

    SequencerEngine& engine;
    AbletonLink*     linkPtr = nullptr;
    juce::TextButton rewind { "Rewind" };
    juce::TextButton play { "Play" };
    juce::TextButton stop { "Stop" };
    juce::TextButton record { "Record" };
    ContextMenuButton linkButton { "LINK" };
    juce::TextButton floatButton { "Float" };
    juce::Label tempo;
};
} // namespace dysekt::metro
