#include "AnalysisEngine.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
namespace
{
float gainToDb(float gain) noexcept
{
    return gain > 1.0e-5f ? 20.0f * std::log10(gain) : -100.0f;
}

float validLoudness(double value) noexcept
{
    return std::isfinite(value) && value > -1000.0 ? static_cast<float>(value) : -100.0f;
}
}

AnalysisEngine::AnalysisEngine()
    : Thread("CSP Analysis and Smart Thread")
{
    for (auto& value : inputState.fftData)
        value = 0.0f;
    for (auto& value : outputState.fftData)
        value = 0.0f;
}

AnalysisEngine::~AnalysisEngine()
{
    stop();
}

void AnalysisEngine::prepare(double newSampleRate)
{
    stop();
    configuredSampleRate.store(std::max(8000.0, newSampleRate), std::memory_order_release);
    inputFifo.reset();
    outputFifo.reset();
    inputState = {};
    outputState = {};
    initialiseLoudnessStates();

    {
        const juce::ScopedLock lock(snapshotLock);
        snapshot = {};
        snapshot.sampleRate = configuredSampleRate.load(std::memory_order_relaxed);
    }

    startThread(juce::Thread::Priority::low);
}

void AnalysisEngine::prepareOffline(double newSampleRate)
{
    stop();
    configuredSampleRate.store(std::max(8000.0, newSampleRate), std::memory_order_release);
    inputFifo.reset();
    outputFifo.reset();
    inputState = {};
    outputState = {};
    initialiseLoudnessStates();
    offlineFrames = 0;
    offlineConsumedInput = false;
    offlineConsumedOutput = false;

    const juce::ScopedLock lock(snapshotLock);
    snapshot = {};
    snapshot.sampleRate = configuredSampleRate.load(std::memory_order_relaxed);
}

void AnalysisEngine::pushOffline(const float* inputLeftSamples, const float* inputRightSamples,
                                 const float* outputLeftSamples, const float* outputRightSamples,
                                 int numSamples)
{
    AnalysisSnapshot next;
    {
        const juce::ScopedLock lock(snapshotLock);
        next = snapshot;
    }

    for (int offset = 0; offset < numSamples;)
    {
        const auto count = std::min(chunkCapacity, numSamples - offset);
        if (inputLeftSamples != nullptr && inputRightSamples != nullptr)
        {
            analyseChunk(inputState, next.input, inputLeftSamples + offset, inputRightSamples + offset, count);
            offlineConsumedInput = true;
        }
        if (outputLeftSamples != nullptr && outputRightSamples != nullptr)
        {
            analyseChunk(outputState, next.processed, outputLeftSamples + offset, outputRightSamples + offset, count);
            offlineConsumedOutput = true;
        }
        offset += count;
    }
    offlineFrames += static_cast<uint64_t>(numSamples);

    const juce::ScopedLock lock(snapshotLock);
    snapshot = next;
}

AnalysisSnapshot AnalysisEngine::finishOfflineUpdate(double updateRateHzToReport)
{
    AnalysisSnapshot next;
    {
        const juce::ScopedLock lock(snapshotLock);
        next = snapshot;
    }

    if (offlineConsumedInput)
    {
        calculateSpectrum(inputState, next.input);
        updateLoudnessMetrics(inputState, next.input, true);
    }
    if (offlineConsumedOutput)
    {
        calculateSpectrum(outputState, next.processed);
        updateLoudnessMetrics(outputState, next.processed, true);
    }
    offlineConsumedInput = false;
    offlineConsumedOutput = false;

    next.sampleRate = configuredSampleRate.load(std::memory_order_relaxed);
    next.analyzedFrames = offlineFrames;
    next.updateRateHz = updateRateHzToReport;
    next.context = classifyContext(next.processed);

    const juce::ScopedLock lock(snapshotLock);
    snapshot = next;
    return next;
}

void AnalysisEngine::stop()
{
    signalThreadShouldExit();
    stopThread(1500);
    destroyLoudnessStates();
}

void AnalysisEngine::push(const float* const* input, int numInputChannels,
                          const float* const* output, int numOutputChannels,
                          int numSamples) noexcept
{
    if (inputAnalysisEnabled.load(std::memory_order_relaxed))
        inputFifo.push(input, numInputChannels, numSamples);
    outputFifo.push(output, numOutputChannels, numSamples);
}

