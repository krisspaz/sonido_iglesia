#pragma once

#include "DSP/DspParameters.h"

#include <juce_data_structures/juce_data_structures.h>

namespace churchstream
{
class PresetManager final
{
public:
    PresetManager();

    [[nodiscard]] const juce::File& getDirectory() const noexcept { return directory; }
    [[nodiscard]] juce::Array<juce::File> findUserPresets() const;
    [[nodiscard]] juce::Result save(const juce::String& name, const DspParameters& parameters) const;
    [[nodiscard]] juce::Result load(const juce::File& file, DspParameters& parameters) const;

private:
    juce::File directory;
};
} // namespace churchstream
