#include "MainComponent.h"

#include <algorithm>
#include <cmath>

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

juce::String percentText(float value)
{
    return juce::String(static_cast<int>(std::round(value * 100.0f))) + "%";
}
}

StatusBadge::StatusBadge(juce::String statusName) : name(std::move(statusName))
{
    setInterceptsMouseClicks(false, false);
}

void StatusBadge::setState(bool isOnline, juce::String stateText, bool warning)
{
    if (online == isOnline && state == stateText && warningState == warning) return;
    online = isOnline;
    state = std::move(stateText);
    warningState = warning;
    repaint();
}

void StatusBadge::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    graphics.setColour(Colours::control);
    graphics.fillRoundedRectangle(bounds, 11.0f);
    graphics.setColour(Colours::cardBorder);
    graphics.drawRoundedRectangle(bounds.reduced(0.5f), 11.0f, 1.0f);
    const auto dotColour = warningState ? Colours::warning : (online ? Colours::primary : Colours::danger);
    graphics.setColour(dotColour.withAlpha(0.16f));
    graphics.fillEllipse(14.0f, bounds.getCentreY() - 7.0f, 14.0f, 14.0f);
    graphics.setColour(dotColour);
    graphics.fillEllipse(18.0f, bounds.getCentreY() - 3.0f, 6.0f, 6.0f);
    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(9.5f).withStyle("Bold")));
    graphics.drawText(name, 36, 6, getWidth() - 44, 13, juce::Justification::centredLeft);
    graphics.setColour(online ? Colours::text : Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(11.5f).withStyle("Bold")));
    graphics.drawText(state, 36, 20, getWidth() - 44, 18, juce::Justification::centredLeft);
}

