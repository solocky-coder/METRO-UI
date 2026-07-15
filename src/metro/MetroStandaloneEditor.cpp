#include "MetroStandaloneEditor.h"
#include "MetroTheme.h"
#include "../sequencer/SequencerEngine.h"
#include "../ui/FileBrowserPanel.h"
#include "../ui/MixerPanel.h"
#include "../ui/PadGridView.h"

namespace dysekt::metro
{
MetroStandaloneEditor::MetroStandaloneEditor (DysektProcessor& processorToEdit)
    : AudioProcessorEditor (processorToEdit), processor (processorToEdit), transportBar (processor.sequencer)
{
    setLookAndFeel (&lookAndFeel);
    arrangementView = std::make_unique<MetroArrangementView> (processor.sequencer);
    arrangementView->setSelectionChangedCallback ([this] (const MetroSelection& selection)
                                                  { onArrangementSelectionChanged (selection); });
    padsView = std::make_unique<PadGridView> (processor);
    browserView = std::make_unique<FileBrowserPanel> (processor);
    mixerView = std::make_unique<MixerPanel> (processor);
    sidebar.onSelectionChanged = [this] (MetroContent content) { showContent (content); };

    for (auto* component : { static_cast<juce::Component*> (&transportBar), static_cast<juce::Component*> (&sidebar),
                             static_cast<juce::Component*> (arrangementView.get()), static_cast<juce::Component*> (&inspector),
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
    arrangementView->setBounds (area); padsView->setBounds (area);
    browserView->setBounds (area); mixerView->setBounds (area);
}

void MetroStandaloneEditor::showContent (MetroContent content)
{
    activeContent = content;
    sidebar.setSelectedContent (content);
    arrangementView->setVisible (content == MetroContent::arrange);
    padsView->setVisible (content == MetroContent::pads);
    browserView->setVisible (content == MetroContent::browser);
    mixerView->setVisible (content == MetroContent::mixer);

    // The arrangement tab drives the inspector through real selection objects
    // (see onArrangementSelectionChanged); every other tab still uses the
    // generic placeholder message until they grow their own selection models.
    if (content == MetroContent::arrange)
        onArrangementSelectionChanged (arrangementView->getSelection());
    else
        inspector.setContextMessage (inspectorMessageFor (content));

    resized();
}

void MetroStandaloneEditor::onArrangementSelectionChanged (const MetroSelection& selection)
{
    if (activeContent != MetroContent::arrange)
        return;

    if (selection.isClip())
        inspector.showClip (selection.clipIndex, selection.track, selection.clip);
    else if (selection.isTrack())
        inspector.showTrack (selection.track);
    else
        inspector.setContextMessage (inspectorMessageFor (MetroContent::arrange));
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
