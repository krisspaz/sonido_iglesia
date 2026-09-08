#include "RoomCalibration.h"

#include <algorithm>
#include <cmath>
#include <juce_dsp/juce_dsp.h>
#include <vector>

namespace churchstream
{
namespace
{
constexpr int analysisFftOrder = 12;
constexpr int analysisFftSize = 1 << analysisFftOrder;
constexpr int analysisHop = analysisFftSize / 2;

// ANSI third-octave centres from 25 Hz to 16 kHz.
float bandCentre(int index)
{
    return 1000.0f * std::pow(2.0f, static_cast<float>(index - 16) / 3.0f);
}

float meanLevel(const RoomResult& result, float lowHz, float highHz)
{
    auto sum = 0.0f;
    auto count = 0;
    for (int index = 0; index < result.bandCount; ++index)
    {
        const auto& band = result.bands[static_cast<size_t>(index)];
        if (band.centreHz >= lowHz && band.centreHz <= highHz)
        {
            sum += band.levelDb;
            ++count;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}
}

RoomCalibration::RoomCalibration() : Thread("CSP Room Calibration", 1024 * 1024) {}
RoomCalibration::~RoomCalibration() { stop(); }

bool RoomCalibration::startAnalysis(const juce::File& measurement)
{
    if (isThreadRunning() || !measurement.existsAsFile()) return false;
    sourceFile = measurement;
    {
        const juce::ScopedLock lock(resultLock);
        result = {};
        result.running = true;
        result.sourceName = measurement.getFileName();
    }
    startThread(juce::Thread::Priority::low);
    return true;
}

void RoomCalibration::stop()
{
    signalThreadShouldExit();
    stopThread(5000);
}

RoomResult RoomCalibration::getResult() const
{
    const juce::ScopedLock lock(resultLock);
    return result;
}

RoomResult RoomCalibration::analyse(const float* mono, int sampleCount, double sampleRate)
{
    RoomResult value;
    if (mono == nullptr || sampleCount < analysisFftSize || sampleRate <= 0.0)
    {
        value.error = "The measurement is too short to analyse";
        return value;
    }

    auto peak = 0.0f;
    double squareSum = 0.0;
    auto peakIndex = 0;
    for (int sample = 0; sample < sampleCount; ++sample)
    {
        const auto magnitude = std::abs(mono[sample]);
        if (magnitude > peak)
        {
            peak = magnitude;
            peakIndex = sample;
        }
        squareSum += static_cast<double>(mono[sample]) * mono[sample];
    }
    const auto rms = std::sqrt(squareSum / sampleCount);
    if (peak < 1.0e-4f || rms < 1.0e-6)
    {
        value.error = "The measurement is silent or far too quiet";
        return value;
    }
    const auto crestDb = static_cast<float>(20.0 * std::log10(peak / rms));

    // A room impulse response is dominated by one transient followed by decay;
    // a pink-noise measurement is not. Only the first case can yield RT60.
    const auto isImpulse = crestDb > 18.0f && peakIndex < sampleCount / 2;
    value.measurementType = isImpulse ? "Impulse response" : "Steady-state noise";

    juce::dsp::FFT fft(analysisFftOrder);
    juce::dsp::WindowingFunction<float> window(analysisFftSize,
                                               juce::dsp::WindowingFunction<float>::hann, true);
    std::vector<float> frame(static_cast<size_t>(analysisFftSize) * 2, 0.0f);
    std::vector<double> power(static_cast<size_t>(analysisFftSize / 2 + 1), 0.0);
    auto frames = 0;
    for (int start = 0; start + analysisFftSize <= sampleCount; start += analysisHop)
    {
        std::copy(mono + start, mono + start + analysisFftSize, frame.begin());
        std::fill(frame.begin() + analysisFftSize, frame.end(), 0.0f);
        window.multiplyWithWindowingTable(frame.data(), analysisFftSize);
        fft.performFrequencyOnlyForwardTransform(frame.data());
        for (size_t bin = 0; bin < power.size(); ++bin)
        {
            const auto magnitude = static_cast<double>(frame[bin]) / (analysisFftSize * 0.5);
            power[bin] += magnitude * magnitude;
        }
        ++frames;
    }
    if (frames == 0)
    {
        value.error = "The measurement is too short to analyse";
        return value;
    }
    for (auto& bin : power)
        bin /= frames;

    const auto binWidth = sampleRate / analysisFftSize;
    const auto nyquist = sampleRate * 0.5;
    for (int index = 0; index < RoomResult::bandCapacity; ++index)
    {
        const auto centre = bandCentre(index);
        const auto low = centre / std::pow(2.0f, 1.0f / 6.0f);
        const auto high = centre * std::pow(2.0f, 1.0f / 6.0f);
        if (high >= nyquist) break;

        double bandPower = 0.0;
        auto bins = 0;
        for (size_t bin = 1; bin < power.size(); ++bin)
        {
            const auto frequency = static_cast<double>(bin) * binWidth;
            if (frequency >= low && frequency < high)
            {
                bandPower += power[bin];
                ++bins;
            }
        }
        if (bins == 0) continue;
        auto& band = value.bands[static_cast<size_t>(value.bandCount++)];
        band.centreHz = centre;
        band.levelDb = static_cast<float>(10.0 * std::log10(std::max(bandPower, 1.0e-20)));
    }
    if (value.bandCount == 0)
    {
        value.error = "The measurement has no usable bandwidth";
        return value;
    }

    // Everything is relative to the speech range: an absolute SPL calibration
    // would need a calibrated microphone and is not what these numbers claim.
    const auto reference = meanLevel(value, 200.0f, 4000.0f);
    for (int index = 0; index < value.bandCount; ++index)
        value.bands[static_cast<size_t>(index)].levelDb -= reference;

    value.lowTiltDb = meanLevel(value, 40.0f, 160.0f);
    value.highTiltDb = meanLevel(value, 4000.0f, 16000.0f);

    if (isImpulse)
    {
        // Schroeder backward integration from the direct sound, T20 extrapolated
        // to RT60. T20 is used because a live room rarely has 60 dB of clean
        // decay above its own noise floor.
        std::vector<double> schroeder(static_cast<size_t>(sampleCount - peakIndex), 0.0);
        double accumulated = 0.0;
        for (int sample = sampleCount - 1; sample >= peakIndex; --sample)
        {
            accumulated += static_cast<double>(mono[sample]) * mono[sample];
            schroeder[static_cast<size_t>(sample - peakIndex)] = accumulated;
        }
        const auto total = schroeder[0];
        if (total > 0.0)
        {
            auto timeAt = [&](double targetDb) -> double
            {
                const auto threshold = total * std::pow(10.0, targetDb / 10.0);
                for (size_t index = 0; index < schroeder.size(); ++index)
                    if (schroeder[index] <= threshold)
                        return static_cast<double>(index) / sampleRate;
                return -1.0;
            };
            const auto fiveDb = timeAt(-5.0);
            const auto twentyFiveDb = timeAt(-25.0);
            if (fiveDb >= 0.0 && twentyFiveDb > fiveDb)
            {
                value.rt60Seconds = static_cast<float>(3.0 * (twentyFiveDb - fiveDb));
                value.rt60Available = value.rt60Seconds > 0.05f && value.rt60Seconds < 15.0f;
            }
        }
    }

    // Recommendations for the X32 matrix EQ, worst offender first. Only cuts:
    // boosting a room null wastes headroom and usually makes feedback worse.
    std::array<int, RoomResult::bandCapacity> order {};
    auto candidates = 0;
    for (int index = 0; index < value.bandCount; ++index)
    {
        const auto& band = value.bands[static_cast<size_t>(index)];
        if (band.centreHz >= 40.0f && band.centreHz <= 500.0f && band.levelDb > 3.0f)
            order[static_cast<size_t>(candidates++)] = index;
    }
    std::sort(order.begin(), order.begin() + candidates, [&](int left, int right)
    {
        return value.bands[static_cast<size_t>(left)].levelDb
             > value.bands[static_cast<size_t>(right)].levelDb;
    });
    for (int index = 0; index < std::min(candidates, 3); ++index)
    {
        const auto& band = value.bands[static_cast<size_t>(order[static_cast<size_t>(index)])];
        auto& recommendation = value.recommendations[static_cast<size_t>(value.recommendationCount++)];
        recommendation.frequencyHz = band.centreHz;
        recommendation.gainDb = -std::min(6.0f, band.levelDb - 1.0f);
        recommendation.q = 4.0f;
        recommendation.reason = "Room resonance " + juce::String(static_cast<double>(band.levelDb), 1)
            + " dB above the speech range";
    }

    if (value.highTiltDb > 4.0f && value.recommendationCount < RoomResult::recommendationCapacity)
    {
        auto& recommendation = value.recommendations[static_cast<size_t>(value.recommendationCount++)];
        recommendation.frequencyHz = 8000.0f;
        recommendation.gainDb = -std::min(4.0f, value.highTiltDb - 2.0f);
        recommendation.q = 0.7f;
        recommendation.reason = "Bright room: high band sits "
            + juce::String(static_cast<double>(value.highTiltDb), 1) + " dB above the speech range";
    }
    else if (value.highTiltDb < -6.0f && value.recommendationCount < RoomResult::recommendationCapacity)
    {
        auto& recommendation = value.recommendations[static_cast<size_t>(value.recommendationCount++)];
        recommendation.frequencyHz = 8000.0f;
        recommendation.gainDb = 0.0f;
        recommendation.q = 0.7f;
        recommendation.reason = "Dull high end. Check speaker coverage and aiming before adding EQ; "
                                "boosting a coverage problem only costs headroom";
    }

    value.complete = true;
    return value;
}

void RoomCalibration::run()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sourceFile));
    if (reader == nullptr) { setError("Unsupported or unreadable measurement file"); return; }
    if (reader->lengthInSamples <= 0) { setError("The measurement file is empty"); return; }

