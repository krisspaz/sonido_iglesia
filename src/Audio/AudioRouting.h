#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace churchstream
{
struct AudioRouting final
{
    static void passthrough(const float* const* inputs,
                            int numInputChannels,
                            float* const* outputs,
                            int numOutputChannels,
                            int numSamples) noexcept
    {
        if (numSamples <= 0)
            return;

        for (int outputChannel = 0; outputChannel < numOutputChannels; ++outputChannel)
        {
            auto* destination = outputs != nullptr ? outputs[outputChannel] : nullptr;
            if (destination == nullptr)
                continue;

            if (inputs == nullptr || numInputChannels == 0)
            {
                juce::FloatVectorOperations::clear(destination, numSamples);
                continue;
            }

            // A mono source is intentionally duplicated to both sides. With two or
            // more inputs the corresponding channel is copied without gain changes.
            const auto sourceChannel = juce::jmin(outputChannel, numInputChannels - 1);
            const auto* source = inputs[sourceChannel];

            if (source != nullptr)
                juce::FloatVectorOperations::copy(destination, source, numSamples);
            else
                juce::FloatVectorOperations::clear(destination, numSamples);
        }
    }
};
} // namespace churchstream

