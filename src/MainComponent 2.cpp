#include "MainComponent.h"

namespace churchstream
{
namespace
{
void drawCard(juce::Graphics& graphics, juce::Rectangle<float> bounds)
{
    graphics.setColour(Colours::card);
    graphics.fillRoundedRectangle(bounds, 14.0f);
    graphics.setColour(Colours::cardBorder);
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), 14.0f, 1.0f);
}
}

StatusBadge::StatusBadge(juce::String statusName)
    : name(std::move(statusName))
{
    setInterceptsMouseClicks(false, false);
}

void StatusBadge::setState(bool isOnline, juce::String stateText)
{
    if (online == isOnline && state == stateText)
        return;

    online = isOnline;
    state = std::move(stateText);
    repaint();
}

void StatusBadge::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(Colours::control);
    graphics.fillRoundedRectangle(bounds, 11.0f);
    graphics.setColour(Colours::cardBorder);
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), 11.0f, 1.0f);

    const auto dotColour = online ? Colours::primary : Colours::danger;
    graphics.setColour(dotColour.withAlpha(0.16f));
    graphics.fillEllipse(14.0f, bounds.getCentreY() - 7.0f, 14.0f, 14.0f);
    graphics.setColour(dotColour);
    graphics.fillEllipse(18.0f, bounds.getCentreY() - 3.0f, 6.0f, 6.0f);

    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(10.0f).withStyle("Bold")));
    graphics.drawText(name, 36, 7, getWidth() - 44, 14, juce::Justification::centredLeft);
    graphics.setColour(online ? Colours::text : Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Bold")));
    graphics.drawText(state, 36, 22, getWidth() - 44, 18, juce::Justification::centredLeft);
}

MainComponent::MainComponent()
{
    setLookAndFeel(&theme);
    setOpaque(true);

    configureHeading(titleLabel, 24.0f, Colours::text);
    titleLabel.setText("CHURCH STREAM PROCESSOR", juce::dontSendNotification);
    addAndMakeVisible(titleLabel);

    configureHeading(phaseLabel, 11.0f, Colours::primary);
    phaseLabel.setText("PHASE 1 · REAL-TIME AUDIO I/O", juce::dontSendNotification);
    phaseLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(phaseLabel);

    performanceProfile.addItem("ECO", 1);
    performanceProfile.addItem("BALANCED", 2);
    performanceProfile.addItem("HIGH QUALITY", 3);
    performanceProfile.setSelectedItemIndex(settings.getPerformanceProfile(), juce::dontSendNotification);
    performanceProfile.setTooltip("ECO reduces visual refresh and is the default low-resource mode.");
    addAndMakeVisible(performanceProfile);

    developmentMode.setToggleState(settings.getDevelopmentMode(), juce::dontSendNotification);
    developmentMode.setColour(juce::ToggleButton::textColourId, Colours::mutedText);
    developmentMode.setColour(juce::ToggleButton::tickColourId, Colours::primary);
    developmentMode.setTooltip("Show measured CPU, RAM, callback time and audio xruns.");
    addAndMakeVisible(developmentMode);

    addAndMakeVisible(x32Status);
    addAndMakeVisible(audioStatus);
    addAndMakeVisible(outputStatus);

    configureHeading(routingTitle, 12.0f, Colours::text);
    routingTitle.setText("AUDIO ROUTING", juce::dontSendNotification);
    addAndMakeVisible(routingTitle);

    configureHeading(inputLabel, 10.0f, Colours::mutedText);
    inputLabel.setText("INPUT", juce::dontSendNotification);
    addAndMakeVisible(inputLabel);

    configureHeading(outputLabel, 10.0f, Colours::mutedText);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    addAndMakeVisible(outputLabel);

    inputDevice.setTextWhenNothingSelected("No input device");
    outputDevice.setTextWhenNothingSelected("No output device");
    inputDevice.setTooltip("Windows input device. Select X32 or X-USB channels 1–2.");
    outputDevice.setTooltip("Output device that will receive the unprocessed Phase 1 signal.");
    addAndMakeVisible(inputDevice);
    addAndMakeVisible(outputDevice);

    autoConfigureButton.setTooltip("Find a connected X32 and the preferred Church Stream virtual output.");
    addAndMakeVisible(autoConfigureButton);

    messageLabel.setColour(juce::Label::textColourId, Colours::mutedText);
    messageLabel.setFont(juce::Font(juce::FontOptions(12.0f).withStyle("Regular")));
    messageLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(messageLabel);

    addAndMakeVisible(inputMeter);
    addAndMakeVisible(outputMeter);

    for (auto* label : { &formatLabel, &latencyLabel, &diagnosticsLabel })
    {
        label->setColour(juce::Label::textColourId, Colours::mutedText);
        label->setFont(juce::Font(juce::FontOptions(11.0f).withStyle("Regular")));
        addAndMakeVisible(*label);
    }
    latencyLabel.setJustificationType(juce::Justification::centred);
    diagnosticsLabel.setJustificationType(juce::Justification::centredRight);
    diagnosticsLabel.setVisible(developmentMode.getToggleState());

    inputDevice.onChange = [this] { handleInputSelection(); };
    outputDevice.onChange = [this] { handleOutputSelection(); };
    autoConfigureButton.onClick = [this] { runAutoConfigure(); };
    performanceProfile.onChange = [this]
    {
        settings.setPerformanceProfile(performanceProfile.getSelectedItemIndex());
        settings.flush();
        applyRefreshRate();
    };
    developmentMode.onClick = [this]
    {
        const auto enabled = developmentMode.getToggleState();
        diagnosticsLabel.setVisible(enabled);
        settings.setDevelopmentMode(enabled);
        settings.flush();
        resized();
    };

    audioEngine.getDeviceManager().addChangeListener(this);
    const auto error = audioEngine.initialise();
    juce::Logger::writeToLog("Audio engine initialise: " + (error.isEmpty() ? juce::String("OK") : error));
    refreshDeviceLists();
    showResult(error, "Audio engine ready");
    updateLiveValues();

    setSize(920, 590);
    applyRefreshRate();
}

