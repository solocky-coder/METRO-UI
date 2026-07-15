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

    DysektProcessor& processor;
    MetroLookAndFeel lookAndFeel;
    MetroTransportBar transportBar;
    MetroSidebar sidebar;
    std::unique_ptr<MetroArrangementView> arrangementView;
    MetroInspector inspector;
    std::unique_ptr<::PadGridView> padsView;
    std::unique_ptr<::FileBrowserPanel> browserView;
    std::unique_ptr<::MixerPanel> mixerView;
    MetroContent activeContent = MetroContent::arrange;
};
} // namespace dysekt::metro
