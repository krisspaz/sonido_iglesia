#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace churchstream
{
// Gives a mono programme some width without changing its mono sum.
//
// Both recorded services arrived at the stream as mono -- left and right
// identical, Side 40-60 dB under Mid -- for everything except the produced
// videos. A side signal is synthesised from the Mid through a chain of short
// Schroeder all-passes: flat in magnitude, scrambled in phase, so the two
// channels decorrelate without either being coloured much. Because it is added
// to one channel and subtracted from the other, L + R is exactly 2 * Mid: on a
// single phone speaker nothing at all changes.
//
// It only engages when the input really is mono. A real stereo source keeps its
// own image untouched.
class MonoSpread final
{
public:
    // Side level relative to Mid. About -13 dB: enough to be heard as width on
    // headphones and a stereo pair, little enough that each channel's ripple
    // stays within about +/-2 dB.
    static constexpr float sideAmount = 0.22f;
    // Below this, nothing is spread: the voice and the kick stay in the centre.
    static constexpr float highPassHz = 350.0f;
    // Side under this much below Mid is treated as mono; over the second value
    // as stereo. Between them the spread fades.
    static constexpr float monoBelowDb = -35.0f;
    static constexpr float stereoAboveDb = -25.0f;
    static constexpr double detectorSeconds = 2.0;
    static constexpr double fadeSeconds = 2.0;
    static constexpr float allPassGain = 0.5f;
    // Mutually prime, all short: long enough to decorrelate, short enough not to
    // be heard as an echo or a room.
    static constexpr std::array<double, 4> allPassMilliseconds { 4.7, 7.3, 11.3, 16.9 };

    void prepare(double sampleRateToUse)
    {
        sampleRate = sampleRateToUse;
        for (size_t stage = 0; stage < allPassMilliseconds.size(); ++stage)
        {
            const auto length = std::max(1, static_cast<int>(std::lround(allPassMilliseconds[stage] * 0.001 * sampleRate)));
            delays[stage].assign(static_cast<size_t>(length), 0.0f);
        }
        highPass.setHighPass(sampleRate, highPassHz);
        detectorCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * detectorSeconds));
        fadeStep = static_cast<float>(1.0 / (sampleRate * fadeSeconds));
        reset();
    }

    void reset() noexcept
    {
        for (auto& line : delays)
            std::fill(line.begin(), line.end(), 0.0f);
        positions.fill(0);
        highPass.reset();
        midSquare = 0.0f;
        sideSquare = 0.0f;
        weight = 0.0f;
    }

    // `inputLeft`/`inputRight` are the untouched console input, used only to
    // decide whether the source is mono. Returns the side signal to add to the
    // processed Mid.
    [[nodiscard]] float process(float inputLeft, float inputRight, float processedMid, bool enabled) noexcept
    {
        const auto inputMid = 0.5f * (inputLeft + inputRight);
        const auto inputSide = 0.5f * (inputLeft - inputRight);
        midSquare = detectorCoefficient * midSquare + (1.0f - detectorCoefficient) * inputMid * inputMid;
        sideSquare = detectorCoefficient * sideSquare + (1.0f - detectorCoefficient) * inputSide * inputSide;

        auto target = 0.0f;
        if (enabled && midSquare > 1.0e-10f)
        {
            const auto sideDb = 10.0f * std::log10(std::max(sideSquare, 1.0e-20f) / midSquare);
            target = std::clamp((stereoAboveDb - sideDb) / (stereoAboveDb - monoBelowDb), 0.0f, 1.0f);
        }
        weight += std::clamp(target - weight, -fadeStep, fadeStep);

        // The all-passes run even at zero weight so that fading in starts from
        // a settled chain rather than from silence.
        auto value = highPass.process(0, processedMid);
        for (size_t stage = 0; stage < delays.size(); ++stage)
        {
            auto& line = delays[stage];
            auto& position = positions[stage];
            const auto delayed = line[position];
            const auto written = value + allPassGain * delayed;
            value = delayed - allPassGain * written;
            line[position] = written;
            if (++position >= line.size())
                position = 0;
        }
        return std::isfinite(value) ? value * sideAmount * weight : 0.0f;
    }

    [[nodiscard]] float getWeight() const noexcept { return weight; }

private:
    double sampleRate = 48000.0;
    std::array<std::vector<float>, allPassMilliseconds.size()> delays;
    std::array<size_t, allPassMilliseconds.size()> positions {};
    Biquad highPass;
    float midSquare = 0.0f;
    float sideSquare = 0.0f;
    float detectorCoefficient = 0.0f;
    float weight = 0.0f;
    float fadeStep = 0.0f;
};
} // namespace churchstream
