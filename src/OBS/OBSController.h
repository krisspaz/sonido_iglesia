#pragma once

#include "LocalWebSocket.h"
#include "Platform/OBSLocator.h"
#include "Settings/AppSettings.h"

#include <juce_core/juce_core.h>

namespace churchstream
{
struct OBSState
{
    bool installed = false;
    bool processRunning = false;
    bool socketConnected = false;
    bool obsConnected = false;
    bool streamActive = false;
    bool recordingActive = false;
    bool audioSourceConfigured = false;
    juce::String currentScene;
    juce::String lastError;
};

class OBSController final : private LocalWebSocket::Listener
{
public:
    explicit OBSController(AppSettings& settingsToUse);
    ~OBSController() override;

    void start();
    void stop();
    void pollInstallationAndProcess();
    bool openOBS();
    void reconnect();
    void setPassword(const juce::String& password);
    [[nodiscard]] OBSState getState() const;

private:
    void localWebSocketConnected() override;
    void localWebSocketDisconnected(const juce::String& reason) override;
    void localWebSocketMessage(const juce::String& message) override;
    void handleHello(const juce::var& data);
    void handleIdentified();
    void handleEvent(const juce::var& data);
    void handleRequestResponse(const juce::var& data);
    void sendRequest(const juce::String& requestType, juce::DynamicObject::Ptr requestData = {});
    void attemptAudioSourceConfiguration();
    static bool boolProperty(const juce::var& object, const juce::Identifier& name);

    AppSettings& settings;
    LocalWebSocket webSocket { *this };
    mutable juce::CriticalSection stateLock;
    OBSState state;
    juce::String password;
    juce::String endpointId;
    bool sourceExists = false;
    bool requestedScene = false;
    bool requestedInputs = false;
};
} // namespace churchstream
