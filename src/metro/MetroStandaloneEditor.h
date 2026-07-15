#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "MetroContent.h"
#include "MetroLookAndFeel.h"
#include "MetroInspector.h"
#include "MetroSidebar.h"
#include "MetroTransportBar.h"

class PadGridView;
class FileBrowserPanel;
class MixerPanel;

namespace dysekt::metro
{
/** Standalone-only Metro shell. The shared processor and plugin editor stay UI-agnostic. */
class MetroStandaloneEditor final : public juce::AudioProcessorEditor
{
public:
    explicit MetroStandaloneEditor (DysektProcessor&);
    ~MetroStandaloneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ArrangementWorkspace;

    void showContent (MetroContent content);
    static juce::String inspectorMessageFor (MetroContent content);

    DysektProcessor& processor;
    MetroLookAndFeel lookAndFeel;
    MetroTransportBar transportBar;
    MetroSidebar sidebar;
    std::unique_ptr<ArrangementWorkspace> arrangementWorkspace;
    MetroInspector inspector;
    std::unique_ptr<::PadGridView> padsView;
    std::unique_ptr<::FileBrowserPanel> browserView;
    std::unique_ptr<::MixerPanel> mixerView;
    MetroContent activeContent = MetroContent::arrange;
};
} // namespace dysekt::metro
