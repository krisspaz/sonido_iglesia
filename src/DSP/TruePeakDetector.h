#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace churchstream
{
// Inter-sample peak detector on a 4x oversampled signal, as recommended by
// ITU-R BS.1770-4. A windowed-sinc polyphase interpolator is used instead of
// the cheaper four-point cubic estimate: cubic interpolation is not band
// limited and understates the real inter-sample peak of dense, limited material
// by several tenths of a dB, which is exactly the margin the -1 dBTP ceiling is
// protecting.
//
// The detector reports the peak of the interval that ends at the sample fed in,
// so its reading lags the input by `latencySamples`. Callers that use it to
// drive a limiter must keep at least that much lookahead.
class TruePeakDetector final
{
public:
    static constexpr int maximumChannels = 2;
    static constexpr int phases = 4;
    static constexpr int taps = 12;
    static constexpr int latencySamples = (taps - 1) / 2;

    TruePeakDetector() noexcept
    {
        buildCoefficients();
        reset();
    }

    void reset() noexcept
    {
        for (auto& line : history)
            line.fill(0.0f);
        position.fill(0);
    }

    float process(int channel, float sample) noexcept
    {
        const auto index = static_cast<size_t>(std::clamp(channel, 0, maximumChannels - 1));
        auto& line = history[index];
        auto& write = position[index];

        // The delay line is stored twice so the sliding window is always
        // contiguous and the inner loop needs no modulo.
        line[static_cast<size_t>(write)] = sample;
        line[static_cast<size_t>(write + taps)] = sample;
        write = write + 1 == taps ? 0 : write + 1;

        const auto* window = line.data() + write;
        auto maximum = std::abs(window[taps / 2]);
        for (int phase = 0; phase < phases; ++phase)
        {
            auto sum = 0.0f;
            for (int tap = 0; tap < taps; ++tap)
                sum += coefficients[static_cast<size_t>(phase)][static_cast<size_t>(tap)] * window[tap];
            maximum = std::max(maximum, std::abs(sum));
        }
        return std::isfinite(maximum) ? maximum : 0.0f;
    }

private:
    void buildCoefficients() noexcept
    {
        constexpr int length = phases * taps;
        constexpr auto centre = (length - 1) * 0.5;
        std::array<double, length> prototype {};
        for (int n = 0; n < length; ++n)
        {
            const auto offset = static_cast<double>(n) - centre;
            const auto argument = std::numbers::pi * offset / phases;
            const auto sinc = std::abs(argument) < 1.0e-9 ? 1.0 : std::sin(argument) / argument;
            const auto ratio = 2.0 * std::numbers::pi * static_cast<double>(n) / (length - 1);
            const auto blackman = 0.42 - 0.5 * std::cos(ratio) + 0.08 * std::cos(2.0 * ratio);
            prototype[static_cast<size_t>(n)] = sinc * blackman;
        }

        for (int phase = 0; phase < phases; ++phase)
        {
            auto sum = 0.0;
            for (int tap = 0; tap < taps; ++tap)
                sum += prototype[static_cast<size_t>(tap * phases + phase)];
            // Normalising every phase to unity DC gain keeps a steady signal at
            // its own level regardless of which phase is evaluated.
            const auto scale = std::abs(sum) > 1.0e-12 ? 1.0 / sum : 1.0;
            for (int tap = 0; tap < taps; ++tap)
                coefficients[static_cast<size_t>(phase)][static_cast<size_t>(taps - 1 - tap)] =
                    static_cast<float>(prototype[static_cast<size_t>(tap * phases + phase)] * scale);
        }
    }

    std::array<std::array<float, taps>, phases> coefficients {};
    std::array<std::array<float, taps * 2>, maximumChannels> history {};
    std::array<int, maximumChannels> position {};
};
} // namespace churchstream
