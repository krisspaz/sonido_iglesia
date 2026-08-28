#include "OBSController.h"

#include "OBSAuthentication.h"
#include "Platform/WindowsAudioEndpoint.h"

namespace churchstream
{
namespace
{
const juce::Identifier opKey { "op" };
const juce::Identifier dataKey { "d" };
const juce::Identifier eventTypeKey { "eventType" };
const juce::Identifier eventDataKey { "eventData" };
const juce::Identifier responseDataKey { "responseData" };
const juce::Identifier requestTypeKey { "requestType" };
const juce::Identifier outputActiveKey { "outputActive" };
const juce::String sourceName { "Church Stream Processor Audio" };
}

OBSController::OBSController(AppSettings& settingsToUse)
    : settings(settingsToUse), password(settings.getObsPassword())
{
}

OBSController::~OBSController() { stop(); }

void OBSController::start()
{
    pollInstallationAndProcess();
    webSocket.start();
}

void OBSController::stop() { webSocket.stop(); }

void OBSController::pollInstallationAndProcess()
{
    const auto installed = OBSLocator::findExecutable().existsAsFile();
    const auto running = OBSLocator::isRunning() || webSocket.isConnected();
    const juce::ScopedLock lock(stateLock);
    state.installed = installed;
    state.processRunning = running;
}

bool OBSController::openOBS()
{
    const auto result = OBSLocator::openOBS();
    if (!result)
    {
        const juce::ScopedLock lock(stateLock);
        state.lastError = "OBS Studio executable was not found";
    }
    return result;
}

void OBSController::reconnect()
{
    webSocket.stop();
    webSocket.start();
}

void OBSController::setPassword(const juce::String& newPassword)
{
    // Stop the listener before replacing the String that handleHello reads.
    // This path is never on the audio thread.
    webSocket.stop();
    password = newPassword;
    settings.setObsPassword(newPassword);
    settings.flush();
    webSocket.start();
}

OBSState OBSController::getState() const
{
    const juce::ScopedLock lock(stateLock);
    return state;
}

void OBSController::localWebSocketConnected()
{
    const juce::ScopedLock lock(stateLock);
    state.socketConnected = true;
    state.processRunning = true;
    state.lastError.clear();
}

void OBSController::localWebSocketDisconnected(const juce::String& reason)
{
    const juce::ScopedLock lock(stateLock);
    state.socketConnected = false;
    state.obsConnected = false;
    state.streamActive = false;
    state.recordingActive = false;
    state.audioSourceConfigured = false;
    if (state.lastError.isEmpty()) state.lastError = reason;
    sourceExists = false;
    requestedScene = false;
    requestedInputs = false;
}

void OBSController::localWebSocketMessage(const juce::String& message)
{
    const auto root = juce::JSON::parse(message);
    auto* object = root.getDynamicObject();
    if (object == nullptr) return;
    const auto op = static_cast<int>(object->getProperty(opKey));
    const auto data = object->getProperty(dataKey);
    if (op == 0) handleHello(data);
    else if (op == 2) handleIdentified();
    else if (op == 5) handleEvent(data);
    else if (op == 7) handleRequestResponse(data);
}

void OBSController::handleHello(const juce::var& data)
{
    auto identify = juce::DynamicObject::Ptr(new juce::DynamicObject());
    identify->setProperty("rpcVersion", 1);
    // General | Scenes | Inputs | Outputs | SceneItems. No high-volume meters.
    identify->setProperty("eventSubscriptions", 205);

    if (auto* hello = data.getDynamicObject())
    {
        const auto authentication = hello->getProperty("authentication");
        if (auto* auth = authentication.getDynamicObject())
        {
            if (password.isEmpty())
            {
                const juce::ScopedLock lock(stateLock);
                state.lastError = "OBS WebSocket requires its local password";
            }
            else
            {
                identify->setProperty("authentication",
                    OBSAuthentication::create(password, auth->getProperty("salt").toString(),
                                              auth->getProperty("challenge").toString()));
            }
        }
    }

    auto root = juce::DynamicObject::Ptr(new juce::DynamicObject());
    root->setProperty(opKey, 1);
    root->setProperty(dataKey, juce::var(identify.get()));
    webSocket.sendText(juce::JSON::toString(juce::var(root.get()), true));
}

void OBSController::handleIdentified()
{
    {
        const juce::ScopedLock lock(stateLock);
        state.obsConnected = true;
        state.lastError.clear();
    }
    juce::Logger::writeToLog("OBS WebSocket connected locally");
    sendRequest("GetStreamStatus");
    sendRequest("GetRecordStatus");
    sendRequest("GetCurrentProgramScene");
    sendRequest("GetInputList");
}

void OBSController::handleEvent(const juce::var& data)
{
    auto* event = data.getDynamicObject();
    if (event == nullptr) return;
    const auto type = event->getProperty(eventTypeKey).toString();
    const auto eventData = event->getProperty(eventDataKey);
    auto shouldReconfigure = false;
    {
        const juce::ScopedLock lock(stateLock);
        if (type == "StreamStateChanged") state.streamActive = boolProperty(eventData, outputActiveKey);
        else if (type == "RecordStateChanged") state.recordingActive = boolProperty(eventData, outputActiveKey);
        else if (type == "CurrentProgramSceneChanged")
        {
            if (auto* details = eventData.getDynamicObject())
            {
                state.currentScene = details->getProperty("sceneName").toString();
                shouldReconfigure = true;
            }
        }
        else if (type == "InputRemoved" || type == "InputNameChanged")
        {
            if (auto* details = eventData.getDynamicObject())
            {
                const auto removedName = type == "InputRemoved"
                    ? details->getProperty("inputName").toString()
                    : details->getProperty("oldInputName").toString();
                if (removedName == sourceName)
                {
                    sourceExists = false;
                    state.audioSourceConfigured = false;
                    shouldReconfigure = true;
                }
            }
        }
        else if (type == "SceneItemRemoved")
        {
            if (auto* details = eventData.getDynamicObject())
                if (details->getProperty("sceneName").toString() == state.currentScene
                    && details->getProperty("sourceName").toString() == sourceName)
                {
                    state.audioSourceConfigured = false;
                    shouldReconfigure = true;
                }
        }
    }
    if (shouldReconfigure) attemptAudioSourceConfiguration();
}

void OBSController::handleRequestResponse(const juce::var& data)
{
    auto* response = data.getDynamicObject();
    if (response == nullptr) return;
    const auto requestType = response->getProperty(requestTypeKey).toString();
    const auto statusVar = response->getProperty("requestStatus");
    auto* status = statusVar.getDynamicObject();
    if (status == nullptr || !(bool) status->getProperty("result"))
    {
        if (requestType == "GetSceneItemId")
        {
            juce::String scene;
            { const juce::ScopedLock lock(stateLock); scene = state.currentScene; }
            auto create = juce::DynamicObject::Ptr(new juce::DynamicObject());
            create->setProperty("sceneName", scene);
            create->setProperty("sourceName", sourceName);
            create->setProperty("sceneItemEnabled", true);
            sendRequest("CreateSceneItem", create);
            return;
        }
        const juce::ScopedLock lock(stateLock);
        state.lastError = "OBS request failed: " + requestType + " - "
            + (status != nullptr ? status->getProperty("comment").toString() : juce::String("unknown error"));
        return;
    }

    const auto responseData = response->getProperty(responseDataKey);
    if (requestType == "GetStreamStatus")
    {
        const juce::ScopedLock lock(stateLock);
        state.streamActive = boolProperty(responseData, outputActiveKey);
    }
    else if (requestType == "GetRecordStatus")
    {
        const juce::ScopedLock lock(stateLock);
        state.recordingActive = boolProperty(responseData, outputActiveKey);
    }
    else if (requestType == "GetCurrentProgramScene")
    {
        if (auto* details = responseData.getDynamicObject())
        {
            const juce::ScopedLock lock(stateLock);
            state.currentScene = details->getProperty("currentProgramSceneName").toString();
        }
        requestedScene = true;
        attemptAudioSourceConfiguration();
    }
    else if (requestType == "GetInputList")
    {
        if (auto* details = responseData.getDynamicObject())
            if (const auto* inputs = details->getProperty("inputs").getArray())
                for (const auto& input : *inputs)
                    if (auto* item = input.getDynamicObject())
                        sourceExists = sourceExists || item->getProperty("inputName").toString() == sourceName;
        requestedInputs = true;
        attemptAudioSourceConfiguration();
    }
    else if (requestType == "CreateInput" || requestType == "SetInputSettings"
             || requestType == "GetSceneItemId" || requestType == "CreateSceneItem")
    {
        if (requestType == "CreateInput") sourceExists = true;
        const juce::ScopedLock lock(stateLock);
        state.audioSourceConfigured = true;
        state.lastError.clear();
        juce::Logger::writeToLog("OBS audio source configured for Church Stream Processor Output");
    }
}

void OBSController::sendRequest(const juce::String& requestType, juce::DynamicObject::Ptr requestData)
{
    auto data = juce::DynamicObject::Ptr(new juce::DynamicObject());
    data->setProperty(requestTypeKey, requestType);
    data->setProperty("requestId", requestType + "-" + juce::Uuid().toString());
    if (requestData != nullptr) data->setProperty("requestData", juce::var(requestData.get()));
    auto root = juce::DynamicObject::Ptr(new juce::DynamicObject());
    root->setProperty(opKey, 6);
    root->setProperty(dataKey, juce::var(data.get()));
    webSocket.sendText(juce::JSON::toString(juce::var(root.get()), true));
}

void OBSController::attemptAudioSourceConfiguration()
{
    if (!requestedScene || !requestedInputs) return;
    endpointId = WindowsAudioEndpoint::findCaptureEndpointId("Church Stream Processor Output");
    juce::String scene;
    {
        const juce::ScopedLock lock(stateLock);
        scene = state.currentScene;
        if (endpointId.isEmpty())
        {
            state.lastError = "Church Stream Processor Output endpoint is not installed";
            return;
        }
    }

    auto inputSettings = juce::DynamicObject::Ptr(new juce::DynamicObject());
    inputSettings->setProperty("device_id", endpointId);
    auto request = juce::DynamicObject::Ptr(new juce::DynamicObject());
    request->setProperty("inputName", sourceName);
    request->setProperty("inputSettings", juce::var(inputSettings.get()));
    if (sourceExists)
    {
        request->setProperty("overlay", true);
        sendRequest("SetInputSettings", request);
        auto sceneItem = juce::DynamicObject::Ptr(new juce::DynamicObject());
        sceneItem->setProperty("sceneName", scene);
        sceneItem->setProperty("sourceName", sourceName);
        sendRequest("GetSceneItemId", sceneItem);
    }
    else
    {
        request->setProperty("sceneName", scene);
        request->setProperty("inputKind", "wasapi_input_capture");
        request->setProperty("sceneItemEnabled", true);
        sendRequest("CreateInput", request);
    }
}

bool OBSController::boolProperty(const juce::var& object, const juce::Identifier& name)
{
    if (auto* value = object.getDynamicObject()) return static_cast<bool>(value->getProperty(name));
    return false;
}
} // namespace churchstream