MainComponent::MainComponent(bool startAudio)
{
    setLookAndFeel(&theme);
    setOpaque(true);

    configureHeading(titleLabel, 23.0f, Colours::text);
    titleLabel.setText("CHURCH STREAM PROCESSOR", juce::dontSendNotification);
    addAndMakeVisible(titleLabel);
    configureHeading(phaseLabel, 10.0f, Colours::primary);
    phaseLabel.setText("LOCAL | NATIVE | LIVE SAFE", juce::dontSendNotification);
    phaseLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(phaseLabel);

    performanceProfile.addItem("ECO", 1);
    performanceProfile.addItem("BALANCED", 2);
    performanceProfile.addItem("HIGH QUALITY", 3);
    performanceProfile.setSelectedItemIndex(settings.getPerformanceProfile(), juce::dontSendNotification);
    performanceProfile.setTooltip("ECO is the default and reduces UI/analysis work.");
    addAndMakeVisible(performanceProfile);
    liveMode.setToggleState(settings.getNumber("liveMode", 1.0) > 0.5, juce::dontSendNotification);
    liveMode.setColour(juce::ToggleButton::textColourId, Colours::primary);
    liveMode.setColour(juce::ToggleButton::tickColourId, Colours::primary);
    liveMode.setTooltip("Prevents audio-device restarts while OBS is live; DSP controls remain active.");
    addAndMakeVisible(liveMode);
    developmentMode.setToggleState(settings.getDevelopmentMode(), juce::dontSendNotification);
    developmentMode.setColour(juce::ToggleButton::textColourId, Colours::mutedText);
    developmentMode.setColour(juce::ToggleButton::tickColourId, Colours::primary);
    addAndMakeVisible(developmentMode);
    addAndMakeVisible(advancedButton);

    for (auto* badge : { &x32Status, &audioStatus, &obsStatus, &streamStatus }) addAndMakeVisible(*badge);
    addAndMakeVisible(inputMeter);
    addAndMakeVisible(outputMeter);
    configureHeading(spectrumTitle, 11.0f, Colours::mutedText);
    spectrumTitle.setText("REAL-TIME SPECTRUM", juce::dontSendNotification);
    addAndMakeVisible(spectrumTitle);
    addAndMakeVisible(spectrum);

    configureHeading(smartTitle, 12.0f, Colours::text);
    smartTitle.setText("SMART ENGINE", juce::dontSendNotification);
    addAndMakeVisible(smartTitle);
    smartProcessing.setColour(juce::ToggleButton::textColourId, Colours::text);
    smartProcessing.setColour(juce::ToggleButton::tickColourId, Colours::primary);
    addAndMakeVisible(smartProcessing);
    operatingMode.addItem("AUTO", 1);
    operatingMode.addItem("SAFE", 2);
    operatingMode.addItem("MANUAL", 3);
    addAndMakeVisible(operatingMode);
    presetSelector.setTextWhenNothingSelected("PRESET");
    addAndMakeVisible(presetSelector);
    addAndMakeVisible(savePresetButton);
    refreshUserPresets();

    const std::array<juce::String, 6> controlNames { "CLEAN", "PUNCH", "CLARITY", "DYNAMICS", "WARMTH", "LOUDNESS TARGET" };
    std::array<juce::Slider*, 6> sliders { &cleanSlider, &punchSlider, &claritySlider,
                                           &dynamicsSlider, &warmthSlider, &loudnessSlider };
    for (size_t index = 0; index < sliders.size(); ++index)
    {
        configureHeading(controlLabels[index], 9.5f, Colours::mutedText);
        controlLabels[index].setText(controlNames[index], juce::dontSendNotification);
        addAndMakeVisible(controlLabels[index]);
        configureControlSlider(*sliders[index], index == 5 ? " LUFS" : " %");
        addAndMakeVisible(*sliders[index]);
    }
    loudnessSlider.setRange(-18.0, -10.0, 0.1);
    for (auto* button : { &autoTuneButton, &abButton, &bypassButton }) addAndMakeVisible(*button);
    bypassButton.setColour(juce::TextButton::buttonColourId, Colours::danger.withAlpha(0.85f));
    abButton.setClickingTogglesState(true);
    bypassButton.setClickingTogglesState(true);

    configureHeading(measurementsTitle, 12.0f, Colours::text);
    measurementsTitle.setText("MEASUREMENTS", juce::dontSendNotification);
    addAndMakeVisible(measurementsTitle);
    const std::array<juce::String, 8> metricTitles { "LUFS INTEGRATED", "LUFS SHORT TERM", "LUFS MOMENTARY", "TRUE PEAK",
                                                     "RMS", "PEAK", "GAIN REDUCTION", "STEREO CORR" };
    for (size_t index = 0; index < metricNames.size(); ++index)
    {
        configureHeading(metricNames[index], 8.5f, Colours::mutedText);
        metricNames[index].setText(metricTitles[index], juce::dontSendNotification);
        addAndMakeVisible(metricNames[index]);
        configureMetricLabel(metricValues[index]);
        addAndMakeVisible(metricValues[index]);
    }
    configureHeading(actionsTitle, 10.0f, Colours::primary);
    actionsTitle.setText("WHAT I'M DOING", juce::dontSendNotification);
    addAndMakeVisible(actionsTitle);
    actionsLabel.setColour(juce::Label::textColourId, Colours::mutedText);
    actionsLabel.setFont(juce::Font(juce::FontOptions(10.5f).withStyle("Regular")));
    actionsLabel.setJustificationType(juce::Justification::topLeft);
    actionsLabel.setText("Monitoring real audio. No confident correction required.", juce::dontSendNotification);
    addAndMakeVisible(actionsLabel);

    configureHeading(routingTitle, 11.0f, Colours::text);
    routingTitle.setText("AUDIO ROUTING", juce::dontSendNotification);
    addAndMakeVisible(routingTitle);
    configureHeading(inputLabel, 9.5f, Colours::mutedText);
    inputLabel.setText("INPUT", juce::dontSendNotification);
    addAndMakeVisible(inputLabel);
    configureHeading(outputLabel, 9.5f, Colours::mutedText);
    outputLabel.setText("OUTPUT", juce::dontSendNotification);
    addAndMakeVisible(outputLabel);
    inputDevice.setTextWhenNothingSelected("No input device");
    outputDevice.setTextWhenNothingSelected("No output device");
    addAndMakeVisible(inputDevice);
    addAndMakeVisible(outputDevice);
    addAndMakeVisible(autoConfigureButton);
    addAndMakeVisible(openObsButton);
    obsPasswordEditor.setText(settings.getObsPassword(), false);
    obsPasswordEditor.setPasswordCharacter(0x2022);
    obsPasswordEditor.setTextToShowWhenEmpty("OBS LOCAL PASSWORD", Colours::mutedText);
    obsPasswordEditor.setTooltip("Stored only on this PC. Leave empty when OBS WebSocket authentication is disabled.");
    addAndMakeVisible(obsPasswordEditor);
    addAndMakeVisible(connectObsButton);
    addAndMakeVisible(offlineTestButton);
    churchNameEditor.setText(settings.getString("churchName", "Mi Iglesia"), false);
    churchNameEditor.setTextToShowWhenEmpty("CHURCH PROFILE NAME", Colours::mutedText);
    churchNameEditor.setTooltip("Persistent local learning profile; no audio is stored.");
    addAndMakeVisible(churchNameEditor);
    startWithWindows.setToggleState(StartupManager::isEnabled(), juce::dontSendNotification);
    startMinimized.setToggleState(settings.getNumber("startMinimized", 0.0) > 0.5, juce::dontSendNotification);
    for (auto* toggle : { &startWithWindows, &startMinimized })
    {
        toggle->setColour(juce::ToggleButton::textColourId, Colours::mutedText);
        toggle->setColour(juce::ToggleButton::tickColourId, Colours::primary);
        addAndMakeVisible(*toggle);
    }
#if ! JUCE_WINDOWS
    startWithWindows.setEnabled(false);
    startMinimized.setEnabled(false);
#endif
    messageLabel.setColour(juce::Label::textColourId, Colours::mutedText);
    messageLabel.setFont(juce::Font(juce::FontOptions(10.5f).withStyle("Regular")));
    addAndMakeVisible(messageLabel);
    for (auto* label : { &formatLabel, &latencyLabel, &diagnosticsLabel })
    {
        label->setColour(juce::Label::textColourId, Colours::mutedText);
        label->setFont(juce::Font(juce::FontOptions(9.5f).withStyle("Regular")));
        addAndMakeVisible(*label);
    }
    latencyLabel.setJustificationType(juce::Justification::centred);
    diagnosticsLabel.setJustificationType(juce::Justification::centredRight);
    diagnosticsLabel.setVisible(developmentMode.getToggleState());

    updateDspControls();
    inputDevice.onChange = [this] { handleInputSelection(); };
    outputDevice.onChange = [this] { handleOutputSelection(); };
    autoConfigureButton.onClick = [this] { runAutoConfigure(); };
    openObsButton.onClick = [this] { obsController.openOBS(); };
    connectObsButton.onClick = [this]
    {
        obsController.setPassword(obsPasswordEditor.getText());
        showResult({}, "Connecting to OBS locally...");
    };
    obsPasswordEditor.onReturnKey = [this] { connectObsButton.triggerClick(); };
    const auto saveChurchName = [this]
    {
        auto name = churchNameEditor.getText().trim();
        if (name.isEmpty()) name = "Mi Iglesia";
        churchNameEditor.setText(name, false);
        settings.setString("churchName", name);
        settings.flush();
        audioEngine.getSmartEngine().setChurchName(name);
    };
    churchNameEditor.onReturnKey = saveChurchName;
    churchNameEditor.onFocusLost = saveChurchName;
    offlineTestButton.onClick = [this]
    {
        offlineFileChooser = std::make_unique<juce::FileChooser>("Select WAV or FLAC for offline test",
                                                                 juce::File(), "*.wav;*.flac;*.aif;*.aiff");
        offlineFileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectFiles,
                                        [this](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file.existsAsFile())
                offlineProcessor.startProcessing(file, audioEngine.getProcessingEngine().getParameters(),
                                                 AppSettings::getDataDirectory().getChildFile("church-profile.json"));
        });
    };
    startWithWindows.onClick = [this]
    {
        const auto enabled = startWithWindows.getToggleState();
        if (!StartupManager::setEnabled(enabled, startMinimized.getToggleState()))
            startWithWindows.setToggleState(StartupManager::isEnabled(), juce::dontSendNotification);
    };
    startMinimized.onClick = [this]
    {
        settings.setNumber("startMinimized", startMinimized.getToggleState() ? 1.0 : 0.0);
        if (startWithWindows.getToggleState())
            StartupManager::setEnabled(true, startMinimized.getToggleState());
    };
    performanceProfile.onChange = [this]
    {
        settings.setPerformanceProfile(performanceProfile.getSelectedItemIndex());
        settings.flush();
        applyRefreshRate();
    };
    developmentMode.onClick = [this]
    {
        diagnosticsLabel.setVisible(developmentMode.getToggleState());
        settings.setDevelopmentMode(developmentMode.getToggleState());
        settings.flush();
    };
    liveMode.onClick = [this]
    {
        settings.setNumber("liveMode", liveMode.getToggleState() ? 1.0 : 0.0);
        settings.flush();
    };
    advancedButton.onClick = [this]
    {
        auto& parameters = audioEngine.getProcessingEngine().getParameters();
        const auto protectLimiter = liveRoutingLocked();
        juce::PopupMenu menu;
        menu.addSectionHeader("DSP MODULES");
        menu.addItem(1, "Rumble control", true, parameters.rumbleEnabled.load());
        menu.addItem(2, "Adaptive / dynamic EQ", true, parameters.adaptiveEqEnabled.load());
        menu.addItem(3, "4-band dynamics", true, parameters.compressorEnabled.load());
        menu.addItem(4, "Harmonic enhancer", true, parameters.saturationEnabled.load());
        menu.addItem(5, protectLimiter ? "True-peak limiter (LIVE SAFE)" : "True-peak limiter",
                     !protectLimiter, parameters.limiterEnabled.load());
        juce::PopupMenu scenes;
        const auto selectedScene = static_cast<SmartScene>(static_cast<int>(settings.getNumber("smartScene", 0.0)));
        for (int scene = 0; scene <= static_cast<int>(SmartScene::ambience); ++scene)
            scenes.addItem(100 + scene, sceneName(static_cast<SmartScene>(scene)), true,
                           selectedScene == static_cast<SmartScene>(scene));
        menu.addSubMenu("SMART SCENE", scenes);
        menu.addSeparator();
        menu.addSectionHeader("X32 GROUPS");
        const auto routeSnapshot = audioEngine.getAutoRouteSnapshot();
        const auto groupsReady = routeSnapshot.phase == AutoRoutePhase::ready;
        menu.addItem(200, groupsReady ? "Smart Masking on separate groups"
                                      : "Smart Masking (waiting for separate groups)",
                     true, audioEngine.isSmartMaskingEnabled());
        menu.addSectionHeader("CONSOLE, READ ONLY");
        const auto console = audioEngine.getX32State();
        menu.addItem(201, console.connected
                              ? "X32 link connected: " + console.model + " " + console.firmware
                              : juce::String("X32 read-only link"),
                     true, settings.getNumber("x32LinkEnabled", 0.0) > 0.5);
        menu.addItem(202, "Set X32 IP address...");
        menu.addSectionHeader("ROOM");
        menu.addItem(203, "Analyse a measurement recording...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(advancedButton),
                           [this](int selected)
        {
            auto& p = audioEngine.getProcessingEngine().getParameters();
            const auto toggle = [this](std::atomic<bool>& value, const char* key)
            {
                const auto enabled = !value.load(std::memory_order_relaxed);
                value.store(enabled, std::memory_order_release);
                settings.setNumber(key, enabled ? 1.0 : 0.0);
                settings.flush();
            };
            if (selected == 1) toggle(p.rumbleEnabled, "rumbleEnabled");
            else if (selected == 2) toggle(p.adaptiveEqEnabled, "adaptiveEqEnabled");
            else if (selected == 3) toggle(p.compressorEnabled, "compressorEnabled");
            else if (selected == 4) toggle(p.saturationEnabled, "saturationEnabled");
            else if (selected == 5 && !liveRoutingLocked()) toggle(p.limiterEnabled, "limiterEnabled");
            else if (selected == 200)
            {
                const auto enabled = !audioEngine.isSmartMaskingEnabled();
                audioEngine.setSmartMaskingEnabled(enabled);
                settings.setNumber("smartMaskingEnabled", enabled ? 1.0 : 0.0);
                settings.flush();
                showResult({}, enabled ? "Smart Masking enabled; it only acts once the X32 groups resolve"
                                       : "Smart Masking disabled");
            }
            else if (selected == 201)
            {
                const auto enabled = settings.getNumber("x32LinkEnabled", 0.0) <= 0.5;
                settings.setNumber("x32LinkEnabled", enabled ? 1.0 : 0.0);
                settings.flush();
                applyConsoleAndGroupSettings();
            }
            else if (selected == 202) askForConsoleAddress();
            else if (selected == 203) chooseRoomMeasurement();
            else if (selected >= 100 && selected <= 100 + static_cast<int>(SmartScene::ambience))
            {
                const auto scene = static_cast<SmartScene>(selected - 100);
                audioEngine.getSmartEngine().setScene(scene);
                settings.setNumber("smartScene", static_cast<int>(scene));
                settings.flush();
            }
        });
    };
    savePresetButton.onClick = [this]
    {
        const auto suggested = presetManager.getDirectory().getChildFile(
            "Custom-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M") + ".cspreset");
        presetFileChooser = std::make_unique<juce::FileChooser>("Save local preset", suggested, "*.cspreset");
        presetFileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                           | juce::FileBrowserComponent::canSelectFiles
                                           | juce::FileBrowserComponent::warnAboutOverwriting,
                                       [this](const juce::FileChooser& chooser)
        {
            const auto chosen = chooser.getResult();
            if (chosen == juce::File()) return;
            const auto result = presetManager.save(chosen.getFileNameWithoutExtension(),
                                                   audioEngine.getProcessingEngine().getParameters());
            showResult(result.failed() ? result.getErrorMessage() : juce::String(),
                       "Preset saved locally");
            if (result.wasOk()) refreshUserPresets();
        });
    };

    audioEngine.getDeviceManager().addChangeListener(this);
    const auto error = startAudio ? audioEngine.initialise()
                                  : juce::String("Audio startup skipped by --no-audio");
    juce::Logger::writeToLog("Audio engine initialise: " + (error.isEmpty() ? juce::String("OK") : error));
    refreshDeviceLists();
    showResult(error, "Audio engine ready");
    obsController.start();
    audioEngine.getSmartEngine().setScene(static_cast<SmartScene>(static_cast<int>(settings.getNumber("smartScene", 0.0))));
    applyConsoleAndGroupSettings();
    updateLiveValues();
    setSize(1180, 980);
    applyRefreshRate();
}

