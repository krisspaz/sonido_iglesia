#include "SmartEngine.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
SmartEngine::SmartEngine(AnalysisEngine& analysisToUse, ProcessingEngine& processingToUse,
                         juce::File profileFileToUse, juce::String churchNameToUse)
    : Thread("CSP Smart Decision Thread"), analysis(analysisToUse), processing(processingToUse),
      profileFile(std::move(profileFileToUse)), churchName(std::move(churchNameToUse))
{
    if (churchName.trim().isEmpty()) churchName = "Mi Iglesia";
    state.churchName = churchName;
    loadProfile();
}

SmartEngine::~SmartEngine()
{
    stop();
    if (profileDirty.load(std::memory_order_acquire)) saveProfile();
}

void SmartEngine::start()
{
    if (!isThreadRunning())
        startThread(juce::Thread::Priority::low);
}

void SmartEngine::stop()
{
    signalThreadShouldExit();
    stopThread(1500);
}

void SmartEngine::startAutoTune() { autoTuneRequested.store(true, std::memory_order_release); }

void SmartEngine::setUpdateRateHz(int rate) noexcept
{
    updateRate.store(std::clamp(rate, 2, 10), std::memory_order_release);
    analysis.setUpdateRateHz(rate);
}

void SmartEngine::setScene(SmartScene scene) noexcept
{
    requestedScene.store(static_cast<int>(scene), std::memory_order_release);
}

void SmartEngine::setChurchName(const juce::String& name)
{
    const auto cleaned = name.trim().substring(0, 80);
    if (cleaned.isEmpty() || cleaned == churchName) return;
    const juce::ScopedLock lock(stateLock);
    churchName = cleaned;
    profileDirty.store(true, std::memory_order_release);
    state.churchName = churchName;
}

void SmartEngine::requestSafetyRollback() noexcept
{
    safetyRollbackRequested.store(true, std::memory_order_release);
}

SmartState SmartEngine::getState() const
{
    const juce::ScopedLock lock(stateLock);
    return state;
}

void SmartEngine::processSnapshotForTesting(const AnalysisSnapshot& snapshot, float elapsedSeconds)
{
    update(snapshot, std::clamp(elapsedSeconds, 0.001f, 1.0f));
}

void SmartEngine::run()
{
    auto previous = juce::Time::getMillisecondCounterHiRes();
    while (!threadShouldExit())
    {
        const auto rate = updateRate.load(std::memory_order_acquire);
        wait(std::max(40, 1000 / rate));
        if (threadShouldExit())
            break;

        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto elapsed = static_cast<float>(std::clamp((now - previous) * 0.001, 0.02, 1.0));
        previous = now;
        update(analysis.getSnapshot(), elapsed);
    }
}

void SmartEngine::update(const AnalysisSnapshot& snapshot, float elapsedSeconds)
{
    if (autoTuneRequested.exchange(false, std::memory_order_acq_rel))
    {
        autoTuneAccumulator = {};
        autoTuneElapsed = 0.0f;
        const juce::ScopedLock lock(stateLock);
        state.autoTuneState = AutoTuneState::analysing;
        state.autoTuneProgress = 0.0f;
        juce::Logger::writeToLog("Smart Engine: Auto Tune started");
    }

    SmartState next;
    {
        const juce::ScopedLock lock(stateLock);
        next = state;
    }

    const auto& parameters = processing.getParameters();
    next.active = parameters.smartProcessing.load(std::memory_order_relaxed)
        && parameters.operatingMode.load(std::memory_order_relaxed) != static_cast<int>(OperatingMode::manual);
    next.context = snapshot.context;
    next.scene = static_cast<SmartScene>(requestedScene.load(std::memory_order_acquire));
    switch (next.scene)
    {
        case SmartScene::preaching:
        case SmartScene::announcements: next.context = MixContext::speech; break;
        case SmartScene::worship:
        case SmartScene::prayer: next.context = MixContext::worshipSoft; break;
        case SmartScene::fullBand: next.context = MixContext::fullBand; break;
        case SmartScene::ambience: next.context = MixContext::ambience; break;
        case SmartScene::autoDetect: break;
    }
    next.updateRateHz = snapshot.updateRateHz;
    next.baselineReady = baseline.ready;
    next.actionCount = 0;

    if (next.autoTuneState == AutoTuneState::analysing)
        updateAutoTune(snapshot.processed, elapsedSeconds);

    {
        const juce::ScopedLock lock(stateLock);
        next.autoTuneState = state.autoTuneState;
        next.autoTuneProgress = state.autoTuneProgress;
        next.profile = state.profile;
    }

    next.quality = calculateQuality(snapshot, next, elapsedSeconds);
    buildProblems(snapshot, next);

    if (safetyRollbackRequested.exchange(false, std::memory_order_acq_rel))
    {
        for (auto& loop : feedback)
        {
            loop.evaluating = false;
            loop.cooldown = 15.0f;
            loop.suppressCorrection = true;
            loop.lastRollback = true;
            loop.lastResult = "SAFETY ROLLBACK";
        }
        ++next.rollbackCount;
        addAction(next, "Safety rollback", "Independent Safety Controller requested last safe state",
                  0.0f, 1.0f, 1, 0.0f, "ROLLED BACK", true);
    }

    if (next.active && snapshot.processed.rmsDb > -60.0f)
        decide(snapshot, elapsedSeconds, next);
    else
    {
        publishTargets({}, 20.0f, 0.0f, 0.0f, elapsedSeconds);
        processing.getAdaptiveTargets().stereoBalanceDb.store(0.0f, std::memory_order_release);
    }

    next.baselineReady = baseline.ready;
    next.bandState = snapshot.processed.bandEnergy;
    next.rumbleState = spectrumEnergy(snapshot.processed, snapshot.sampleRate, 20.0f, 40.0f);
    next.dynamicsState = std::clamp(snapshot.processed.crestFactorDb / 18.0f, 0.0f, 1.0f);
    next.transientState = std::clamp(snapshot.processed.transientDensity / 12.0f, 0.0f, 1.0f);

    profileSaveElapsed += elapsedSeconds;
    if (profileDirty.load(std::memory_order_acquire) && profileSaveElapsed >= 30.0f)
        saveProfile();

    const juce::ScopedLock lock(stateLock);
    state = std::move(next);
}

