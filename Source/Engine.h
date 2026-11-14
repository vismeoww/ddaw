#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// Shared transport: the UI thread writes, audio thread reads. Lock-free.
struct Transport
{
    std::atomic<bool> playing { false };
};

//==============================================================================
// Modular device interface (Bitwig-style chain element).
// All buffers/state are allocated in prepare(); process() never allocates or locks.
struct Device
{
    virtual ~Device() = default;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() {}
    virtual void process (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) = 0;
};

//------------------------------------------------------------------------------
// Source device: tiny mono synth driven by MIDI.
class SynthDevice : public Device
{
public:
    enum class Wave { saw, square };
    explicit SynthDevice (Wave w) : wave (w) {}

    void prepare (double sr, int) override { sampleRate = sr; reset(); }
    void reset() override { gate = false; env = 0.0f; phase = 0.0; }

    void process (juce::AudioBuffer<float>& buf, juce::MidiBuffer& midi) override
    {
        const int n = buf.getNumSamples();
        auto* l = buf.getWritePointer (0);
        auto* r = buf.getWritePointer (1);
        auto it = midi.begin();

        for (int i = 0; i < n; ++i)
        {
            while (it != midi.end() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                ++it;
                if (m.isNoteOn())
                {
                    phaseInc = juce::MidiMessage::getMidiNoteInHertz (m.getNoteNumber()) / sampleRate;
                    gate = true;
                }
                else if (m.isNoteOff() || m.isAllNotesOff())
                    gate = false;
            }

            env += ((gate ? 1.0f : 0.0f) - env) * 0.002f;      // click-free gate
            phase += phaseInc;
            if (phase >= 1.0) phase -= 1.0;

            const float osc = (wave == Wave::saw) ? (float) (2.0 * phase - 1.0)
                                                  : (phase < 0.5 ? 1.0f : -1.0f);
            l[i] = r[i] = osc * env * 0.2f;
        }
    }

private:
    Wave wave;
    double sampleRate = 44100.0, phase = 0.0, phaseInc = 0.0;
    float env = 0.0f;
    bool gate = false;
};

//------------------------------------------------------------------------------
// Filter device: lowpass, cutoff is an atomic parameter set from the UI thread.
class FilterDevice : public Device
{
public:
    std::atomic<float> cutoffHz { 2000.0f };

    void prepare (double sr, int maxBlock) override
    {
        filter.prepare ({ sr, (juce::uint32) maxBlock, 2 });
        filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
        filter.setResonance (0.7f);
    }
    void reset() override { filter.reset(); }