MainComponent::~MainComponent()
{
    stopTimer();
    offlineProcessor.stop();
    roomCalibration.stop();
    audioEngine.stopX32Link();
    obsController.stop();
    audioEngine.getDeviceManager().removeChangeListener(this);
    audioEngine.shutdown();
    sessionDiagnostics.finish();
    settings.flush();
    setLookAndFeel(nullptr);
}

void MainComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(Colours::background);
    for (const auto& card : cardBounds) drawCard(graphics, card.toFloat());
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(20);
    auto header = bounds.removeFromTop(44);
    titleLabel.setBounds(header.removeFromLeft(345));
    phaseLabel.setBounds(header.removeFromLeft(145));
    header.removeFromLeft(10);
    performanceProfile.setBounds(header.removeFromLeft(110).reduced(0, 4));
    header.removeFromLeft(6);
    liveMode.setBounds(header.removeFromLeft(100));
    header.removeFromLeft(6);
    developmentMode.setBounds(header.removeFromLeft(112));
    header.removeFromLeft(6);
    advancedButton.setBounds(header.removeFromLeft(90).reduced(0, 4));

    bounds.removeFromTop(8);
    auto statuses = bounds.removeFromTop(52);
    const auto badgeWidth = (statuses.getWidth() - 30) / 4;
    for (auto* badge : { &x32Status, &audioStatus, &obsStatus, &streamStatus })
    {
        badge->setBounds(statuses.removeFromLeft(badgeWidth));
        statuses.removeFromLeft(10);
    }

    bounds.removeFromTop(10);
    cardBounds[0] = bounds.removeFromTop(122);
    auto meters = cardBounds[0].reduced(18, 12);
    const auto meterWidth = (meters.getWidth() - 28) / 2;
    inputMeter.setBounds(meters.removeFromLeft(meterWidth));
    meters.removeFromLeft(28);
    outputMeter.setBounds(meters);

    bounds.removeFromTop(10);
    cardBounds[1] = bounds.removeFromTop(148);
    auto spectrumArea = cardBounds[1].reduced(18, 10);
    spectrumTitle.setBounds(spectrumArea.removeFromTop(18));
    spectrum.setBounds(spectrumArea);

    bounds.removeFromTop(10);
    auto central = bounds.removeFromTop(std::max(300, bounds.getHeight() - 168));
    cardBounds[2] = central.removeFromLeft(static_cast<int>(static_cast<float>(central.getWidth()) * 0.57f));
    central.removeFromLeft(10);
    cardBounds[3] = central;

    auto smart = cardBounds[2].reduced(18, 12);
    auto smartHeader = smart.removeFromTop(35);
    smartTitle.setBounds(smartHeader.removeFromLeft(115));
    smartProcessing.setBounds(smartHeader.removeFromLeft(155));
    smartHeader.removeFromLeft(8);
    operatingMode.setBounds(smartHeader.removeFromLeft(105));
    smartHeader.removeFromLeft(8);
    savePresetButton.setBounds(smartHeader.removeFromRight(62));
    smartHeader.removeFromRight(6);
    presetSelector.setBounds(smartHeader);
    smart.removeFromTop(4);
    std::array<juce::Slider*, 6> sliders { &cleanSlider, &punchSlider, &claritySlider, &dynamicsSlider, &warmthSlider, &loudnessSlider };
    for (size_t index = 0; index < sliders.size(); ++index)
    {
        auto row = smart.removeFromTop(34);
        controlLabels[index].setBounds(row.removeFromLeft(118));
        sliders[index]->setBounds(row);
    }
    smart.removeFromTop(5);
    auto buttons = smart.removeFromTop(38);
    const auto buttonWidth = (buttons.getWidth() - 16) / 3;
    autoTuneButton.setBounds(buttons.removeFromLeft(buttonWidth));
    buttons.removeFromLeft(8);
    abButton.setBounds(buttons.removeFromLeft(buttonWidth));
    buttons.removeFromLeft(8);
    bypassButton.setBounds(buttons);

    auto measurements = cardBounds[3].reduced(18, 12);
    measurementsTitle.setBounds(measurements.removeFromTop(24));
    auto metricArea = measurements.removeFromTop(122);
    auto metrics = metricArea.removeFromTop(61);
    const auto metricWidth = metrics.getWidth() / 4;
    for (size_t index = 0; index < metricNames.size(); ++index)
    {
        if (index == 4) metrics = metricArea;
        auto metric = metrics.removeFromLeft(metricWidth);
        metricNames[index].setBounds(metric.removeFromTop(18));
        metricValues[index].setBounds(metric);
    }
    measurements.removeFromTop(4);
    actionsTitle.setBounds(measurements.removeFromTop(22));
    actionsLabel.setBounds(measurements);

    bounds.removeFromTop(10);
    cardBounds[4] = bounds;
    auto routing = cardBounds[4].reduced(18, 10);
    auto routingHeader = routing.removeFromTop(22);
    routingTitle.setBounds(routingHeader.removeFromLeft(150));
    formatLabel.setBounds(routingHeader.removeFromLeft(220));
    latencyLabel.setBounds(routingHeader.removeFromLeft(160));
    diagnosticsLabel.setBounds(routingHeader);
    auto message = routing.removeFromBottom(26);
    messageLabel.setBounds(message);

    // Three bounded columns keep every routing/OBS control usable at the
    // minimum 1080 px window width and at common Windows DPI scales.
    auto toggles = routing.removeFromRight(190);
    routing.removeFromRight(10);
    auto actions = routing.removeFromRight(390);
    routing.removeFromRight(12);
    startWithWindows.setBounds(toggles.removeFromTop(28));
    startMinimized.setBounds(toggles.removeFromTop(28));
    churchNameEditor.setBounds(toggles.removeFromTop(34).reduced(0, 3));
    auto inputRow = routing.removeFromTop(42);
    inputLabel.setBounds(inputRow.removeFromLeft(58));
    inputDevice.setBounds(inputRow);
    auto outputRow = routing.removeFromTop(42);
    outputLabel.setBounds(outputRow.removeFromLeft(58));
    outputDevice.setBounds(outputRow);

    auto actionRow = actions.removeFromTop(42);
    autoConfigureButton.setBounds(actionRow.removeFromLeft(145));
    actionRow.removeFromLeft(8);
    openObsButton.setBounds(actionRow.removeFromLeft(105));
    actionRow.removeFromLeft(8);
    offlineTestButton.setBounds(actionRow);
    auto obsRow = actions.removeFromTop(42);
    obsPasswordEditor.setBounds(obsRow.removeFromLeft(215).reduced(0, 4));
    obsRow.removeFromLeft(8);
    connectObsButton.setBounds(obsRow.reduced(0, 3));
}