    constexpr int64_t maximumSamples = 60 * 192000;
    const auto sampleCount = static_cast<int>(std::min<int64_t>(reader->lengthInSamples, maximumSamples));
    juce::AudioBuffer<float> buffer(static_cast<int>(std::max(1u, reader->numChannels)), sampleCount);
    buffer.clear();
    reader->read(&buffer, 0, sampleCount, 0, true, true);

    std::vector<float> mono(static_cast<size_t>(sampleCount), 0.0f);
    const auto channels = buffer.getNumChannels();
    for (int channel = 0; channel < channels; ++channel)
    {
        const auto* source = buffer.getReadPointer(channel);
        for (int sample = 0; sample < sampleCount; ++sample)
            mono[static_cast<size_t>(sample)] += source[sample] / static_cast<float>(channels);
    }
    {
        const juce::ScopedLock lock(resultLock);
        result.progress = 0.4f;
    }
    if (threadShouldExit()) { setError("Cancelled"); return; }

    auto analysed = analyse(mono.data(), sampleCount, reader->sampleRate);
    analysed.sourceName = sourceFile.getFileName();
    if (!analysed.complete)
    {
        setError(analysed.error.isNotEmpty() ? analysed.error : "The measurement could not be analysed");
        return;
    }

    const auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                               .getChildFile("ChurchStreamProcessor").getChildFile("Room Calibration");
    directory.createDirectory();
    analysed.reportFile = directory.getChildFile(
        sourceFile.getFileNameWithoutExtension() + "-room-"
        + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".txt");
    writeReport(analysed.reportFile, analysed);

