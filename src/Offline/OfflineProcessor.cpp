#include "OfflineProcessor.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace churchstream
{
namespace
{
constexpr int offlineBlockSize = 1024;
constexpr int offlineUpdateRateHz = 8;
constexpr size_t maximumLoggedEvents = 400;

float gainToDb(float gain) { return gain > 1.0e-5f ? 20.0f * std::log10(gain) : -100.0f; }
float dbToGain(float decibels) { return std::pow(10.0f, decibels / 20.0f); }

juce::File offlineDirectory()
{
    const auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                               .getChildFile("ChurchStreamProcessor").getChildFile("Offline Tests");
    directory.createDirectory();
    return directory;
}
}

// The simulation owns a full analysis engine, whose FFT and chunk buffers are
// far larger than a default thread stack allows.
OfflineProcessor::OfflineProcessor() : Thread("CSP Offline Processor", 1024 * 1024) {}
OfflineProcessor::~OfflineProcessor() { stop(); }

bool OfflineProcessor::startProcessing(const juce::File& source, const DspParameters& parameters,
                                       const juce::File& churchProfile)
{
    if (isThreadRunning() || !source.existsAsFile()) return false;
    sourceFile = source;
    profileSource = churchProfile;
    parameterCopy.clean = parameters.clean.load();
    parameterCopy.punch = parameters.punch.load();
    parameterCopy.clarity = parameters.clarity.load();
    parameterCopy.dynamics = parameters.dynamics.load();
    parameterCopy.warmth = parameters.warmth.load();
    parameterCopy.loudnessTarget = parameters.loudnessTarget.load();
    parameterCopy.operatingMode = parameters.operatingMode.load();
    parameterCopy.rumble = parameters.rumbleEnabled.load();
    parameterCopy.eq = parameters.adaptiveEqEnabled.load();
    parameterCopy.compressor = parameters.compressorEnabled.load();
    parameterCopy.saturation = parameters.saturationEnabled.load();
    parameterCopy.limiter = parameters.limiterEnabled.load();
    {
        const juce::ScopedLock lock(resultLock);
        result = {};
        result.running = true;
        result.sourceName = source.getFileName();
        result.stage = "Preparing";
    }
    startThread(juce::Thread::Priority::low);
    return true;
}

void OfflineProcessor::stop()
{
    signalThreadShouldExit();
    stopThread(5000);
}

OfflineResult OfflineProcessor::getResult() const
{
    const juce::ScopedLock lock(resultLock);
    return result;
}

void OfflineProcessor::run()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sourceFile));
    if (reader == nullptr) { setError("Unsupported or unreadable audio file"); return; }

    const auto outputDirectory = offlineDirectory();
    const auto timestamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    const auto baseName = sourceFile.getFileNameWithoutExtension() + "-" + timestamp;
    const auto renderFile = outputDirectory.getChildFile(baseName + "-render.tmp.wav");
    std::unique_ptr<juce::OutputStream> outputStream = renderFile.createOutputStream();
    if (outputStream == nullptr) { setError("Could not create offline output file"); return; }
    juce::WavAudioFormat wav;
    auto options = juce::AudioFormatWriterOptions().withSampleRate(reader->sampleRate)
        .withChannelLayout(juce::AudioChannelSet::stereo()).withBitsPerSample(24);
    auto writer = wav.createWriterFor(outputStream, options);
    if (writer == nullptr) { setError("Could not create WAV writer"); return; }

    // The Smart Engine writes back what it learns. Work on a copy so a test run
    // never rewrites the profile the church actually uses on Sunday.
    const auto profileCopy = outputDirectory.getChildFile("offline-profile.json");
    profileCopy.deleteFile();
    if (profileSource != juce::File() && profileSource.existsAsFile())
        profileSource.copyFileTo(profileCopy);

    auto enginePointer = std::make_unique<ProcessingEngine>();
    auto& engine = *enginePointer;
    engine.prepare(reader->sampleRate, offlineBlockSize, 2);
    auto& parameters = engine.getParameters();
    parameters.smartProcessing.store(true);
    parameters.operatingMode.store(parameterCopy.operatingMode);
    parameters.loudnessTarget.store(parameterCopy.loudnessTarget);
    parameters.clean.store(parameterCopy.clean);
    parameters.punch.store(parameterCopy.punch);
    parameters.clarity.store(parameterCopy.clarity);
    parameters.dynamics.store(parameterCopy.dynamics);
    parameters.warmth.store(parameterCopy.warmth);
    parameters.rumbleEnabled.store(parameterCopy.rumble);
    parameters.adaptiveEqEnabled.store(parameterCopy.eq);
    parameters.compressorEnabled.store(parameterCopy.compressor);
    parameters.saturationEnabled.store(parameterCopy.saturation);
    parameters.limiterEnabled.store(parameterCopy.limiter);

    auto analysisPointer = std::make_unique<AnalysisEngine>();
    auto& analysis = *analysisPointer;
    analysis.prepareOffline(reader->sampleRate);
    SmartEngine smart(analysis, engine, profileCopy);
    SafetyController safety;
    // Same calibration the operator would run at the start of a service.
    smart.startAutoTune();

    MetricAccumulator original, processed;
    const auto modes = EBUR128_MODE_I | EBUR128_MODE_SAMPLE_PEAK | EBUR128_MODE_TRUE_PEAK;
    original.loudness = ebur128_init(2, static_cast<unsigned long>(reader->sampleRate), modes);
    processed.loudness = ebur128_init(2, static_cast<unsigned long>(reader->sampleRate), modes);
    if (original.loudness == nullptr || processed.loudness == nullptr)
    {
        if (original.loudness != nullptr) ebur128_destroy(&original.loudness);
        if (processed.loudness != nullptr) ebur128_destroy(&processed.loudness);
        setError("Could not initialise offline loudness meters");
        return;
    }

    juce::AudioBuffer<float> buffer(2, offlineBlockSize);
    juce::AudioBuffer<float> inputCopy(2, offlineBlockSize);
    std::vector<float> interleaved(static_cast<size_t>(offlineBlockSize * 2));
    int64_t sourcePosition = 0;
    int64_t producedFrames = 0;
    int64_t processedTimeline = 0;
    const auto totalFrames = reader->lengthInSamples;
    const auto latency = engine.getLatencySamples();
    const auto samplesPerUpdate = std::max<int64_t>(1,
        static_cast<int64_t>(std::llround(reader->sampleRate / offlineUpdateRateHz)));
    const auto updateSeconds = static_cast<float>(samplesPerUpdate / reader->sampleRate);
    int64_t samplesSinceUpdate = 0;

    OfflineReport report;
    report.problemCount = 0;
    double scoreSum = 0.0;
    int scoreCount = 0;
    std::array<juce::String, 16> lastLoggedResult;
    std::array<juce::String, 16> lastLoggedName;
    int loggedNameCount = 0;

    const auto recordUpdate = [&](const SmartState& smartState, double timeSeconds)
    {
        if (smartState.quality.overall > 0.0f)
        {
            scoreSum += smartState.quality.overall;
            ++scoreCount;
            report.minimumScore = std::min(report.minimumScore, smartState.quality.overall);
        }
        for (int index = 0; index < smartState.problemCount; ++index)
        {
            const auto& problem = smartState.problems[static_cast<size_t>(index)];
            auto slot = -1;
            for (int existing = 0; existing < report.problemCount; ++existing)
                if (report.problemNames[static_cast<size_t>(existing)] == problem.name) slot = existing;
            if (slot < 0 && report.problemCount < static_cast<int>(report.problemNames.size()))
            {
                slot = report.problemCount++;
                report.problemNames[static_cast<size_t>(slot)] = problem.name;
            }
            if (slot >= 0 && problem.warning)
                ++report.problemSeconds[static_cast<size_t>(slot)];
        }
        for (int index = 0; index < smartState.actionCount; ++index)
        {
            const auto& action = smartState.actions[static_cast<size_t>(index)];
            auto slot = -1;
            for (int existing = 0; existing < loggedNameCount; ++existing)
                if (lastLoggedName[static_cast<size_t>(existing)] == action.name) slot = existing;
            if (slot < 0 && loggedNameCount < static_cast<int>(lastLoggedName.size()))
            {
                slot = loggedNameCount++;
                lastLoggedName[static_cast<size_t>(slot)] = action.name;
            }
            if (slot < 0) continue;
            const auto signature = action.result + (action.rolledBack ? "|R" : "|K");
            if (signature == lastLoggedResult[static_cast<size_t>(slot)]) continue;
            lastLoggedResult[static_cast<size_t>(slot)] = signature;
            if (action.result.isEmpty()) continue;

            ++report.corrections;
            if (action.rolledBack) ++report.rollbacks;
            else if (action.result.startsWith("IMPROVED")) ++report.improved;
            else if (action.result.startsWith("STABLE")) ++report.retained;
            if (report.events.size() < maximumLoggedEvents)
                report.events.push_back({ timeSeconds, action.name, action.frequencyHz, action.amountDb,
                                          action.confidence, smartState.quality.overall, action.result,
                                          action.rolledBack });
        }
    };

    setStage("Simulating Smart Engine", 0.0f);
    while (sourcePosition < totalFrames && !threadShouldExit())
    {
        const auto count = static_cast<int>(std::min<int64_t>(offlineBlockSize, totalFrames - sourcePosition));
        buffer.clear();
        reader->read(&buffer, 0, count, sourcePosition, true, true);
        if (reader->numChannels == 1) buffer.copyFrom(1, 0, buffer, 0, 0, count);
        addMetrics(original, buffer, 0, count, interleaved);
        for (int channel = 0; channel < 2; ++channel)
            inputCopy.copyFrom(channel, 0, buffer, channel, 0, count);

        float* channels[] { buffer.getWritePointer(0), buffer.getWritePointer(1) };
        engine.process(channels, 2, count);

        analysis.pushOffline(inputCopy.getReadPointer(0), inputCopy.getReadPointer(1),
                             channels[0], channels[1], count);
        samplesSinceUpdate += count;
        if (samplesSinceUpdate >= samplesPerUpdate)
        {
            samplesSinceUpdate = 0;
            const auto snapshot = analysis.finishOfflineUpdate(offlineUpdateRateHz);
            smart.processSnapshotForTesting(snapshot, updateSeconds);

            SafetyInput safetyInput;
            safetyInput.audioRunning = true;
            safetyInput.x32Connected = true;
            safetyInput.obsConnected = true;
            safetyInput.obsAudioConfigured = true;
            safetyInput.streamActive = true;
            safetyInput.compressorReductionDb = engine.getMetrics().compressorGainReductionDb.load();
            safetyInput.limiterReductionDb = engine.getMetrics().limiterGainReductionDb.load();
            safetyInput.analysis = snapshot;
            if (safety.evaluate(safetyInput, updateSeconds).requestSmartRollback)
                smart.requestSafetyRollback();

            recordUpdate(smart.getState(), static_cast<double>(sourcePosition) / reader->sampleRate);
        }

        const auto skip = static_cast<int>(std::clamp<int64_t>(latency - processedTimeline, 0, count));
        const auto available = std::min<int64_t>(count - skip, totalFrames - producedFrames);
        if (available > 0)
        {
            addMetrics(processed, buffer, skip, static_cast<int>(available), interleaved);
            writer->writeFromAudioSampleBuffer(buffer, skip, static_cast<int>(available));
            producedFrames += available;
        }
        processedTimeline += count;
        sourcePosition += count;
        const juce::ScopedLock lock(resultLock);
        result.progress = totalFrames > 0
            ? 0.75f * static_cast<float>(static_cast<double>(sourcePosition) / static_cast<double>(totalFrames)) : 0.0f;
    }

    while (producedFrames < totalFrames && !threadShouldExit())
    {
        const auto count = static_cast<int>(std::min<int64_t>(offlineBlockSize, totalFrames - producedFrames));
        buffer.clear();
        float* channels[] { buffer.getWritePointer(0), buffer.getWritePointer(1) };
        engine.process(channels, 2, count);
        addMetrics(processed, buffer, 0, count, interleaved);
        writer->writeFromAudioSampleBuffer(buffer, 0, count);
        producedFrames += count;
    }

    const auto originalMetrics = finishMetrics(original);
    const auto processedMetrics = finishMetrics(processed);
    ebur128_destroy(&original.loudness);
    ebur128_destroy(&processed.loudness);
    writer.reset();

    const auto finalState = smart.getState();
    report.durationSeconds = totalFrames > 0 ? static_cast<double>(totalFrames) / reader->sampleRate : 0.0;
    report.averageScore = scoreCount > 0 ? static_cast<float>(scoreSum / scoreCount) : 0.0f;
    if (scoreCount == 0) report.minimumScore = 0.0f;
    report.detectedProfile = profileName(finalState.profile);
    report.baselineLearned = finalState.baselineReady;

    if (threadShouldExit())
    {
        renderFile.deleteFile();
        const juce::ScopedLock lock(resultLock);
        result.running = false;
        return;
    }

    // Loudness matching. Both renders are pulled down to the quieter of the two
    // so neither file can clip and neither side wins the comparison on level.
    const auto validLoudness = originalMetrics.lufsIntegrated > -70.0f
        && processedMetrics.lufsIntegrated > -70.0f;
    const auto reference = validLoudness
        ? std::min(originalMetrics.lufsIntegrated, processedMetrics.lufsIntegrated) : 0.0f;
    report.matchGainOriginalDb = validLoudness
        ? std::clamp(reference - originalMetrics.lufsIntegrated, -24.0f, 0.0f) : 0.0f;
    report.matchGainProcessedDb = validLoudness
        ? std::clamp(reference - processedMetrics.lufsIntegrated, -24.0f, 0.0f) : 0.0f;

    setStage("Rendering loudness-matched A/B", 0.80f);
    const auto originalFile = outputDirectory.getChildFile(baseName + "-original.wav");
    const auto processedFile = outputDirectory.getChildFile(baseName + "-processed.wav");
    if (!renderMatchedPair(renderFile, formats, originalFile, processedFile,
                           report.matchGainOriginalDb, report.matchGainProcessedDb))
    {
        renderFile.deleteFile();
        setError("Could not render the loudness-matched A/B pair");
        return;
    }
    renderFile.deleteFile();

    OfflineResult finished;
    finished.running = false;
    finished.complete = true;
    finished.progress = 1.0f;
    finished.stage = "Complete";
    finished.sourceName = sourceFile.getFileName();
    finished.processedFile = processedFile;
    finished.originalFile = originalFile;
    finished.reportFile = outputDirectory.getChildFile(baseName + "-report.txt");
    finished.original = originalMetrics;
    finished.processed = processedMetrics;
    finished.report = std::move(report);
    writeReport(finished.reportFile, finished);

    {
        const juce::ScopedLock lock(resultLock);
        result = finished;
    }
    juce::Logger::writeToLog("Offline test complete: " + finished.reportFile.getFullPathName());
}

