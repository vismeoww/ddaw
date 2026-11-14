#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"

class MiniDAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "MiniDAW"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }

    void initialise (const juce::String&) override { window = std::make_unique<MainWindow> (getApplicationName()); }
    void shutdown() override                        { window = nullptr; }
    void systemRequestedQuit() override             { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name, juce::Colours::darkgrey, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (false, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (MiniDAWApplication)