void SmartEngine::updateAutoTune(const SignalMetrics& metrics, float elapsedSeconds)
{
    if (metrics.rmsDb < -55.0f)
        return;

    autoTuneElapsed += elapsedSeconds;
    for (size_t band = 0; band < autoTuneAccumulator.bands.size(); ++band)
        autoTuneAccumulator.bands[band] += metrics.bandEnergy[band];
    autoTuneAccumulator.loudness += metrics.lufsShortTerm > -70.0f ? metrics.lufsShortTerm : metrics.rmsDb;
    autoTuneAccumulator.crest += metrics.crestFactorDb;
    autoTuneAccumulator.centroid += metrics.spectralCentroidHz;
    autoTuneAccumulator.stereoWidth += metrics.stereoWidth;
    autoTuneAccumulator.sibilance += spectrumEnergy(metrics, processing.getSampleRate(), 5500.0f, 9000.0f);
    ++autoTuneAccumulator.count;

    {
        const juce::ScopedLock lock(stateLock);
        state.autoTuneProgress = std::clamp(autoTuneElapsed / 25.0f, 0.0f, 1.0f);
    }

    if (autoTuneElapsed < 25.0f || autoTuneAccumulator.count == 0)
        return;

    const auto divisor = static_cast<double>(autoTuneAccumulator.count);
    for (size_t band = 0; band < baseline.bands.size(); ++band)
        baseline.bands[band] = static_cast<float>(autoTuneAccumulator.bands[band] / divisor);
    baseline.loudness = static_cast<float>(autoTuneAccumulator.loudness / divisor);
    baseline.crest = static_cast<float>(autoTuneAccumulator.crest / divisor);
    baseline.centroid = static_cast<float>(autoTuneAccumulator.centroid / divisor);
    baseline.stereoWidth = static_cast<float>(autoTuneAccumulator.stereoWidth / divisor);
    baseline.sibilance = static_cast<float>(autoTuneAccumulator.sibilance / divisor);
    baseline.ready = true;

    const auto profile = detectProfile(baseline);
    {
        const juce::ScopedLock lock(stateLock);
        state.autoTuneState = AutoTuneState::complete;
        state.autoTuneProgress = 1.0f;
        state.profile = profile;
    }
    juce::Logger::writeToLog("Smart Engine: Auto Tune complete, profile=" + profileName(profile));
    profileDirty.store(true, std::memory_order_release);
    saveProfile();
}