void MainComponent::setBypassed(bool shouldBypass)
{
    bypassButton.setToggleState(shouldBypass, juce::sendNotification);
}

void MainComponent::refreshUserPresets()
{
    presetSelector.clear(juce::dontSendNotification);
    for (const auto* name : { "Church Live", "Worship", "Acoustic", "Band", "Speech + Music", "Clean Stream", "Manual" })
        presetSelector.addItem(name, presetSelector.getNumItems() + 1);
    userPresetFiles = presetManager.findUserPresets();
    for (int index = 0; index < userPresetFiles.size(); ++index)
        presetSelector.addItem("USER | " + userPresetFiles[index].getFileNameWithoutExtension(), 1000 + index);
}

void MainComponent::reconnectOBS()
{
    obsController.reconnect();
}

void MainComponent::timerCallback()
{
    const auto visuallySuspended = !isShowing() || (getPeer() != nullptr && getPeer()->isMinimised());
    if (uiSuspended != visuallySuspended) { uiSuspended = visuallySuspended; applyRefreshRate(); }

    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastDiagnosticsSampleMs >= 1000.0)
    {
        const auto elapsedSafety = lastSafetySampleMs > 0.0
            ? static_cast<float>((now - lastSafetySampleMs) * 0.001) : 1.0f;
        lastSafetySampleMs = now;
        lastDiagnosticsSampleMs = now;
        systemCpuPercent = systemMonitor.sampleSystemCpuPercent();
        processCpuPercent = systemMonitor.sampleProcessCpuPercent();
        if (developmentMode.getToggleState()) processMemoryMegabytes = SystemMonitor::getProcessMemoryMegabytes();
        const auto analysis = audioEngine.getAnalysisEngine().getSnapshot();
        const auto obs = obsController.getState();
        auto& dsp = audioEngine.getProcessingEngine().getMetrics();
        SafetyInput safetyInput;
        safetyInput.audioRunning = audioEngine.isAudioRunning();
        safetyInput.x32Connected = audioEngine.isX32Connected();
        safetyInput.obsConnected = obs.obsConnected;
        safetyInput.obsAudioConfigured = obs.audioSourceConfigured;
        safetyInput.streamActive = obs.streamActive;
        safetyInput.systemCpuPercent = systemCpuPercent;
        safetyInput.xruns = audioEngine.getXRunCount();
        safetyInput.analysisDrops = analysis.droppedOutputSamples;
        safetyInput.compressorReductionDb = dsp.compressorGainReductionDb.load(std::memory_order_relaxed);
        safetyInput.limiterReductionDb = dsp.limiterGainReductionDb.load(std::memory_order_relaxed);
        safetyInput.analysis = analysis;
        const auto safety = safetyController.evaluate(safetyInput, elapsedSafety);
        if (safety.forceLimiter)
            audioEngine.getProcessingEngine().getParameters().limiterEnabled.store(true, std::memory_order_release);
        if (safety.requestSmartRollback)
            audioEngine.getSmartEngine().requestSafetyRollback();
        if (safety.requestX32Reconnect)
            [[maybe_unused]] const auto reconnected = audioEngine.reconnectIfNeeded();
        if (safety.requestObsReconnect && now - lastSafetyObsReconnectMs >= 10000.0)
        {
            lastSafetyObsReconnectMs = now;
            obsController.reconnect();
        }
        const auto protect = systemCpuPercent >= 80.0 || safety.reduceSecondaryWork;
        if (protect != highLoadProtection) { highLoadProtection = protect; applyRefreshRate(); }
        const auto smartState = audioEngine.getSmartEngine().getState();
        const auto stableNow = smartState.quality.overall >= 92.0f && smartState.actionCount == 0
            && safety.healthy && analysis.processed.rmsDb > -60.0f;
        stableMixSeconds = stableNow ? stableMixSeconds + elapsedSafety : 0.0f;
        const auto useStableEco = stableMixSeconds >= 10.0f;
        if (useStableEco != stableMixEco) { stableMixEco = useStableEco; applyRefreshRate(); }
        sessionDiagnostics.observe(analysis, smartState, safety,
                                   processCpuPercent, obs.streamActive);
    }
    if (now - lastObsPollMs >= 2000.0) { lastObsPollMs = now; obsController.pollInstallationAndProcess(); }
    if (!uiSuspended) updateLiveValues();

    if (deviceListsDirty)
    {
        deviceListsDirty = false;
        if (audioEngine.isInitialised() && !audioEngine.isX32Connected())
        {
            [[maybe_unused]] const auto recoveryResult = audioEngine.autoConfigure();
            // Recovery is allowed even in LIVE MODE after a physical disconnect.
        }
        refreshDeviceLists();
    }
    if (now - lastReconnectAttemptMs >= 5000.0)
    {
        lastReconnectAttemptMs = now;
        if (audioEngine.isInitialised() && !audioEngine.isX32Connected()
            && audioEngine.reconnectIfNeeded()) refreshDeviceLists();
        // Console names are only naming hints for the group router, so a slow
        // refresh off the audio thread is enough.
        audioEngine.updateGroupNamingHints();
    }
    if (now - lastPerformanceLogMs >= 30000.0)
    {
        lastPerformanceLogMs = now;
        juce::Logger::writeToLog("Performance: system=" + juce::String(systemCpuPercent, 1)
            + "% app=" + juce::String(processCpuPercent, 2)
            + "% audio=" + juce::String(audioEngine.getAudioCpuPercent(), 2)
            + "% dsp=" + juce::String(audioEngine.getProcessingTimeMicroseconds(), 1)
            + "us ram=" + juce::String(SystemMonitor::getProcessMemoryMegabytes(), 1)
            + "MB xruns=" + juce::String(audioEngine.getXRunCount())
            + " analysisDrops=" + juce::String(audioEngine.getAnalysisEngine().getSnapshot().droppedOutputSamples));
    }
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*) { deviceListsDirty = true; }

