#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace churchstream
{
class Biquad final
{
public:
    void reset() noexcept
    {
        z1.fill(0.0f);
        z2.fill(0.0f);
    }

    void setIdentity() noexcept
    {
        b0 = 1.0f;
        b1 = b2 = a1 = a2 = 0.0f;
    }

    void setHighPass(double sampleRate, float frequency, float q = 0.70710678f) noexcept
    {
        if (sampleRate <= 0.0)
            return;

        const auto omega = 2.0 * std::numbers::pi * std::clamp(static_cast<double>(frequency), 5.0, sampleRate * 0.45) / sampleRate;
        const auto cosine = std::cos(omega);
        const auto sine = std::sin(omega);
        const auto alpha = sine / (2.0 * std::max(0.05, static_cast<double>(q)));
        const auto a0 = 1.0 + alpha;
        b0 = static_cast<float>(((1.0 + cosine) * 0.5) / a0);
        b1 = static_cast<float>(-(1.0 + cosine) / a0);
        b2 = b0;
        a1 = static_cast<float>((-2.0 * cosine) / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    void setLowPass(double sampleRate, float frequency, float q = 0.70710678f) noexcept
    {
        if (sampleRate <= 0.0)
            return;

        const auto omega = 2.0 * std::numbers::pi * std::clamp(static_cast<double>(frequency), 10.0, sampleRate * 0.45) / sampleRate;
        const auto cosine = std::cos(omega);
        const auto sine = std::sin(omega);
        const auto alpha = sine / (2.0 * std::max(0.05, static_cast<double>(q)));
        const auto a0 = 1.0 + alpha;
        b0 = static_cast<float>(((1.0 - cosine) * 0.5) / a0);
        b1 = static_cast<float>((1.0 - cosine) / a0);
        b2 = b0;
        a1 = static_cast<float>((-2.0 * cosine) / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    // Constant peak gain band-pass, used for band energy detection.
    void setBandPass(double sampleRate, float frequency, float q) noexcept
    {
        if (sampleRate <= 0.0)
            return;

        const auto omega = 2.0 * std::numbers::pi * std::clamp(static_cast<double>(frequency), 10.0, sampleRate * 0.45) / sampleRate;
        const auto cosine = std::cos(omega);
        const auto sine = std::sin(omega);
        const auto alpha = sine / (2.0 * std::max(0.05, static_cast<double>(q)));
        const auto a0 = 1.0 + alpha;
        b0 = static_cast<float>(alpha / a0);
        b1 = 0.0f;
        b2 = static_cast<float>(-alpha / a0);
        a1 = static_cast<float>((-2.0 * cosine) / a0);
        a2 = static_cast<float>((1.0 - alpha) / a0);
    }

    void setPeak(double sampleRate, float frequency, float q, float gainDb) noexcept
    {
        if (sampleRate <= 0.0)
            return;

        const auto amplitude = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
        const auto omega = 2.0 * std::numbers::pi * std::clamp(static_cast<double>(frequency), 10.0, sampleRate * 0.45) / sampleRate;
        const auto cosine = std::cos(omega);
        const auto alpha = std::sin(omega) / (2.0 * std::max(0.05, static_cast<double>(q)));
        const auto a0 = 1.0 + alpha / amplitude;
        b0 = static_cast<float>((1.0 + alpha * amplitude) / a0);
        b1 = static_cast<float>((-2.0 * cosine) / a0);
        b2 = static_cast<float>((1.0 - alpha * amplitude) / a0);
        a1 = static_cast<float>((-2.0 * cosine) / a0);
        a2 = static_cast<float>((1.0 - alpha / amplitude) / a0);
    }

    float process(int channel, float input) noexcept
    {
        const auto index = static_cast<size_t>(std::clamp(channel, 0, 1));
        const auto output = b0 * input + z1[index];
        z1[index] = b1 * input - a1 * output + z2[index];
        z2[index] = b2 * input - a2 * output;
        return std::isfinite(output) ? output : 0.0f;
    }

private:
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    std::array<float, 2> z1 { 0.0f, 0.0f };
    std::array<float, 2> z2 { 0.0f, 0.0f };
};
} // namespace churchstream