void SmartEngine::decide(const AnalysisSnapshot& snapshot, float elapsedSeconds, SmartState& next)
{
    const auto& metrics = snapshot.processed;
    const auto lowSeverity = std::max(0.0f, metrics.bandEnergy[0] - baseline.bands[0] - 0.035f) / 0.12f
        * (next.context == MixContext::fullBand ? 1.10f : 1.0f);
    const auto mudSeverity = std::max(0.0f, metrics.bandEnergy[1] - baseline.bands[1] - 0.045f) / 0.15f
        * (next.context == MixContext::denseMusic ? 1.15f : 1.0f);
    const auto claritySeverity = std::max(0.0f, baseline.bands[2] - metrics.bandEnergy[2] - 0.05f) / 0.15f
        * (next.context == MixContext::speech || next.context == MixContext::soloVocal ? 1.15f : 1.0f);
    const auto harshSeverity = std::max(0.0f, metrics.bandEnergy[3] - baseline.bands[3] - 0.045f) / 0.14f;
    const auto highSeverity = std::max(0.0f, metrics.bandEnergy[4] - baseline.bands[4] - 0.025f) / 0.10f;
    const auto rumbleRatio = spectrumEnergy(metrics, snapshot.sampleRate, 20.0f, 40.0f);
    const auto rumbleSeverity = std::max(0.0f, rumbleRatio - 0.035f) / 0.08f;
    const auto stereoSeverity = std::max(0.0f, -0.15f - metrics.stereoCorrelation) / 0.85f;
    const auto imbalanceSeverity = std::max(0.0f, std::abs(metrics.leftRightImbalanceDb) - 6.0f) / 12.0f;
    // Sibilance lives inside the 5.5-9 kHz slice; the wider harshness band
    // cannot separate an aggressive "s" from a bright cymbal.
    const auto sibilanceRatio = spectrumEnergy(metrics, snapshot.sampleRate, 5500.0f, 9000.0f);
    const auto sibilanceSeverity = std::max(0.0f, sibilanceRatio - baseline.sibilance - 0.020f) / 0.060f
        * (next.context == MixContext::speech || next.context == MixContext::soloVocal ? 1.20f : 1.0f);
    const std::array<float, 9> severity { lowSeverity, mudSeverity, claritySeverity,
                                          harshSeverity, highSeverity, rumbleSeverity, stereoSeverity,
                                          imbalanceSeverity, sibilanceSeverity };

    for (size_t index = 0; index < persistence.size(); ++index)
    {
        if (severity[index] > 0.05f)
            persistence[index] = std::min(10.0f, persistence[index] + elapsedSeconds);
        else
            persistence[index] = std::max(0.0f, persistence[index] - elapsedSeconds * 0.5f);
    }

    const auto lowConfidence = confidenceFromPersistence(lowSeverity, persistence[0], 4.0f);
    const auto mudConfidence = confidenceFromPersistence(mudSeverity, persistence[1], 4.0f);
    auto clarityConfidence = confidenceFromPersistence(claritySeverity, persistence[2], 5.0f);
    const auto harshConfidence = confidenceFromPersistence(harshSeverity, persistence[3], 3.5f);
    const auto highConfidence = confidenceFromPersistence(highSeverity, persistence[4], 4.5f);
    const auto rumbleConfidence = confidenceFromPersistence(rumbleSeverity, persistence[5], 4.0f);
    const auto stereoConfidence = confidenceFromPersistence(stereoSeverity, persistence[6], 3.0f);
    const auto imbalanceConfidence = confidenceFromPersistence(imbalanceSeverity, persistence[7], 5.0f);
    const auto sibilanceConfidence = confidenceFromPersistence(sibilanceSeverity, persistence[8], 2.5f);
    if (harshConfidence > 0.65f)
        clarityConfidence *= 0.35f;

    const auto autoMode = processing.getParameters().operatingMode.load(std::memory_order_relaxed)
        == static_cast<int>(OperatingMode::autoMode);
    auto learnedScale = [this](size_t index) { return 0.75f + learnedEffectiveness[index] * 0.50f; };
    auto lowTarget = lowConfidence > 0.6f ? -std::min(autoMode ? 2.0f : 1.5f, lowSeverity * 1.2f * learnedScale(0)) : 0.0f;
    auto mudTarget = mudConfidence > 0.6f ? -std::min(autoMode ? 3.0f : 2.0f, mudSeverity * 1.5f * learnedScale(1)) : 0.0f;
    auto clarityTarget = clarityConfidence > 0.68f ? std::min(autoMode ? 1.5f : 1.0f, claritySeverity * 0.9f * learnedScale(2)) : 0.0f;
    auto harshTarget = harshConfidence > 0.62f ? -std::min(autoMode ? 3.0f : 2.0f, harshSeverity * 1.5f * learnedScale(3)) : 0.0f;
    auto sibilanceTarget = sibilanceConfidence > 0.60f
        ? -std::min(autoMode ? 4.0f : 2.5f, sibilanceSeverity * 2.0f * learnedScale(4)) : 0.0f;
    auto highTarget = highConfidence > 0.68f ? -std::min(autoMode ? 2.0f : 1.5f, highSeverity * learnedScale(5)) : 0.0f;
    const auto rumbleTarget = rumbleConfidence > 0.7f ? std::clamp(25.0f + rumbleSeverity * 15.0f, 25.0f, 45.0f) : 20.0f;

    // The de-esser is deliberately outside the shared tonal budget: it is
    // narrow and surgical, so spending the broadband budget on it would starve
    // the low-mid and harshness corrections.
    const auto reductionBudget = std::abs(lowTarget) + std::abs(mudTarget) + std::abs(harshTarget) + std::abs(highTarget);
    const auto tonalBudget = autoMode ? 5.0f : 3.5f;
    if (reductionBudget > tonalBudget)
    {
        const auto scale = tonalBudget / reductionBudget;
        lowTarget *= scale;
        mudTarget *= scale;
        harshTarget *= scale;
        highTarget *= scale;
    }

    std::array<float, tonalCorrections> tonalTargets { lowTarget, mudTarget, clarityTarget,
                                                       harshTarget, sibilanceTarget, highTarget };
    const std::array<float, tonalCorrections> tonalSeverity { lowSeverity, mudSeverity, claritySeverity,
                                                               harshSeverity, sibilanceSeverity, highSeverity };
    evaluateClosedLoop(tonalSeverity, tonalTargets, elapsedSeconds, next);

    auto compressionTarget = next.context == MixContext::quiet
        ? 2.0f : (metrics.crestFactorDb < 6.5f ? 2.0f : (metrics.crestFactorDb > 15.0f ? -0.5f : 0.0f));
    if (next.context == MixContext::worshipSoft || next.scene == SmartScene::prayer)
        compressionTarget = std::max(compressionTarget, 1.0f);
    else if (next.context == MixContext::speech)
        compressionTarget = std::min(compressionTarget, 0.0f);
    const auto targetLoudness = processing.getParameters().loudnessTarget.load(std::memory_order_relaxed);
    const auto loudnessError = targetLoudness - metrics.lufsShortTerm;
    const auto gainTarget = next.context != MixContext::quiet && metrics.lufsShortTerm > -50.0f
        && std::abs(loudnessError) > 1.0f
        ? std::clamp(loudnessError * 0.18f, autoMode ? -4.0f : -3.0f,
                     autoMode ? 4.0f : 3.0f) : 0.0f;

    publishTargets(tonalTargets, rumbleTarget, compressionTarget, gainTarget, elapsedSeconds);
    processing.getAdaptiveTargets().stereoWidth.store(stereoConfidence > 0.75f ? 0.85f : 1.0f,
                                                       std::memory_order_release);
    const auto balanceCorrection = imbalanceConfidence > 0.75f
        ? std::clamp(-metrics.leftRightImbalanceDb * 0.10f, -0.75f, 0.75f) : 0.0f;
    processing.getAdaptiveTargets().stereoBalanceDb.store(balanceCorrection, std::memory_order_release);

    if (lowConfidence > 0.6f) addAction(next, "Controlling low-end", "Persistent 20-120 Hz energy", currentTargets[0], lowConfidence, 2, 90.0f, feedback[0].lastResult, feedback[0].lastRollback);
    if (mudConfidence > 0.6f) addAction(next, "Reducing mud", "Persistent 120-500 Hz buildup", currentTargets[1], mudConfidence, 2, 240.0f, feedback[1].lastResult, feedback[1].lastRollback);
    if (clarityConfidence > 0.68f) addAction(next, "Improving clarity", "Presence below mix baseline", currentTargets[2], clarityConfidence, 3, 2500.0f, feedback[2].lastResult, feedback[2].lastRollback);
    if (harshConfidence > 0.62f) addAction(next, "Controlling harshness", "Sustained 2.5-8 kHz excess", currentTargets[3], harshConfidence, 2, 5200.0f, feedback[3].lastResult, feedback[3].lastRollback);
    if (sibilanceConfidence > 0.60f) addAction(next, "Taming sibilance", "Sustained 5.5-9 kHz excess over the church baseline", currentTargets[4], sibilanceConfidence, 2, 7400.0f, feedback[4].lastResult, feedback[4].lastRollback);
    if (highConfidence > 0.68f) addAction(next, "Smoothing highs", "Persistent 8-20 kHz excess", currentTargets[5], highConfidence, 3, 12000.0f, feedback[5].lastResult, feedback[5].lastRollback);
    if (rumbleConfidence > 0.7f) addAction(next, "Cleaning rumble", "Persistent energy below 40 Hz", currentTargets[6] - 20.0f, rumbleConfidence, 2);
    if (std::abs(currentTargets[7]) > 0.1f) addAction(next, "Loudness stabilisation", "Short-term loudness outside target deadband", currentTargets[7], std::min(1.0f, std::abs(loudnessError) / 6.0f), 3);
    if (stereoConfidence > 0.75f) addAction(next, "Protecting stereo compatibility", "Sustained negative phase correlation", -15.0f, stereoConfidence, 2);
    if (imbalanceConfidence > 0.75f) addAction(next, "Balancing stereo", "Persistent L/R level imbalance", balanceCorrection, imbalanceConfidence, 3);

    if (baseline.ready && next.actionCount == 0)
    {
        const auto alpha = 1.0f - std::exp(-elapsedSeconds / 180.0f);
        for (size_t band = 0; band < baseline.bands.size(); ++band)
            baseline.bands[band] += alpha * (metrics.bandEnergy[band] - baseline.bands[band]);
        baseline.crest += alpha * (metrics.crestFactorDb - baseline.crest);
        baseline.centroid += alpha * (metrics.spectralCentroidHz - baseline.centroid);
    }
}