AnalysisSnapshot AnalysisEngine::getSnapshot() const
{
    const juce::ScopedLock lock(snapshotLock);
    return snapshot;
}

void AnalysisEngine::setUpdateRateHz(int rate) noexcept
{
    updateRate.store(std::clamp(rate, 2, 10), std::memory_order_release);
}

int AnalysisEngine::getUpdateRateHz() const noexcept
{
    return updateRate.load(std::memory_order_acquire);
}

void AnalysisEngine::setInputAnalysisEnabled(bool enabled) noexcept
{
    inputAnalysisEnabled.store(enabled, std::memory_order_release);
    if (!enabled) inputFifo.reset();
}

void AnalysisEngine::run()
{
    auto previousUpdateTime = juce::Time::getMillisecondCounterHiRes();
    uint64_t totalFrames = 0;

    while (!threadShouldExit())
    {
        const auto rate = getUpdateRateHz();
        wait(std::max(20, 1000 / rate));
        if (threadShouldExit())
            break;

        AnalysisSnapshot next;
        {
            const juce::ScopedLock lock(snapshotLock);
            next = snapshot;
        }

        auto consumedAny = false;
        auto consumedInput = false;
        auto consumedOutput = false;
        for (;;)
        {
            const auto inputSamples = inputAnalysisEnabled.load(std::memory_order_relaxed)
                ? inputFifo.pop(inputLeft.data(), inputRight.data(), chunkCapacity) : 0;
            const auto outputSamples = outputFifo.pop(outputLeft.data(), outputRight.data(), chunkCapacity);
            if (inputSamples == 0 && outputSamples == 0) break;
            consumedAny = true;
            if (inputSamples > 0)
            {
                consumedInput = true;
                analyseChunk(inputState, next.input, inputLeft.data(), inputRight.data(), inputSamples);
            }
            if (outputSamples > 0)
            {
                consumedOutput = true;
                analyseChunk(outputState, next.processed, outputLeft.data(), outputRight.data(), outputSamples);
            }
            totalFrames += static_cast<uint64_t>(std::max(inputSamples, outputSamples));
            if (inputSamples < chunkCapacity && outputSamples < chunkCapacity) break;
        }
        if (!consumedAny) continue;
        if (consumedInput) calculateSpectrum(inputState, next.input);
        if (consumedOutput) calculateSpectrum(outputState, next.processed);
        if (consumedInput) updateLoudnessMetrics(inputState, next.input, false);
        if (consumedOutput) updateLoudnessMetrics(outputState, next.processed, true);
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto elapsed = std::max(1.0, now - previousUpdateTime);
        previousUpdateTime = now;
        next.sampleRate = configuredSampleRate.load(std::memory_order_relaxed);
        next.analyzedFrames = totalFrames;
        next.droppedInputSamples = inputFifo.getDroppedSamples();
        next.droppedOutputSamples = outputFifo.getDroppedSamples();
        next.updateRateHz = 1000.0 / elapsed;
        next.context = classifyContext(next.processed);

        {
            const juce::ScopedLock lock(snapshotLock);
            snapshot = next;
        }
    }
}

void AnalysisEngine::destroyLoudnessStates()
{
    if (inputState.loudness != nullptr)
        ebur128_destroy(&inputState.loudness);
    if (outputState.loudness != nullptr)
        ebur128_destroy(&outputState.loudness);
}

void AnalysisEngine::initialiseLoudnessStates()
{
    const auto rate = static_cast<unsigned long>(configuredSampleRate.load(std::memory_order_relaxed));
    const auto commonModes = EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_SAMPLE_PEAK;
    // INPUT is visual/reference analysis only. Avoid a second full EBU R128
    // filter bank; all displayed loudness and Smart decisions use OUTPUT.
    inputState.loudness = nullptr;
    outputState.loudness = ebur128_init(2, rate, commonModes | EBUR128_MODE_TRUE_PEAK);
}

