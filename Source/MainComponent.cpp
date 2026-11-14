#include "MainComponent.h"

using Wave = SynthDevice::Wave;
using Note = TrackProcessor::Note;

MainComponent::MainComponent()
{
    formatManager.addDefaultFormats();   // VST3 (+ AU on macOS)

    playButton.onClick = [this] { transport.playing.store (true,  std::memory_order_release); };
    stopButton.onClick = [this] { transport.playing.store (false, std::memory_order_release); };
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);

    // Audio engine: device -> AudioProcessorPlayer -> AudioProcessorGraph
    deviceManager.initialise (0, 2, nullptr, true);
    player.setProcessor (&graph);
    deviceManager.addAudioCallback (&player);

    outNode = graph.addNode (std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor> (
                  juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    addTrack ("Lead", Wave::saw,    { { 0, 0.8, 60 }, { 1, 0.8, 64 }, { 2, 0.8, 67 }, { 3, 0.8, 72 } }, 4.0);
    addTrack ("Bass", Wave::square, { { 0, 1.8, 36 }, { 2, 1.8, 43 } }, 4.0);

    setSize (720, 60 + 90 * (int) rows.size());
}

MainComponent::~MainComponent()
{
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
}

void MainComponent::addTrack (const juce::String& name, Wave wave, std::vector<Note> notes, double beats)
{
    auto row = std::make_unique<TrackRow>();
    auto proc = std::make_unique<TrackProcessor> (transport, name, wave, std::move (notes), beats);
    row->proc = proc.get();
    row->trackNode = graph.addNode (std::move (proc));

    row->nameLabel.setText (name, juce::dontSendNotification);
    row->nameLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    row->chainLabel.setText ("Synth > Filter > Out   |   Insert: (empty)", juce::dontSendNotification);

    row->cutoff.setSliderStyle (juce::Slider::LinearHorizontal);
    row->cutoff.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
    row->cutoff.setRange (100.0, 10000.0, 1.0);
    row->cutoff.setSkewFactorFromMidPoint (1000.0);
    row->cutoff.setTextValueSuffix (" Hz");
    row->cutoff.setValue (2000.0, juce::dontSendNotification);

    auto* r = row.get();
    row->cutoff.onValueChange = [r] { r->proc->getFilter().cutoffHz.store ((float) r->cutoff.getValue()); };
    row->loadVst.onClick      = [this, r] { chooseAndLoadPlugin (*r); };

    addAndMakeVisible (row->nameLabel);
    addAndMakeVisible (row->chainLabel);
    addAndMakeVisible (row->cutoff);
    addAndMakeVisible (row->loadVst);

    rewire (*row);
    rows.push_back (std::move (row));
}

// Track -> [plugin insert] -> output. The graph swaps its render sequence atomically,
// so the audio thread never blocks on this.
void MainComponent::rewire (TrackRow& r)
{
    graph.disconnectNode (r.trackNode->nodeID);

    const auto dst = r.pluginNode != nullptr ? r.pluginNode->nodeID : outNode->nodeID;
    for (int ch = 0; ch < 2; ++ch)
        graph.addConnection ({ { r.trackNode->nodeID, ch }, { dst, ch } });

    if (r.pluginNode != nullptr)
        for (int ch = 0; ch < 2; ++ch)
            graph.addConnection ({ { r.pluginNode->nodeID, ch }, { outNode->nodeID, ch } });
}

void MainComponent::chooseAndLoadPlugin (TrackRow& row)
{
    chooser = std::make_unique<juce::FileChooser> ("Select a VST3 / AU effect", juce::File(), "*.vst3;*.component");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::canSelectDirectories;   // .vst3/.component are bundles

    chooser->launchAsync (flags, [this, r = &row] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file == juce::File()) return;

        juce::OwnedArray<juce::PluginDescription> found;
        for (int i = 0; i < formatManager.getNumFormats(); ++i)
        {
            auto* fmt = formatManager.getFormat (i);
            if (fmt->fileMightContainThisPluginType (file.getFullPathName()))
                fmt->findAllTypesForFile (found, file.getFullPathName());
        }

        auto* device = deviceManager.getCurrentAudioDevice();
        if (found.isEmpty() || device == nullptr)
        {
            r->chainLabel.setText ("No plugin found in selection", juce::dontSendNotification);
            return;
        }

        const juce::PluginDescription desc = *found[0];
        formatManager.createPluginInstanceAsync (desc, device->getCurrentSampleRate(),
                                                 device->getCurrentBufferSizeSamples(),
            [this, r, name = desc.name] (std::unique_ptr<juce::AudioPluginInstance> inst, const juce::String& err)
            {
                if (inst == nullptr)
                {
                    r->chainLabel.setText ("Load failed: " + err, juce::dontSendNotification);
                    return;
                }
                if (inst->getTotalNumInputChannels() == 0)
                {
                    r->chainLabel.setText ("Instrument plugins not supported in this PoC (effects only)",
                                           juce::dontSendNotification);
                    return;
                }

                if (r->pluginNode != nullptr)
                    graph.removeNode (r->pluginNode->nodeID);

                r->pluginNode = graph.addNode (std::move (inst));
                rewire (*r);
                r->chainLabel.setText ("Synth > Filter > Out   |   Insert: " + name, juce::dontSendNotification);
            });
    });
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    for (size_t i = 0; i < rows.size(); ++i)
        g.drawHorizontalLine (60 + 90 * (int) i, 0.0f, (float) getWidth());
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto top = area.removeFromTop (40);
    playButton.setBounds (top.removeFromLeft (80));
    top.removeFromLeft (10);
    stopButton.setBounds (top.removeFromLeft (80));
    area.removeFromTop (10);

    for (auto& r : rows)
    {
        auto row = area.removeFromTop (90).reduced (0, 5);
        r->nameLabel.setBounds (row.removeFromLeft (70).removeFromTop (30));
        auto right = row.removeFromRight (100);
        r->loadVst.setBounds (right.removeFromTop (30));
        auto top2 = row.removeFromTop (30);
        r->cutoff.setBounds (top2);
        r->chainLabel.setBounds (row);
    }
}