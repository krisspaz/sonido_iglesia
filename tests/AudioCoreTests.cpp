#include "Audio/AudioRouting.h"
#include "Audio/MeterSource.h"
#include "Analysis/AnalysisEngine.h"
#include "Analysis/Psychoacoustics.h"
#include "DSP/ProcessingEngine.h"
#include "Smart/SmartEngine.h"
#include "Offline/OfflineProcessor.h"
#include "OBS/OBSAuthentication.h"
#include "Safety/SafetyController.h"
#include "Groups/SmartMaskingController.h"
#include "Groups/AutoGroupRouter.h"
#include "DSP/TruePeakDetector.h"
#include "Groups/GroupMixer.h"
#include "X32/X32Client.h"
#include "Room/RoomCalibration.h"

#include <ebur128.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <algorithm>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

namespace
{
int failures = 0;

// Test fixtures own large DSP objects. Keeping each case out of line stops the
// optimiser from merging every fixture into one main() frame, which overflows
// the stack once AddressSanitizer adds its redzones.
#if defined(_MSC_VER)
 #define CSP_TEST_CASE __declspec(noinline)
#else
 #define CSP_TEST_CASE __attribute__((noinline))
#endif

void expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool approximately(float actual, float expected, float tolerance = 1.0e-5f)
{
    return std::abs(actual - expected) <= tolerance;
}

CSP_TEST_CASE void testStereoPassthrough()
{
    constexpr int samples = 8;
    float left[samples] { -0.8f, -0.4f, 0.0f, 0.2f, 0.7f, 0.1f, -0.3f, 0.5f };
    float right[samples] { 0.1f, 0.2f, 0.3f, 0.4f, -0.5f, -0.6f, 0.7f, 0.8f };
    float outputLeft[samples] {};
    float outputRight[samples] {};
    const float* inputs[] { left, right };
    float* outputs[] { outputLeft, outputRight };

    churchstream::AudioRouting::passthrough(inputs, 2, outputs, 2, samples);
    expect(std::equal(std::begin(left), std::end(left), std::begin(outputLeft)),
           "left channel must be copied bit-for-bit");
    expect(std::equal(std::begin(right), std::end(right), std::begin(outputRight)),
           "right channel must be copied bit-for-bit");
}

CSP_TEST_CASE void testMonoDuplicationAndSilence()
{
    constexpr int samples = 4;
    float mono[samples] { 0.25f, -0.25f, 0.5f, -0.5f };
    float left[samples] {};
    float right[samples] {};
    const float* inputs[] { mono };
    float* outputs[] { left, right };

    churchstream::AudioRouting::passthrough(inputs, 1, outputs, 2, samples);
    expect(std::equal(std::begin(mono), std::end(mono), std::begin(left)),
           "mono must feed the left output");
    expect(std::equal(std::begin(mono), std::end(mono), std::begin(right)),
           "mono must be duplicated to the right output");

    churchstream::AudioRouting::passthrough(nullptr, 0, outputs, 2, samples);
    expect(std::all_of(std::begin(left), std::end(left), [](float value) { return value == 0.0f; }),
           "missing input must produce silence");
    expect(std::all_of(std::begin(right), std::end(right), [](float value) { return value == 0.0f; }),
           "missing input must clear every output");
}

CSP_TEST_CASE void testMeterValues()
{
    constexpr int samples = 4;
    float left[samples] { 0.5f, -0.5f, 0.5f, -0.5f };
    float right[samples] { 0.0f, 0.0f, 1.0f, 0.0f };
    const float* inputs[] { left, right };
    churchstream::MeterSource meter;
    meter.push(inputs, 2, samples);

    expect(approximately(meter.getPeak(0), 0.5f), "left peak must come from real samples");
    expect(approximately(meter.getRms(0), 0.5f), "left RMS must come from real samples");
    expect(approximately(meter.getPeak(1), 1.0f), "right peak must detect full scale");
    expect(approximately(meter.getRms(1), 0.5f), "right RMS must be mathematically correct");
}

CSP_TEST_CASE void testLimiterAndBypassAreReal()
{
    constexpr int blockSize = 256;
    churchstream::ProcessingEngine engine;
    engine.prepare(48000.0, blockSize, 2);
    auto& parameters = engine.getParameters();
    parameters.clean.store(0.0f);
    parameters.clarity.store(0.0f);
    parameters.dynamics.store(0.0f);
    parameters.warmth.store(0.0f);
    parameters.smartProcessing.store(false);
    parameters.compressorEnabled.store(false);
    parameters.saturationEnabled.store(false);

    std::vector<float> left(blockSize, 0.0f);
    std::vector<float> right(blockSize, 0.0f);
    float* channels[] { left.data(), right.data() };
    double phase = 0.0;
    const auto phaseStep = 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;

    // Run enough blocks to fill the lookahead and complete the wet crossfade.
    for (int block = 0; block < 16; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            left[static_cast<size_t>(sample)] = 1.5f * static_cast<float>(std::sin(phase));
            right[static_cast<size_t>(sample)] = -left[static_cast<size_t>(sample)];
            phase += phaseStep;
        }
        engine.process(channels, 2, blockSize);
    }

    const auto maximum = std::max(*std::max_element(left.begin(), left.end()),
                                  std::abs(*std::min_element(right.begin(), right.end())));
    expect(maximum <= 0.892f, "enabled limiter must respect the -1 dBFS safety ceiling");
    expect(engine.getMetrics().limiterGainReductionDb.load() > 0.0f,
           "limiter gain reduction must be measured from processing");

    parameters.bypass.store(true);
    for (int block = 0; block < 16; ++block)
    {
        std::fill(left.begin(), left.end(), 0.25f);
        std::fill(right.begin(), right.end(), -0.25f);
        engine.process(channels, 2, blockSize);
    }
    expect(approximately(left.back(), 0.25f, 1.0e-4f), "bypass must crossfade to the delayed original signal");
    expect(approximately(right.back(), -0.25f, 1.0e-4f), "bypass must preserve stereo polarity");
}

CSP_TEST_CASE void testFourBandRecombinationIsLevelNeutral()
{
    constexpr int blockSize = 256;
    constexpr double sampleRate = 48000.0;
    churchstream::ProcessingEngine engine;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    parameters.smartProcessing.store(false);
    parameters.rumbleEnabled.store(false);
    parameters.adaptiveEqEnabled.store(false);
    parameters.compressorEnabled.store(false);
    parameters.saturationEnabled.store(false);
    parameters.limiterEnabled.store(false);

    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    double phase = 0.0;
    const auto phaseStep = 2.0 * 3.14159265358979323846 * 1000.0 / sampleRate;
    for (int block = 0; block < 80; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            left[static_cast<size_t>(sample)] = 0.2f * static_cast<float>(std::sin(phase));
            right[static_cast<size_t>(sample)] = left[static_cast<size_t>(sample)];
            phase += phaseStep;
        }
        engine.process(channels, 2, blockSize);
    }
    double squareSum = 0.0;
    for (const auto sample : left) squareSum += static_cast<double>(sample) * sample;
    const auto rms = static_cast<float>(std::sqrt(squareSum / blockSize));
    expect(approximately(rms, 0.2f / std::sqrt(2.0f), 0.004f),
           "four-band crossover must recombine with level-neutral magnitude");
}

CSP_TEST_CASE void testSampleRatesBuffersAndLiveChanges()
{
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto blockSize : { 64, 128, 256, 512, 1024 })
        {
            churchstream::ProcessingEngine engine;
            engine.prepare(sampleRate, blockSize, 2);
            auto& parameters = engine.getParameters();
            std::vector<float> left(static_cast<size_t>(blockSize));
            std::vector<float> right(static_cast<size_t>(blockSize));
            float* channels[] { left.data(), right.data() };
            double phase = 0.0;
            const auto step = 2.0 * 3.14159265358979323846 * 997.0 / sampleRate;
            for (int block = 0; block < 80; ++block)
            {
                parameters.clean.store(static_cast<float>((block * 17) % 101) / 100.0f);
                parameters.punch.store(static_cast<float>((block * 23) % 101) / 100.0f);
                parameters.clarity.store(static_cast<float>((block * 31) % 101) / 100.0f);
                parameters.dynamics.store(static_cast<float>((block * 43) % 101) / 100.0f);
                if (block % 11 == 0) parameters.bypass.store(!parameters.bypass.load());
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    left[static_cast<size_t>(sample)] = 0.7f * static_cast<float>(std::sin(phase));
                    right[static_cast<size_t>(sample)] = 0.6f * static_cast<float>(std::sin(phase * 1.003));
                    phase += step;
                }
                engine.process(channels, 2, blockSize);
                expect(std::all_of(left.begin(), left.end(), [](float value) { return std::isfinite(value); }),
                       "live parameter changes must never produce non-finite left samples");
                expect(std::all_of(right.begin(), right.end(), [](float value) { return std::isfinite(value); }),
                       "live parameter changes must never produce non-finite right samples");
            }
        }
    }
}

CSP_TEST_CASE void testRealFftLoudnessAndStereoAnalysis()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    churchstream::AnalysisEngine analysis;
    analysis.prepare(sampleRate);
    analysis.setUpdateRateHz(10);
    std::vector<float> left(blockSize), right(blockSize);
    const float* channels[] { left.data(), right.data() };
    double phase = 0.0;
    const auto step = 2.0 * 3.14159265358979323846 * 1000.0 / sampleRate;
    for (int block = 0; block < 240; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.1f * static_cast<float>(std::sin(phase));
            left[static_cast<size_t>(sample)] = value;
            right[static_cast<size_t>(sample)] = value;
            phase += step;
        }
        analysis.push(channels, 2, channels, 2, blockSize);
    }
    juce::Thread::sleep(1200);
    const auto snapshot = analysis.getSnapshot();
    analysis.stop();

    expect(snapshot.analyzedFrames >= 48000, "analysis thread must consume real queued samples");
    expect(snapshot.processed.rmsDb > -24.0f && snapshot.processed.rmsDb < -21.0f,
           "RMS analyzer must measure a -20 dBFS sine correctly");
    expect(snapshot.processed.lufsIntegrated > -23.0f && snapshot.processed.lufsIntegrated < -17.0f,
           "EBU R128 integrated loudness must be finite and plausible for the reference sine");
    expect(snapshot.processed.stereoCorrelation > 0.99f,
           "identical stereo channels must measure near +1 correlation");

    const auto expectedBin = static_cast<int>(std::round(1000.0 * churchstream::fftSize / sampleRate));
    auto strongestBin = 1;
    for (int bin = 2; bin < churchstream::spectrumBins; ++bin)
        if (snapshot.processed.spectrumDb[static_cast<size_t>(bin)]
            > snapshot.processed.spectrumDb[static_cast<size_t>(strongestBin)])
            strongestBin = bin;
    expect(std::abs(strongestBin - expectedBin) <= 2,
           "2048-point FFT must locate the real 1 kHz tone");
}

