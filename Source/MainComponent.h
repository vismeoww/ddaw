#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Engine.h"
#include "PianoRoll.h"

class MainComponent : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct TrackRow
    {
        juce::Label nameLabel, chainLabel;
        juce::Slider cutoff;
        juce::TextButton loadVst { "Load VST" };
        std::unique_ptr<PianoRoll> roll;
        TrackProcessor* proc = nullptr;                       // owned by graph
        juce::AudioProcessorGraph::Node::Ptr trackNode, pluginNode;
    };

    void addTrack (const juce::String& name, SynthDevice::Wave wave, int baseNote,
                   const std::vector<std::pair<int, int>>& seedCells);   // (row, step)
    void rewire (TrackRow&);
    void chooseAndLoadPlugin (TrackRow&);

    // Declaration order matters for destruction order.
    Transport transport;
    juce::AudioDeviceManager deviceManager;
    juce::AudioPluginFormatManager formatManager;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    juce::AudioProcessorGraph::Node::Ptr outNode;

    juce::TextButton playButton { "Play" }, stopButton { "Stop" };
    std::vector<std::unique_ptr<TrackRow>> rows;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};