MainComponent::~MainComponent()
{
    stopTimer();
    audioEngine.getDeviceManager().removeChangeListener(this);
    audioEngine.shutdown();
    setLookAndFeel(nullptr);
}

void MainComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(Colours::background);

    auto bounds = getLocalBounds().toFloat().reduced(24.0f);
    bounds.removeFromTop(65.0f);
    bounds.removeFromTop(78.0f);
    drawCard(graphics, bounds.removeFromTop(176.0f));
    bounds.removeFromTop(14.0f);
    drawCard(graphics, bounds.removeFromTop(194.0f));
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(24);
    auto header = bounds.removeFromTop(58);
    titleLabel.setBounds(header.removeFromLeft(420));
    phaseLabel.setBounds(header.removeFromLeft(165));
    header.removeFromLeft(10);
    performanceProfile.setBounds(header.removeFromLeft(120).reduced(0, 7));
    header.removeFromLeft(8);
    developmentMode.setBounds(header);

    bounds.removeFromTop(7);
    auto statuses = bounds.removeFromTop(64);
    const auto statusWidth = (statuses.getWidth() - 20) / 3;
    x32Status.setBounds(statuses.removeFromLeft(statusWidth));
    statuses.removeFromLeft(10);
    audioStatus.setBounds(statuses.removeFromLeft(statusWidth));
    statuses.removeFromLeft(10);
    outputStatus.setBounds(statuses);

    bounds.removeFromTop(14);
    auto routing = bounds.removeFromTop(176).reduced(18, 13);
    routingTitle.setBounds(routing.removeFromTop(22));
    routing.removeFromTop(4);

    auto inputRow = routing.removeFromTop(43);
    inputLabel.setBounds(inputRow.removeFromLeft(60));
    inputDevice.setBounds(inputRow.removeFromLeft(520));
    inputRow.removeFromLeft(14);
    autoConfigureButton.setBounds(inputRow);

    routing.removeFromTop(8);
    auto outputRow = routing.removeFromTop(43);
    outputLabel.setBounds(outputRow.removeFromLeft(60));
    outputDevice.setBounds(outputRow.removeFromLeft(520));
    messageLabel.setBounds(outputRow.withTrimmedLeft(14));

    bounds.removeFromTop(14);
    auto meters = bounds.removeFromTop(194).reduced(18, 13);
    auto details = meters.removeFromBottom(30);
    const auto meterWidth = (meters.getWidth() - 28) / 2;
    inputMeter.setBounds(meters.removeFromLeft(meterWidth));
    meters.removeFromLeft(28);
    outputMeter.setBounds(meters);

    const auto detailWidth = details.getWidth() / 4;
    formatLabel.setBounds(details.removeFromLeft(detailWidth));
    latencyLabel.setBounds(details.removeFromLeft(detailWidth));
    diagnosticsLabel.setBounds(details);
}