CSP_TEST_CASE void testSmartEnginePersistenceConfidenceAndLimits()
{
    churchstream::AnalysisEngine analysis;
    churchstream::ProcessingEngine processing;
    processing.prepare(48000.0, 256, 2);
    churchstream::SmartEngine smart(analysis, processing);

    churchstream::AnalysisSnapshot snapshot;
    snapshot.sampleRate = 48000.0;
    snapshot.processed.rmsDb = -18.0f;
    snapshot.processed.lufsShortTerm = -14.0f;
    snapshot.processed.crestFactorDb = 10.0f;
    snapshot.processed.bandEnergy = { 0.42f, 0.18f, 0.25f, 0.11f, 0.04f };
    snapshot.processed.stereoCorrelation = 0.8f;

    // A single instant must not trigger tonal correction.
    smart.processSnapshotForTesting(snapshot, 0.1f);
    expect(processing.getAdaptiveTargets().lowGainDb.load() == 0.0f,
           "Smart Engine must ignore a single low-end event");

    // Persistent evidence crosses the confidence threshold and ramps slowly.
    for (int update = 0; update < 60; ++update)
        smart.processSnapshotForTesting(snapshot, 0.1f);

    const auto state = smart.getState();
    const auto lowTarget = processing.getAdaptiveTargets().lowGainDb.load();
    expect(lowTarget < -0.05f, "persistent low-end excess must produce a real adaptive reduction");
    expect(lowTarget >= -1.5f, "automatic low-end reduction must respect its safe limit");
    expect(state.actionCount > 0, "Smart Engine must publish transparent actions");
    expect(state.actions[0].confidence >= 0.0f && state.actions[0].confidence <= 1.0f,
           "Smart confidence must remain normalised");

    snapshot.processed.bandEnergy = { 0.12f, 0.24f, 0.35f, 0.23f, 0.06f };
    const auto beforeRelease = lowTarget;
    smart.processSnapshotForTesting(snapshot, 0.1f);
    expect(processing.getAdaptiveTargets().lowGainDb.load() < 0.0f
           && processing.getAdaptiveTargets().lowGainDb.load() >= beforeRelease,
           "adaptive corrections must release gradually, never jump to zero");
}

CSP_TEST_CASE void testAutoTuneBuildsRealBaseline()
{
    churchstream::AnalysisEngine analysis;
    churchstream::ProcessingEngine processing;
    processing.prepare(48000.0, 256, 2);
    churchstream::SmartEngine smart(analysis, processing);
    churchstream::AnalysisSnapshot snapshot;
    snapshot.sampleRate = 48000.0;
    snapshot.processed.rmsDb = -18.0f;
    snapshot.processed.lufsShortTerm = -14.5f;
    snapshot.processed.crestFactorDb = 9.0f;
    snapshot.processed.spectralCentroidHz = 3600.0f;
    snapshot.processed.stereoWidth = 0.6f;
    snapshot.processed.stereoCorrelation = 0.8f;
    snapshot.processed.bandEnergy = { 0.08f, 0.18f, 0.30f, 0.32f, 0.12f };

    smart.startAutoTune();
    auto quiet = snapshot;
    quiet.processed.rmsDb = -80.0f;
    smart.processSnapshotForTesting(quiet, 0.5f);
    expect(smart.getState().autoTuneProgress == 0.0f,
           "Auto Tune must not learn a silent or disconnected input");

    for (int update = 0; update < 260; ++update)
        smart.processSnapshotForTesting(snapshot, 0.1f);

    const auto state = smart.getState();
    expect(state.autoTuneState == churchstream::AutoTuneState::complete,
           "Auto Tune must complete after approximately 25 seconds of valid audio");
    expect(state.baselineReady, "Auto Tune must publish a real mix baseline");
    expect(state.profile == churchstream::MixProfile::bright,
           "Auto Tune profile must be derived from measured band energy");
}

CSP_TEST_CASE void testSmartQualityClosedLoopAndRollback()
{
    churchstream::AnalysisEngine analysis;
    churchstream::ProcessingEngine processing;
    processing.prepare(48000.0, 256, 2);
    churchstream::SmartEngine smart(analysis, processing);
    churchstream::AnalysisSnapshot snapshot;
    snapshot.sampleRate = 48000.0;
    snapshot.processed.rmsDb = -18.0f;
    snapshot.processed.lufsShortTerm = -14.0f;
    snapshot.processed.truePeakDbtp = -1.2f;
    snapshot.processed.crestFactorDb = 10.0f;
    snapshot.processed.stereoCorrelation = 0.8f;
    snapshot.processed.bandEnergy = { 0.12f, 0.45f, 0.25f, 0.12f, 0.06f };

    for (int update = 0; update < 30; ++update)
        smart.processSnapshotForTesting(snapshot, 0.1f);
    auto state = smart.getState();
    expect(state.quality.overall > 0.0f && state.quality.overall <= 100.0f,
           "Smart Engine 2.0 must publish a normalised stream quality score");
    expect(state.problemCount >= 5, "quality evaluator must publish explicit mix checks");

    snapshot.processed.bandEnergy = { 0.05f, 0.75f, 0.08f, 0.08f, 0.04f };
    for (int update = 0; update < 50; ++update)
        smart.processSnapshotForTesting(snapshot, 0.1f);
    state = smart.getState();
    expect(state.rollbackCount > 0,
           "closed-loop evaluation must rollback when severity and mix score deteriorate");
}

CSP_TEST_CASE void testSafetyControllerAndSmartMasking()
{
    churchstream::SafetyController safety;
    churchstream::SafetyInput input;
    input.audioRunning = true;
    input.x32Connected = true;
    input.obsConnected = true;
    input.obsAudioConfigured = true;
    input.streamActive = true;
    input.analysis.processed.rmsDb = -14.0f;
    input.analysis.processed.stereoCorrelation = 0.8f;
    input.compressorReductionDb = 8.0f;
    input.limiterReductionDb = 4.0f;
    churchstream::SafetyState safetyState;
    auto rollbackRequested = false;
    for (int second = 0; second < 3; ++second)
    {
        safetyState = safety.evaluate(input, 1.0f);
        rollbackRequested = rollbackRequested || safetyState.requestSmartRollback;
    }
    expect(rollbackRequested,
           "Safety Controller must rollback sustained excessive processing");

    namespace psy = churchstream::psychoacoustics;
    churchstream::SmartMaskingController masking;
    churchstream::GroupFeatures voice, music;
    voice.rmsDb = -18.0f;
    voice.voiceProbability = 0.95f;
    voice.bandLevelDb.fill(-30.0f);

    const auto settle = [&](const churchstream::GroupFeatures& v,
                            const churchstream::GroupFeatures& m) {
        masking.reset();
        churchstream::MaskingDecision result;
        for (int update = 0; update < 60; ++update)
            result = masking.update(v, m, 0.1f);
        return result;
    };

    // Music well below the voice: intelligibility is intact and nothing may be
    // touched, however loud the music happens to be overall.
    music.rmsDb = -15.0f;
    music.bandLevelDb.fill(-55.0f);
    auto decision = settle(voice, music);
    expect(decision.speechIntelligibility > 0.95f,
           "a voice clear of the music must score a high SII");
    expect(!decision.active,
           "Smart Masking must do nothing while the preaching is already intelligible");

    // Music over the voice across the consonant range: the SII collapses and
    // the music must give way.
    music.bandLevelDb.fill(-20.0f);
    decision = settle(voice, music);
    expect(decision.speechIntelligibility < 0.45f,
           "music above the voice must be measured as destroying intelligibility");
    expect(decision.active, "Smart Masking must engage once the SII falls below target");
    for (const auto gainDb : decision.musicGainDb)
        expect(gainDb <= 0.0f && gainDb >= -churchstream::SmartMaskingController::maximumReductionDb,
               "masking may only reduce, and only within its limit");

    // The case that decides whether the model is doing anything a simple
    // per-band ducker could not. Music sits only at 1.6-1.85 kHz, in the
    // second application zone. The voice it buries is at 2.5-2.9 kHz, three
    // Bark higher, in the third zone -- masked entirely by upward spreading.
    // The reduction must land on the music that is doing the masking, in zone
    // 1. Attenuating zone 3, where the masked voice is, would remove music
    // that is not there and leave the masker untouched.
    voice.bandLevelDb.fill(-100.0f);
    voice.bandLevelDb[13] = voice.bandLevelDb[14] = -30.0f;
    music.bandLevelDb.fill(-100.0f);
    music.bandLevelDb[10] = music.bandLevelDb[11] = 10.0f;
    decision = settle(voice, music);
    expect(decision.speechIntelligibility < 0.75f,
           "a masker three Bark below the voice must still be measured as masking it");
    expect(decision.musicGainDb[1] < decision.musicGainDb[2] - 0.5f,
           "the reduction must land on the band doing the masking, not the band being masked");
    expect(decision.musicGainDb[1] < -0.5f,
           "the masking band must actually be reduced");
}