bool OfflineProcessor::renderMatchedPair(const juce::File& processedSource,
                                         juce::AudioFormatManager& formats,
                                         const juce::File& originalTarget,
                                         const juce::File& processedTarget,
                                         float originalGainDb, float processedGainDb)
{
    std::unique_ptr<juce::AudioFormatReader> originalReader(formats.createReaderFor(sourceFile));
    std::unique_ptr<juce::AudioFormatReader> processedReader(formats.createReaderFor(processedSource));
    if (originalReader == nullptr || processedReader == nullptr) return false;
    return renderWithGain(*originalReader, originalTarget, originalGainDb)
        && renderWithGain(*processedReader, processedTarget, processedGainDb);
}

bool OfflineProcessor::renderWithGain(juce::AudioFormatReader& reader, const juce::File& target,
                                      float gainDb)
{
    std::unique_ptr<juce::OutputStream> stream = target.createOutputStream();
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    auto options = juce::AudioFormatWriterOptions().withSampleRate(reader.sampleRate)
        .withChannelLayout(juce::AudioChannelSet::stereo()).withBitsPerSample(24);
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr) return false;

    const auto gain = dbToGain(gainDb);
    juce::AudioBuffer<float> buffer(2, offlineBlockSize);
    for (int64_t position = 0; position < reader.lengthInSamples;)
    {
        const auto count = static_cast<int>(std::min<int64_t>(offlineBlockSize,
                                                              reader.lengthInSamples - position));
        buffer.clear();
        reader.read(&buffer, 0, count, position, true, true);
        if (reader.numChannels == 1) buffer.copyFrom(1, 0, buffer, 0, 0, count);
        if (std::abs(gainDb) > 1.0e-4f)
            buffer.applyGain(0, count, gain);
        writer->writeFromAudioSampleBuffer(buffer, 0, count);
        position += count;
    }
    return true;
}

