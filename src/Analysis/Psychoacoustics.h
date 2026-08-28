#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Zwicker/Schroeder masking and ANSI S3.5 intelligibility, as pure functions.
//
// Everything here is deliberately free of any audio plumbing: these are the
// parts that can be checked against published numbers, and keeping them
// separate is what makes that possible.
namespace churchstream::psychoacoustics
{
// The 21 critical bands of ANSI S3.5-1997, with the band importance function
// for average speech. The weights are the standard's, not a fit of our own,
// and they sum to 1 so an SII of 1.0 means every band is fully audible.
constexpr int criticalBandCount = 21;

constexpr std::array<float, criticalBandCount> criticalBandCentresHz {
    150.0f, 250.0f, 350.0f, 450.0f, 570.0f, 700.0f, 840.0f,
    1000.0f, 1170.0f, 1370.0f, 1600.0f, 1850.0f, 2150.0f,
    2500.0f, 2900.0f, 3400.0f, 4000.0f, 4800.0f, 5800.0f, 7000.0f, 8500.0f
};

constexpr std::array<float, criticalBandCount> speechImportance {
    0.0103f, 0.0261f, 0.0419f, 0.0577f, 0.0577f, 0.0577f, 0.0577f,
    0.0577f, 0.0577f, 0.0577f, 0.0577f, 0.0577f, 0.0577f,
    0.0577f, 0.0577f, 0.0577f, 0.0577f, 0.0460f, 0.0343f, 0.0226f, 0.0110f
};

// Zwicker's critical band rate. Returns the Bark value of a frequency, which
// is the scale on which masking spreads at a constant rate.
[[nodiscard]] inline float barkFromHertz(float hertz) noexcept
{
    const auto f = std::max(0.0f, hertz);
    return 13.0f * std::atan(0.00076f * f)
         + 3.5f * std::atan((f / 7500.0f) * (f / 7500.0f));
}

// Schroeder's spreading function, in dB relative to the masker. It peaks at
// roughly 0 dB on the masker itself and is asymmetric: masking reaches much
// further up in frequency than down, which is why a loud low guitar covers a
// voice above it and not the other way round.
[[nodiscard]] inline float spreadingDb(float deltaBark) noexcept
{
    const auto x = deltaBark + 0.474f;
    return 15.81f + 7.5f * x - 17.5f * std::sqrt(1.0f + x * x);
}

// The masking threshold each band imposes on the others, given per-band masker
// levels in dB. Contributions are summed as powers, not as decibels, because
// two maskers 3 dB apart do not mask 3 dB better than one.
inline void maskingThresholdDb(const std::array<float, criticalBandCount>& maskerDb,
                               std::array<float, criticalBandCount>& thresholdDb) noexcept
{
    static const auto bandBark = [] {
        std::array<float, criticalBandCount> value {};
        for (int band = 0; band < criticalBandCount; ++band)
            value[static_cast<size_t>(band)] = barkFromHertz(criticalBandCentresHz[static_cast<size_t>(band)]);
        return value;
    }();

    for (int target = 0; target < criticalBandCount; ++target)
    {
        auto power = 0.0f;
        for (int masker = 0; masker < criticalBandCount; ++masker)
        {
            const auto spread = spreadingDb(bandBark[static_cast<size_t>(target)]
                                            - bandBark[static_cast<size_t>(masker)]);
            power += std::pow(10.0f, (maskerDb[static_cast<size_t>(masker)] + spread) * 0.1f);
        }
        thresholdDb[static_cast<size_t>(target)] = power > 1.0e-30f
            ? 10.0f * std::log10(power) : -200.0f;
    }
}

// As above, but also reports, for each target band, what fraction of its
// masking threshold each masker band is responsible for. Knowing a voice band
// is masked is only half the problem: the reduction has to be applied to
// whichever music band is doing the masking, which is frequently not the band
// the voice is in. Schroeder's function reaches upwards, so a loud low guitar
// masks a vowel well above it, and attenuating the music at the vowel's own
// frequency would remove music that was never the cause.
inline void maskingThresholdAndContributions(
    const std::array<float, criticalBandCount>& maskerDb,
    std::array<float, criticalBandCount>& thresholdDb,
    std::array<std::array<float, criticalBandCount>, criticalBandCount>& contribution) noexcept
{
    static const auto bandBark = [] {
        std::array<float, criticalBandCount> value {};
        for (int band = 0; band < criticalBandCount; ++band)
            value[static_cast<size_t>(band)] = barkFromHertz(criticalBandCentresHz[static_cast<size_t>(band)]);
        return value;
    }();

    for (int target = 0; target < criticalBandCount; ++target)
    {
        const auto t = static_cast<size_t>(target);
        auto total = 0.0f;
        for (int masker = 0; masker < criticalBandCount; ++masker)
        {
            const auto m = static_cast<size_t>(masker);
            const auto spread = spreadingDb(bandBark[t] - bandBark[m]);
            const auto power = std::pow(10.0f, (maskerDb[m] + spread) * 0.1f);
            contribution[t][m] = power;
            total += power;
        }
        thresholdDb[t] = total > 1.0e-30f ? 10.0f * std::log10(total) : -200.0f;
        const auto scale = total > 1.0e-30f ? 1.0f / total : 0.0f;
        for (int masker = 0; masker < criticalBandCount; ++masker)
            contribution[t][static_cast<size_t>(masker)] *= scale;
    }
}

// Per-band audibility: nothing below the masker, everything 30 dB above it,
// linear in between. This is the SII's band audibility function with its
// standard 30 dB dynamic range.
[[nodiscard]] inline float bandAudibility(float speechDb, float maskerDb) noexcept
{
    return std::clamp((speechDb - maskerDb + 15.0f) / 30.0f, 0.0f, 1.0f);
}

// Speech Intelligibility Index, 0 to 1. Below about 0.75 a listener starts
// having to work for the words; below 0.45 a sermon is being lost.
[[nodiscard]] inline float speechIntelligibilityIndex(
    const std::array<float, criticalBandCount>& speechDb,
    const std::array<float, criticalBandCount>& maskerDb) noexcept
{
    auto index = 0.0f;
    for (int band = 0; band < criticalBandCount; ++band)
        index += speechImportance[static_cast<size_t>(band)]
               * bandAudibility(speechDb[static_cast<size_t>(band)],
                                maskerDb[static_cast<size_t>(band)]);
    return std::clamp(index, 0.0f, 1.0f);
}

// Inverts the Bark scale by bisection. Only used when laying out filters, so
// the cost is irrelevant and a closed form is not worth the approximation error.
[[nodiscard]] inline float hertzFromBark(float bark) noexcept
{
    auto low = 0.0f;
    auto high = 20000.0f;
    for (int iteration = 0; iteration < 60; ++iteration)
    {
        const auto middle = 0.5f * (low + high);
        if (barkFromHertz(middle) < bark) low = middle; else high = middle;
    }
    return 0.5f * (low + high);
}
} // namespace churchstream::psychoacoustics
