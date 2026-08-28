#include "AudioEngine.h"

namespace churchstream
{
AudioEngine::AudioEngine(AppSettings& settingsToUse)
    : settings(settingsToUse),
      smartEngine(analysisEngine, processingEngine,
                  AppSettings::getDataDirectory().getChildFile("church-profile.json"),
                  settingsToUse.getString("churchName", "Mi Iglesia"))
{
}

AudioEngine::~AudioEngine()
{
    shutdown();
}

juce::String AudioEngine::initialise()
{
    if (initialised)
        return {};

    auto savedState = settings.loadAudioDeviceState();
    auto error = deviceManager.initialise(8, 2, savedState.get(), true);
    deviceManager.addAudioCallback(this);
    smartEngine.start();
    initialised = true;

    // A stale/default Windows device state must never prevent X32 discovery.
    // If X32 is absent, autoConfigure is non-destructive and leaves the
    // already-open fallback device running for diagnostics/offline work.
    if (savedState == nullptr || !isAudioRunning() || !looksLikeX32(getCurrentInputName()))
    {
        const auto autoError = autoConfigure();
        if (error.isEmpty())
            error = autoError;
    }

    {
        const juce::ScopedLock lock(errorLock);
        lastError = error;
    }

    return error;
}

void AudioEngine::shutdown()
{
    if (!initialised)
        return;

    rememberCurrentSetup();
    smartEngine.stop();
    analysisEngine.stop();
    deviceManager.removeAudioCallback(this);
    deviceManager.closeAudioDevice();
    settings.flush();
    initialised = false;
}

juce::StringArray AudioEngine::scanInputDevices()
{
    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
    {
        type->scanForDevices();
        return type->getDeviceNames(true);
    }

    return {};
}

juce::StringArray AudioEngine::scanOutputDevices()
{
    if (auto* type = deviceManager.getCurrentDeviceTypeObject())
    {
        type->scanForDevices();
        return type->getDeviceNames(false);
    }

    return {};
}

juce::String AudioEngine::selectInput(const juce::String& name)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = name;
    setup.useDefaultInputChannels = true;
    return applySetup(setup);
}

juce::String AudioEngine::selectOutput(const juce::String& name)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = name;
    setup.useDefaultOutputChannels = true;
    return applySetup(setup);
}

juce::String AudioEngine::autoConfigure()
{
    const auto& types = deviceManager.getAvailableDeviceTypes();

    const auto preferredInput = settings.getPreferredInput();
    const auto preferredOutput = settings.getPreferredOutput();

    for (auto* type : types)
    {
        type->scanForDevices();
        const auto inputs = type->getDeviceNames(true);
        const auto outputs = type->getDeviceNames(false);

        juce::String input = inputs.contains(preferredInput) && looksLikeX32(preferredInput)
            ? preferredInput : juce::String();
        if (input.isEmpty())
            for (const auto& candidate : inputs)
                if (looksLikeX32(candidate))
                {
                    input = candidate;
                    break;
                }
        if (input.isEmpty())
        {
            continue;
        }

        deviceManager.setCurrentAudioDeviceType(type->getTypeName(), true);
        auto setup = deviceManager.getAudioDeviceSetup();
        setup.inputDeviceName = input;
        setup.useDefaultInputChannels = true;

        juce::String output = outputs.contains(preferredOutput) && looksLikeVirtualOutput(preferredOutput)
            ? preferredOutput : juce::String();
        if (output.isEmpty())
            for (const auto& candidate : outputs)
                if (looksLikeVirtualOutput(candidate))
                {
                    output = candidate;
                    break;
                }
        if (output.isEmpty())
        {
            if (outputs.contains(setup.outputDeviceName))
                output = setup.outputDeviceName;
            else if (!outputs.isEmpty())
                output = outputs[0];
        }

        setup.outputDeviceName = output;
        setup.useDefaultOutputChannels = true;
        return applySetup(setup);
    }

    return juce::String::fromUTF8("No se encontr\xc3\xb3 una entrada Behringer X32. Selecciona el dispositivo manualmente o pulsa AUTO CONFIGURE cuando est\xc3\xa9 conectado.");
}

bool AudioEngine::reconnectIfNeeded()
{
    if (isX32Connected())
        return false;

    const auto error = autoConfigure();
    {
        const juce::ScopedLock lock(errorLock);
        lastError = error;
    }
    return isX32Connected();
}

bool AudioEngine::isAudioRunning()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->isOpen() && device->isPlaying() && callbackActive.load(std::memory_order_acquire);

    return false;
}

bool AudioEngine::isX32Connected()
{
    return isAudioRunning() && looksLikeX32(getCurrentInputName());
}

juce::String AudioEngine::getCurrentInputName() const
{
    return deviceManager.getAudioDeviceSetup().inputDeviceName;
}

juce::String AudioEngine::getCurrentOutputName() const
{
    return deviceManager.getAudioDeviceSetup().outputDeviceName;
}

double AudioEngine::getSampleRate()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();
    return 0.0;
}

int AudioEngine::getBufferSize()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();
    return 0;
}

double AudioEngine::getLatencyMilliseconds()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto sampleRate = device->getCurrentSampleRate();
        if (sampleRate > 0.0)
            return 1000.0 * static_cast<double>(device->getInputLatencyInSamples()
                                                + device->getOutputLatencyInSamples()
                                                + processingEngine.getLatencySamples()) / sampleRate;
    }
    return 0.0;
}

double AudioEngine::getAudioCpuPercent() const
{
    return deviceManager.getCpuUsage() * 100.0;
}