void MainComponent::refreshDeviceLists()
{
    const auto selectedInput = audioEngine.getCurrentInputName();
    const auto selectedOutput = audioEngine.getCurrentOutputName();
    inputDeviceNames = audioEngine.scanInputDevices();
    outputDeviceNames = audioEngine.scanOutputDevices();
    juce::ScopedValueSetter<bool> suppress(suppressSelectionCallbacks, true);
    inputDevice.clear(juce::dontSendNotification);
    outputDevice.clear(juce::dontSendNotification);
    for (int index = 0; index < inputDeviceNames.size(); ++index) inputDevice.addItem(inputDeviceNames[index], index + 1);
    for (int index = 0; index < outputDeviceNames.size(); ++index) outputDevice.addItem(outputDeviceNames[index], index + 1);
    inputDevice.setSelectedItemIndex(inputDeviceNames.indexOf(selectedInput), juce::dontSendNotification);
    outputDevice.setSelectedItemIndex(outputDeviceNames.indexOf(selectedOutput), juce::dontSendNotification);
    juce::Logger::writeToLog("Devices: input=[" + inputDeviceNames.joinIntoString(", ")
                             + "] output=[" + outputDeviceNames.joinIntoString(", ") + "]");
}

void MainComponent::handleInputSelection()
{
    if (suppressSelectionCallbacks) return;
    if (liveRoutingLocked())
    {
        refreshDeviceLists();
        showResult("LIVE MODE blocks input-device changes while OBS is streaming", {});
        return;
    }
    const auto index = inputDevice.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(index, inputDeviceNames.size())) return;
    const auto error = audioEngine.selectInput(inputDeviceNames[index]);
    showResult(error, "Input connected");
}

void MainComponent::handleOutputSelection()
{
    if (suppressSelectionCallbacks) return;
    if (liveRoutingLocked())
    {
        refreshDeviceLists();
        showResult("LIVE MODE blocks output-device changes while OBS is streaming", {});
        return;
    }
    const auto index = outputDevice.getSelectedItemIndex();
    if (!juce::isPositiveAndBelow(index, outputDeviceNames.size())) return;
    const auto error = audioEngine.selectOutput(outputDeviceNames[index]);
    showResult(error, "Output connected");
}

void MainComponent::runAutoConfigure()
{
    if (liveRoutingLocked())
    {
        showResult("LIVE MODE blocks audio reconfiguration while OBS is streaming", {});
        return;
    }
    const auto error = audioEngine.isInitialised() ? audioEngine.autoConfigure()
                                                   : audioEngine.initialise();
    refreshDeviceLists();
    showResult(error, "X32 route configured");
}

bool MainComponent::liveRoutingLocked() const
{
    return liveMode.getToggleState() && obsController.getState().streamActive;
}

void MainComponent::showResult(const juce::String& error, const juce::String& successMessage)
{
    messageLabel.setColour(juce::Label::textColourId, error.isEmpty() ? Colours::primary : Colours::warning);
    messageLabel.setText(error.isEmpty() ? successMessage : error, juce::dontSendNotification);
    messageLabel.setTooltip(messageLabel.getText());
}

