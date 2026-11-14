#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

// Minimal step-grid piano roll. Click to toggle a note, drag to paint/erase.
class PianoRoll : public juce::Component, private juce::Timer
{
public:
    explicit PianoRoll (TrackProcessor& t)
        : track (t), pattern (t.getPattern()), baseNote (t.getBaseNote())
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        const auto grid = getGridBounds();
        const float cw = grid.getWidth()  / (float) kSteps;
        const float ch = grid.getHeight() / (float) kRows;

        g.fillAll (juce::Colour (0xff1b1b20));

        // pitch rows (black-key rows darker) + C labels in the gutter
        for (int r = 0; r < kRows; ++r)
        {
            const float y = grid.getY() + (float) (kRows - 1 - r) * ch;
            const int note = baseNote + r, pc = note % 12;
            const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;

            g.setColour (black ? juce::Colour (0xff202026) : juce::Colour (0xff2b2b33));
            g.fillRect (grid.getX(), y, grid.getWidth(), ch);

            if (pc == 0)
            {
                g.setColour (juce::Colours::grey);
                g.setFont (11.0f);
                g.drawText (juce::MidiMessage::getMidiNoteName (note, true, true, 4),
                            0, (int) (y + ch * 0.5f - 6.0f), kGutter - 4, 12,
                            juce::Justification::centredRight);
            }
        }

        // step lines (brighter on beats)
        for (int s = 0; s <= kSteps; ++s)
        {
            g.setColour (juce::Colours::white.withAlpha (s % 4 == 0 ? 0.25f : 0.08f));
            g.drawVerticalLine ((int) (grid.getX() + (float) s * cw), grid.getY(), grid.getBottom());
        }

        // playhead
        if (lastStep >= 0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.10f));
            g.fillRect (grid.getX() + (float) lastStep * cw, grid.getY(), cw, grid.getHeight());
        }

        // notes
        g.setColour (juce::Colour (0xffff9f43));
        for (int s = 0; s < kSteps; ++s)
            for (int r = 0; r < kRows; ++r)
                if (pattern.get (r, s))
                    g.fillRoundedRectangle (grid.getX() + (float) s * cw + 1.0f,
                                            grid.getY() + (float) (kRows - 1 - r) * ch + 1.0f,
                                            cw - 2.0f, ch - 2.0f, 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        int row, step;
        if (cellAt (e.position, row, step))
        {
            paintValue = ! pattern.get (row, step);   // first click decides paint vs. erase
            apply (row, step);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        int row, step;
        if (cellAt (e.position, row, step))
            apply (row, step);
    }

private:
    static constexpr int kSteps  = TrackProcessor::Pattern::numSteps;
    static constexpr int kRows   = TrackProcessor::Pattern::numRows;
    static constexpr int kGutter = 36;

    juce::Rectangle<float> getGridBounds() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft ((float) kGutter);
    }

    bool cellAt (juce::Point<float> p, int& row, int& step) const
    {
        const auto grid = getGridBounds();
        if (! grid.contains (p)) return false;
        step = juce::jlimit (0, kSteps - 1, (int) ((p.x - grid.getX()) / (grid.getWidth() / (float) kSteps)));
        row  = kRows - 1 - juce::jlimit (0, kRows - 1, (int) ((p.y - grid.getY()) / (grid.getHeight() / (float) kRows)));
        return true;
    }

    void apply (int row, int step)
    {
        if (pattern.get (row, step) != paintValue)
        {
            pattern.set (row, step, paintValue);
            repaint();
        }
    }

    void timerCallback() override
    {
        const int s = track.getCurrentStep();
        if (s != lastStep) { lastStep = s; repaint(); }
    }

    TrackProcessor& track;
    TrackProcessor::Pattern& pattern;
    const int baseNote;
    bool paintValue = true;
    int lastStep = -1;
};