#pragma once

#include "Audio/AudioEngine.h"
#include "Settings/AppSettings.h"
#include "System/SystemMonitor.h"
#include "UI/LevelMeter.h"
#include "UI/Theme.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace churchstream
{
class StatusBadge final : public juce::Component
{
public:
    explicit StatusBadge(juce::String statusName);
    void setState(bool isOnline, juce::String stateText);
    void paint(juce::Graphics&) override;

private:
    juce::String name;
    juce::String state { "WAITING" };
    bool online = false;
};

class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void refreshDeviceLists();
    void handleInputSelection();
    void handleOutputSelection();
    void runAutoConfigure();
    void showResult(const juce::String& error, const juce::String& successMessage);
    void updateLiveValues();
    void applyRefreshRate();
    [[nodiscard]] int getVisibleRefreshRate() const;
    static void configureHeading(juce::Label& label, float size, juce::Colour colour);

    Theme theme;
    AppSettings settings;
    AudioEngine audioEngine { settings };

    juce::Label titleLabel;
    juce::Label phaseLabel;
    juce::ComboBox performanceProfile;
    juce::ToggleButton developmentMode { "DEVELOPMENT" };
    StatusBadge x32Status { "X32" };
    StatusBadge audioStatus { "AUDIO" };
    StatusBadge outputStatus { "OUTPUT" };

    juce::Label routingTitle;
    juce::Label inputLabel;
    juce::Label outputLabel;
    juce::ComboBox inputDevice;
    juce::ComboBox outputDevice;
    juce::TextButton autoConfigureButton { "AUTO CONFIGURE" };
    juce::Label messageLabel;

    LevelMeter inputMeter { "Input" };
    LevelMeter outputMeter { "Output" };
    juce::Label formatLabel;
    juce::Label latencyLabel;
    juce::Label diagnosticsLabel;

    SystemMonitor systemMonitor;

    juce::StringArray inputDeviceNames;
    juce::StringArray outputDeviceNames;
    bool suppressSelectionCallbacks = false;
    bool deviceListsDirty = false;
    bool uiSuspended = false;
    double lastReconnectAttemptMs = 0.0;
    double lastDiagnosticsSampleMs = 0.0;
    double systemCpuPercent = 0.0;
    double processMemoryMegabytes = 0.0;
};
} // namespace churchstream