CSP_TEST_CASE void testAutomaticMultigroupRouting()
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    churchstream::AutoGroupRouter router;
    router.prepare(sampleRate);
    // These names are what the network integration reads from X32 buses or
    // matrices. Acoustic evidence still has to remain stable before READY.
    router.setCandidateName(0, "ROOM AMBIENCE");
    router.setCandidateName(1, "BAND MUSIC");
    router.setCandidateName(2, "PASTOR VOZ");
    router.setCandidateName(3, "SPARE");

    std::array<std::array<float, blockSize>, 8> audio {};
    std::array<const float*, 8> channels {};
    for (size_t channel = 0; channel < channels.size(); ++channel)
        channels[channel] = audio[channel].data();

    double phaseVoice = 0.0, phaseMusicLow = 0.0, phaseMusicHigh = 0.0, phaseRoom = 0.0;
    for (int block = 0; block < 1400; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto voice = 0.10f * static_cast<float>(std::sin(phaseVoice));
            audio[4][sample] = voice;
            audio[5][sample] = voice * 0.98f;
            const auto musicL = 0.10f * static_cast<float>(std::sin(phaseMusicLow))
                + 0.04f * static_cast<float>(std::sin(phaseMusicHigh));
            const auto musicR = 0.10f * static_cast<float>(std::sin(phaseMusicLow + 0.6))
                + 0.04f * static_cast<float>(std::sin(phaseMusicHigh + 1.1));
            audio[2][sample] = musicL;
            audio[3][sample] = musicR;
            audio[0][sample] = 0.018f * static_cast<float>(std::sin(phaseRoom));
            audio[1][sample] = 0.016f * static_cast<float>(std::sin(phaseRoom * 1.017 + 1.3));
            audio[6][sample] = audio[7][sample] = 0.00001f;
            phaseVoice += 2.0 * 3.14159265358979323846 * 1250.0 / sampleRate;
            phaseMusicLow += 2.0 * 3.14159265358979323846 * 110.0 / sampleRate;
            phaseMusicHigh += 2.0 * 3.14159265358979323846 * 5200.0 / sampleRate;
            phaseRoom += 2.0 * 3.14159265358979323846 * 3600.0 / sampleRate;
        }
        router.process(channels.data(), static_cast<int>(channels.size()), blockSize);
    }

    const auto state = router.getSnapshot();
    if (state.phase != churchstream::AutoRoutePhase::ready)
    {
        std::cerr << "AutoGroupRouter diagnostics: phase=" << static_cast<int>(state.phase)
                  << " seconds=" << state.analysedSeconds
                  << " confidence=" << state.confidence[0] << ',' << state.confidence[1]
                  << ',' << state.confidence[2] << '\n';
    }
    expect(state.phase == churchstream::AutoRoutePhase::ready,
           "multigroup routing must require stable audio before becoming ready");
    expect(state.routes.voice.leftChannel == 4 && state.routes.music.leftChannel == 2
               && state.routes.ambience.leftChannel == 0,
           "multigroup routing must map named and acoustically confirmed stems");
    expect(*std::min_element(state.confidence.begin(), state.confidence.end()) >= 0.52f,
           "every automatic stem assignment must clear the safety confidence floor");
}

float renderToneRms(float frequency, float amplitude, const std::function<void(churchstream::ProcessingEngine&)>& configure)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    churchstream::ProcessingEngine engine;
    engine.prepare(sampleRate, blockSize, 2);
    auto& p = engine.getParameters();
    p.rumbleEnabled.store(false);
    p.adaptiveEqEnabled.store(true);
    p.compressorEnabled.store(false);
    p.saturationEnabled.store(false);
    p.limiterEnabled.store(false);
    p.clean.store(0.0f);
    p.clarity.store(0.0f);
    p.warmth.store(0.0f);
    p.smartProcessing.store(false);
    configure(engine);

    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    auto phase = 0.0;
    const auto step = 2.0 * 3.14159265358979323846 * frequency / sampleRate;
    double squares = 0.0;
    for (int block = 0; block < 320; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = amplitude * static_cast<float>(std::sin(phase));
            left[static_cast<size_t>(sample)] = value;
            right[static_cast<size_t>(sample)] = value;
            phase += step;
        }
        engine.process(channels, 2, blockSize);
        if (block >= 280)
            for (const auto value : left) squares += static_cast<double>(value) * value;
    }
    return static_cast<float>(std::sqrt(squares / (40.0 * blockSize)));
}

CSP_TEST_CASE void testEqDynamicEqAndCompressorBehaviour()
{
    const auto neutralPresence = renderToneRms(2600.0f, 0.15f, [](auto&) {});
    const auto clearPresence = renderToneRms(2600.0f, 0.15f, [](auto& engine)
    {
        engine.getParameters().clarity.store(1.0f);
    });
    expect(clearPresence > neutralPresence * 1.06f,
           "CLARITY must produce measured presence gain in the real EQ");

    const auto neutralHarsh = renderToneRms(5200.0f, 0.15f, [](auto&) {});
    const auto controlledHarsh = renderToneRms(5200.0f, 0.15f, [](auto& engine)
    {
        engine.getParameters().smartProcessing.store(true);
        engine.getAdaptiveTargets().harshGainDb.store(-2.0f);
    });
    expect(controlledHarsh < neutralHarsh * 0.93f,
           "dynamic harshness target must measurably reduce high-mid energy");

    churchstream::ProcessingEngine compressor;
    compressor.prepare(48000.0, 256, 2);
    auto& p = compressor.getParameters();
    p.smartProcessing.store(false);
    p.rumbleEnabled.store(false);
    p.adaptiveEqEnabled.store(false);
    p.saturationEnabled.store(false);
    p.limiterEnabled.store(false);
    p.compressorEnabled.store(true);
    p.dynamics.store(1.0f);
    std::vector<float> left(256), right(256);
    float* channels[] { left.data(), right.data() };
    auto phase = 0.0;
    const auto step = 2.0 * 3.14159265358979323846 * 220.0 / 48000.0;
    for (int block = 0; block < 240; ++block)
    {
        for (int sample = 0; sample < 256; ++sample)
        {
            left[static_cast<size_t>(sample)] = 0.7f * static_cast<float>(std::sin(phase));
            right[static_cast<size_t>(sample)] = left[static_cast<size_t>(sample)];
            phase += step;
        }
        compressor.process(channels, 2, 256);
    }
    expect(compressor.getMetrics().compressorGainReductionDb.load() > 0.5f,
           "4-band compressor must report gain reduction from real processing");
}

CSP_TEST_CASE void testBroadcastLevelerStabilisesStereoProgramme()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 256;
    churchstream::ProcessingEngine engine;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    parameters.smartProcessing.store(false);
    parameters.rumbleEnabled.store(false);
    parameters.adaptiveEqEnabled.store(false);
    parameters.compressorEnabled.store(false);
    parameters.saturationEnabled.store(false);
    parameters.limiterEnabled.store(false);
    parameters.broadcastLevelerEnabled.store(true);

    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    auto phase = 0.0;
    const auto step = 2.0 * juce::MathConstants<double>::pi * 1300.0 / sampleRate;
    double quietSquares = 0.0;
    for (int block = 0; block < 2200; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.010f * static_cast<float>(std::sin(phase));
            left[static_cast<size_t>(sample)] = value;
            right[static_cast<size_t>(sample)] = value;
            phase += step;
        }
        engine.process(channels, 2, blockSize);
        if (block >= 2100)
            for (const auto value : left) quietSquares += static_cast<double>(value) * value;
    }
    const auto quietRms = static_cast<float>(std::sqrt(quietSquares / (100.0 * blockSize)));
    expect(engine.getMetrics().broadcastLevelGainDb.load() > 12.0f,
           "Broadcast leveler must recover a consistently quiet stereo programme within its +15 dB limit");
    expect(quietRms > 0.030f,
           "Broadcast leveler must audibly raise the quiet programme rather than only reporting it");

    for (int block = 0; block < 1000; ++block)
    {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        engine.process(channels, 2, blockSize);
    }
    expect(std::abs(engine.getMetrics().broadcastLevelGainDb.load()) < 0.5f,
           "Broadcast leveler must return to unity during extended silence instead of preparing to raise room noise");
}

CSP_TEST_CASE void testOfflineFileProcessing()
{
    constexpr int sampleRate = 48000;
    const auto inputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("csp-offline-test", ".wav", false);
    {
        std::unique_ptr<juce::OutputStream> stream = inputFile.createOutputStream();
        juce::WavAudioFormat wav;
        auto options = juce::AudioFormatWriterOptions().withSampleRate(sampleRate)
            .withChannelLayout(juce::AudioChannelSet::stereo()).withBitsPerSample(24);
        auto writer = wav.createWriterFor(stream, options);
        expect(writer != nullptr, "offline test fixture WAV writer must open");
        if (writer != nullptr)
        {
            juce::AudioBuffer<float> audio(2, sampleRate);
            for (int sample = 0; sample < sampleRate; ++sample)
            {
                const auto value = 0.2f * std::sin(static_cast<float>(sample) * 2.0f
                                                   * juce::MathConstants<float>::pi * 440.0f / sampleRate);
                audio.setSample(0, sample, value);
                audio.setSample(1, sample, value);
            }
            writer->writeFromAudioSampleBuffer(audio, 0, sampleRate);
        }
    }

    churchstream::ProcessingEngine settingsSource;
    settingsSource.prepare(sampleRate, 1024, 2);
    churchstream::OfflineProcessor offline;
    expect(offline.startProcessing(inputFile, settingsSource.getParameters()),
           "Offline Test must accept a real local WAV");
    for (int attempt = 0; attempt < 100 && offline.getResult().running; ++attempt)
        juce::Thread::sleep(20);
    const auto result = offline.getResult();
    offline.stop();
    expect(result.complete && result.error.isEmpty(), "Offline Test must finish without error");
    expect(result.processedFile.existsAsFile(), "Offline Test must render a real processed WAV");
    expect(result.original.lufsIntegrated > -100.0f && result.processed.lufsIntegrated > -100.0f,
           "Offline Test metrics must be measured, not placeholders");
    inputFile.deleteFile();
    if (result.processedFile.existsAsFile()) result.processedFile.deleteFile();
}


float measureIntegratedLoudness(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) return -100.0f;

    auto* meter = ebur128_init(2, static_cast<unsigned long>(reader->sampleRate), EBUR128_MODE_I);
    if (meter == nullptr) return -100.0f;

    constexpr int blockSize = 4096;
    juce::AudioBuffer<float> buffer(2, blockSize);
    std::vector<float> interleaved(static_cast<size_t>(blockSize * 2));
    for (int64_t position = 0; position < reader->lengthInSamples;)
    {
        const auto count = static_cast<int>(std::min<int64_t>(blockSize, reader->lengthInSamples - position));
        buffer.clear();
        reader->read(&buffer, 0, count, position, true, true);
        for (int sample = 0; sample < count; ++sample)
        {
            interleaved[static_cast<size_t>(sample * 2)] = buffer.getSample(0, sample);
            interleaved[static_cast<size_t>(sample * 2 + 1)] = buffer.getSample(1, sample);
        }
        ebur128_add_frames_float(meter, interleaved.data(), static_cast<size_t>(count));
        position += count;
    }
    double value = -100.0;
    ebur128_loudness_global(meter, &value);
    ebur128_destroy(&meter);
    return std::isfinite(value) ? static_cast<float>(value) : -100.0f;
}

