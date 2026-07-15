#include "MetroStandaloneEditor.h"
#include "MetroTheme.h"
#include "../sequencer/SequencerEngine.h"
#include "../ui/FileBrowserPanel.h"
#include "../ui/MixerPanel.h"
#include "../ui/PadGridView.h"

namespace dysekt::metro
{
class MetroStandaloneEditor::ArrangementWorkspace final : public juce::Component,
                                                           private juce::Timer
{
public:
    explicit ArrangementWorkspace (SequencerEngine& sequencer) : engine (sequencer) { startTimerHz (15); }

    void paint (juce::Graphics& graphics) override
    {
        const auto bounds = getLocalBounds();
        const auto beatWidth = MetroTheme::Metrics::timelineBeatWidth;
        graphics.fillAll (MetroTheme::Colours::windowBackground);
        graphics.setFont (MetroTheme::smallFont());

        for (int x = 0; x < bounds.getWidth(); x += beatWidth)
        {
            graphics.setColour (MetroTheme::Colours::separator.withAlpha (0.55f));
            graphics.drawVerticalLine (x, 0.0f, static_cast<float> (bounds.getHeight()));
            graphics.setColour (MetroTheme::Colours::textDisabled);
            graphics.drawText (juce::String (x / beatWidth + 1), x + MetroTheme::Metrics::grid, 0,
                               beatWidth, MetroTheme::Metrics::grid * 3,
                               juce::Justification::centredLeft);
        }

        for (int index = 0; index < engine.getNumTracks(); ++index)
        {
            const auto y = MetroTheme::Metrics::grid * 3 + index * MetroTheme::Metrics::trackHeight;
            auto row = juce::Rectangle<int> (0, y, bounds.getWidth(), MetroTheme::Metrics::trackHeight);
            const auto track = engine.getTrackInfo (index);
            graphics.setColour (MetroTheme::Colours::panelBackground.withAlpha (0.75f));
            graphics.fillRect (row);
            graphics.setColour (MetroTheme::Colours::separator);
            graphics.drawHorizontalLine (row.getBottom() - 1, 0.0f, static_cast<float> (bounds.getWidth()));
            graphics.setColour (track.colour.brighter (0.2f));
            graphics.fillRect (row.removeFromLeft (MetroTheme::Metrics::grid / 2));
            graphics.setColour (MetroTheme::Colours::textPrimary);
            graphics.drawText (track.name, MetroTheme::Metrics::grid * 2, y,
                               MetroTheme::Metrics::grid * 18, MetroTheme::Metrics::trackHeight,
                               juce::Justification::centredLeft);

            for (int clipIndex = 0; clipIndex < engine.getNumClips (index); ++clipIndex)
            {
                const auto clip = engine.getClipInfo (index, clipIndex);
                const auto clipX = static_cast<int> (clip.startTick / 960) * beatWidth;
                const auto clipWidth = juce::jmax (beatWidth, static_cast<int> (clip.lengthTicks / 960) * beatWidth);
                const auto clipBounds = juce::Rectangle<int> (clipX, y + MetroTheme::Metrics::grid,
                    clipWidth, MetroTheme::Metrics::trackHeight - MetroTheme::Metrics::grid * 2);
                graphics.setColour (track.colour.withAlpha (0.78f));
                graphics.fillRect (clipBounds);
                graphics.setColour (MetroTheme::Colours::textPrimary);
                graphics.drawText ("Clip " + juce::String (clipIndex + 1),
                                   clipBounds.reduced (MetroTheme::Metrics::grid),
                                   juce::Justification::centredLeft, true);
            }
        }

        graphics.setColour (MetroTheme::Colours::accent);
        graphics.drawVerticalLine (static_cast<int> (engine.getPlayheadBeats()) * beatWidth,
                                   0.0f, static_cast<float> (bounds.getHeight()));
    }

private:
    void timerCallback() override { repaint(); }
    SequencerEngine& engine;
};

MetroStandaloneEditor::MetroStandaloneEditor (DysektProcessor& processorToEdit)
    : AudioProcessorEditor (processorToEdit), processor (processorToEdit), transportBar (processor.sequencer)
{
    setLookAndFeel (&lookAndFeel);
    arrangementWorkspace = std::make_unique<ArrangementWorkspace> (processor.sequencer);
    padsView = std::make_unique<PadGridView> (processor);
    browserView = std::make_unique<FileBrowserPanel> (processor);
    mixerView = std::make_unique<MixerPanel> (processor);
    sidebar.onSelectionChanged = [this] (MetroContent content) { showContent (content); };

    for (auto* component : { static_cast<juce::Component*> (&transportBar), static_cast<juce::Component*> (&sidebar),
                             static_cast<juce::Component*> (arrangementWorkspace.get()), static_cast<juce::Component*> (&inspector),
                             static_cast<juce::Component*> (padsView.get()), static_cast<juce::Component*> (browserView.get()),
                             static_cast<juce::Component*> (mixerView.get()) })
        addAndMakeVisible (*component);

    showContent (MetroContent::arrange);
    setResizable (true, true);
    setSize (MetroTheme::Metrics::grid * 150, MetroTheme::Metrics::grid * 100);
}

MetroStandaloneEditor::~MetroStandaloneEditor() { setLookAndFeel (nullptr); }
void MetroStandaloneEditor::paint (juce::Graphics& graphics) { graphics.fillAll (MetroTheme::Colours::windowBackground); }

void MetroStandaloneEditor::resized()
{
    auto area = getLocalBounds();
    transportBar.setBounds (area.removeFromTop (MetroTheme::Metrics::transportHeight));
    inspector.setBounds (area.removeFromBottom (MetroTheme::Metrics::inspectorHeight));
    sidebar.setBounds (area.removeFromLeft (MetroTheme::Metrics::sidebarWidth));
    arrangementWorkspace->setBounds (area); padsView->setBounds (area);
    browserView->setBounds (area); mixerView->setBounds (area);
}

void MetroStandaloneEditor::showContent (MetroContent content)
{
    activeContent = content;
    sidebar.setSelectedContent (content);
    arrangementWorkspace->setVisible (content == MetroContent::arrange);
    padsView->setVisible (content == MetroContent::pads);
    browserView->setVisible (content == MetroContent::browser);
    mixerView->setVisible (content == MetroContent::mixer);
    inspector.setContextMessage (inspectorMessageFor (content));
    resized();
}

juce::String MetroStandaloneEditor::inspectorMessageFor (MetroContent content)
{
    switch (content)
    {
        case MetroContent::arrange: return "Select a clip or track to inspect its properties";
        case MetroContent::pads: return "Select a pad to inspect its sample and playback settings";
        case MetroContent::browser: return "Select an item to inspect its metadata";
        case MetroContent::mixer: return "Select a channel to inspect its routing and levels";
    }
    return {};
}
} // namespace dysekt::metro