void OfflineProcessor::writeReport(const juce::File& target, const OfflineResult& value) const
{
    const auto& report = value.report;
    const auto minutes = static_cast<int>(report.durationSeconds) / 60;
    const auto seconds = static_cast<int>(report.durationSeconds) % 60;

    juce::String text;
    text << "OFFLINE SMART ENGINE SIMULATION\n"
         << "Source: " << value.sourceName << "\n"
         << "Duration: " << minutes << "m " << seconds << "s\n"
         << "Church profile: " << report.detectedProfile
         << (report.baselineLearned ? " (baseline learned)" : " (baseline not learned)") << "\n\n"
         << "LEVELS\n"
         << "Original:  " << juce::String(static_cast<double>(value.original.lufsIntegrated), 1) << " LUFS | "
         << juce::String(static_cast<double>(value.original.truePeakDbtp), 1) << " dBTP | DR "
         << juce::String(static_cast<double>(value.original.dynamicRangeDb), 1) << " dB\n"
         << "Processed: " << juce::String(static_cast<double>(value.processed.lufsIntegrated), 1) << " LUFS | "
         << juce::String(static_cast<double>(value.processed.truePeakDbtp), 1) << " dBTP | DR "
         << juce::String(static_cast<double>(value.processed.dynamicRangeDb), 1) << " dB\n\n"
         << "LOUDNESS-MATCHED A/B\n"
         << "Original file gain:  " << juce::String(static_cast<double>(report.matchGainOriginalDb), 2) << " dB\n"
         << "Processed file gain: " << juce::String(static_cast<double>(report.matchGainProcessedDb), 2) << " dB\n"
         << "Both files play at the same loudness, so the comparison is decided by the\n"
         << "processing and not by the level.\n\n"
         << "MIX SCORE\n"
         << "Average: " << juce::String(static_cast<double>(report.averageScore), 1) << " / 100\n"
         << "Minimum: " << juce::String(static_cast<double>(report.minimumScore), 1) << " / 100\n\n"
         << "CORRECTIONS\n"
         << "Evaluated: " << report.corrections << "\n"
         << "Improved:  " << report.improved << "\n"
         << "Retained:  " << report.retained << "\n"
         << "Rollbacks: " << report.rollbacks << "\n";
    if (report.corrections > 0)
        text << "Kept ratio: "
             << juce::String(100.0 * static_cast<double>(report.corrections - report.rollbacks)
                                 / static_cast<double>(report.corrections), 0) << "%\n";

    text << "\nTIME FLAGGED AS A PROBLEM\n";
    for (int index = 0; index < report.problemCount; ++index)
        text << report.problemNames[static_cast<size_t>(index)] << ": "
             << juce::String(static_cast<double>(report.problemSeconds[static_cast<size_t>(index)])
                                 / offlineUpdateRateHz, 1) << " s\n";

    text << "\nEVENT LOG\n";
    for (const auto& event : report.events)
    {
        const auto stamp = juce::String(static_cast<int>(event.timeSeconds) / 60).paddedLeft('0', 2)
            + ":" + juce::String(static_cast<int>(event.timeSeconds) % 60).paddedLeft('0', 2);
        text << stamp << " | " << event.action;
        if (event.frequencyHz > 0.0f) text << " | " << juce::String(static_cast<double>(event.frequencyHz), 0) << " Hz";
        text << " | " << juce::String(static_cast<double>(event.amountDb), 2) << " dB"
             << " | confidence " << juce::String(static_cast<double>(event.confidence) * 100.0, 0) << "%"
             << " | score " << juce::String(static_cast<double>(event.mixScore), 0)
             << " | " << event.result << "\n";
    }
    if (report.events.size() >= maximumLoggedEvents)
        text << "(event log truncated)\n";

    text << "\nNo audio is stored in this report.\n";
    target.replaceWithText(text);
}