float blockRmsDb(const juce::AudioBuffer<float>& buffer, int start, int count)
{
    double sum = 0.0;
    for (int sample = start; sample < start + count; ++sample)
        sum += 0.5 * (static_cast<double>(buffer.getSample(0, sample)) * buffer.getSample(0, sample)
                      + static_cast<double>(buffer.getSample(1, sample)) * buffer.getSample(1, sample));
    const auto rms = std::sqrt(sum / std::max(1, count));
    return rms > 1.0e-9 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f;
}

CSP_TEST_CASE void testPsychoacousticModels()
{
    namespace psy = churchstream::psychoacoustics;

    // Zwicker's scale against its published anchors.
    expect(approximately(psy::barkFromHertz(0.0f), 0.0f, 1.0e-4f), "0 Hz must be 0 Bark");
    expect(approximately(psy::barkFromHertz(1000.0f), 8.51f, 0.02f),
           "1 kHz must land at 8.5 Bark");
    expect(psy::barkFromHertz(4000.0f) > 17.0f && psy::barkFromHertz(4000.0f) < 18.5f,
           "4 kHz must land near 17.9 Bark");
    // Monotonic and compressive: an octave is worth fewer Bark the higher it is.
    expect(psy::barkFromHertz(2000.0f) - psy::barkFromHertz(1000.0f)
               > psy::barkFromHertz(8000.0f) - psy::barkFromHertz(4000.0f),
           "the Bark scale must compress towards high frequencies");
    expect(approximately(psy::hertzFromBark(psy::barkFromHertz(2500.0f)), 2500.0f, 1.0f),
           "the Bark inversion must round-trip");

    // Schroeder's spreading function peaks on the masker and is asymmetric.
    expect(approximately(psy::spreadingDb(0.0f), 0.0f, 0.05f),
           "a masker must mask its own band at roughly 0 dB");
    expect(psy::spreadingDb(1.0f) > psy::spreadingDb(-1.0f),
           "masking must reach further upwards in frequency than downwards");
    expect(psy::spreadingDb(4.0f) < -20.0f && psy::spreadingDb(-4.0f) < -30.0f,
           "masking must decay away from the masker on both sides");

    // A single loud band must project a threshold onto its neighbours that
    // falls off, rather than covering the whole spectrum equally.
    std::array<float, psy::criticalBandCount> masker {};
    masker.fill(-120.0f);
    masker[7] = 0.0f;  // 1 kHz
    std::array<float, psy::criticalBandCount> threshold {};
    psy::maskingThresholdDb(masker, threshold);
    expect(approximately(threshold[7], 0.0f, 0.5f),
           "the masking threshold at the masker must be the masker's own level");
    expect(threshold[8] > threshold[10] && threshold[10] > threshold[13],
           "the projected threshold must fall with distance from the masker");
    expect(threshold[13] < -20.0f,
           "a 1 kHz masker must not be masking 2.5 kHz at full strength");

    // SII endpoints. The weights sum to one, so a fully audible voice is 1.0.
    std::array<float, psy::criticalBandCount> speech {};
    std::array<float, psy::criticalBandCount> noise {};
    speech.fill(0.0f);
    noise.fill(-40.0f);
    expect(approximately(psy::speechIntelligibilityIndex(speech, noise), 1.0f, 1.0e-3f),
           "speech far above the masker must score a full SII");
    noise.fill(40.0f);
    expect(approximately(psy::speechIntelligibilityIndex(speech, noise), 0.0f, 1.0e-3f),
           "speech buried under the masker must score zero");
    noise.fill(0.0f);
    expect(approximately(psy::speechIntelligibilityIndex(speech, noise), 0.5f, 1.0e-3f),
           "speech level with the masker must sit at the middle of the audibility ramp");

    // Losing the 1-4 kHz consonant region must cost far more than losing the
    // same number of bands at the edges. That weighting is the whole point of
    // using the standard's importance function instead of flat bands.
    noise.fill(-40.0f);
    auto edges = noise;
    edges[0] = edges[1] = edges[19] = edges[20] = 40.0f;
    const auto edgeLoss = 1.0f - psy::speechIntelligibilityIndex(speech, edges);
    auto consonants = noise;
    consonants[13] = consonants[14] = consonants[15] = consonants[16] = 40.0f;
    const auto consonantLoss = 1.0f - psy::speechIntelligibilityIndex(speech, consonants);
    expect(consonantLoss > edgeLoss * 2.0f,
           "masking the consonant bands must cost far more intelligibility than masking the edges");
}

CSP_TEST_CASE void testMonoCompatibilityCollapsesLowSide()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 256;
    auto enginePointer = std::make_unique<churchstream::ProcessingEngine>();
    auto& engine = *enginePointer;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    parameters.smartProcessing.store(false);
    parameters.rumbleEnabled.store(false);
    parameters.adaptiveEqEnabled.store(false);
    parameters.compressorEnabled.store(false);
    parameters.saturationEnabled.store(false);
    parameters.limiterEnabled.store(false);
    parameters.monoCompatibilityEnabled.store(true);
    parameters.phaseCoherenceEnabled.store(false);

    // Renders either a pure Side signal (L = -R) or a pure Mid signal (L = R)
    // at one frequency, and reports how much Side and Mid survive the chain.
    const auto measure = [&](float frequency, bool sideSignal, float& sideRms, float& midRms) {
        engine.reset();
        std::vector<float> left(blockSize), right(blockSize);
        float* channels[] { left.data(), right.data() };
        auto phase = 0.0;
        const auto step = 2.0 * juce::MathConstants<double>::pi * frequency / sampleRate;
        double sideSquares = 0.0, midSquares = 0.0;
        auto counted = 0;
        for (int block = 0; block < 400; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.25f * static_cast<float>(std::sin(phase));
                left[static_cast<size_t>(sample)] = value;
                right[static_cast<size_t>(sample)] = sideSignal ? -value : value;
                phase += step;
            }
            engine.process(channels, 2, blockSize);
            if (block < 200) continue;
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto side = 0.5 * (left[static_cast<size_t>(sample)] - right[static_cast<size_t>(sample)]);
                const auto mid = 0.5 * (left[static_cast<size_t>(sample)] + right[static_cast<size_t>(sample)]);
                sideSquares += side * side;
                midSquares += mid * mid;
                ++counted;
            }
        }
        sideRms = static_cast<float>(std::sqrt(sideSquares / std::max(counted, 1)));
        midRms = static_cast<float>(std::sqrt(midSquares / std::max(counted, 1)));
    };

    // A 0.25 peak sine carries 0.1768 RMS into whichever of Mid or Side it is
    // placed in.
    constexpr auto sourceRms = 0.25f * 0.70710678f;
    float sideRms = 0.0f, midRms = 0.0f;

    // 50 Hz Side is what cancels on a phone speaker, and it is the one thing
    // that must not survive.
    measure(50.0f, true, sideRms, midRms);
    expect(sideRms < sourceRms * 0.15f,
           "low-frequency Side must be collapsed towards mono");

    // The same signal well above the corner must be left alone, otherwise this
    // is not a bass-mono filter, it is a width control.
    measure(5000.0f, true, sideRms, midRms);
    expect(sideRms > sourceRms * 0.98f,
           "high-frequency Side must pass untouched");

    // Mid is the mono sum, and it must not pay for the collapsed Side. The
    // reference is measured with the feature off rather than assumed: the DC
    // blocker is a 38 Hz high-pass and already costs 2 dB at 50 Hz.
    measure(50.0f, false, sideRms, midRms);
    const auto monoCompatibleMidRms = midRms;
    expect(sideRms < 1.0e-4f, "a mono input must stay mono");
    parameters.monoCompatibilityEnabled.store(false);
    measure(50.0f, false, sideRms, midRms);
    expect(monoCompatibleMidRms > midRms * 0.99f,
           "low-frequency Mid must be preserved: bass mono moves energy, it does not remove it");
}

CSP_TEST_CASE void testPhaseCoherenceNarrowsOnlyIncoherentProgramme()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 256;
    auto enginePointer = std::make_unique<churchstream::ProcessingEngine>();
    auto& engine = *enginePointer;
    auto& parameters = engine.getParameters();
    auto& metrics = engine.getMetrics();

    const auto render = [&](bool antiPhase) {
        engine.prepare(sampleRate, blockSize, 2);
        parameters.smartProcessing.store(false);
        parameters.rumbleEnabled.store(false);
        parameters.adaptiveEqEnabled.store(false);
        parameters.compressorEnabled.store(false);
        parameters.saturationEnabled.store(false);
        parameters.limiterEnabled.store(false);
        parameters.monoCompatibilityEnabled.store(false);
        parameters.phaseCoherenceEnabled.store(true);
        std::vector<float> left(blockSize), right(blockSize);
        float* channels[] { left.data(), right.data() };
        auto phase = 0.0;
        const auto step = 2.0 * juce::MathConstants<double>::pi * 900.0 / sampleRate;
        for (int block = 0; block < 3000; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.25f * static_cast<float>(std::sin(phase));
                left[static_cast<size_t>(sample)] = value;
                right[static_cast<size_t>(sample)] = antiPhase ? -value : value;
                phase += step;
            }
            engine.process(channels, 2, blockSize);
        }
    };

    render(false);
    expect(metrics.programmeCorrelation.load() > 0.95f,
           "an in-phase programme must measure as correlated");
    expect(metrics.appliedStereoWidth.load() > 0.99f,
           "a coherent programme must keep its full width");

    render(true);
    expect(metrics.programmeCorrelation.load() < -0.95f,
           "an anti-phase programme must measure as uncorrelated");
    expect(metrics.appliedStereoWidth.load() < 0.60f,
           "an incoherent programme must have its Side pulled in before it reaches a mono speaker");
}