    void process (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
    {
        filter.setCutoffFrequency (cutoffHz.load (std::memory_order_relaxed));
        juce::dsp::AudioBlock<float> block (buf);
        filter.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

private:
    juce::dsp::StateVariableTPTFilter<float> filter;
};

//------------------------------------------------------------------------------
// Output device: smoothed gain.
class OutputDevice : public Device
{
public:
    std::atomic<float> gain { 0.8f };

    void prepare (double, int) override { last = gain.load(); }
    void process (juce::AudioBuffer<float>& buf, juce::MidiBuffer&) override
    {
        const float g = gain.load (std::memory_order_relaxed);
        buf.applyGainRamp (0, buf.getNumSamples(), last, g);
        last = g;
    }

private:
    float last = 0.8f;
};

//==============================================================================
// A track = a step pattern (piano roll data) + a fixed serial device chain
// (Synth -> Filter -> Out). Exposed to the host graph as a regular AudioProcessor
// so a VST3/AU insert can follow it.
class TrackProcessor : public juce::AudioProcessor
{
public:
    //--------------------------------------------------------------------------
    // Piano-roll data: a grid of atomic cells. The UI thread writes, the audio
    // thread reads; no locks, no allocation, no shared containers.
    struct Pattern
    {
        static constexpr int numSteps = 16;   // 16 sixteenth-notes = 1 bar of 4/4
        static constexpr int numRows  = 24;   // 2 octaves above baseNote

        Pattern() { for (auto& c : cells) c.store (0); }

        bool get (int row, int step) const { return cells[(size_t) (step * numRows + row)].load (std::memory_order_relaxed) != 0; }
        void set (int row, int step, bool on) { cells[(size_t) (step * numRows + row)].store (on ? 1 : 0, std::memory_order_relaxed); }

    private:
        std::array<std::atomic<juce::uint8>, (size_t) (numSteps * numRows)> cells;
    };

    TrackProcessor (Transport& t, juce::String name, SynthDevice::Wave wave, int baseNoteIn)
        : AudioProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          transport (t), trackName (std::move (name)), baseNote (baseNoteIn)
    {
        auto f = std::make_unique<FilterDevice>();
        filter = f.get();
        chain.push_back (std::make_unique<SynthDevice> (wave));
        chain.push_back (std::move (f));
        chain.push_back (std::make_unique<OutputDevice>());
    }

    FilterDevice& getFilter()      { return *filter; }
    Pattern&      getPattern()     { return pattern; }
    int           getBaseNote() const { return baseNote; }
    int           getCurrentStep() const { return currentStep.load (std::memory_order_relaxed); }   // -1 = stopped

    //--------------------------------------------------------------------------
    void prepareToPlay (double sr, int maxBlock) override
    {
        // Message/prepare thread: allocate everything here.
        stepLen = std::max<juce::int64> (1, (juce::int64) (sr * 60.0 / bpm / 4.0));
        loopLen = stepLen * Pattern::numSteps;

        clipMidi.ensureSize (2048);
        for (auto& d : chain) d->prepare (sr, maxBlock);
        pos = 0;
        wasPlaying = false;
        currentStep.store (-1);
    }

    void releaseResources() override {}

    using juce::AudioProcessor::processBlock;   // keep the double overload visible
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals noDenormals;
        midi.clear();
        buffer.clear();

        if (buffer.getNumChannels() < 2) return;

        if (! transport.playing.load (std::memory_order_acquire))
        {
            if (wasPlaying)                       // stopped: rewind and silence
            {
                wasPlaying = false;
                pos = 0;
                for (auto& d : chain) d->reset();
            }
            currentStep.store (-1, std::memory_order_relaxed);
            return;
        }
        wasPlaying = true;

        // --- pattern playback: scan the grid for steps that start/end inside this block
        const int n = buffer.getNumSamples();
        const juce::int64 start = pos % loopLen;
        clipMidi.clear();

        auto inBlock = [&] (juce::int64 t, int& samplePos)
        {
            if (t < start) t += loopLen;                      // wrapped into this block?
            if (t >= start && t < start + n) { samplePos = (int) (t - start); return true; }
            return false;
        };

        for (int s = 0; s < Pattern::numSteps; ++s)
        {
            const juce::int64 onT  = s * stepLen;
            const juce::int64 offT = onT + (stepLen * 9) / 10;   // 90% gate
            int p = 0;

            if (inBlock (offT, p))
                clipMidi.addEvent (juce::MidiMessage::allNotesOff (1), p);

            if (inBlock (onT, p))
                for (int r = 0; r < Pattern::numRows; ++r)
                    if (pattern.get (r, s))
                        clipMidi.addEvent (juce::MidiMessage::noteOn (1, baseNote + r, (juce::uint8) 100), p);
        }

        currentStep.store ((int) (start / stepLen), std::memory_order_relaxed);

        // --- serial device chain
        for (auto& d : chain) d->process (buffer, clipMidi);

        pos += n;
    }

    //--------------------------------------------------------------------------
    const juce::String getName() const override { return trackName; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool hasEditor() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && l.getMainInputChannelSet().isDisabled();
    }

private:
    Transport& transport;
    juce::String trackName;
    const int baseNote;
    const double bpm = 120.0;

    Pattern pattern;
    std::atomic<int> currentStep { -1 };

    juce::MidiBuffer clipMidi;
    std::vector<std::unique_ptr<Device>> chain;
    FilterDevice* filter = nullptr;

    juce::int64 stepLen = 1, loopLen = 1, pos = 0;   // audio-thread only (after prepare)
    bool wasPlaying = false;                          // audio-thread only
};