#include "MainComponent.h"
#include "UI/TrayController.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace churchstream
{
class ChurchStreamProcessorApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override
    {
        const auto dataDirectory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                       .getChildFile("ChurchStreamProcessor");
        const auto logDirectory = dataDirectory.getChildFile("logs");
        logDirectory.createDirectory();
        juce::Array<juce::File> oldLogs;
        logDirectory.findChildFiles(oldLogs, juce::File::findFiles, false, "ChurchStreamProcessor-*.log");
        oldLogs.sort();
        while (oldLogs.size() >= 10)
        {
            oldLogs.getFirst().deleteFile();
            oldLogs.remove(0);
        }
        const auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
        const auto logFile = logDirectory.getChildFile("ChurchStreamProcessor-" + timestamp + ".log");
        logger = std::make_unique<juce::FileLogger>(logFile,
                                                    "Church Stream Processor startup",
                                                    256 * 1024);
        juce::Logger::setCurrentLogger(logger.get());
        juce::Logger::writeToLog("Version " + getApplicationVersion()
                                 + " on " + juce::SystemStats::getOperatingSystemName());
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), commandLine);
    }

    void shutdown() override
    {
        mainWindow.reset();
        juce::Logger::writeToLog("Clean shutdown");
        juce::Logger::setCurrentLogger(nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override
    {
        if (mainWindow != nullptr)
        {
            mainWindow->setVisible(true);
            mainWindow->setMinimised(false);
            mainWindow->toFront(true);
        }
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, const juce::String& commandLine)
            : DocumentWindow(name, Colours::background,
                             DocumentWindow::minimiseButton | DocumentWindow::closeButton)
        {
            const auto startMinimised = commandLine.containsIgnoreCase("--minimized");
            const auto startAudio = !commandLine.containsIgnoreCase("--no-audio");
            setUsingNativeTitleBar(true);
            setResizable(true, true);
            setResizeLimits(1080, 900, 1600, 1200);
            auto* main = new MainComponent(startAudio);
            setContentOwned(main, true);
            tray = std::make_unique<TrayController>();
            tray->open = [this] { setVisible(true); setMinimised(false); toFront(true); };
            tray->bypass = [main] { main->setBypassed(true); };
            tray->resume = [main] { main->setBypassed(false); };
            tray->connectObs = [main] { main->reconnectOBS(); };
            tray->quit = []
            {
                if (auto* app = JUCEApplication::getInstance()) app->systemRequestedQuit();
            };
            centreWithSize(getWidth(), getHeight());
            setVisible(!startMinimised);
        }

        void closeButtonPressed() override
        {
            setVisible(false);
        }
        std::unique_ptr<TrayController> tray;
    };

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace churchstream

START_JUCE_APPLICATION(churchstream::ChurchStreamProcessorApplication)