double AudioEngine::getProcessingTimeMicroseconds() const noexcept
{
    return processingTimeMicroseconds.load(std::memory_order_acquire);
}

int AudioEngine::getXRunCount() const noexcept
{
    return deviceManager.getXRunCount();
}

juce::String AudioEngine::getLastError() const
{
    const juce::ScopedLock lock(errorLock);
    return lastError;
}

uint64_t AudioEngine::getCallbackCount() const noexcept
{
    return callbackCount.load(std::memory_order_acquire);
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&)
{
    const auto count = callbackCount.load(std::memory_order_relaxed);
    const auto measureThisBlock = (count & 31U) == 0U;
    const auto startTicks = measureThisBlock ? juce::Time::getHighResolutionTicks() : 0;

    inputMeter.push(inputChannelData, numInputChannels, numSamples);
    autoGroupRouter.process(inputChannelData, numInputChannels, numSamples);
    GroupRoutingConfig groupRoutes;
    // Any failure to resolve or use the three stems falls straight back to the
    // plain stereo path. A service must never go silent because the group
    // detection was unsure.
    const auto mixedGroups = autoGroupRouter.getResolvedRoutes(groupRoutes)
        && groupMixer.process(inputChannelData, numInputChannels,
                              outputChannelData, numOutputChannels, numSamples, groupRoutes);
    if (!mixedGroups)
        AudioRouting::passthrough(inputChannelData, numInputChannels,
                                  outputChannelData, numOutputChannels, numSamples);
    processingEngine.process(outputChannelData, numOutputChannels, numSamples);

    const float* meterOutputs[MeterSource::channelCount] { nullptr, nullptr };
    for (int channel = 0; channel < juce::jmin(numOutputChannels, MeterSource::channelCount); ++channel)
        meterOutputs[channel] = outputChannelData[channel];
    outputMeter.push(meterOutputs, juce::jmin(numOutputChannels, MeterSource::channelCount), numSamples);
    analysisEngine.push(inputChannelData, numInputChannels,
                        meterOutputs, juce::jmin(numOutputChannels, MeterSource::channelCount),
                        numSamples);

    if (measureThisBlock)
    {
        const auto elapsedTicks = juce::Time::getHighResolutionTicks() - startTicks;
        processingTimeMicroseconds.store(
            juce::Time::highResolutionTicksToSeconds(elapsedTicks) * 1.0e6,
            std::memory_order_release);
    }
    callbackCount.fetch_add(1, std::memory_order_relaxed);
}

void AudioEngine::startX32Link(const juce::String& hostName)
{
    x32Client.start(hostName);
}

void AudioEngine::stopX32Link()
{
    x32Client.stop();
}

void AudioEngine::updateGroupNamingHints()
{
    const auto console = x32Client.getState();
    if (!console.connected) return;

    for (int pair = 0; pair < AutoGroupRouter::maxCandidatePairs; ++pair)
    {
        const auto leftIndex = static_cast<size_t>(pair * 2);
        if (leftIndex + 1 >= console.channels.size()) break;
        const auto& left = console.channels[leftIndex];
        const auto& right = console.channels[leftIndex + 1];
        auto name = left.name;
        if (right.name.isNotEmpty() && right.name != left.name)
            name += " " + right.name;
        // Names are a hint only. The router still needs the audio itself to
        // agree before it changes anything.
        if (name.isNotEmpty())
            autoGroupRouter.setCandidateName(pair, name.toStdString());
    }
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    inputMeter.reset();
    outputMeter.reset();
    processingTimeMicroseconds.store(0.0, std::memory_order_release);
    callbackActive.store(device != nullptr, std::memory_order_release);

    if (device != nullptr)
    {
        processingEngine.prepare(device->getCurrentSampleRate(),
                                 device->getCurrentBufferSizeSamples(),
                                 2);
        analysisEngine.prepare(device->getCurrentSampleRate());
        autoGroupRouter.prepare(device->getCurrentSampleRate());
        groupMixer.prepare(device->getCurrentSampleRate());
    }

    const juce::ScopedLock lock(errorLock);
    lastError.clear();
}

void AudioEngine::audioDeviceStopped()
{
    callbackActive.store(false, std::memory_order_release);
    inputMeter.reset();
    outputMeter.reset();
    processingEngine.reset();
    analysisEngine.stop();
    autoGroupRouter.reset();
}

void AudioEngine::audioDeviceError(const juce::String& errorMessage)
{
    callbackActive.store(false, std::memory_order_release);
    const juce::ScopedLock lock(errorLock);
    lastError = errorMessage;
}

bool AudioEngine::looksLikeX32(const juce::String& name)
{
    const auto lower = name.toLowerCase();
    return lower.contains("x32") || lower.contains("x-usb") || lower.contains("x-live")
        || (lower.contains("behringer") && lower.contains("usb"));
}

bool AudioEngine::looksLikeVirtualOutput(const juce::String& name)
{
    const auto lower = name.toLowerCase();
    return lower.contains("church stream processor output")
        || lower.contains("church stream virtual");
}

void AudioEngine::rememberCurrentSetup()
{
    auto state = deviceManager.createStateXml();
    settings.saveAudioDeviceState(state.get());
    settings.setPreferredDevices(getCurrentInputName(), getCurrentOutputName());
}

juce::String AudioEngine::applySetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup)
{
    const auto error = deviceManager.setAudioDeviceSetup(setup, true);
    {
        const juce::ScopedLock lock(errorLock);
        lastError = error;
    }

    if (error.isEmpty())
        rememberCurrentSetup();

    return error;
}
} // namespace churchstream