void OfflineProcessor::addMetrics(MetricAccumulator& accumulator, const juce::AudioBuffer<float>& buffer,
                                  int start, int count, std::vector<float>& interleaved)
{
    const auto* left = buffer.getReadPointer(0, start);
    const auto* right = buffer.getReadPointer(1, start);
    for (int sample = 0; sample < count; ++sample)
    {
        const auto l = left[sample], r = right[sample];
        interleaved[static_cast<size_t>(sample * 2)] = l;
        interleaved[static_cast<size_t>(sample * 2 + 1)] = r;
        accumulator.peak = std::max({ accumulator.peak, std::abs(l), std::abs(r) });
        accumulator.sumSquares += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
    }
    accumulator.samples += static_cast<uint64_t>(count);
    ebur128_add_frames_float(accumulator.loudness, interleaved.data(), static_cast<size_t>(count));
}

OfflineMetrics OfflineProcessor::finishMetrics(MetricAccumulator& accumulator)
{
    OfflineMetrics metrics;
    metrics.peakDb = gainToDb(accumulator.peak);
    metrics.rmsDb = accumulator.samples > 0
        ? gainToDb(static_cast<float>(std::sqrt(accumulator.sumSquares / static_cast<double>(accumulator.samples)))) : -100.0f;
    metrics.dynamicRangeDb = std::max(0.0f, metrics.peakDb - metrics.rmsDb);
    double value = 0.0;
    if (ebur128_loudness_global(accumulator.loudness, &value) == EBUR128_SUCCESS && std::isfinite(value))
        metrics.lufsIntegrated = static_cast<float>(value);
    double left = 0.0, right = 0.0;
    if (ebur128_true_peak(accumulator.loudness, 0, &left) == EBUR128_SUCCESS
        && ebur128_true_peak(accumulator.loudness, 1, &right) == EBUR128_SUCCESS)
        metrics.truePeakDbtp = gainToDb(static_cast<float>(std::max(left, right)));
    return metrics;
}

void OfflineProcessor::setError(const juce::String& message)
{
    const juce::ScopedLock lock(resultLock);
    result.running = false;
    result.stage = "Error";
    result.error = message;
    juce::Logger::writeToLog("Offline test error: " + message);
}

void OfflineProcessor::setStage(const juce::String& stage, float progress)
{
    const juce::ScopedLock lock(resultLock);
    result.stage = stage;
    result.progress = progress;
}
} // namespace churchstream
