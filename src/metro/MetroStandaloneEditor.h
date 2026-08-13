#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "MetroArrangementView.h"
#include "MetroContent.h"
#include "MetroLookAndFeel.h"
#include "MetroInspector.h"
#include "MetroSelection.h"
#include "MetroSidebar.h"
#include "MetroTransportBar.h"
#include "FloatingTransportBar.h"

class PadGridView;
class FileBrowserPanel;
class MixerPanel;

namespace dysekt::metro
{
/** Standalone-only Metro shell. The shared processor and plugin editor stay UI-agnostic.
    This class only coordinates child components (creation, layout, tab switching); it
    contains no timeline/arrangement drawing or interaction logic of its own — that lives
    entirely in MetroArrangementView. */
class MetroStandaloneEditor final : public juce::AudioProcessorEditor
{
public:
    explicit MetroStandaloneEditor (DysektProcessor&);
    ~MetroStandaloneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showContent (MetroContent content);
    void onArrangementSelectionChanged (const MetroSelection& selection);
    static juce::String inspectorMessageFor (MetroContent content);

    /** Lazily creates (on first use) and shows the floating transport panel,
        hiding the docked MetroTransportBar while it's up. Wired to
        transportBar.onFloatRequested. */
    void showFloatingTransport();

    /** Hides/destroys the floating panel and restores the docked transport
        bar. Wired to FloatingTransportBar::onDockRequested. */
    void dockTransport();

    DysektProcessor& processor;
    MetroLookAndFeel lookAndFeel;
    MetroTransportBar transportBar;
    std::unique_ptr<FloatingTransportBar> floatingTransport;
    MetroSidebar sidebar;
    std::unique_ptr<MetroArrangementView> arrangementView;
    MetroInspector inspector;
    std::unique_ptr<::PadGridView> padsView;
    std::unique_ptr<::FileBrowserPanel> browserView;
    std::unique_ptr<::MixerPanel> mixerView;
    MetroContent activeContent = MetroContent::arrange;
};
} // namespace dysekt::metro
