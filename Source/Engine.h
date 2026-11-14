#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
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
// A track = a looping MIDI clip + a fixed serial device chain (Synth -> Filter -> Out).
// Exposed to the host graph as a regular AudioProcessor so a VST3/AU insert can follow it.
class TrackProcessor : public juce::AudioProcessor
{
public:
    struct Note { double startBeat, lengthBeat; int note; };

    TrackProcessor (Transport& t, juce::String name, SynthDevice::Wave wave,
                    std::vector<Note> clipNotes, double clipBeats)
        : AudioProcessor (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          transport (t), trackName (std::move (name)),
          notes (std::move (clipNotes)), clipLengthBeats (clipBeats)
    {
        auto f = std::make_unique<FilterDevice>();
        auto o = std::make_unique<OutputDevice>();
        filter = f.get();
        output = o.get();
        chain.push_back (std::make_unique<SynthDevice> (wave));
        chain.push_back (std::move (f));
        chain.push_back (std::move (o));
    }

    FilterDevice& getFilter() { return *filter; }

    //--------------------------------------------------------------------------
    void prepareToPlay (double sr, int maxBlock) override
    {
        // Message/prepare thread: allocate everything here.
        const double samplesPerBeat = sr * 60.0 / bpm;
        loopLen = std::max<juce::int64> (1, (juce::int64) (clipLengthBeats * samplesPerBeat));

        events.clear();
        events.reserve (notes.size() * 2);
        for (auto& n : notes)
        {
            const auto on  = (juce::int64) (n.startBeat * samplesPerBeat);
            const auto off = juce::jmin (loopLen - 1, on + (juce::int64) (n.lengthBeat * samplesPerBeat));
            events.push_back ({ on,  n.note, true  });
            events.push_back ({ off, n.note, false });
        }

        clipMidi.ensureSize (2048);
        for (auto& d : chain) d->prepare (sr, maxBlock);
        pos = 0;
        wasPlaying = false;
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
            return;
        }
        wasPlaying = true;

        // --- clip playback: turn clip notes into sample-accurate MIDI for this block
        const int n = buffer.getNumSamples();
        const juce::int64 start = pos % loopLen;
        clipMidi.clear();

        for (const auto& e : events)
        {
            juce::int64 t = e.time;
            if (t < start) t += loopLen;                      // wrapped into this block?
            if (t >= start && t < start + n)
                clipMidi.addEvent (e.on ? juce::MidiMessage::noteOn  (1, e.note, (juce::uint8) 100)
                                        : juce::MidiMessage::noteOff (1, e.note),
                                   (int) (t - start));
        }

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
    struct Event { juce::int64 time; int note; bool on; };

    Transport& transport;
    juce::String trackName;
    std::vector<Note> notes;          // immutable after construction
    double clipLengthBeats;
    const double bpm = 120.0;

    std::vector<Event> events;        // built in prepareToPlay, read-only on audio thread
    juce::MidiBuffer clipMidi;
    std::vector<std::unique_ptr<Device>> chain;
    FilterDevice* filter = nullptr;
    OutputDevice* output = nullptr;

    juce::int64 pos = 0, loopLen = 1; // audio-thread only
    bool wasPlaying = false;          // audio-thread only
};