CSP_TEST_CASE void testLevelerTracksSectionsWithoutPumping()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 256;
    auto enginePointer = std::make_unique<churchstream::ProcessingEngine>();
    auto& engine = *enginePointer;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    auto& metrics = engine.getMetrics();
    parameters.smartProcessing.store(false);
    parameters.rumbleEnabled.store(false);
    parameters.adaptiveEqEnabled.store(false);
    parameters.compressorEnabled.store(false);
    parameters.saturationEnabled.store(false);
    parameters.limiterEnabled.store(false);
    parameters.broadcastLevelerEnabled.store(true);

    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    auto phase = 0.0;
    const auto step = 2.0 * juce::MathConstants<double>::pi * 700.0 / sampleRate;
    auto envelopePhase = 0.0;
    const auto envelopeStep = 2.0 * juce::MathConstants<double>::pi * 3.0 / sampleRate;

    // `syllables` adds +/-6 dB of 3 Hz amplitude movement, which is roughly how
    // much a speaking voice moves without the section having changed at all.
    const auto run = [&](float levelDb, bool syllables, int blocks,
                         float& minGainDb, float& maxGainDb, int settleBlocks) {
        minGainDb = 1.0e9f;
        maxGainDb = -1.0e9f;
        const auto base = std::pow(10.0f, levelDb / 20.0f);
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                auto amplitude = base;
                if (syllables)
                {
                    amplitude *= std::pow(10.0f, 6.0f * static_cast<float>(std::sin(envelopePhase)) / 20.0f);
                    envelopePhase += envelopeStep;
                }
                const auto value = amplitude * static_cast<float>(std::sin(phase));
                left[static_cast<size_t>(sample)] = value;
                right[static_cast<size_t>(sample)] = value;
                phase += step;
            }
            engine.process(channels, 2, blockSize);
            if (block < settleBlocks) continue;
            const auto gainDb = metrics.broadcastLevelGainDb.load();
            minGainDb = std::min(minGainDb, gainDb);
            maxGainDb = std::max(maxGainDb, gainDb);
        }
    };

    // Both levels are chosen so the required gain stays inside the -10/+15 dB
    // recovery range. Outside it the clamp hides how fast the estimate moved,
    // and the test would pass with any smoother at all.
    float minGainDb = 0.0f, maxGainDb = 0.0f;

    // A steady speaking voice: the gain must sit still. A fixed fast smoother
    // rides the syllables here, which is exactly the pumping complaint.
    run(-13.0f, true, 4000, minGainDb, maxGainDb, 2000);
    expect(maxGainDb - minGainDb < 1.5f,
           "the leveler must not ride syllable-rate movement inside one section");
    const auto steadyGainDb = 0.5f * (minGainDb + maxGainDb);

    // Worship ends, prayer begins: 12 dB down in one step. The gate spends
    // 2.5 s deciding this is a section and not a pause, which leaves about a
    // second to cover the 12 dB. A filter locked at the slow time constant
    // gets less than half way in that time; raising the process noise once the
    // innovation persists is what makes the difference.
    run(-25.0f, true, 700, minGainDb, maxGainDb, 699);
    const auto afterSectionChangeDb = metrics.broadcastLevelGainDb.load();
    expect(afterSectionChangeDb - steadyGainDb > 9.0f,
           "a real section change must be tracked within about a second of being recognised");
}

CSP_TEST_CASE void testLevelerGateIgnoresPauses()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 256;
    auto enginePointer = std::make_unique<churchstream::ProcessingEngine>();
    auto& engine = *enginePointer;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    auto& metrics = engine.getMetrics();
    parameters.smartProcessing.store(false);
    parameters.rumbleEnabled.store(false);
    parameters.adaptiveEqEnabled.store(false);
    parameters.compressorEnabled.store(false);
    parameters.saturationEnabled.store(false);
    parameters.limiterEnabled.store(false);
    parameters.broadcastLevelerEnabled.store(true);

    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    auto phase = 0.0;
    const auto step = 2.0 * juce::MathConstants<double>::pi * 700.0 / sampleRate;

    const auto run = [&](float levelDb, int blocks) {
        const auto amplitude = std::pow(10.0f, levelDb / 20.0f);
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = amplitude * static_cast<float>(std::sin(phase));
                left[static_cast<size_t>(sample)] = value;
                right[static_cast<size_t>(sample)] = value;
                phase += step;
            }
            engine.process(channels, 2, blockSize);
        }
    };

    run(-26.0f, 4000);
    expect(metrics.levelerGateOpen.load(), "programme must hold the gate open");
    const auto programmeGainDb = metrics.broadcastLevelGainDb.load();

    // A pause: not silence, just room tone 25 dB below the programme. This is
    // air conditioning and microphone hiss, and it must not be levelled up.
    run(-51.0f, 800);
    expect(!metrics.levelerGateOpen.load(),
           "room tone well below the programme must close the relative gate");
    // Not zero: the gate integrates over 400 ms to avoid chattering on
    // syllables, and the estimate keeps moving at its slow time constant until
    // the gate closes. That costs about 2.5 dB of drift at the start of a
    // pause, after which the leveler is frozen. Chasing the last of it would
    // mean gating faster, which brings the chattering back.
    expect(metrics.broadcastLevelGainDb.load() < programmeGainDb + 3.0f,
           "a closed gate must freeze the leveler instead of amplifying room tone");
}

CSP_TEST_CASE void testTruePeakDetectorFindsIntersamplePeaks()
{
    // A full scale tone at fs/4 offset by 45 degrees never lands on its own
    // peak: every sample reads 0.707 while the reconstructed waveform reaches
    // 1.0. This is exactly the case a sample-peak limiter lets through.
    churchstream::TruePeakDetector detector;
    auto samplePeak = 0.0f;
    auto truePeak = 0.0f;
    for (int sample = 0; sample < 4096; ++sample)
    {
        const auto value = std::cos(juce::MathConstants<float>::halfPi * static_cast<float>(sample)
                                    + juce::MathConstants<float>::pi * 0.25f);
        const auto detected = detector.process(0, value);
        if (sample > 64)
        {
            samplePeak = std::max(samplePeak, std::abs(value));
            truePeak = std::max(truePeak, detected);
        }
    }
    expect(approximately(samplePeak, 0.70710678f, 1.0e-3f),
           "the inter-sample fixture must read 0.707 on sample peak");
    expect(truePeak > 0.93f,
           "4x oversampled detection must recover the real inter-sample peak the samples hide");

    churchstream::TruePeakDetector steady;
    auto steadyPeak = 0.0f;
    for (int sample = 0; sample < 512; ++sample)
    {
        const auto detected = steady.process(0, 0.5f);
        // The first samples are the step response of an empty delay line.
        if (sample > 64) steadyPeak = std::max(steadyPeak, detected);
    }
    expect(approximately(steadyPeak, 0.5f, 0.02f),
           "a steady signal must not be inflated by the interpolator");
}

CSP_TEST_CASE void testLimiterStaysUnderTruePeakCeiling()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 512;
    auto enginePointer = std::make_unique<churchstream::ProcessingEngine>();
    auto& engine = *enginePointer;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    parameters.limiterEnabled.store(true);
    parameters.smartProcessing.store(false);

    auto* meter = ebur128_init(2, sampleRate, EBUR128_MODE_TRUE_PEAK);
    expect(meter != nullptr, "true peak meter must initialise");
    if (meter == nullptr) return;

    juce::AudioBuffer<float> buffer(2, blockSize);
    std::vector<float> interleaved(static_cast<size_t>(blockSize * 2));
    int phase = 0;
    for (int block = 0; block < 200; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = std::cos(juce::MathConstants<float>::halfPi * static_cast<float>(phase++)
                                        + juce::MathConstants<float>::pi * 0.25f);
            buffer.setSample(0, sample, value);
            buffer.setSample(1, sample, value);
        }
        float* channels[] { buffer.getWritePointer(0), buffer.getWritePointer(1) };
        engine.process(channels, 2, blockSize);
        if (block < 20) continue;
        for (int sample = 0; sample < blockSize; ++sample)
        {
            interleaved[static_cast<size_t>(sample * 2)] = buffer.getSample(0, sample);
            interleaved[static_cast<size_t>(sample * 2 + 1)] = buffer.getSample(1, sample);
        }
        ebur128_add_frames_float(meter, interleaved.data(), static_cast<size_t>(blockSize));
    }

    double left = 0.0, right = 0.0;
    ebur128_true_peak(meter, 0, &left);
    ebur128_true_peak(meter, 1, &right);
    ebur128_destroy(&meter);
    const auto truePeakDbtp = 20.0 * std::log10(std::max({ left, right, 1.0e-9 }));
    expect(truePeakDbtp <= -0.9,
           "the limiter must hold an inter-sample-peak source under the -1 dBTP ceiling");
}

CSP_TEST_CASE void renderAbComparison(bool matchEnabled, float& processedRmsDb, float& alternativeRmsDb)
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 512;
    auto enginePointer = std::make_unique<churchstream::ProcessingEngine>();
    auto& engine = *enginePointer;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    parameters.smartProcessing.store(true);
    parameters.operatingMode.store(static_cast<int>(churchstream::OperatingMode::autoMode));
    parameters.abLoudnessMatch.store(matchEnabled);
    parameters.abProcessed.store(true);
    parameters.compressorEnabled.store(false);
    parameters.saturationEnabled.store(false);
    parameters.limiterEnabled.store(false);
    parameters.adaptiveEqEnabled.store(false);
    engine.getAdaptiveTargets().loudnessGainDb.store(3.0f);

    uint32_t noise = 0x1234567u;
    const auto nextSample = [&noise]
    {
        noise = noise * 1664525u + 1013904223u;
        return 0.1f * (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f);
    };

    juce::AudioBuffer<float> buffer(2, blockSize);
    const auto renderSeconds = [&](float& rmsDb)
    {
        auto accumulated = -100.0f;
        const auto blocks = sampleRate * 8 / blockSize;
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = nextSample();
                buffer.setSample(0, sample, value);
                buffer.setSample(1, sample, value);
            }
            float* channels[] { buffer.getWritePointer(0), buffer.getWritePointer(1) };
            engine.process(channels, 2, blockSize);
            // Only the settled tail is measured; the match integrator needs a
            // couple of seconds, exactly as it would in front of an operator.
            if (block >= blocks - 40)
                accumulated = accumulated < -99.0f ? blockRmsDb(buffer, 0, blockSize)
                                                   : 0.5f * (accumulated + blockRmsDb(buffer, 0, blockSize));
        }
        rmsDb = accumulated;
    };

    renderSeconds(processedRmsDb);
    parameters.abProcessed.store(false);
    renderSeconds(alternativeRmsDb);
}

