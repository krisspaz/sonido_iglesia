#pragma once

#include "Audio/AudioEngine.h"
#include "OBS/OBSController.h"
#include "Offline/OfflineProcessor.h"
#include "Room/RoomCalibration.h"
#include "Platform/StartupManager.h"
#include "Settings/AppSettings.h"
#include "Settings/PresetManager.h"
#include "System/SystemMonitor.h"
#include "Safety/SafetyController.h"
#include "Diagnostics/SessionDiagnostics.h"
#include "UI/LevelMeter.h"
#include "UI/SpectrumComponent.h"
#include "UI/Theme.h"

#include <array>
#include <juce_gui_extra/juce_gui_extra.h>

namespace churchstream
{
class StatusBadge final : public juce::Component
{
public:
    explicit StatusBadge(juce::String statusName);
    void setState(bool isOnline, juce::String stateText, bool warning = false);
    void paint(juce::Graphics&) override;

private:
    juce::String name;
    juce::String state { "WAITING" };
    bool online = false;
    bool warningState = false;
};

class MainComponent final : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    explicit MainComponent(bool startAudio = true);
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void setBypassed(bool shouldBypass);
    void reconnectOBS();

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void refreshDeviceLists();
    void handleInputSelection();
    void handleOutputSelection();
    void runAutoConfigure();
    void showResult(const juce::String& error, const juce::String& successMessage);
    void updateLiveValues();
    void updateDspControls();
    void applyRefreshRate();
    void applyConsoleAndGroupSettings();
    void askForConsoleAddress();
    void chooseRoomMeasurement();
    void applyBuiltInPreset(int presetIndex);
    void refreshUserPresets();
    [[nodiscard]] bool liveRoutingLocked() const;
    [[nodiscard]] int getVisibleRefreshRate() const;
    [[nodiscard]] int getSmartUpdateRate() const;
    static void configureHeading(juce::Label&, float size, juce::Colour);
    static void configureControlSlider(juce::Slider&, const juce::String& suffix = " %");
    static void configureMetricLabel(juce::Label&);
    static juce::String formatMetric(float value, int decimals, const juce::String& suffix);

    Theme theme;
    AppSettings settings;
    PresetManager presetManager;
    AudioEngine audioEngine { settings };
    OBSController obsController { settings };
    SystemMonitor systemMonitor;
    SafetyController safetyController;
    SessionDiagnostics sessionDiagnostics { AppSettings::getDataDirectory() };
    OfflineProcessor offlineProcessor;
    RoomCalibration roomCalibration;

    juce::Label titleLabel;
    juce::Label phaseLabel;
    juce::ComboBox performanceProfile;
    juce::ToggleButton liveMode { "LIVE MODE" };
    juce::ToggleButton developmentMode { "DEVELOPMENT" };
    juce::TextButton advancedButton { "ADVANCED" };
    StatusBadge x32Status { "X32" };
    StatusBadge audioStatus { "AUDIO" };
    StatusBadge obsStatus { "OBS" };
    StatusBadge streamStatus { "STREAM" };

    LevelMeter inputMeter { "Input" };
    LevelMeter outputMeter { "Output" };
    SpectrumComponent spectrum;
    juce::Label spectrumTitle;

    juce::Label smartTitle;
    juce::ToggleButton smartProcessing { "SMART PROCESSING" };
    juce::ComboBox operatingMode;
    juce::ComboBox presetSelector;
    juce::TextButton savePresetButton { "SAVE" };
    juce::Slider cleanSlider;
    juce::Slider punchSlider;
    juce::Slider claritySlider;
    juce::Slider dynamicsSlider;
    juce::Slider warmthSlider;
    juce::Slider loudnessSlider;
    std::array<juce::Label, 6> controlLabels;
    juce::TextButton autoTuneButton { "AUTO TUNE" };
    juce::TextButton abButton { "A/B : B" };
    juce::TextButton bypassButton { "BYPASS" };

    juce::Label measurementsTitle;
    std::array<juce::Label, 8> metricNames;
    std::array<juce::Label, 8> metricValues;
    juce::Label actionsTitle;
    juce::Label actionsLabel;

    juce::Label routingTitle;
    juce::Label inputLabel;
    juce::Label outputLabel;
    juce::ComboBox inputDevice;
    juce::ComboBox outputDevice;
    juce::TextButton autoConfigureButton { "AUTO CONFIGURE" };
    juce::TextButton openObsButton { "OPEN OBS" };
    juce::TextEditor obsPasswordEditor;
    juce::TextButton connectObsButton { "CONNECT OBS" };
    juce::TextButton offlineTestButton { "OFFLINE TEST" };
    juce::ToggleButton startWithWindows { "START WITH WINDOWS" };
    juce::ToggleButton startMinimized { "START MINIMIZED" };
    juce::TextEditor churchNameEditor;
    juce::Label messageLabel;
    juce::Label formatLabel;
    juce::Label latencyLabel;
    juce::Label diagnosticsLabel;
    std::unique_ptr<juce::FileChooser> offlineFileChooser;
    std::unique_ptr<juce::FileChooser> roomFileChooser;
    std::unique_ptr<juce::AlertWindow> consoleAddressWindow;
    std::unique_ptr<juce::FileChooser> presetFileChooser;
    juce::Array<juce::File> userPresetFiles;

    std::array<juce::Rectangle<int>, 5> cardBounds;
    juce::StringArray inputDeviceNames;
    juce::StringArray outputDeviceNames;
    bool suppressSelectionCallbacks = false;
    bool suppressDspCallbacks = false;
    bool deviceListsDirty = false;
    bool uiSuspended = false;
    bool highLoadProtection = false;
    bool stableMixEco = false;
    float stableMixSeconds = 0.0f;
    double lastReconnectAttemptMs = 0.0;
    double lastDiagnosticsSampleMs = 0.0;
    double lastObsPollMs = 0.0;
    double lastPerformanceLogMs = 0.0;
    double lastSafetySampleMs = 0.0;
    double lastSafetyObsReconnectMs = 0.0;
    double systemCpuPercent = 0.0;
    double processCpuPercent = 0.0;
    double processMemoryMegabytes = 0.0;
    uint64_t uiFrameCounter = 0;
    uint64_t lastSpectrumAnalyzedFrames = 0;
};
} // namespace churchstream