void MainComponent::updateLiveValues()
{
    auto& input = audioEngine.getInputMeter();
    auto& output = audioEngine.getOutputMeter();
    inputMeter.setLevels(input.getPeak(0), input.getPeak(1), input.getRms(0), input.getRms(1));
    outputMeter.setLevels(output.getPeak(0), output.getPeak(1), output.getRms(0), output.getRms(1));
    const auto analysis = audioEngine.getAnalysisEngine().getSnapshot();
    const auto smart = audioEngine.getSmartEngine().getState();
    if (analysis.analyzedFrames != lastSpectrumAnalyzedFrames)
    {
        lastSpectrumAnalyzedFrames = analysis.analyzedFrames;
        spectrum.setSnapshot(analysis);
    }
    if ((++uiFrameCounter % 3U) != 0U)
        return; // Meters/spectrum stay fluid; text/status work is intentionally 5 Hz in ECO.
    const auto obs = obsController.getState();
    const auto running = audioEngine.isAudioRunning();
    x32Status.setState(audioEngine.isX32Connected(), audioEngine.isX32Connected() ? "CONNECTED" : "DISCONNECTED");
    audioStatus.setState(running, running ? (audioEngine.getXRunCount() == 0 ? "LIVE SAFE" : "PROCESSING") : "STOPPED",
                         running && audioEngine.getXRunCount() > 0);
    obsStatus.setState(obs.obsConnected, obs.obsConnected ? (obs.recordingActive ? "CONNECTED | REC" : "CONNECTED")
                                                         : (obs.processRunning ? "CONNECTING" : "CLOSED"),
                       obs.processRunning && !obs.obsConnected);
    streamStatus.setState(obs.streamActive, obs.streamActive ? "LIVE" : "OFFLINE");
    openObsButton.setEnabled(obs.installed && !obs.processRunning);
    openObsButton.setButtonText(obs.processRunning ? "OBS OPEN" : "OPEN OBS");
    const auto loadState = systemCpuPercent >= 80.0 ? "HIGH" : (systemCpuPercent >= 65.0 ? "MEDIUM" : "LOW");
    phaseLabel.setText("SYSTEM LOAD " + juce::String(loadState)
                       + (liveMode.getToggleState() ? " | LIVE SAFE" : " | LIVE MODE OFF"),
                       juce::dontSendNotification);

    metricValues[0].setText(formatMetric(analysis.processed.lufsIntegrated, 1, " LUFS"), juce::dontSendNotification);
    metricValues[1].setText(formatMetric(analysis.processed.lufsShortTerm, 1, " LUFS"), juce::dontSendNotification);
    metricValues[2].setText(formatMetric(analysis.processed.lufsMomentary, 1, " LUFS"), juce::dontSendNotification);
    metricValues[3].setText(formatMetric(analysis.processed.truePeakDbtp, 1, " dBTP"), juce::dontSendNotification);
    metricValues[4].setText(formatMetric(analysis.processed.rmsDb, 1, " dB"), juce::dontSendNotification);
    metricValues[5].setText(formatMetric(analysis.processed.peakDb, 1, " dBFS"), juce::dontSendNotification);
    auto& dspMetrics = audioEngine.getProcessingEngine().getMetrics();
    metricValues[6].setText(juce::String(dspMetrics.compressorGainReductionDb.load()
                                                + dspMetrics.limiterGainReductionDb.load(), 1) + " dB",
                            juce::dontSendNotification);
    metricValues[7].setText(juce::String(analysis.processed.stereoCorrelation, 2), juce::dontSendNotification);
    measurementsTitle.setText("STREAM QUALITY  " + juce::String(static_cast<int>(std::round(smart.quality.overall)))
                                  + " / 100  |  " + sceneName(smart.scene),
                              juce::dontSendNotification);

    juce::String actions;
    for (int index = 0; index < smart.problemCount; ++index)
    {
        const auto& problem = smart.problems[static_cast<size_t>(index)];
        actions += (problem.warning ? "! " : "+ ") + problem.name + ": " + problem.detail + "\n";
    }
    for (int index = 0; index < smart.actionCount; ++index)
    {
        const auto& action = smart.actions[static_cast<size_t>(index)];
        actions += (action.rolledBack ? "ROLLBACK | " : "ACTION | ") + action.name;
        if (action.frequencyHz > 0.0f) actions += " @ " + juce::String(action.frequencyHz, 0) + " Hz";
        actions += "  " + juce::String(action.amountDb, 1) + " dB | "
            + percentText(action.confidence) + " confidence";
        if (action.result.isNotEmpty()) actions += " | " + action.result;
        actions += "\n";
    }
    const auto safety = safetyController.getState();
    for (int index = 0; index < safety.eventCount; ++index)
    {
        const auto& event = safety.events[static_cast<size_t>(index)];
        actions += "SAFETY | " + event.name + " -> " + event.response + "\n";
    }
    if (actions.isEmpty()) actions = "Monitoring " + contextName(smart.context) + ". No confident correction required.";
    const auto offline = offlineProcessor.getResult();
    if (offline.running)
        actions = "OFFLINE SIMULATION | " + offline.sourceName + " | "
            + juce::String(static_cast<int>(offline.progress * 100.0f)) + "%\n"
            + offline.stage + " on a low-priority background thread.";
    else if (offline.complete)
        actions = "OFFLINE SIMULATION COMPLETE | " + offline.sourceName
            + "\nOriginal: " + juce::String(offline.original.lufsIntegrated, 1) + " LUFS | "
            + juce::String(offline.original.truePeakDbtp, 1) + " dBTP"
            + "\nProcessed: " + juce::String(offline.processed.lufsIntegrated, 1) + " LUFS | "
            + juce::String(offline.processed.truePeakDbtp, 1) + " dBTP"
            + "\nMix score " + juce::String(static_cast<double>(offline.report.averageScore), 1) + " avg / "
            + juce::String(static_cast<double>(offline.report.minimumScore), 1) + " min | corrections "
            + juce::String(offline.report.corrections) + " | rollbacks "
            + juce::String(offline.report.rollbacks)
            + "\nA/B matched: original " + juce::String(static_cast<double>(offline.report.matchGainOriginalDb), 2)
            + " dB, processed " + juce::String(static_cast<double>(offline.report.matchGainProcessedDb), 2) + " dB\n"
            + offline.reportFile.getFullPathName();
    else if (offline.error.isNotEmpty())
        actions = "OFFLINE TEST ERROR\n" + offline.error;

    const auto room = roomCalibration.getResult();
    if (room.running)
        actions = "ROOM CALIBRATION | " + room.sourceName + " | analysing";
    else if (room.complete)
    {
        actions = "ROOM CALIBRATION | " + room.sourceName + " | " + room.measurementType;
        if (room.rt60Available)
            actions += "\nRT60 " + juce::String(static_cast<double>(room.rt60Seconds), 2) + " s";
        for (int index = 0; index < room.recommendationCount; ++index)
        {
            const auto& recommendation = room.recommendations[static_cast<size_t>(index)];
            actions += "\nX32 MATRIX | " + juce::String(static_cast<double>(recommendation.frequencyHz), 0)
                + " Hz  " + juce::String(static_cast<double>(recommendation.gainDb), 1) + " dB  Q "
                + juce::String(static_cast<double>(recommendation.q), 1);
        }
        actions += "\nRecommendations only, nothing was applied.\n" + room.reportFile.getFullPathName();
    }
    else if (room.error.isNotEmpty())
        actions = "ROOM CALIBRATION ERROR\n" + room.error;

    const auto masking = audioEngine.getMaskingDecision();
    if (masking.active)
        actions += juce::String(actions.isEmpty() ? "" : "\n")
            + (masking.applied ? "MASKING | " : "MASKING (advisory) | ")
            + "music presence " + juce::String(masking.musicGainDb[2], 1) + " dB, upper "
            + juce::String(masking.musicGainDb[3], 1) + " dB | " + percentText(masking.confidence)
            + " confidence";

    actionsLabel.setText(actions.trimEnd(), juce::dontSendNotification);
    if (smart.autoTuneState == AutoTuneState::analysing)
        autoTuneButton.setButtonText("ANALYZING " + juce::String(static_cast<int>(smart.autoTuneProgress * 100.0f)) + "%");
    else if (smart.autoTuneState == AutoTuneState::complete)
        autoTuneButton.setButtonText(profileName(smart.profile));
    else autoTuneButton.setButtonText("AUTO TUNE");

    const auto sampleRate = audioEngine.getSampleRate();
    formatLabel.setText(sampleRate > 0.0 ? juce::String(sampleRate / 1000.0, 1) + " kHz | "
                                              + juce::String(audioEngine.getBufferSize()) + " samples"
                                        : "No active format", juce::dontSendNotification);
    latencyLabel.setText("Latency " + juce::String(audioEngine.getLatencyMilliseconds(), 1) + " ms", juce::dontSendNotification);
    if (developmentMode.getToggleState())
        diagnosticsLabel.setText("APP " + juce::String(processCpuPercent, 2) + "% | SYSTEM "
            + juce::String(systemCpuPercent, 1) + "% | AUDIO "
            + juce::String(audioEngine.getAudioCpuPercent(), 2) + "% | DSP "
            + juce::String(audioEngine.getProcessingTimeMicroseconds(), 1) + " us | RAM "
            + juce::String(processMemoryMegabytes, 1) + " MB | XRUNS " + juce::String(audioEngine.getXRunCount())
            + " | SMART " + juce::String(smart.updateRateHz, 1) + " Hz", juce::dontSendNotification);
    if (obs.lastError.isNotEmpty() && messageLabel.getText().isEmpty()) showResult(obs.lastError, {});
}

