#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

namespace churchstream
{
class MeterSource final
{
public:
    static constexpr int channelCount = 2;

    void reset() noexcept
    {
        for (auto& channel : channels)
        {
            channel.peak.store(0.0f, std::memory_order_relaxed);
            channel.rms.store(0.0f, std::memory_order_relaxed);
        }
    }

    void push(const float* const* samples, int numChannels, int numSamples) noexcept
    {
        for (int channel = 0; channel < channelCount; ++channel)
        {
            if (samples == nullptr || channel >= numChannels || samples[channel] == nullptr || numSamples <= 0)
            {
                channels[static_cast<size_t>(channel)].peak.store(0.0f, std::memory_order_relaxed);
                channels[static_cast<size_t>(channel)].rms.store(0.0f, std::memory_order_relaxed);
                continue;
            }

            const auto* data = samples[channel];
            auto peak = 0.0f;
            double squareSum = 0.0;

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const auto value = data[sample];
                peak = juce::jmax(peak, std::abs(value));
                squareSum += static_cast<double>(value) * static_cast<double>(value);
            }

            channels[static_cast<size_t>(channel)].peak.store(peak, std::memory_order_release);
            channels[static_cast<size_t>(channel)].rms.store(
                static_cast<float>(std::sqrt(squareSum / static_cast<double>(numSamples))),
                std::memory_order_release);
        }
    }

    [[nodiscard]] float getPeak(int channel) const noexcept
    {
        return isValid(channel) ? channels[static_cast<size_t>(channel)].peak.load(std::memory_order_acquire) : 0.0f;
    }

    [[nodiscard]] float getRms(int channel) const noexcept
    {
        return isValid(channel) ? channels[static_cast<size_t>(channel)].rms.load(std::memory_order_acquire) : 0.0f;
    }

private:
    struct Channel
    {
        std::atomic<float> peak { 0.0f };
        std::atomic<float> rms { 0.0f };
    };

    static constexpr bool isValid(int channel) noexcept
    {
        return channel >= 0 && channel < channelCount;
    }

    std::array<Channel, channelCount> channels;
};
} // namespace churchstream