QualityScores SmartEngine::calculateQuality(const AnalysisSnapshot& snapshot, const SmartState& previous,
                                            float) noexcept
{
    const auto& m = snapshot.processed;
    QualityScores score;
    if (m.rmsDb <= -70.0f)
        return score;

    auto tonalDistance = 0.0f;
    for (size_t band = 0; band < baseline.bands.size(); ++band)
        tonalDistance += std::abs(m.bandEnergy[band] - baseline.bands[band]);
    score.tonalBalance = std::clamp(100.0f - tonalDistance * 150.0f, 0.0f, 100.0f);

    auto desiredCrest = 10.0f;
    if (previous.context == MixContext::worshipSoft || previous.context == MixContext::ambience)
        desiredCrest = 12.0f;
    else if (previous.context == MixContext::speech)
        desiredCrest = 8.5f;
    score.dynamics = std::clamp(100.0f - std::abs(m.crestFactorDb - desiredCrest) * 8.0f, 0.0f, 100.0f);

    const auto targetLoudness = processing.getParameters().loudnessTarget.load(std::memory_order_relaxed);
    const auto measuredLoudness = m.lufsShortTerm > -70.0f ? m.lufsShortTerm : m.rmsDb;
    score.loudness = std::clamp(100.0f - std::abs(measuredLoudness - targetLoudness) * 13.0f, 0.0f, 100.0f);
    score.truePeak = m.truePeakDbtp <= -1.0f ? 100.0f
        : std::clamp(100.0f - (m.truePeakDbtp + 1.0f) * 55.0f, 0.0f, 100.0f);
    const auto clarityDeficit = std::max(0.0f, baseline.bands[2] - m.bandEnergy[2] - 0.035f);
    score.clarity = std::clamp(100.0f - clarityDeficit * 500.0f, 0.0f, 100.0f);
    score.stereo = std::clamp(100.0f - std::max(0.0f, 0.15f - m.stereoCorrelation) * 85.0f
                                      - std::max(0.0f, std::abs(m.leftRightImbalanceDb) - 2.0f) * 4.5f,
                              0.0f, 100.0f);
    score.noise = previous.context == MixContext::quiet
        ? std::clamp(100.0f - std::max(0.0f, m.rmsDb + 58.0f) * 9.0f, 0.0f, 100.0f)
        : 100.0f;
    const auto reduction = processing.getMetrics().compressorGainReductionDb.load(std::memory_order_relaxed)
        + processing.getMetrics().limiterGainReductionDb.load(std::memory_order_relaxed);
    score.compression = std::clamp(100.0f - std::max(0.0f, reduction - 4.0f) * 12.0f, 0.0f, 100.0f);

    auto movement = std::abs(measuredLoudness - previousLoudness) * 0.10f;
    for (size_t band = 0; band < previousBandEnergy.size(); ++band)
        movement += std::abs(m.bandEnergy[band] - previousBandEnergy[band]);
    score.stability = std::clamp(100.0f - movement * 180.0f, 0.0f, 100.0f);
    previousBandEnergy = m.bandEnergy;
    previousLoudness = measuredLoudness;

    score.overall = score.tonalBalance * 0.18f + score.dynamics * 0.12f
        + score.loudness * 0.16f + score.truePeak * 0.14f + score.clarity * 0.10f
        + score.stereo * 0.10f + score.noise * 0.05f + score.compression * 0.08f
        + score.stability * 0.07f;
    return score;
}