CSP_TEST_CASE void testWatchdogFailsafeAndInputSanitising()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 256;
    auto enginePointer = std::make_unique<churchstream::ProcessingEngine>();
    auto& engine = *enginePointer;
    engine.prepare(sampleRate, blockSize, 2);
    auto& parameters = engine.getParameters();
    auto& metrics = engine.getMetrics();
    parameters.smartProcessing.store(false);
    parameters.operatingMode.store(static_cast<int>(churchstream::OperatingMode::manual));

    juce::AudioBuffer<float> buffer(2, blockSize);
    int phase = 0;
    auto worstOutput = 0.0f;
    auto allFinite = true;

    // Renders `blocks` blocks of a steady tone and returns the output RMS in dB,
    // ignoring the first `skip` blocks so smoothed gains have settled.
    const auto render = [&](int blocks, int skip) {
        auto square = 0.0;
        auto counted = 0;
        for (int block = 0; block < blocks; ++block)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = 0.3f * std::sin(juce::MathConstants<float>::twoPi
                                                   * 220.0f * static_cast<float>(phase++) / sampleRate);
                buffer.setSample(0, sample, value);
                buffer.setSample(1, sample, value);
            }
            float* channels[] { buffer.getWritePointer(0), buffer.getWritePointer(1) };
            engine.process(channels, 2, blockSize);
            for (int channel = 0; channel < 2; ++channel)
            {
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto out = buffer.getSample(channel, sample);
                    allFinite = allFinite && std::isfinite(out);
                    worstOutput = std::max(worstOutput, std::isfinite(out) ? std::abs(out) : 1.0e9f);
                    if (block >= skip)
                    {
                        square += static_cast<double>(out) * out;
                        ++counted;
                    }
                }
            }
        }
        return counted > 0
            ? static_cast<float>(10.0 * std::log10(std::max(square / counted, 1.0e-12)))
            : -100.0f;
    };

    const auto referenceRmsDb = render(40, 20);
    expect(!metrics.failsafeActive.load(), "the watchdog must stay out of the way on healthy audio");
    expect(metrics.failsafeEngagements.load() == 0u,
           "healthy programme must never be counted as a DSP fault");

    // The panic switch must reach the dry path without silence and without a
    // fault being reported, then hand the processed path back.
    parameters.forceFailsafe.store(true);
    const auto forcedRmsDb = render(20, 5);
    expect(metrics.failsafeActive.load(), "a forced failsafe must engage the dry safety path");
    expect(metrics.failsafeEngagements.load() == 0u,
           "an operator-forced failsafe is not a DSP fault and must not be counted as one");
    // The tone is 0.3 peak, so the untouched dry path sits at -13.5 dBFS RMS.
    expect(std::abs(forcedRmsDb + 13.5f) < 1.0f,
           "the failsafe path must carry the dry signal, not silence or a gain-matched copy");

    parameters.forceFailsafe.store(false);
    const auto recoveredRmsDb = render(120, 100);
    expect(!metrics.failsafeActive.load(), "the engine must return to the processed path when released");
    expect(std::abs(recoveredRmsDb - referenceRmsDb) < 1.0f,
           "the processed path must sound the same after a failsafe as before it");

    // Broken samples from the driver: NaN, infinities and an absurd magnitude.
    for (int block = 0; block < 4; ++block)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = 0.3f * std::sin(juce::MathConstants<float>::twoPi
                                               * 220.0f * static_cast<float>(phase++) / sampleRate);
            buffer.setSample(0, sample, value);
            buffer.setSample(1, sample, value);
        }
        buffer.setSample(0, 10, std::numeric_limits<float>::quiet_NaN());
        buffer.setSample(1, 11, std::numeric_limits<float>::infinity());
        buffer.setSample(0, 12, -std::numeric_limits<float>::infinity());
        buffer.setSample(1, 13, 1.0e30f);
        float* channels[] { buffer.getWritePointer(0), buffer.getWritePointer(1) };
        engine.process(channels, 2, blockSize);
        for (int channel = 0; channel < 2; ++channel)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto out = buffer.getSample(channel, sample);
                allFinite = allFinite && std::isfinite(out);
                worstOutput = std::max(worstOutput, std::isfinite(out) ? std::abs(out) : 1.0e9f);
            }
        }
    }
    expect(metrics.nonFiniteInputSamples.load() >= 3u,
           "non-finite input samples must be counted so the fault is attributed upstream");

    // The real test of the watchdog is not the glitch block itself but what
    // comes after it: a poisoned recursive state would keep the output dead or
    // broken long after the input recovered.
    const auto afterFaultRmsDb = render(120, 100);
    expect(allFinite, "no non-finite sample may ever reach the output");
    expect(worstOutput < 5.0f, "the engine must never emit an absurd magnitude");
    expect(std::abs(afterFaultRmsDb - referenceRmsDb) < 1.0f,
           "broken input samples must not poison the filter state permanently");
}

CSP_TEST_CASE void testLoudnessMatchedAb()
{
    float matchedProcessed = 0.0f, matchedOriginal = 0.0f;
    renderAbComparison(true, matchedProcessed, matchedOriginal);
    expect(std::abs(matchedProcessed - matchedOriginal) < 1.0f,
           "loudness-matched A/B must present both sides at the same level");

    float rawProcessed = 0.0f, rawOriginal = 0.0f;
    renderAbComparison(false, rawProcessed, rawOriginal);
    expect(rawProcessed - rawOriginal > 2.0f,
           "without matching the processed side is measurably louder, which is the bias being removed");
}

CSP_TEST_CASE void testSibilanceDeEsser()
{
    // Heap allocated: with optimisation these fixtures are inlined into main
    // and their FFT buffers would blow the stack under AddressSanitizer.
    auto analysis = std::make_unique<churchstream::AnalysisEngine>();
    auto processing = std::make_unique<churchstream::ProcessingEngine>();
    processing->prepare(48000.0, 256, 2);
    churchstream::SmartEngine smart(*analysis, *processing);

    auto snapshotHolder = std::make_unique<churchstream::AnalysisSnapshot>();
    auto& snapshot = *snapshotHolder;
    snapshot.sampleRate = 48000.0;
    snapshot.processed.rmsDb = -18.0f;
    snapshot.processed.lufsShortTerm = -14.0f;
    snapshot.processed.truePeakDbtp = -1.5f;
    snapshot.processed.crestFactorDb = 10.0f;
    snapshot.processed.stereoCorrelation = 0.85f;
    snapshot.processed.bandEnergy = { 0.12f, 0.24f, 0.35f, 0.23f, 0.06f };
    for (int bin = 0; bin < churchstream::spectrumBins; ++bin)
    {
        const auto frequency = static_cast<double>(bin) * snapshot.sampleRate / churchstream::fftSize;
        snapshot.processed.spectrumDb[static_cast<size_t>(bin)] =
            frequency >= 5500.0 && frequency < 9000.0 ? -12.0f : -50.0f;
    }

    for (int update = 0; update < 60; ++update)
        smart.processSnapshotForTesting(snapshot, 0.1f);

    const auto sibilance = processing->getAdaptiveTargets().sibilanceGainDb.load();
    expect(sibilance < -0.4f, "sustained 5.5-9 kHz excess must engage the de-esser");
    expect(sibilance >= -2.5f, "the de-esser must respect the SAFE mode limit");

    const auto state = smart.getState();
    auto reported = false;
    for (int index = 0; index < state.actionCount; ++index)
        reported = reported || state.actions[static_cast<size_t>(index)].name == "Taming sibilance";
    expect(reported, "the de-esser must be reported as an explicit action");

    auto sibilanceProblem = false;
    for (int index = 0; index < state.problemCount; ++index)
        sibilanceProblem = sibilanceProblem
            || (state.problems[static_cast<size_t>(index)].name == "Sibilance"
                && state.problems[static_cast<size_t>(index)].warning);
    expect(sibilanceProblem, "sibilance must appear in the active problem list");

    // A clean top end must release the de-esser instead of holding it down.
    for (int bin = 0; bin < churchstream::spectrumBins; ++bin)
    {
        const auto frequency = static_cast<double>(bin) * snapshot.sampleRate / churchstream::fftSize;
        snapshot.processed.spectrumDb[static_cast<size_t>(bin)] = frequency < 2000.0 ? -12.0f : -60.0f;
    }
    for (int update = 0; update < 200; ++update)
        smart.processSnapshotForTesting(snapshot, 0.1f);
    expect(processing->getAdaptiveTargets().sibilanceGainDb.load() > sibilance,
           "the de-esser must release once the sibilance is gone");
}

CSP_TEST_CASE void testOfflineSmartSimulationAndMatchedRender()
{
    constexpr int sampleRate = 48000;
    constexpr int seconds = 40;
    const auto inputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getNonexistentChildFile("csp-offline-sim", ".wav", false);
    {
        std::unique_ptr<juce::OutputStream> stream = inputFile.createOutputStream();
        juce::WavAudioFormat wav;
        auto options = juce::AudioFormatWriterOptions().withSampleRate(sampleRate)
            .withChannelLayout(juce::AudioChannelSet::stereo()).withBitsPerSample(24);
        auto writer = wav.createWriterFor(stream, options);
        expect(writer != nullptr, "offline simulation fixture WAV writer must open");
        if (writer == nullptr) return;

        uint32_t noise = 0x2468aceu;
        juce::AudioBuffer<float> audio(2, sampleRate);
        for (int second = 0; second < seconds; ++second)
        {
            for (int sample = 0; sample < sampleRate; ++sample)
            {
                noise = noise * 1664525u + 1013904223u;
                const auto white = 0.06f * (static_cast<float>(noise >> 8) / 8388608.0f - 1.0f);
                const auto position = static_cast<float>(second * sampleRate + sample);
                auto value = white + 0.08f * std::sin(position * 2.0f
                    * juce::MathConstants<float>::pi * 500.0f / sampleRate);
                // The second half adds a sustained 240 Hz buildup, the classic
                // low-mid problem the engine is supposed to notice and correct.
                if (second >= 26)
                    value += 0.24f * std::sin(position * 2.0f
                        * juce::MathConstants<float>::pi * 240.0f / sampleRate);
                audio.setSample(0, sample, value);
                audio.setSample(1, sample, value * 0.98f);
            }
            writer->writeFromAudioSampleBuffer(audio, 0, sampleRate);
        }
    }

    auto settingsSource = std::make_unique<churchstream::ProcessingEngine>();
    settingsSource->prepare(sampleRate, 1024, 2);
    churchstream::OfflineProcessor offline;
    expect(offline.startProcessing(inputFile, settingsSource->getParameters()),
           "offline simulation must accept a real local WAV");
    for (int attempt = 0; attempt < 3000 && offline.getResult().running; ++attempt)
        juce::Thread::sleep(20);
    const auto result = offline.getResult();
    offline.stop();

    expect(result.complete && result.error.isEmpty(), "offline simulation must finish without error");
    expect(result.originalFile.existsAsFile() && result.processedFile.existsAsFile(),
           "offline simulation must render both sides of the A/B");
    expect(result.reportFile.existsAsFile(), "offline simulation must write a diagnostic report");
    expect(result.report.baselineLearned,
           "a 40 s recording must be enough for the simulated Auto Tune to learn a baseline");
    expect(result.report.averageScore > 0.0f && result.report.averageScore <= 100.0f,
           "the simulation must score the mix, which only happens with the Smart Engine running");
    expect(result.report.corrections > 0,
           "a deliberate low-mid buildup must produce evaluated corrections offline");

    if (result.originalFile.existsAsFile() && result.processedFile.existsAsFile())
    {
        const auto originalLoudness = measureIntegratedLoudness(result.originalFile);
        const auto processedLoudness = measureIntegratedLoudness(result.processedFile);
        expect(std::abs(originalLoudness - processedLoudness) < 0.7f,
               "the rendered A/B pair must be loudness matched on disk, not just in the report");
    }

    inputFile.deleteFile();
    result.originalFile.deleteFile();
    result.processedFile.deleteFile();
    result.reportFile.deleteFile();
}