void AnalysisEngine::analyseChunk(StreamState& state, SignalMetrics& metrics,
                                  const float* left, const float* right, int numSamples)
{
    const auto measureTruePeak = state.loudness == nullptr;
    auto chunkTruePeak = 0.0f;
    double squareSum = 0.0;
    double leftSquare = 0.0;
    double rightSquare = 0.0;
    double crossSum = 0.0;
    auto peak = 0.0f;

    const auto envelopeRelease = std::exp(-1.0f / static_cast<float>(configuredSampleRate.load() * 0.100));
    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto l = left[sample];
        const auto r = right[sample];
        const auto mono = 0.5f * (l + r);
        peak = std::max({ peak, std::abs(l), std::abs(r) });
        if (measureTruePeak)
            chunkTruePeak = std::max({ chunkTruePeak, state.truePeak.process(0, l), state.truePeak.process(1, r) });
        squareSum += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
        leftSquare += static_cast<double>(l) * l;
        rightSquare += static_cast<double>(r) * r;
        crossSum += static_cast<double>(l) * r;

        const auto absolute = std::abs(mono);
        state.transientEnvelope = std::max(absolute, state.transientEnvelope * envelopeRelease);
        if (state.transientCooldown > 0)
            --state.transientCooldown;
        else if (absolute > 0.02f && absolute > state.transientEnvelope * 0.92f
                 && absolute > std::abs(state.previousPeakSample) * 1.8f)
        {
            ++state.transientCount;
            state.transientCooldown = static_cast<int>(configuredSampleRate.load() * 0.020);
        }
        state.previousPeakSample = mono;
        ++state.transientSamples;

        state.fftInput[static_cast<size_t>(state.fftWritePosition)] = mono;
        state.fftWritePosition = (state.fftWritePosition + 1) & (fftSize - 1);
        ++state.fftSamplesSeen;
    }

    const auto rms = static_cast<float>(std::sqrt(squareSum / static_cast<double>(numSamples)));
    metrics.peakDb = gainToDb(peak);
    metrics.rmsDb = gainToDb(rms);
    metrics.crestFactorDb = std::clamp(metrics.peakDb - metrics.rmsDb, 0.0f, 40.0f);
    metrics.dynamicRangeDb = metrics.crestFactorDb;
    const auto denominator = std::sqrt(leftSquare * rightSquare);
    metrics.stereoCorrelation = denominator > 1.0e-12
        ? std::clamp(static_cast<float>(crossSum / denominator), -1.0f, 1.0f)
        : 1.0f;
    metrics.leftRightImbalanceDb = leftSquare > 1.0e-12 && rightSquare > 1.0e-12
        ? std::clamp(static_cast<float>(10.0 * std::log10(leftSquare / rightSquare)), -30.0f, 30.0f)
        : 0.0f;
    const auto midEnergy = std::max(1.0e-12, 0.5 * (leftSquare + rightSquare + 2.0 * crossSum));
    const auto sideEnergy = std::max(0.0, 0.5 * (leftSquare + rightSquare - 2.0 * crossSum));
    metrics.stereoWidth = std::clamp(static_cast<float>(std::sqrt(sideEnergy / midEnergy)), 0.0f, 2.0f);
    const auto seconds = static_cast<double>(state.transientSamples) / configuredSampleRate.load();
    metrics.transientDensity = seconds > 0.0
        ? static_cast<float>(static_cast<double>(state.transientCount) / seconds)
        : 0.0f;

    if (measureTruePeak)
        state.chunkTruePeak = std::max(state.chunkTruePeak, chunkTruePeak);

    addLoudnessFrames(state, left, right, numSamples);
}

void AnalysisEngine::calculateSpectrum(StreamState& state, SignalMetrics& metrics)
{
    if (state.fftSamplesSeen < static_cast<uint64_t>(fftSize)) return;
    // fftWritePosition points to the oldest sample in the circular window.
    for (int sample = 0; sample < fftSize; ++sample)
        state.fftData[static_cast<size_t>(sample)] = state.fftInput[
            static_cast<size_t>((state.fftWritePosition + sample) & (fftSize - 1))];
    std::fill(state.fftData.begin() + fftSize, state.fftData.end(), 0.0f);
    window.multiplyWithWindowingTable(state.fftData.data(), fftSize);
    fft.performFrequencyOnlyForwardTransform(state.fftData.data());

    const auto rate = configuredSampleRate.load(std::memory_order_relaxed);
    std::array<double, 5> energy {};
    double totalEnergy = 0.0;
    double weightedFrequency = 0.0;
    double magnitudeSum = 0.0;

    for (int bin = 0; bin < spectrumBins; ++bin)
    {
        const auto magnitude = state.fftData[static_cast<size_t>(bin)] / static_cast<float>(fftSize * 0.5);
        metrics.spectrumDb[static_cast<size_t>(bin)] = gainToDb(magnitude);
        const auto frequency = static_cast<double>(bin) * rate / static_cast<double>(fftSize);
        const auto power = static_cast<double>(magnitude) * magnitude;
        const auto band = frequency < 120.0 ? 0 : frequency < 500.0 ? 1 : frequency < 2500.0 ? 2
            : frequency < 8000.0 ? 3 : 4;
        energy[static_cast<size_t>(band)] += power;
        totalEnergy += power;
        weightedFrequency += frequency * magnitude;
        magnitudeSum += magnitude;
    }

    for (size_t band = 0; band < energy.size(); ++band)
        metrics.bandEnergy[band] = totalEnergy > 1.0e-18
            ? static_cast<float>(energy[band] / totalEnergy)
            : 0.0f;
    metrics.spectralCentroidHz = magnitudeSum > 1.0e-12
        ? static_cast<float>(weightedFrequency / magnitudeSum)
        : 0.0f;
}