void SmartEngine::buildProblems(const AnalysisSnapshot& snapshot, SmartState& next)
{
    next.problemCount = 0;
    const auto& m = snapshot.processed;
    const auto target = processing.getParameters().loudnessTarget.load(std::memory_order_relaxed);
    const auto loudnessError = m.lufsShortTerm > -70.0f ? std::abs(m.lufsShortTerm - target) : 100.0f;
    addProblem(next, "Loudness", loudnessError <= 1.5f ? "Stable" : "Outside target",
               std::clamp(loudnessError / 6.0f, 0.0f, 1.0f), loudnessError > 1.5f);
    addProblem(next, "True Peak", m.truePeakDbtp <= -1.0f ? "Safe" : "Above -1 dBTP",
               std::clamp((m.truePeakDbtp + 1.0f) / 3.0f, 0.0f, 1.0f), m.truePeakDbtp > -1.0f);
    const auto mud = std::max(0.0f, m.bandEnergy[1] - baseline.bands[1] - 0.045f) / 0.15f;
    addProblem(next, "Low-mid", mud > 0.20f ? "Buildup near 240 Hz" : "Balanced",
               std::clamp(mud, 0.0f, 1.0f), mud > 0.20f);
    const auto harsh = std::max(0.0f, m.bandEnergy[3] - baseline.bands[3] - 0.045f) / 0.14f;
    addProblem(next, "Harshness", harsh > 0.20f ? "High-mid excess" : "Controlled",
               std::clamp(harsh, 0.0f, 1.0f), harsh > 0.20f);
    const auto sibilance = std::max(0.0f, spectrumEnergy(m, snapshot.sampleRate, 5500.0f, 9000.0f)
                                              - baseline.sibilance - 0.020f) / 0.060f;
    addProblem(next, "Sibilance", sibilance > 0.20f ? "Excess near 7 kHz" : "Controlled",
               std::clamp(sibilance, 0.0f, 1.0f), sibilance > 0.20f);
    const auto stereoWarning = m.stereoCorrelation < -0.15f || std::abs(m.leftRightImbalanceDb) > 6.0f;
    addProblem(next, "Stereo", stereoWarning ? "Phase or balance risk" : "Compatible",
               stereoWarning ? 0.75f : 0.0f, stereoWarning);
    const auto reduction = processing.getMetrics().compressorGainReductionDb.load(std::memory_order_relaxed)
        + processing.getMetrics().limiterGainReductionDb.load(std::memory_order_relaxed);
    addProblem(next, "Dynamics", reduction > 8.0f ? "Excessive gain reduction" : "Preserved",
               std::clamp((reduction - 4.0f) / 8.0f, 0.0f, 1.0f), reduction > 8.0f);
}