CSP_TEST_CASE void testOscEncodingAndDecoding()
{
    // Hand-checked OSC 1.0 packet: "/ch/01/mix/fader" is 16 characters, so it
    // needs four padding zeros; the type tag ",f" pads to four bytes; the float
    // follows big-endian.
    churchstream::OscMessage message("/ch/01/mix/fader");
    message.addFloat(0.75f);
    const auto encoded = message.encode();
    expect(encoded.size() == 20 + 4 + 4, "an OSC address of 16 chars must pad to 20 bytes plus tags and payload");
    expect(encoded[16] == 0 && encoded[19] == 0, "the OSC address must be null padded to a 4 byte boundary");
    expect(encoded[20] == static_cast<uint8_t>(',') && encoded[21] == static_cast<uint8_t>('f'),
           "the type tag string must start with a comma");
    expect(encoded[24] == 0x3F && encoded[25] == 0x40 && encoded[26] == 0x00 && encoded[27] == 0x00,
           "0.75f must be written big-endian as 3F 40 00 00");

    churchstream::OscMessage decoded;
    expect(churchstream::OscMessage::decode(encoded.data(), encoded.size(), decoded),
           "a well formed OSC packet must decode");
    expect(decoded.getAddress() == "/ch/01/mix/fader", "the decoded address must round-trip");
    expect(approximately(decoded.getFloat(0), 0.75f), "the decoded float must round-trip");

    churchstream::OscMessage strings("/ch/02/config/name");
    strings.addString("PASTOR").addInt(7);
    const auto stringPacket = strings.encode();
    churchstream::OscMessage decodedStrings;
    expect(churchstream::OscMessage::decode(stringPacket.data(), stringPacket.size(), decodedStrings),
           "a mixed string/int packet must decode");
    expect(decodedStrings.getString(0) == "PASTOR" && decodedStrings.getInt(1) == 7,
           "string and int arguments must round-trip");

    // Malformed input must be rejected instead of read past the end.
    const std::vector<uint8_t> truncated { '/', 'c', 'h', 0, ',', 'f', 0, 0, 0x3F };
    churchstream::OscMessage rejected;
    expect(!churchstream::OscMessage::decode(truncated.data(), truncated.size(), rejected),
           "a truncated OSC packet must be rejected");
    const std::vector<uint8_t> unterminated { 'n', 'o', 's', 'l', 'a', 's', 'h' };
    expect(!churchstream::OscMessage::decode(unterminated.data(), unterminated.size(), rejected),
           "a packet without a valid OSC address must be rejected");
}

CSP_TEST_CASE void testX32ClientIsReadOnly()
{
    using churchstream::OscMessage;
    using churchstream::X32Client;

    expect(X32Client::isReadOnlyQuery(OscMessage("/info")), "/info is a query and must be allowed");
    expect(X32Client::isReadOnlyQuery(OscMessage("/xremote")), "/xremote is a subscription and must be allowed");
    expect(X32Client::isReadOnlyQuery(OscMessage("/ch/01/config/name")),
           "reading a channel name must be allowed");

    OscMessage setFader("/ch/01/mix/fader");
    setFader.addFloat(0.9f);
    expect(!X32Client::isReadOnlyQuery(setFader),
           "a message carrying a value is a write and must be refused");
    OscMessage mute("/ch/01/mix/on");
    mute.addInt(0);
    expect(!X32Client::isReadOnlyQuery(mute), "muting a channel must be refused");
    expect(!X32Client::isReadOnlyQuery(OscMessage("/-action/setclock")),
           "an address outside the read allowlist must be refused");
    expect(!X32Client::isReadOnlyQuery(OscMessage("/-stat/solosw/01")),
           "an address outside the read allowlist must be refused even without arguments");

    // X32 fader taper, from the four documented segments.
    expect(approximately(X32Client::faderToDb(1.0f), 10.0f, 0.01f), "unity top of fader must read +10 dB");
    expect(approximately(X32Client::faderToDb(0.75f), 0.0f, 0.01f), "0.75 must read 0 dB");
    expect(approximately(X32Client::faderToDb(0.5f), -10.0f, 0.01f), "0.5 must read -10 dB");
    expect(approximately(X32Client::faderToDb(0.25f), -30.0f, 0.01f), "0.25 must read -30 dB");
    expect(X32Client::faderToDb(0.0f) <= -89.0f, "a closed fader must read minus infinity");
}

namespace
{
// Stand-in for the console. It answers the same OSC addresses an X32 answers,
// so the client's socket loop can run end to end without hardware, and it
// records every packet it receives - which is the only way to prove over the
// wire, rather than by inspecting a function, that the client never writes.
class FakeX32Console final : public juce::Thread
{
public:
    FakeX32Console() : juce::Thread("Fake X32 Console", 256 * 1024) {}
    ~FakeX32Console() override { signalThreadShouldExit(); stopThread(2000); }

    bool startConsole()
    {
        if (!socket.bindToPort(0)) return false;
        boundPort = socket.getBoundPort();
        if (boundPort <= 0) return false;
        startThread(juce::Thread::Priority::normal);
        return true;
    }

    // Keeps the socket open but stops answering: what a console looks like when
    // it is unplugged or its switch port dies mid service.
    void setMuted(bool shouldBeMuted) noexcept { muted.store(shouldBeMuted); }

    [[nodiscard]] int getPort() const noexcept { return boundPort; }
    [[nodiscard]] int getWritesReceived() const noexcept { return writesReceived.load(); }
    [[nodiscard]] int getQueriesReceived() const noexcept { return queriesReceived.load(); }
    [[nodiscard]] bool sawSubscription() const noexcept { return subscriptionSeen.load(); }

private:
    void run() override
    {
        std::array<uint8_t, 2048> buffer {};
        while (!threadShouldExit())
        {
            if (socket.waitUntilReady(true, 40) != 1) continue;

            juce::String sender;
            int senderPort = 0;
            const auto size = socket.read(buffer.data(), static_cast<int>(buffer.size()), false,
                                          sender, senderPort);
            if (size <= 0) continue;

            churchstream::OscMessage request;
            if (!churchstream::OscMessage::decode(buffer.data(), static_cast<size_t>(size), request))
                continue;
            if (request.getArgumentCount() > 0)
            {
                writesReceived.fetch_add(1);
                continue;
            }
            queriesReceived.fetch_add(1);
            if (!muted.load()) reply(request.getAddress(), sender, senderPort);
        }
    }

    void reply(const juce::String& address, const juce::String& sender, int senderPort)
    {
        if (address == "/xremote")
        {
            subscriptionSeen.store(true);
            return;
        }

        churchstream::OscMessage response(address);
        if (address == "/info")
            response.addString("V2.07").addString("TEMPLO X32").addString("X32").addString("4.06");
        else if (address == "/status")
            response.addString("active").addString("127.0.0.1").addString("TEMPLO X32");
        else if (address.startsWith("/ch/") && address.endsWith("/config/name"))
            response.addString(channelName(address));
        else if (address.startsWith("/ch/") && address.endsWith("/mix/fader"))
            response.addFloat(0.75f);
        else if (address.startsWith("/ch/") && address.endsWith("/mix/on"))
            response.addInt(1);
        else if (address.startsWith("/bus/") && address.endsWith("/config/name"))
            response.addString("MATRIX L");
        else
            return;

        // Real networks carry junk. A malformed datagram in front of every valid
        // reply proves the client keeps going instead of stalling or crashing.
        const uint8_t garbage[] { 0xFF, 0x00, 0x2F, 0x01, 0x99 };
        socket.write(sender, senderPort, garbage, static_cast<int>(sizeof(garbage)));

        const auto data = response.encode();
        socket.write(sender, senderPort, data.data(), static_cast<int>(data.size()));
    }

    static juce::String channelName(const juce::String& address)
    {
        const auto number = address.substring(4, 6);
        if (number == "01") return "PASTOR";
        if (number == "02") return "PIANO L";
        return "CH " + number;
    }

    juce::DatagramSocket socket { false };
    int boundPort = 0;
    std::atomic<bool> muted { false };
    std::atomic<int> writesReceived { 0 };
    std::atomic<int> queriesReceived { 0 };
    std::atomic<bool> subscriptionSeen { false };
};
}

