#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace churchstream
{
class AppSettings final
{
public:
    AppSettings();

    [[nodiscard]] std::unique_ptr<juce::XmlElement> loadAudioDeviceState() const;
    void saveAudioDeviceState(const juce::XmlElement* state);

    [[nodiscard]] juce::String getPreferredInput() const;
    [[nodiscard]] juce::String getPreferredOutput() const;
    void setPreferredDevices(const juce::String& input, const juce::String& output);

    [[nodiscard]] int getPerformanceProfile() const;
    void setPerformanceProfile(int profile);
    [[nodiscard]] bool getDevelopmentMode() const;
    void setDevelopmentMode(bool enabled);
    [[nodiscard]] juce::String getObsPassword() const;
    void setObsPassword(const juce::String& password);
    [[nodiscard]] double getNumber(const juce::String& key, double defaultValue) const;
    void setNumber(const juce::String& key, double value);
    [[nodiscard]] juce::String getString(const juce::String& key, const juce::String& defaultValue = {}) const;
    void setString(const juce::String& key, const juce::String& value);
    [[nodiscard]] static juce::File getDataDirectory();

    void flush();

private:
    mutable juce::ApplicationProperties properties;
};
} // namespace churchstream