void MainComponent::timerCallback()
{
    const auto minimised = getPeer() != nullptr && getPeer()->isMinimised();
    if (uiSuspended != minimised)
    {
        uiSuspended = minimised;
        applyRefreshRate();
    }

    // No meter polling or repaint is performed while the window is minimised.
    if (!uiSuspended)
        updateLiveValues();

    if (deviceListsDirty)
    {
        deviceListsDirty = false;
        refreshDeviceLists();
    }

    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (nowMs - lastReconnectAttemptMs >= 5000.0)
    {
        lastReconnectAttemptMs = nowMs;
        if (!audioEngine.isAudioRunning())
        {
            juce::Logger::writeToLog("Audio device unavailable; attempting automatic reconnect");
            if (audioEngine.reconnectIfNeeded())
            {
                juce::Logger::writeToLog("Audio device reconnected");
                refreshDeviceLists();
            }
        }
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    deviceListsDirty = true;
}

void MainComponent::refreshDeviceLists()
{
    const auto selectedInput = audioEngine.getCurrentInputName();
    const auto selectedOutput = audioEngine.getCurrentOutputName();
    inputDeviceNames = audioEngine.scanInputDevices();
    outputDeviceNames = audioEngine.scanOutputDevices();

    juce::ScopedValueSetter<bool> suppress(suppressSelectionCallbacks, true);
    inputDevice.clear(juce::dontSendNotification);
    outputDevice.clear(juce::dontSendNotification);

    for (int index = 0; index < inputDeviceNames.size(); ++index)
        inputDevice.addItem(inputDeviceNames[index], index + 1);
    for (int index = 0; index < outputDeviceNames.size(); ++index)
        outputDevice.addItem(outputDeviceNames[index], index + 1);

    inputDevice.setSelectedItemIndex(inputDeviceNames.indexOf(selectedInput), juce::dontSendNotification);
    outputDevice.setSelectedItemIndex(outputDeviceNames.indexOf(selectedOutput), juce::dontSendNotification);

    juce::Logger::writeToLog("Devices: input=[" + inputDeviceNames.joinIntoString(", ")
                             + "] output=[" + outputDeviceNames.joinIntoString(", ") + "]");
}

void MainComponent::handleInputSelection()
{
    if (suppressSelectionCallbacks)
        return;

    const auto index = inputDevice.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(index, inputDeviceNames.size()))
        return;

    const auto name = inputDeviceNames[index];
    const auto error = audioEngine.selectInput(name);
    juce::Logger::writeToLog("Select input '" + name + "': " + (error.isEmpty() ? "OK" : error));
    showResult(error, "Input connected");
}

void MainComponent::handleOutputSelection()
{
    if (suppressSelectionCallbacks)
        return;

    const auto index = outputDevice.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(index, outputDeviceNames.size()))
        return;

    const auto name = outputDeviceNames[index];
    const auto error = audioEngine.selectOutput(name);
    juce::Logger::writeToLog("Select output '" + name + "': " + (error.isEmpty() ? "OK" : error));
    showResult(error, "Output connected");
}

