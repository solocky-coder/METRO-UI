#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "MetroLookAndFeel.h"

class PadGridView;
class FileBrowserPanel;
class MixerPanel;

namespace dysekt::metro {
class MetroStandaloneEditor final : public juce::AudioProcessorEditor {
public:
    explicit MetroStandaloneEditor (DysektProcessor&); ~MetroStandaloneEditor() override;
    void paint (juce::Graphics&) override; void resized() override;
private:
    class TransportBar; class Sidebar; class ArrangementWorkspace; class Inspector;
    enum class Content { arrange, pads, browser, mixer };
    void showContent (Content);
    DysektProcessor& processor; MetroLookAndFeel lookAndFeel;
    std::unique_ptr<TransportBar> transportBar; std::unique_ptr<Sidebar> sidebar;
    std::unique_ptr<ArrangementWorkspace> workspace; std::unique_ptr<Inspector> inspector;
    std::unique_ptr<::PadGridView> padsView;
    std::unique_ptr<::FileBrowserPanel> browserView;
    std::unique_ptr<::MixerPanel> mixerView;
    Content activeContent = Content::arrange;
};
}
