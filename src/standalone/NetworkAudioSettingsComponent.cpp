#include "NetworkAudioSettingsComponent.h"

NetworkAudioSettingsComponent::NetworkAudioSettingsComponent (juce::AudioDeviceManager& dm, MetroNetworkAudio& net)
    : audioSelector (dm, 0, 0, 1, 2, false, false, false, false), networkAudio (net), sourceModel (networkAudio)
{
    addAndMakeVisible (audioSelector);
    networkTitle.setText ("NETWORK AUDIO", juce::dontSendNotification);
    networkTitle.setFont (juce::Font (17.0f, juce::Font::bold));
    addAndMakeVisible (networkTitle);
    transportLabel.setText ("SonoBus / AOO • Wi-Fi LAN", juce::dontSendNotification);
    addAndMakeVisible (transportLabel);
    serverLabel.setText ("Server", juce::dontSendNotification);
    portLabel.setText ("Port", juce::dontSendNotification);
    userLabel.setText ("User", juce::dontSendNotification);
    groupLabel.setText ("Group", juce::dontSendNotification);
    passwordLabel.setText ("Password", juce::dontSendNotification);
    for (auto* l : { &serverLabel, &portLabel, &userLabel, &groupLabel, &passwordLabel }) addAndMakeVisible (l);
    serverEditor.setText (MetroNetworkAudio::defaultServer);
    portEditor.setText (juce::String (MetroNetworkAudio::defaultServerPort));
    userEditor.setText (juce::SystemStats::getComputerName());
    groupEditor.setText ("METRO");
    passwordEditor.setPasswordCharacter ('*');
    for (auto* e : { &serverEditor, &portEditor, &userEditor, &groupEditor, &passwordEditor }) addAndMakeVisible (e);
    publicGroupButton.setButtonText ("Public group");
    addAndMakeVisible (publicGroupButton);
    connectButton.onClick = [this] { connectClicked(); };
    disconnectButton.onClick = [this] { disconnectClicked(); };
    addAndMakeVisible (connectButton);
    addAndMakeVisible (disconnectButton);
    statusLabel.setText ("Disconnected", juce::dontSendNotification);
    addAndMakeVisible (statusLabel);
    sourcesLabel.setText ("Available network sources", juce::dontSendNotification);
    addAndMakeVisible (sourcesLabel);
    sourceList.setModel (&sourceModel);
    sourceList.setRowHeight (30);
    addAndMakeVisible (sourceList);
    setSize (760, 720);
    startTimerHz (4);
}

NetworkAudioSettingsComponent::~NetworkAudioSettingsComponent() { sourceList.setModel (nullptr); networkAudio.stop(); }
void NetworkAudioSettingsComponent::paint (juce::Graphics& g) { g.fillAll (juce::Colour (0xff0d0d14)); }

void NetworkAudioSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced (16);
    audioSelector.setBounds (area.removeFromTop (160)); area.removeFromTop (14);
    networkTitle.setBounds (area.removeFromTop (25)); transportLabel.setBounds (area.removeFromTop (20)); area.removeFromTop (8);
    auto row = area.removeFromTop (30);
    serverLabel.setBounds (row.removeFromLeft (55)); serverEditor.setBounds (row.removeFromLeft (250)); row.removeFromLeft (14);
    portLabel.setBounds (row.removeFromLeft (35)); portEditor.setBounds (row.removeFromLeft (90));
    row = area.removeFromTop (30);
    userLabel.setBounds (row.removeFromLeft (55)); userEditor.setBounds (row.removeFromLeft (180)); row.removeFromLeft (14);
    groupLabel.setBounds (row.removeFromLeft (45)); groupEditor.setBounds (row.removeFromLeft (180));
    row = area.removeFromTop (30);
    passwordLabel.setBounds (row.removeFromLeft (65)); passwordEditor.setBounds (row.removeFromLeft (180));
    publicGroupButton.setBounds (row.removeFromLeft (120)); connectButton.setBounds (row.removeFromLeft (145).reduced (4, 0)); disconnectButton.setBounds (row.reduced (4, 0));
    statusLabel.setBounds (area.removeFromTop (28)); sourcesLabel.setBounds (area.removeFromTop (25)); sourceList.setBounds (area);
}

void NetworkAudioSettingsComponent::connectClicked()
{
    if (! networkAudio.start()) { updateStatus ("AOO backend could not start"); return; }
    const auto host = serverEditor.getText().trim(); const int port = portEditor.getText().getIntValue();
    const auto user = userEditor.getText().trim(); const auto password = passwordEditor.getText(); const auto group = groupEditor.getText().trim();
    if (host.isEmpty() || port <= 0 || port > 65535 || user.isEmpty() || group.isEmpty()) { updateStatus ("Enter valid connection fields"); return; }
    if (! networkAudio.connectToServer (host, port, user, password)) { updateStatus ("Server connection failed"); return; }
    if (! networkAudio.joinGroup (group, password, publicGroupButton.getToggleState())) { updateStatus ("Group join failed"); return; }
    updateStatus ("Connected • listening for network sources");
}
void NetworkAudioSettingsComponent::disconnectClicked() { networkAudio.disconnect(); updateStatus ("Disconnected"); refreshSources(); }
void NetworkAudioSettingsComponent::timerCallback() { refreshSources(); }
void NetworkAudioSettingsComponent::refreshSources() { sourceList.updateContent(); sourceList.repaint(); }
void NetworkAudioSettingsComponent::updateStatus (const juce::String& text) { statusLabel.setText (text, juce::dontSendNotification); }
int NetworkAudioSettingsComponent::SourceListModel::getNumRows()
{
    const auto sources = owner.getSources();
    return (int) std::count_if (sources.begin(), sources.end(), [] (const auto& source) { return source.online; });
}
void NetworkAudioSettingsComponent::SourceListModel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    const auto sources = owner.getSources();
    const MetroNetworkAudio::SourceInfo* source = nullptr;
    int visibleRow = 0;
    for (const auto& candidate : sources)
    {
        if (! candidate.online) continue;
        if (visibleRow++ == row) { source = &candidate; break; }
    }
    if (source == nullptr) return;
    const auto& s = *source;
    g.fillAll (selected ? juce::Colour (0xff29323a) : juce::Colour (0xff111116)); g.setColour (juce::Colours::white); g.setFont (14.0f);
    const auto name = (s.user.isEmpty() ? juce::String ("Source ") + juce::String (s.sourceId) : s.user)
        + " • source #" + juce::String (s.sourceId);
    const auto details = (s.group.isEmpty() ? juce::String() : s.group + " • ") + juce::String (s.sampleRate, 0) + " Hz • " + juce::String (s.channels) + " ch";
    g.drawText (name, 10, 2, width / 2, height - 4, juce::Justification::centredLeft); g.setColour (juce::Colours::lightgrey);
    g.drawText (details, width / 2, 2, width / 2 - 10, height - 4, juce::Justification::centredRight);
}