void SmartEngine::evaluateClosedLoop(const std::array<float, tonalCorrections>& severity,
                                     std::array<float, tonalCorrections>& tonalTargets, float elapsedSeconds,
                                     SmartState& next)
{
    for (size_t index = 0; index < feedback.size(); ++index)
    {
        auto& loop = feedback[index];
        loop.cooldown = std::max(0.0f, loop.cooldown - elapsedSeconds);
        if (loop.cooldown > 0.0f)
        {
            if (loop.suppressCorrection) tonalTargets[index] = 0.0f;
            continue;
        }
        loop.suppressCorrection = false;

        if (!loop.evaluating && std::abs(tonalTargets[index]) > 0.05f)
        {
            loop.evaluating = true;
            loop.startingSeverity = std::max(0.01f, severity[index]);
            loop.startingScore = next.quality.overall;
            loop.target = tonalTargets[index];
            loop.elapsed = 0.0f;
            loop.lastResult = "EVALUATING";
            loop.lastRollback = false;
        }
        if (!loop.evaluating) continue;

        loop.elapsed += elapsedSeconds;
        if (loop.elapsed < 3.5f) continue;
        const auto improvement = (loop.startingSeverity - severity[index]) / loop.startingSeverity;
        const auto scoreDelta = next.quality.overall - loop.startingScore;
        if (severity[index] > loop.startingSeverity * 1.18f || scoreDelta < -5.0f)
        {
            tonalTargets[index] = 0.0f;
            loop.cooldown = 12.0f;
            loop.suppressCorrection = true;
            loop.lastResult = scoreDelta < -5.0f ? "ROLLBACK: MIX SCORE DECREASED"
                                                  : "ROLLBACK: PROBLEM INCREASED";
            loop.lastRollback = true;
            learnedEffectiveness[index] = std::max(0.0f, learnedEffectiveness[index] - 0.10f);
            ++next.rollbackCount;
            profileDirty.store(true, std::memory_order_release);
        }
        else
        {
            loop.lastResult = improvement >= 0.08f ? "IMPROVED " + juce::String(improvement * 100.0f, 0) + "%"
                                                   : "STABLE / RETAINED";
            learnedEffectiveness[index] = std::min(1.0f, learnedEffectiveness[index]
                + (improvement >= 0.08f ? 0.05f : 0.01f));
            loop.cooldown = 10.0f;
            loop.suppressCorrection = false;
            profileDirty.store(true, std::memory_order_release);
        }
        loop.evaluating = false;
    }
}