CSP_TEST_CASE void testX32ClientTalksToAConsole()
{
    auto console = std::make_unique<FakeX32Console>();
    if (!console->startConsole())
    {
        expect(false, "the loopback console could not open a UDP port");
        return;
    }

    churchstream::X32Client client;
    client.start("127.0.0.1", console->getPort());

    churchstream::X32State state;
    for (int attempt = 0; attempt < 400; ++attempt)
    {
        state = client.getState();
        if (state.connected && state.consoleName.isNotEmpty() && state.namedChannels >= 8
            && state.busNames[0].isNotEmpty())
            break;
        juce::Thread::sleep(25);
    }

    expect(state.connected, "the client must report a live link once the console answers");
    expect(console->sawSubscription(), "the client must renew /xremote or the console stops sending");
    expect(console->getQueriesReceived() > 0, "the client must actually put packets on the wire");
    expect(state.consoleName == "TEMPLO X32", "the console name from /info must reach the state");
    expect(state.model == "X32" && state.firmware == "4.06", "model and firmware must be parsed");
    expect(state.channels[0].name == "PASTOR", "channel 1 name must land on channel index 0");
    expect(state.channels[1].name == "PIANO L", "channel 2 name must land on channel index 1");
    expect(approximately(state.channels[0].faderDb, 0.0f, 0.01f),
           "a 0.75 fader must be reported as 0 dB, not as 0.75");
    expect(state.channels[0].on, "the /mix/on flag must be parsed");
    expect(state.busNames[0] == "MATRIX L", "bus names must be parsed");
    expect(state.namedChannels >= 8, "the first poll must name at least one slice of channels");

    // The whole point of the module: nothing the client emitted was a write.
    expect(console->getWritesReceived() == 0,
           "the console must never receive a message carrying a value");
    expect(state.rejectedWrites == 0, "no query should have been blocked by the allowlist");

    // Console disappears mid service: the link must go cold on its own.
    console->setMuted(true);
    const auto muteStart = juce::Time::getMillisecondCounterHiRes();
    auto coldAfterMs = 0.0;
    for (int attempt = 0; attempt < 600; ++attempt)
    {
        juce::Thread::sleep(25);
        if (!client.getState().connected)
        {
            coldAfterMs = juce::Time::getMillisecondCounterHiRes() - muteStart;
            break;
        }
    }
    expect(coldAfterMs > 0.0, "a console that stops answering must drop the link, not stay green");
    // The client documents a five second timeout. Counting a fixed tick instead
    // of measuring elapsed time once made every timer in the loop run at half
    // speed, so the bound is asserted rather than assumed.
    expect(coldAfterMs > 0.0 && coldAfterMs < 8000.0,
           "the link must go cold near the documented five seconds, not twice that");

    client.stop();
    expect(!client.getState().connected, "stopping the client must clear the connected flag");
    expect(console->getWritesReceived() == 0, "no write may leak while the console is unresponsive");
}

CSP_TEST_CASE void testGroupMixerFallbackAndMasking()
{
    constexpr int sampleRate = 48000;
    constexpr int blockSize = 512;
    auto mixer = std::make_unique<churchstream::GroupMixer>();
    mixer->prepare(sampleRate);

    std::vector<std::vector<float>> inputStorage(6, std::vector<float>(blockSize, 0.0f));
    std::vector<std::vector<float>> outputStorage(2, std::vector<float>(blockSize, 0.0f));
    std::array<const float*, 6> inputs {};
    std::array<float*, 2> outputs {};
    for (int channel = 0; channel < 6; ++channel) inputs[static_cast<size_t>(channel)] = inputStorage[static_cast<size_t>(channel)].data();
    for (int channel = 0; channel < 2; ++channel) outputs[static_cast<size_t>(channel)] = outputStorage[static_cast<size_t>(channel)].data();

    churchstream::GroupRoutingConfig routes;
    expect(!mixer->process(inputs.data(), 2, outputs.data(), 2, blockSize, routes),
           "the mixer must refuse to run when the stems are not really there");

    // Voice on 1-2, dense music covering the same presence range on 3-4,
    // quiet room mics on 5-6.
    int phase = 0;
    const auto fill = [&](int blockIndex)
    {
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto position = static_cast<float>(phase + sample);
            const auto voice = 0.25f * std::sin(position * 2.0f * juce::MathConstants<float>::pi * 900.0f / sampleRate)
                + 0.20f * std::sin(position * 2.0f * juce::MathConstants<float>::pi * 2200.0f / sampleRate)
                + 0.12f * std::sin(position * 2.0f * juce::MathConstants<float>::pi * 3400.0f / sampleRate);
            const auto music = 0.22f * std::sin(position * 2.0f * juce::MathConstants<float>::pi * 1200.0f / sampleRate)
                + 0.20f * std::sin(position * 2.0f * juce::MathConstants<float>::pi * 3000.0f / sampleRate);
            inputStorage[0][static_cast<size_t>(sample)] = voice;
            inputStorage[1][static_cast<size_t>(sample)] = voice;
            inputStorage[2][static_cast<size_t>(sample)] = music;
            inputStorage[3][static_cast<size_t>(sample)] = music;
            inputStorage[4][static_cast<size_t>(sample)] = 0.02f * voice;
            inputStorage[5][static_cast<size_t>(sample)] = 0.02f * voice;
        }
        phase += blockSize;
        juce::ignoreUnused(blockIndex);
    };

    // Masking off: the mixer must still deliver the summed stems untouched.
    const auto blocks = sampleRate * 6 / blockSize;
    for (int block = 0; block < blocks; ++block)
    {
        fill(block);
        expect(mixer->process(inputs.data(), 6, outputs.data(), 2, blockSize, routes)
               || block > 0, "the mixer must accept six valid input channels");
    }
    expect(!mixer->getDecision().applied,
           "masking must not reach the audio until it is explicitly enabled");
    auto referencePeak = 0.0f;
    for (int sample = 0; sample < blockSize; ++sample)
        referencePeak = std::max(referencePeak, std::abs(outputStorage[0][static_cast<size_t>(sample)]));
    expect(referencePeak > 0.1f, "the summed group mix must produce real signal");

    mixer->setMaskingEnabled(true);
    for (int block = 0; block < blocks; ++block)
    {
        fill(block);
        mixer->process(inputs.data(), 6, outputs.data(), 2, blockSize, routes);
    }
    const auto decision = mixer->getDecision();
    expect(decision.active && decision.applied,
           "music covering the voice presence range must engage Smart Masking once enabled");
    expect(decision.speechIntelligibility < 1.0f,
           "the mixer must report the intelligibility it measured, not a default");
    for (const auto gainDb : decision.musicGainDb)
    {
        expect(gainDb <= 0.0f, "masking may only reduce, never boost");
        expect(gainDb >= -churchstream::SmartMaskingController::maximumReductionDb,
               "masking must respect its reduction limit");
    }
}

CSP_TEST_CASE void testRoomCalibrationMeasuresDecayAndResonance()
{
    constexpr double sampleRate = 48000.0;
    constexpr int sampleCount = static_cast<int>(sampleRate * 3.0);
    std::vector<float> impulse(static_cast<size_t>(sampleCount), 0.0f);

    // Synthetic room: exponentially decaying noise with a known RT60 of 1.2 s
    // plus a sustained 125 Hz mode.
    constexpr float targetRt60 = 1.2f;
    const auto decay = std::exp(-6.907755f / (targetRt60 * static_cast<float>(sampleRate)));
    uint32_t noise = 0x13579bdfu;
    auto envelope = 1.0f;
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        noise = noise * 1664525u + 1013904223u;
        const auto white = static_cast<float>(noise >> 8) / 8388608.0f - 1.0f;
        const auto mode = 2.2f * std::sin(static_cast<float>(sample) * 2.0f
            * juce::MathConstants<float>::pi * 125.0f / static_cast<float>(sampleRate));
        impulse[static_cast<size_t>(sample)] = envelope * (white + mode);
        envelope *= decay;
    }
    impulse[0] = 1.0f;

    const auto result = churchstream::RoomCalibration::analyse(impulse.data(), sampleCount, sampleRate);
    expect(result.complete && result.error.isEmpty(), "a valid measurement must analyse without error");
    expect(result.measurementType == "Impulse response",
           "a decaying measurement must be recognised as an impulse response");
    expect(result.rt60Available, "an impulse response must yield an RT60");
    expect(std::abs(result.rt60Seconds - targetRt60) < 0.25f,
           "the RT60 estimate must land near the synthesised decay");
    expect(result.bandCount > 20, "the third-octave analysis must cover the audible range");

    auto foundMode = false;
    for (int index = 0; index < result.recommendationCount; ++index)
    {
        const auto& recommendation = result.recommendations[static_cast<size_t>(index)];
        if (recommendation.frequencyHz > 100.0f && recommendation.frequencyHz < 160.0f)
            foundMode = true;
        expect(recommendation.gainDb <= 0.0f,
               "room recommendations must only cut; boosting a room costs headroom");
    }
    expect(foundMode, "a 125 Hz room mode must appear as a matrix EQ recommendation");

    std::vector<float> silence(static_cast<size_t>(sampleRate), 0.0f);
    const auto quiet = churchstream::RoomCalibration::analyse(silence.data(),
                                                              static_cast<int>(sampleRate), sampleRate);
    expect(!quiet.complete && quiet.error.isNotEmpty(),
           "a silent measurement must be reported as unusable, not analysed");
}

CSP_TEST_CASE void testObsWebSocketAuthenticationVector()
{
    const auto actual = churchstream::OBSAuthentication::create(
        "supersecretpassword",
        "lM1GncleQOaCu9lT1yeUZhFYnqhsLLP1G5lAGo3ixaI=",
        "+IxH4CnCiqpX1rM9scsNynZzbOe4KhDeYcTNS3PDaeY=");
    expect(actual == "1Ct943GAT+6YQUUX47Ia/ncufilbe6+oD6lY+5kaCu4=",
           "OBS WebSocket v5 authentication must match the independently verified SHA-256 vector");
}
}

int main()
{
    testStereoPassthrough();
    testMonoDuplicationAndSilence();
    testMeterValues();
    testLimiterAndBypassAreReal();
    testFourBandRecombinationIsLevelNeutral();
    testSampleRatesBuffersAndLiveChanges();
    testRealFftLoudnessAndStereoAnalysis();
    testSmartEnginePersistenceConfidenceAndLimits();
    testAutoTuneBuildsRealBaseline();
    testSmartQualityClosedLoopAndRollback();
    testSafetyControllerAndSmartMasking();
    testAutomaticMultigroupRouting();
    testEqDynamicEqAndCompressorBehaviour();
    testOfflineFileProcessing();
    testBroadcastLevelerStabilisesStereoProgramme();
    testPsychoacousticModels();
    testMonoCompatibilityCollapsesLowSide();
    testPhaseCoherenceNarrowsOnlyIncoherentProgramme();
    testLevelerTracksSectionsWithoutPumping();
    testLevelerGateIgnoresPauses();
    testTruePeakDetectorFindsIntersamplePeaks();
    testLimiterStaysUnderTruePeakCeiling();
    testWatchdogFailsafeAndInputSanitising();
    testLoudnessMatchedAb();
    testSibilanceDeEsser();
    testOfflineSmartSimulationAndMatchedRender();
    testOscEncodingAndDecoding();
    testX32ClientIsReadOnly();
    testX32ClientTalksToAConsole();
    testGroupMixerFallbackAndMasking();
    testRoomCalibrationMeasuresDecayAndResonance();
    testObsWebSocketAuthenticationVector();

    if (failures == 0)
        std::cout << "All audio, DSP, analyzer, and Smart Engine tests passed.\n";

    return failures == 0 ? 0 : 1;
}
