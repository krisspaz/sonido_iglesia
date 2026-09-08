#pragma once

#include "AnalysisTypes.h"
#include "StereoAudioFifo.h"
#include "DSP/TruePeakDetector.h"

#include <array>
#include <ebur128.h>
#include <juce_dsp/juce_dsp.h>

namespace churchstream
{
class AnalysisEngine final : private juce::Thread
{
public:
    AnalysisEngine();
    ~AnalysisEngine() override;

    void prepare(double newSampleRate);
    void stop();

    // Deterministic offline entry point. It reuses the exact same analysis code
    // as the background thread but is driven by sample count instead of wall
    // clock, so an offline run of the same file always produces the same
    // snapshots. No thread is started.
    void prepareOffline(double newSampleRate);
    void pushOffline(const float* inputLeft, const float* inputRight,
                     const float* outputLeft, const float* outputRight, int numSamples);
    [[nodiscard]] AnalysisSnapshot finishOfflineUpdate(double updateRateHzToReport);
    void push(const float* const* input, int numInputChannels,
              const float* const* output, int numOutputChannels,
              int numSamples) noexcept;

    [[nodiscard]] AnalysisSnapshot getSnapshot() const;
    void setUpdateRateHz(int rate) noexcept;
    void setInputAnalysisEnabled(bool enabled) noexcept;
    [[nodiscard]] int getUpdateRateHz() const noexcept;

private:
    static constexpr size_t fifoCapacity = 1U << 17;
    static constexpr int chunkCapacity = 8192;
    // Half-window overlap, the usual Welch choice: with a Hann window it makes
    // the averaged estimate use every sample with equal weight.
    static constexpr int spectrumHop = fftSize / 2;

    struct StreamState
    {
        std::array<float, fftSize * 2> fftData {};
        std::array<float, fftSize> fftInput {};
        int fftWritePosition = 0;
        uint64_t fftSamplesSeen = 0;
        float previousPeakSample = 0.0f;
        float transientEnvelope = 0.0f;
        int transientCooldown = 0;
        // Transients per second over a moving window. This used to be a
        // lifetime average of two counters that were never reset: ten minutes
        // into a service a new chunk moved it by hundredths of a percent, so it
        // froze, and `classifyContext` reads it in five of its seven branches.
        // The whole context classifier therefore stopped responding partway
        // through every service.
        float transientRate = 0.0f;
        // Welch-averaged power spectrum. A single 2048-point periodogram has a
        // variance equal to its own mean, and taking one per update discarded
        // roughly two thirds of the audio as well. Every tonal decision is made
        // on this, so the noise was going straight into the corrections.
        std::array<double, spectrumBins> spectrumAccumulator {};
        int spectrumFrames = 0;
        int hopCounter = 0;
        float chunkTruePeak = 0.0f;
        TruePeakDetector truePeak;
        ebur128_state* loudness = nullptr;
    };

    void run() override;
    void destroyLoudnessStates();
    void initialiseLoudnessStates();
    void analyseChunk(StreamState& state, SignalMetrics& metrics,
                      const float* left, const float* right, int numSamples);
    void calculateSpectrum(StreamState& state, SignalMetrics& metrics);
    void accumulateSpectrum(StreamState& state);
    void addLoudnessFrames(StreamState& state, const float* left, const float* right, int numSamples);
    static void updateLoudnessMetrics(StreamState& state, SignalMetrics& metrics, bool calculateTruePeak);
    static MixContext classifyContext(const SignalMetrics& metrics) noexcept;

    StereoAudioFifo<fifoCapacity> inputFifo;
    StereoAudioFifo<fifoCapacity> outputFifo;
    std::array<float, chunkCapacity> inputLeft {};
    std::array<float, chunkCapacity> inputRight {};
    std::array<float, chunkCapacity> outputLeft {};
    std::array<float, chunkCapacity> outputRight {};
    std::array<float, chunkCapacity * 2> interleaved {};
    StreamState inputState;
    StreamState outputState;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { fftSize, juce::dsp::WindowingFunction<float>::hann, true };

    mutable juce::CriticalSection snapshotLock;
    AnalysisSnapshot snapshot;
    std::atomic<int> updateRate { 8 };
    std::atomic<bool> inputAnalysisEnabled { true };
    std::atomic<double> configuredSampleRate { 48000.0 };
    uint64_t offlineFrames = 0;
    bool offlineConsumedInput = false;
    bool offlineConsumedOutput = false;
};
} // namespace churchstream