void MainComponent::updateDspControls()
{
    auto& parameters = audioEngine.getProcessingEngine().getParameters();
    suppressDspCallbacks = true;
    cleanSlider.setValue(settings.getNumber("clean", 50.0), juce::dontSendNotification);
    punchSlider.setValue(settings.getNumber("punch", 50.0), juce::dontSendNotification);
    claritySlider.setValue(settings.getNumber("clarity", 50.0), juce::dontSendNotification);
    dynamicsSlider.setValue(settings.getNumber("dynamics", 50.0), juce::dontSendNotification);
    warmthSlider.setValue(settings.getNumber("warmth", 35.0), juce::dontSendNotification);
    loudnessSlider.setValue(settings.getNumber("loudnessTarget", -14.0), juce::dontSendNotification);
    smartProcessing.setToggleState(settings.getNumber("smartProcessing", 1.0) > 0.5, juce::dontSendNotification);
    operatingMode.setSelectedItemIndex(static_cast<int>(settings.getNumber("operatingMode", 1.0)), juce::dontSendNotification);
    abButton.setToggleState(true, juce::dontSendNotification);
    bypassButton.setToggleState(false, juce::dontSendNotification);
    suppressDspCallbacks = false;

    const auto update = [this](juce::Slider& slider, std::atomic<float>& target, const char* key, float scale)
    {
        slider.onValueChange = [this, &slider, &target, key, scale]
        {
            if (suppressDspCallbacks) return;
            target.store(static_cast<float>(slider.getValue()) * scale, std::memory_order_release);
            settings.setNumber(key, slider.getValue());
        };
        target.store(static_cast<float>(slider.getValue()) * scale, std::memory_order_release);
    };
    update(cleanSlider, parameters.clean, "clean", 0.01f);
    update(punchSlider, parameters.punch, "punch", 0.01f);
    update(claritySlider, parameters.clarity, "clarity", 0.01f);
    update(dynamicsSlider, parameters.dynamics, "dynamics", 0.01f);
    update(warmthSlider, parameters.warmth, "warmth", 0.01f);
    update(loudnessSlider, parameters.loudnessTarget, "loudnessTarget", 1.0f);
    smartProcessing.onClick = [this, &parameters]
    {
        parameters.smartProcessing.store(smartProcessing.getToggleState(), std::memory_order_release);
        settings.setNumber("smartProcessing", smartProcessing.getToggleState() ? 1.0 : 0.0);
    };
    parameters.smartProcessing.store(smartProcessing.getToggleState(), std::memory_order_release);
    operatingMode.onChange = [this, &parameters]
    {
        const auto mode = operatingMode.getSelectedItemIndex();
        parameters.operatingMode.store(mode, std::memory_order_release);
        settings.setNumber("operatingMode", mode);
    };
    parameters.operatingMode.store(operatingMode.getSelectedItemIndex(), std::memory_order_release);
    parameters.rumbleEnabled.store(settings.getNumber("rumbleEnabled", 1.0) > 0.5, std::memory_order_release);
    parameters.adaptiveEqEnabled.store(settings.getNumber("adaptiveEqEnabled", 1.0) > 0.5, std::memory_order_release);
    parameters.compressorEnabled.store(settings.getNumber("compressorEnabled", 1.0) > 0.5, std::memory_order_release);
    parameters.saturationEnabled.store(settings.getNumber("saturationEnabled", 1.0) > 0.5, std::memory_order_release);
    parameters.limiterEnabled.store(settings.getNumber("limiterEnabled", 1.0) > 0.5, std::memory_order_release);
    presetSelector.onChange = [this]
    {
        const auto selected = presetSelector.getSelectedItemIndex();
        if (selected < 7)
        {
            applyBuiltInPreset(selected);
            return;
        }

        const auto userIndex = selected - 7;
        if (!juce::isPositiveAndBelow(userIndex, userPresetFiles.size())) return;
        auto& loadedParameters = audioEngine.getProcessingEngine().getParameters();
        const auto result = presetManager.load(userPresetFiles.getReference(userIndex), loadedParameters);
        if (result.failed()) { showResult(result.getErrorMessage(), {}); return; }
        if (liveRoutingLocked()) loadedParameters.limiterEnabled.store(true, std::memory_order_release);

        suppressDspCallbacks = true;
        cleanSlider.setValue(loadedParameters.clean.load() * 100.0f, juce::dontSendNotification);
        punchSlider.setValue(loadedParameters.punch.load() * 100.0f, juce::dontSendNotification);
        claritySlider.setValue(loadedParameters.clarity.load() * 100.0f, juce::dontSendNotification);
        dynamicsSlider.setValue(loadedParameters.dynamics.load() * 100.0f, juce::dontSendNotification);
        warmthSlider.setValue(loadedParameters.warmth.load() * 100.0f, juce::dontSendNotification);
        loudnessSlider.setValue(loadedParameters.loudnessTarget.load(), juce::dontSendNotification);
        smartProcessing.setToggleState(loadedParameters.smartProcessing.load(), juce::dontSendNotification);
        operatingMode.setSelectedItemIndex(loadedParameters.operatingMode.load(), juce::dontSendNotification);
        suppressDspCallbacks = false;
        showResult({}, "Local preset loaded");
    };
    autoTuneButton.onClick = [this] { audioEngine.getSmartEngine().startAutoTune(); };
    abButton.onClick = [this, &parameters]
    {
        parameters.abProcessed.store(abButton.getToggleState(), std::memory_order_release);
        abButton.setButtonText(abButton.getToggleState() ? "A/B : B" : "A/B : A (matched)");
    };
    parameters.abProcessed.store(true, std::memory_order_release);
    parameters.abLoudnessMatch.store(true, std::memory_order_release);
    bypassButton.onClick = [this, &parameters]
    {
        parameters.bypass.store(bypassButton.getToggleState(), std::memory_order_release);
    };
}

