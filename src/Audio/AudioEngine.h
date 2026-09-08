#pragma once

#include "AudioRouting.h"
#include "MeterSource.h"
#include "Analysis/AnalysisEngine.h"
#include "DSP/ProcessingEngine.h"
#include "Smart/SmartEngine.h"
#include "Settings/AppSettings.h"
#include "Groups/AutoGroupRouter.h"
#include "Groups/GroupMixer.h"
#include "X32/X32Client.h"

#include <juce_audio_devices/juce_audio_devices.h>

namespace churchstream
{
class AudioEngine final : private juce::AudioIODeviceCallback
{
public:
    explicit AudioEngine(AppSettings& settingsToUse);
    ~AudioEngine() override;

    juce::String initialise();
    void shutdown();

    [[nodiscard]] juce::StringArray scanInputDevices();
    [[nodiscard]] juce::StringArray scanOutputDevices();
    [[nodiscard]] juce::String selectInput(const juce::String& name);
    [[nodiscard]] juce::String selectOutput(const juce::String& name);
    [[nodiscard]] juce::String autoConfigure();
    bool reconnectIfNeeded();

    [[nodiscard]] bool isAudioRunning();
    [[nodiscard]] bool isX32Connected();
    [[nodiscard]] juce::String getCurrentInputName() const;
    [[nodiscard]] juce::String getCurrentOutputName() const;
    [[nodiscard]] double getSampleRate();
    [[nodiscard]] int getBufferSize();
    [[nodiscard]] double getLatencyMilliseconds();
    [[nodiscard]] double getAudioCpuPercent() const;
    [[nodiscard]] double getProcessingTimeMicroseconds() const noexcept;
    [[nodiscard]] int getXRunCount() const noexcept;
    [[nodiscard]] juce::String getLastError() const;
    [[nodiscard]] uint64_t getCallbackCount() const noexcept;
    [[nodiscard]] bool isInitialised() const noexcept { return initialised; }

    MeterSource& getInputMeter() noexcept { return inputMeter; }
    MeterSource& getOutputMeter() noexcept { return outputMeter; }
    ProcessingEngine& getProcessingEngine() noexcept { return processingEngine; }
    AnalysisEngine& getAnalysisEngine() noexcept { return analysisEngine; }
    SmartEngine& getSmartEngine() noexcept { return smartEngine; }
    [[nodiscard]] AutoRouteSnapshot getAutoRouteSnapshot() const noexcept { return autoGroupRouter.getSnapshot(); }
    [[nodiscard]] MaskingDecision getMaskingDecision() const noexcept { return groupMixer.getDecision(); }
    [[nodiscard]] bool isSmartMaskingEnabled() const noexcept { return groupMixer.isMaskingEnabled(); }
    void setSmartMaskingEnabled(bool shouldBeEnabled) noexcept { groupMixer.setMaskingEnabled(shouldBeEnabled); }

    // Read-only console link. It only feeds naming hints and diagnostics; it
    // never sends anything that changes the X32.
    void startX32Link(const juce::String& host);
    void stopX32Link();
    // Copies console channel names into the group router as naming hints.
    // Assumes the X32 default card routing, Card Out 1-8 = channels 1-8.
    void updateGroupNamingHints();
    [[nodiscard]] X32State getX32State() const { return x32Client.getState(); }
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

    [[nodiscard]] static bool looksLikeX32(const juce::String& name);
    [[nodiscard]] static bool looksLikeVirtualOutput(const juce::String& name);
    void rememberCurrentSetup();
    juce::String applySetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup);

    AppSettings& settings;
    juce::AudioDeviceManager deviceManager;
    MeterSource inputMeter;
    MeterSource outputMeter;
    ProcessingEngine processingEngine;
    AnalysisEngine analysisEngine;
    SmartEngine smartEngine;
    AutoGroupRouter autoGroupRouter;
    GroupMixer groupMixer;
    X32Client x32Client;
    std::atomic<int> x32NameCursor { 0 };
    std::atomic<uint64_t> callbackCount { 0 };
    std::atomic<double> processingTimeMicroseconds { 0.0 };
    std::atomic<bool> callbackActive { false };
    mutable juce::CriticalSection errorLock;
    juce::String lastError;
    bool initialised = false;
};
} // namespace churchstream
