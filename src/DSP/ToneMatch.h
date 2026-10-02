#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace churchstream
{
// Slow, broad EQ that brings the programme's long-term tonal balance to a fixed
// target curve.
//
// The Smart Engine's tonal corrections are judged against a baseline it learns
// from the church itself, which is right for reacting to a problem that comes
// and goes and wrong for a room that always sounds the same way: a service that
// is dull every week teaches the baseline that dull is normal, and nothing is
// ever opened up. This compares against an absolute curve instead.
//
// What to correct is decided open loop: the balance is measured on the signal
// entering these filters, so the target correction depends on the programme and
// never on the correction itself. How far the filters have got is measured too,
// through the same bands on their output. That second measurement is what the
// gains are integrated against, because the effect of a filter on a band is not
// its response at the band centre: the air band spans roughly 9-19 kHz, and on a
// real programme -- falling steeply, and cut above 16 kHz by every streaming
// codec -- nearly all of its energy sits in the bottom of that span, where a
// 12 kHz shelf does far less than at 13 kHz. Solving on centre responses left
// the shelf at its limit and the band 3 dB short on recorded services. The
// out/in ratio depends only on the filters and the current spectrum, so the
// loop it closes cannot hunt.
class ToneMatch final
{
public:
    // Six measurement bands; band 2 (1 kHz) is the reference the others are
    // judged against, so the result is about shape and not about level.
    static constexpr int bandCount = 6;
    static constexpr int referenceBand = 2;
    static constexpr std::array<float, bandCount> bandCentresHz { 100.0f, 280.0f, 1000.0f, 3200.0f, 7000.0f, 13000.0f };
    // Each band is two identical band-pass sections in cascade, 12 dB/octave
    // skirts. A single section falls only 6 dB/octave, and on a programme whose
    // spectrum falls steeply that lets the neighbouring band dominate: the air
    // band read mostly 7 kHz energy and the 280 Hz band, during a sermon, mostly
    // the 1 kHz region, so a -4 dB cut there moved its reading by under 1 dB.
    static constexpr float bandQ = 0.7f;

    // The target, in dB relative to 1 kHz, measured through the same band-pass
    // filters as the live programme. Taken from the produced videos that open
    // and close the church's own streams (three segments, within 1.5 dB of each
    // other in every band but the lowest), which is the sound the services were
    // being compared with. Live services measured against it were 4-10 dB short
    // in the air band and, during the sermon, 4 dB heavy around 280 Hz.
    static constexpr std::array<float, bandCount> targetDb { -0.7f, -1.5f, 0.0f, -6.6f, -9.8f, -13.0f };

    // One control per band other than the reference: a low shelf, three peaks
    // and a high shelf, each centred on the band it corrects.
    static constexpr int controlCount = 5;
    static constexpr std::array<int, controlCount> controlBand { 0, 1, 3, 4, 5 };
    // Asymmetric on purpose. Adding low end on a stream heard mostly through
    // phone speakers buys little and costs headroom, and a sermon's low band is
    // mostly handling noise and room; the top end is where live services fall
    // short. The top two stop short of what a sermon would need to reach the
    // target fully (+8 dB of air, measured): the target is a produced music
    // mix, and lifting a live speech microphone all the way to it sits after
    // the de-esser, where a lifted sibilant is no longer caught. The cuts are
    // gentle too: the target is music, and speech carries more energy around
    // 400-500 Hz than music does, so part of what reads as a heavy 280 Hz band
    // in a sermon is the voice itself and cutting it all would thin it; and the
    // low band moves with every song, which is not worth following far.
    static constexpr std::array<float, controlCount> minimumGainDb { -3.0f, -3.0f, -3.0f, -3.0f, -4.0f };
    static constexpr std::array<float, controlCount> maximumGainDb { 2.0f, 1.0f, 4.0f, 5.0f, 6.0f };

    // Long enough that a single song or a loud phrase cannot steer the target;
    // short enough to follow the change from worship to sermon within a minute.
    static constexpr double measurementSeconds = 15.0;
    // The filters' own effect is a ratio of output to input over the same
    // stretch of programme, so it settles far faster than the balance itself.
    static constexpr double effectSeconds = 3.0;
    // Nothing is applied until this much programme has been measured.
    static constexpr double warmUpSeconds = 10.0;
    // Integration rate, in dB per second per dB of error, and the hard limit on
    // how fast any gain may move, so the change is never heard as movement.
    static constexpr float integrationRate = 0.15f;
    static constexpr float slewDbPerSecond = 0.5f;

    void prepare(double sampleRateToUse) noexcept
    {
        sampleRate = sampleRateToUse;
        for (size_t band = 0; band < bandCount; ++band)
            for (size_t section = 0; section < detectorSections; ++section)
            {
                inputDetectors[band][section].setBandPass(sampleRate, bandCentresHz[band], bandQ);
                outputDetectors[band][section].setBandPass(sampleRate, bandCentresHz[band], bandQ);
            }
        reset();
    }

    void reset() noexcept
    {
        for (auto& band : inputDetectors)
            for (auto& section : band)
                section.reset();
        for (auto& band : outputDetectors)
            for (auto& section : band)
                section.reset();
        for (auto& filter : filters)
            filter.reset();
        inputSquares.fill(0.0);
        outputSquares.fill(0.0);
        balancePower.fill(0.0);
        effectInputPower.fill(0.0);
        effectOutputPower.fill(0.0);
        measuredSeconds = 0.0;
        blockSamples = 0;
        gainDb.fill(0.0f);
        configure(filters, gainDb, sampleRate);
    }

    // One frame: `inputMono` is the mono sum entering the filters, and the
    // filtered stereo pair is written back in place.
    void process(float* frame, int channels) noexcept
    {
        auto inputMono = 0.0f;
        auto outputMono = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
        {
            inputMono += frame[channel];
            auto value = frame[channel];
            for (auto& filter : filters)
                value = filter.process(channel, value);
            frame[channel] = value;
            outputMono += value;
        }
        const auto scale = 1.0f / static_cast<float>(std::max(1, channels));
        inputMono *= scale;
        outputMono *= scale;
        for (size_t band = 0; band < bandCount; ++band)
        {
            auto in = inputMono;
            auto out = outputMono;
            for (size_t section = 0; section < detectorSections; ++section)
            {
                in = inputDetectors[band][section].process(0, in);
                out = outputDetectors[band][section].process(0, out);
            }
            inputSquares[band] += static_cast<double>(in) * in;
            outputSquares[band] += static_cast<double>(out) * out;
        }
        ++blockSamples;
    }

    // Called once per block. `programmePresent` gates the measurement so pauses
    // and silence do not count as a dull programme; `active` false walks every
    // correction back to flat.
    void endBlock(bool programmePresent, bool active) noexcept
    {
        const auto samples = std::max(1, blockSamples);
        const auto seconds = static_cast<double>(samples) / sampleRate;
        if (programmePresent && blockSamples > 0)
        {
            measuredSeconds += seconds;
            // A cumulative mean until the window has filled, then exponential:
            // the same reasoning as the leveller's gate reference.
            const auto balanceAlpha = std::max(seconds / measuredSeconds,
                                               1.0 - std::exp(-seconds / measurementSeconds));
            const auto effectAlpha = std::max(seconds / measuredSeconds,
                                              1.0 - std::exp(-seconds / effectSeconds));
            for (size_t band = 0; band < bandCount; ++band)
            {
                const auto in = inputSquares[band] / samples;
                const auto out = outputSquares[band] / samples;
                balancePower[band] += balanceAlpha * (in - balancePower[band]);
                effectInputPower[band] += effectAlpha * (in - effectInputPower[band]);
                effectOutputPower[band] += effectAlpha * (out - effectOutputPower[band]);
            }
        }
        inputSquares.fill(0.0);
        outputSquares.fill(0.0);
        blockSamples = 0;

        const auto ready = active && measuredSeconds >= warmUpSeconds && balancePower[referenceBand] > 1.0e-14;
        const auto limit = slewDbPerSecond * static_cast<float>(seconds);
        auto changed = false;
        for (size_t control = 0; control < controlCount; ++control)
        {
            auto delta = 0.0f;
            if (ready && programmePresent)
            {
                const auto band = controlBand[control];
                const auto error = targetDb[static_cast<size_t>(band)] - getMeasuredDb(band) - getEffectDb(band);
                delta = integrationRate * error * static_cast<float>(seconds);
            }
            else if (!active)
            {
                delta = -gainDb[control];
            }
            // A closed gate with the correction active holds it where it is.
            delta = std::clamp(delta, -limit, limit);
            const auto next = std::clamp(gainDb[control] + delta, minimumGainDb[control], maximumGainDb[control]);
            changed = changed || std::abs(next - gainDb[control]) > 1.0e-6f;
            gainDb[control] = next;
        }
        if (changed)
            configure(filters, gainDb, sampleRate);
    }

    [[nodiscard]] const std::array<float, controlCount>& getGainsDb() const noexcept { return gainDb; }

    // Measured balance of the programme entering the filters, dB relative to
    // 1 kHz.
    [[nodiscard]] float getMeasuredDb(int band) const noexcept
    {
        return ratioDb(balancePower[static_cast<size_t>(band)], balancePower[referenceBand]);
    }

    // What the filters are currently doing to `band`, relative to what they do
    // to the 1 kHz reference.
    [[nodiscard]] float getEffectDb(int band) const noexcept
    {
        const auto index = static_cast<size_t>(band);
        return ratioDb(effectOutputPower[index], effectInputPower[index])
            - ratioDb(effectOutputPower[referenceBand], effectInputPower[referenceBand]);
    }

    // Combined magnitude response of the tone-match filters for a given set of
    // gains. Exposed so the Smart Engine can judge the mix as it would be
    // without them.
    [[nodiscard]] static float responseDb(const std::array<float, controlCount>& gains,
                                          double sampleRate, float frequency) noexcept
    {
        return responseDb(makeFilters(gains, sampleRate), sampleRate, frequency);
    }

    [[nodiscard]] static float responseDb(const std::array<Biquad, controlCount>& filtersToEvaluate,
                                          double sampleRate, float frequency) noexcept
    {
        auto total = 0.0;
        for (const auto& filter : filtersToEvaluate)
            total += filter.magnitudeDb(sampleRate, frequency);
        return static_cast<float>(total);
    }

    // The filters for a given set of gains, for evaluating many frequencies
    // without rebuilding them each time.
    [[nodiscard]] static std::array<Biquad, controlCount> makeFilters(const std::array<float, controlCount>& gains,
                                                                      double sampleRate) noexcept
    {
        std::array<Biquad, controlCount> result;
        configure(result, gains, sampleRate);
        return result;
    }

private:
    static float ratioDb(double numerator, double denominator) noexcept
    {
        return numerator > 1.0e-14 && denominator > 1.0e-14
            ? static_cast<float>(10.0 * std::log10(numerator / denominator)) : 0.0f;
    }

    static void configure(std::array<Biquad, controlCount>& target,
                          const std::array<float, controlCount>& gains, double sampleRate) noexcept
    {
        target[0].setLowShelf(sampleRate, 120.0f, 0.70f, gains[0]);
        // Wider and higher than the 280 Hz band it is judged on: in a sermon the
        // energy of that band sits towards 400-500 Hz, where a 280 Hz Q 1 peak
        // at -4 dB moved the band by under 1 dB.
        target[1].setPeak(sampleRate, 350.0f, 0.80f, gains[1]);
        target[2].setPeak(sampleRate, 3200.0f, 0.90f, gains[2]);
        target[3].setPeak(sampleRate, 7000.0f, 1.00f, gains[3]);
        target[4].setHighShelf(sampleRate, 12000.0f, 0.70f, gains[4]);
    }

    static constexpr size_t detectorSections = 2;
    double sampleRate = 48000.0;
    std::array<std::array<Biquad, detectorSections>, bandCount> inputDetectors;
    std::array<std::array<Biquad, detectorSections>, bandCount> outputDetectors;
    std::array<Biquad, controlCount> filters;
    std::array<double, bandCount> inputSquares {};
    std::array<double, bandCount> outputSquares {};
    std::array<double, bandCount> balancePower {};
    std::array<double, bandCount> effectInputPower {};
    std::array<double, bandCount> effectOutputPower {};
    double measuredSeconds = 0.0;
    int blockSamples = 0;
    std::array<float, controlCount> gainDb {};
};
} // namespace churchstream