void SmartEngine::loadProfile()
{
    if (profileFile == juce::File() || !profileFile.existsAsFile()) return;
    const auto parsed = juce::JSON::parse(profileFile.loadFileAsString());
    auto* root = parsed.getDynamicObject();
    if (root == nullptr) return;
    churchName = root->getProperty("churchName").toString().trim();
    if (churchName.isEmpty()) churchName = "Mi Iglesia";
    if (const auto* bands = root->getProperty("baselineBands").getArray())
        for (int index = 0; index < std::min(5, bands->size()); ++index)
            baseline.bands[static_cast<size_t>(index)] = static_cast<float>((*bands)[index]);
    if (const auto* learned = root->getProperty("learnedEffectiveness").getArray())
        for (int index = 0; index < std::min(static_cast<int>(tonalCorrections), learned->size()); ++index)
            learnedEffectiveness[static_cast<size_t>(index)] = std::clamp(static_cast<float>((*learned)[index]), 0.0f, 1.0f);
    baseline.loudness = static_cast<float>(root->getProperty("baselineLoudness"));
    baseline.crest = static_cast<float>(root->getProperty("baselineCrest"));
    baseline.centroid = static_cast<float>(root->getProperty("baselineCentroid"));
    baseline.stereoWidth = static_cast<float>(root->getProperty("baselineStereoWidth"));
    if (root->hasProperty("baselineSibilance"))
        baseline.sibilance = std::clamp(static_cast<float>(root->getProperty("baselineSibilance")), 0.0f, 0.6f);
    baseline.ready = static_cast<bool>(root->getProperty("baselineReady"));
    state.profile = static_cast<MixProfile>(std::clamp(static_cast<int>(root->getProperty("mixProfile")), 0, 8));
    state.baselineReady = baseline.ready;
    state.churchName = churchName;
}

void SmartEngine::saveProfile()
{
    if (profileFile == juce::File()) return;
    profileFile.getParentDirectory().createDirectory();
    auto root = juce::DynamicObject::Ptr(new juce::DynamicObject());
    root->setProperty("schemaVersion", 3);
    juce::String profileChurchName;
    {
        const juce::ScopedLock lock(stateLock);
        profileChurchName = churchName;
    }
    root->setProperty("churchName", profileChurchName);
    root->setProperty("lastCalibration", juce::Time::getCurrentTime().toISO8601(true));
    root->setProperty("sampleRate", processing.getSampleRate());
    root->setProperty("baselineReady", baseline.ready);
    root->setProperty("baselineLoudness", baseline.loudness);
    root->setProperty("baselineCrest", baseline.crest);
    root->setProperty("baselineCentroid", baseline.centroid);
    root->setProperty("baselineStereoWidth", baseline.stereoWidth);
    root->setProperty("baselineSibilance", baseline.sibilance);
    root->setProperty("mixProfile", static_cast<int>(detectProfile(baseline)));
    juce::Array<juce::var> bands, learned;
    for (const auto value : baseline.bands) bands.add(value);
    for (const auto value : learnedEffectiveness) learned.add(value);
    root->setProperty("baselineBands", juce::var(bands));
    root->setProperty("learnedEffectiveness", juce::var(learned));
    if (profileFile.replaceWithText(juce::JSON::toString(juce::var(root.get()), true)))
    {
        profileDirty.store(false, std::memory_order_release);
        profileSaveElapsed = 0.0f;
    }
}