void MainComponent::applyConsoleAndGroupSettings()
{
    audioEngine.setSmartMaskingEnabled(settings.getNumber("smartMaskingEnabled", 0.0) > 0.5);

    const auto address = settings.getString("x32Address", "");
    if (settings.getNumber("x32LinkEnabled", 0.0) > 0.5 && address.isNotEmpty())
    {
        audioEngine.startX32Link(address);
        showResult({}, "X32 read-only link started on " + address);
    }
    else
    {
        audioEngine.stopX32Link();
    }
}

void MainComponent::askForConsoleAddress()
{
    if (consoleAddressWindow != nullptr) return;
    consoleAddressWindow = std::make_unique<juce::AlertWindow>(
        "X32 read-only link",
        "IP address of the console on this LAN. The application only reads names, faders and\n"
        "buses; it never sends anything that changes the X32.",
        juce::MessageBoxIconType::QuestionIcon);
    consoleAddressWindow->addTextEditor("address", settings.getString("x32Address", ""), "IP address");
    consoleAddressWindow->addButton("SAVE", 1);
    consoleAddressWindow->addButton("CANCEL", 0);
    consoleAddressWindow->enterModalState(true, juce::ModalCallbackFunction::create([this](int choice)
    {
        if (consoleAddressWindow == nullptr) return;
        if (choice == 1)
        {
            const auto address = consoleAddressWindow->getTextEditorContents("address").trim();
            settings.setString("x32Address", address);
            settings.setNumber("x32LinkEnabled", address.isNotEmpty() ? 1.0 : 0.0);
            settings.flush();
        }
        consoleAddressWindow.reset();
        applyConsoleAndGroupSettings();
    }), false);
}

void MainComponent::chooseRoomMeasurement()
{
    roomFileChooser = std::make_unique<juce::FileChooser>(
        "Select a measurement microphone recording", juce::File(), "*.wav;*.flac;*.aif;*.aiff");
    roomFileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                     | juce::FileBrowserComponent::canSelectFiles,
                                 [this](const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();
        if (file.existsAsFile())
            roomCalibration.startAnalysis(file);
    });
}

void MainComponent::applyRefreshRate()
{
    startTimerHz(uiSuspended ? 2 : (highLoadProtection ? 10 : getVisibleRefreshRate()));
    const auto normalRate = getSmartUpdateRate();
    audioEngine.getSmartEngine().setUpdateRateHz(uiSuspended || highLoadProtection ? 2
                                                  : (stableMixEco ? std::max(2, normalRate / 2) : normalRate));
    audioEngine.getAnalysisEngine().setInputAnalysisEnabled(!uiSuspended && !highLoadProtection);
}

void MainComponent::applyBuiltInPreset(int presetIndex)
{
    struct Values { double clean, punch, clarity, dynamics, warmth, loudness; int mode; };
    const std::array<Values, 7> values {{
        { 55, 60, 62, 52, 35, -14.0, 1 }, { 50, 66, 58, 48, 52, -14.0, 1 },
        { 35, 62, 58, 35, 45, -15.0, 1 }, { 58, 70, 60, 62, 38, -14.0, 1 },
        { 48, 42, 72, 55, 30, -14.0, 1 }, { 72, 45, 68, 48, 20, -15.0, 1 },
        { 50, 50, 50, 50, 35, -14.0, 2 }
    }};
    if (!juce::isPositiveAndBelow(presetIndex, static_cast<int>(values.size()))) return;
    const auto& preset = values[static_cast<size_t>(presetIndex)];
    cleanSlider.setValue(preset.clean); punchSlider.setValue(preset.punch); claritySlider.setValue(preset.clarity);
    dynamicsSlider.setValue(preset.dynamics); warmthSlider.setValue(preset.warmth); loudnessSlider.setValue(preset.loudness);
    operatingMode.setSelectedItemIndex(preset.mode);
}

int MainComponent::getVisibleRefreshRate() const
{
    return performanceProfile.getSelectedItemIndex() == 0 ? 15 : 30;
}

int MainComponent::getSmartUpdateRate() const
{
    switch (performanceProfile.getSelectedItemIndex()) { case 2: return 10; case 1: return 8; default: return 5; }
}

void MainComponent::configureHeading(juce::Label& label, float size, juce::Colour colour)
{
    label.setColour(juce::Label::textColourId, colour);
    label.setFont(juce::Font(juce::FontOptions(size).withStyle("Bold")));
}

void MainComponent::configureControlSlider(juce::Slider& slider, const juce::String& suffix)
{
    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 22);
    slider.setRange(0.0, 100.0, 1.0);
    slider.setTextValueSuffix(suffix);
    slider.setColour(juce::Slider::trackColourId, Colours::primary);
    slider.setColour(juce::Slider::backgroundColourId, Colours::control);
    slider.setColour(juce::Slider::thumbColourId, Colours::text);
    slider.setColour(juce::Slider::textBoxTextColourId, Colours::text);
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void MainComponent::configureMetricLabel(juce::Label& label)
{
    label.setColour(juce::Label::textColourId, Colours::text);
    label.setFont(juce::Font(juce::FontOptions(15.0f).withStyle("Bold")));
}

juce::String MainComponent::formatMetric(float value, int decimals, const juce::String& suffix)
{
    return std::isfinite(value) && value > -99.0f ? juce::String(value, decimals) + suffix : "--";
}
} // namespace churchstream