void AnalysisEngine::addLoudnessFrames(StreamState& state, const float* left, const float* right,
                                       int numSamples)
{
    if (state.loudness == nullptr)
        return;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        interleaved[static_cast<size_t>(sample * 2)] = left[sample];
        interleaved[static_cast<size_t>(sample * 2 + 1)] = right[sample];
    }
    ebur128_add_frames_float(state.loudness, interleaved.data(), static_cast<size_t>(numSamples));
}

void AnalysisEngine::updateLoudnessMetrics(StreamState& state, SignalMetrics& metrics,
                                           bool calculateTruePeak)
{
    if (state.loudness == nullptr)
    {
        // No EBU R128 filter bank on this stream; the 4x oversampled detector
        // supplies a real inter-sample reading instead of the sample peak.
        metrics.truePeakDbtp = std::max(metrics.peakDb, gainToDb(state.chunkTruePeak));
        state.chunkTruePeak = 0.0f;
        return;
    }

    double value = 0.0;
    if (ebur128_loudness_momentary(state.loudness, &value) == EBUR128_SUCCESS)
        metrics.lufsMomentary = validLoudness(value);
    if (ebur128_loudness_shortterm(state.loudness, &value) == EBUR128_SUCCESS)
        metrics.lufsShortTerm = validLoudness(value);
    if (ebur128_loudness_global(state.loudness, &value) == EBUR128_SUCCESS)
        metrics.lufsIntegrated = validLoudness(value);
    if (ebur128_loudness_range(state.loudness, &value) == EBUR128_SUCCESS)
        metrics.loudnessRange = std::isfinite(value) ? static_cast<float>(value) : 0.0f;

    if (calculateTruePeak)
    {
        double leftPeak = 0.0;
        double rightPeak = 0.0;
        if (ebur128_true_peak(state.loudness, 0, &leftPeak) == EBUR128_SUCCESS
            && ebur128_true_peak(state.loudness, 1, &rightPeak) == EBUR128_SUCCESS)
            metrics.truePeakDbtp = gainToDb(static_cast<float>(std::max(leftPeak, rightPeak)));
    }
    else
    {
        metrics.truePeakDbtp = metrics.peakDb;
    }
}

MixContext AnalysisEngine::classifyContext(const SignalMetrics& metrics) noexcept
{
    if (metrics.rmsDb < -48.0f)
        return MixContext::quiet;
    if (metrics.bandEnergy[2] + metrics.bandEnergy[3] > 0.76f
        && metrics.transientDensity < 2.2f && metrics.stereoWidth < 0.20f)
        return MixContext::soloVocal;
    if (metrics.bandEnergy[2] + metrics.bandEnergy[3] > 0.72f
        && metrics.transientDensity < 3.0f && metrics.stereoWidth < 0.35f)
        return MixContext::speech;
    if (metrics.rmsDb < -23.0f && metrics.crestFactorDb > 9.0f
        && metrics.transientDensity < 4.0f)
        return MixContext::worshipSoft;
    if (metrics.crestFactorDb < 6.0f && metrics.rmsDb > -20.0f)
        return MixContext::denseMusic;
    if (metrics.bandEnergy[0] > 0.08f && metrics.transientDensity > 2.0f)
        return MixContext::fullBand;
    if (metrics.rmsDb < -32.0f && metrics.crestFactorDb < 8.0f)
        return MixContext::ambience;
    return MixContext::music;
}
} // namespace churchstream