void SmartEngine::publishTargets(const std::array<float, tonalCorrections>& tonalTargets,
                                 float rumble, float compression, float loudness, float elapsedSeconds)
{
    constexpr size_t rumbleIndex = tonalCorrections;
    std::array<float, tonalCorrections + 2> targets {};
    for (size_t index = 0; index < tonalCorrections; ++index)
        targets[index] = tonalTargets[index];
    targets[rumbleIndex] = rumble;
    targets[rumbleIndex + 1] = loudness;
    for (size_t index = 0; index < currentTargets.size(); ++index)
        currentTargets[index] = smooth(currentTargets[index], targets[index], elapsedSeconds,
                                       index == rumbleIndex ? 3.0f : 2.0f,
                                       index == rumbleIndex ? 10.0f : 8.0f);

    auto& adaptive = processing.getAdaptiveTargets();
    adaptive.lowGainDb.store(currentTargets[0], std::memory_order_release);
    adaptive.mudGainDb.store(currentTargets[1], std::memory_order_release);
    adaptive.clarityGainDb.store(currentTargets[2], std::memory_order_release);
    adaptive.harshGainDb.store(currentTargets[3], std::memory_order_release);
    adaptive.sibilanceGainDb.store(currentTargets[4], std::memory_order_release);
    adaptive.highGainDb.store(currentTargets[5], std::memory_order_release);
    adaptive.rumbleCutoffHz.store(currentTargets[6], std::memory_order_release);
    adaptive.compressionDb.store(smooth(adaptive.compressionDb.load(), compression, elapsedSeconds, 2.0f, 8.0f), std::memory_order_release);
    adaptive.loudnessGainDb.store(currentTargets[7], std::memory_order_release);
    adaptive.stereoWidth.store(1.0f, std::memory_order_release);
}

MixProfile SmartEngine::detectProfile(const Baseline& value) noexcept
{
    if (value.bands[0] > 0.22f) return MixProfile::bassHeavy;
    if (value.bands[3] + value.bands[4] > 0.36f) return MixProfile::bright;
    if (value.bands[3] + value.bands[4] < 0.16f) return MixProfile::dark;
    if (value.crest > 14.0f) return MixProfile::dynamic;
    if (value.crest < 6.5f) return MixProfile::dense;
    if (value.bands[2] > 0.45f && value.stereoWidth < 0.45f) return MixProfile::vocalHeavy;
    if (value.bands[0] < 0.10f && value.crest > 10.0f) return MixProfile::acoustic;
    return MixProfile::balanced;
}

float SmartEngine::confidenceFromPersistence(float severity, float seconds, float requiredSeconds) noexcept
{
    return std::clamp(severity, 0.0f, 1.0f) * std::clamp(seconds / requiredSeconds, 0.0f, 1.0f);
}

float SmartEngine::spectrumEnergy(const SignalMetrics& metrics, double sampleRate, float lowHz, float highHz) noexcept
{
    if (sampleRate <= 0.0) return 0.0f;
    double selected = 0.0;
    double total = 0.0;
    for (int bin = 1; bin < spectrumBins; ++bin)
    {
        const auto frequency = static_cast<float>(static_cast<double>(bin) * sampleRate / fftSize);
        const auto magnitude = std::pow(10.0f, metrics.spectrumDb[static_cast<size_t>(bin)] / 20.0f);
        const auto power = static_cast<double>(magnitude) * magnitude;
        total += power;
        if (frequency >= lowHz && frequency < highHz) selected += power;
    }
    return total > 1.0e-18 ? static_cast<float>(selected / total) : 0.0f;
}

void SmartEngine::addAction(SmartState& value, juce::String name, juce::String reason,
                            float amount, float confidence, int priority, float frequencyHz,
                            juce::String result, bool rolledBack)
{
    if (value.actionCount >= static_cast<int>(value.actions.size())) return;
    auto& action = value.actions[static_cast<size_t>(value.actionCount++)];
    action.name = std::move(name);
    action.reason = std::move(reason);
    action.amountDb = amount;
    action.confidence = std::clamp(confidence, 0.0f, 1.0f);
    action.frequencyHz = frequencyHz;
    action.priority = priority;
    action.active = true;
    action.result = std::move(result);
    action.rolledBack = rolledBack;
}

void SmartEngine::addProblem(SmartState& value, juce::String name, juce::String detail,
                             float severity, bool warning)
{
    if (value.problemCount >= static_cast<int>(value.problems.size())) return;
    auto& problem = value.problems[static_cast<size_t>(value.problemCount++)];
    problem.name = std::move(name);
    problem.detail = std::move(detail);
    problem.severity = std::clamp(severity, 0.0f, 1.0f);
    problem.warning = warning;
}

float SmartEngine::smooth(float current, float target, float elapsedSeconds,
                          float attackSeconds, float releaseSeconds) noexcept
{
    const auto time = std::abs(target) > std::abs(current) ? attackSeconds : releaseSeconds;
    const auto alpha = 1.0f - std::exp(-elapsedSeconds / std::max(0.01f, time));
    return current + alpha * (target - current);
}
} // namespace churchstream