void MainComponent::runAutoConfigure()
{
    const auto error = audioEngine.autoConfigure();
    juce::Logger::writeToLog("Auto configure: " + (error.isEmpty() ? juce::String("OK") : error));
    refreshDeviceLists();
    showResult(error, "X32 route configured");
}

void MainComponent::showResult(const juce::String& error, const juce::String& successMessage)
{
    const auto success = error.isEmpty();
    messageLabel.setColour(juce::Label::textColourId, success ? Colours::primary : Colours::warning);
    messageLabel.setText(success ? successMessage : error, juce::dontSendNotification);
    messageLabel.setTooltip(messageLabel.getText());
}

void MainComponent::updateLiveValues()
{
    auto& input = audioEngine.getInputMeter();
    auto& output = audioEngine.getOutputMeter();
    inputMeter.setLevels(input.getPeak(0), input.getPeak(1), input.getRms(0), input.getRms(1));
    outputMeter.setLevels(output.getPeak(0), output.getPeak(1), output.getRms(0), output.getRms(1));

    const auto running = audioEngine.isAudioRunning();
    x32Status.setState(audioEngine.isX32Connected(),
                       audioEngine.isX32Connected() ? "CONNECTED" : "DISCONNECTED");
    audioStatus.setState(running, running ? "PROCESSING" : "STOPPED");
    outputStatus.setState(running && audioEngine.getCurrentOutputName().isNotEmpty(),
                          running ? "CONNECTED" : "DISCONNECTED");

    const auto sampleRate = audioEngine.getSampleRate();
    formatLabel.setText(sampleRate > 0.0
                            ? juce::String(sampleRate / 1000.0, 1) + " kHz  ·  "
                                  + juce::String(audioEngine.getBufferSize()) + " samples"
                            : "No active audio format",
                        juce::dontSendNotification);
    latencyLabel.setText("I/O latency  " + juce::String(audioEngine.getLatencyMilliseconds(), 1) + " ms",
                         juce::dontSendNotification);
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (developmentMode.getToggleState() && nowMs - lastDiagnosticsSampleMs >= 1000.0)
    {
        lastDiagnosticsSampleMs = nowMs;
        systemCpuPercent = systemMonitor.sampleSystemCpuPercent();
        processMemoryMegabytes = SystemMonitor::getProcessMemoryMegabytes();
    }

    if (developmentMode.getToggleState())
    {
        const auto loadState = systemCpuPercent < 55.0 ? "LOW" : (systemCpuPercent < 80.0 ? "MEDIUM" : "HIGH");
        diagnosticsLabel.setText("SYSTEM " + juce::String(systemCpuPercent, 1) + "% " + loadState
                                     + "  ·  AUDIO " + juce::String(audioEngine.getAudioCpuPercent(), 2) + "%"
                                     + "  ·  DSP " + juce::String(audioEngine.getProcessingTimeMicroseconds(), 1) + " µs"
                                     + "  ·  RAM " + juce::String(processMemoryMegabytes, 1) + " MB"
                                     + "  ·  XRUNS " + juce::String(audioEngine.getXRunCount()),
                                 juce::dontSendNotification);
    }
}

void MainComponent::applyRefreshRate()
{
    startTimerHz(uiSuspended ? 2 : getVisibleRefreshRate());
}

int MainComponent::getVisibleRefreshRate() const
{
    switch (performanceProfile.getSelectedItemIndex())
    {
        case 2: return 30; // High Quality; Phase 1 differs only in future analysis policy.
        case 1: return 30;
        default: return 20; // ECO is the default.
    }
}

void MainComponent::configureHeading(juce::Label& label, float size, juce::Colour colour)
{
    label.setColour(juce::Label::textColourId, colour);
    label.setFont(juce::Font(juce::FontOptions(size).withStyle("Bold")));
}
} // namespace churchstream