    analysed.running = false;
    analysed.progress = 1.0f;
    {
        const juce::ScopedLock lock(resultLock);
        result = analysed;
    }
    juce::Logger::writeToLog("Room calibration complete: " + analysed.reportFile.getFullPathName());
}

void RoomCalibration::writeReport(const juce::File& target, const RoomResult& value) const
{
    juce::String text;
    text << "ROOM CALIBRATION\n"
         << "Source: " << value.sourceName << "\n"
         << "Measurement type: " << value.measurementType << "\n"
         << "Reverberation: "
         << (value.rt60Available
                 ? "RT60 " + juce::String(static_cast<double>(value.rt60Seconds), 2) + " s (T20 extrapolated)"
                 : juce::String("not available from this measurement"))
         << "\n\n"
         << "THIRD-OCTAVE RESPONSE, relative to the 200 Hz - 4 kHz average\n";
    for (int index = 0; index < value.bandCount; ++index)
    {
        const auto& band = value.bands[static_cast<size_t>(index)];
        text << juce::String(static_cast<double>(band.centreHz), 0).paddedLeft(' ', 6) << " Hz  "
             << juce::String(static_cast<double>(band.levelDb), 1).paddedLeft(' ', 6) << " dB\n";
    }

    text << "\nTILT\n"
         << "40-160 Hz:   " << juce::String(static_cast<double>(value.lowTiltDb), 1) << " dB\n"
         << "4-16 kHz:    " << juce::String(static_cast<double>(value.highTiltDb), 1) << " dB\n";

    text << "\nRECOMMENDED X32 MATRIX EQ\n";
    if (value.recommendationCount == 0)
        text << "None. No resonance above +3 dB in 40-500 Hz and no strong tilt.\n";
    for (int index = 0; index < value.recommendationCount; ++index)
    {
        const auto& recommendation = value.recommendations[static_cast<size_t>(index)];
        text << juce::String(static_cast<double>(recommendation.frequencyHz), 0) << " Hz | "
             << juce::String(static_cast<double>(recommendation.gainDb), 1) << " dB | Q "
             << juce::String(static_cast<double>(recommendation.q), 1) << " | "
             << recommendation.reason << "\n";
    }

    text << "\nThese are recommendations only. Nothing was applied to the console, and the\n"
            "streaming output is never sent back into the PA: that path would add latency\n"
            "and can create feedback. Apply them by hand, one at a time, and listen.\n";
    target.replaceWithText(text);
}

void RoomCalibration::setError(const juce::String& message)
{
    const juce::ScopedLock lock(resultLock);
    result.running = false;
    result.error = message;
    juce::Logger::writeToLog("Room calibration error: " + message);
}
} // namespace churchstream
