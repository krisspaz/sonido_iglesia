#include "AppSettings.h"

#include <cstring>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <dpapi.h>
#endif

namespace churchstream
{
namespace
{
constexpr auto audioStateKey = "audioDeviceState";
constexpr auto preferredInputKey = "preferredInput";
constexpr auto preferredOutputKey = "preferredOutput";
constexpr auto performanceProfileKey = "performanceProfile";
constexpr auto developmentModeKey = "developmentMode";
constexpr auto obsPasswordKey = "obsWebSocketPassword";
}

AppSettings::AppSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "ChurchStreamProcessor";
    options.filenameSuffix = ".settings";
    options.folderName = "ChurchStreamProcessor";
    options.osxLibrarySubFolder = "Application Support";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    properties.setStorageParameters(options);
}

std::unique_ptr<juce::XmlElement> AppSettings::loadAudioDeviceState() const
{
    if (auto* settings = properties.getUserSettings())
        return juce::parseXML(settings->getValue(audioStateKey));

    return {};
}

void AppSettings::saveAudioDeviceState(const juce::XmlElement* state)
{
    if (state == nullptr)
        return;

    if (auto* settings = properties.getUserSettings())
        settings->setValue(audioStateKey, state->toString());
}

juce::String AppSettings::getPreferredInput() const
{
    if (auto* settings = properties.getUserSettings())
        return settings->getValue(preferredInputKey);

    return {};
}

juce::String AppSettings::getPreferredOutput() const
{
    if (auto* settings = properties.getUserSettings())
        return settings->getValue(preferredOutputKey);

    return {};
}

void AppSettings::setPreferredDevices(const juce::String& input, const juce::String& output)
{
    if (auto* settings = properties.getUserSettings())
    {
        settings->setValue(preferredInputKey, input);
        settings->setValue(preferredOutputKey, output);
    }
}

int AppSettings::getPerformanceProfile() const
{
    if (auto* settings = properties.getUserSettings())
        return juce::jlimit(0, 2, settings->getIntValue(performanceProfileKey, 0));

    return 0;
}

void AppSettings::setPerformanceProfile(int profile)
{
    if (auto* settings = properties.getUserSettings())
        settings->setValue(performanceProfileKey, juce::jlimit(0, 2, profile));
}

bool AppSettings::getDevelopmentMode() const
{
    if (auto* settings = properties.getUserSettings())
        return settings->getBoolValue(developmentModeKey, false);

    return false;
}

void AppSettings::setDevelopmentMode(bool enabled)
{
    if (auto* settings = properties.getUserSettings())
        settings->setValue(developmentModeKey, enabled);
}

juce::String AppSettings::getObsPassword() const
{
    if (auto* settings = properties.getUserSettings())
    {
        const auto stored = settings->getValue(obsPasswordKey);
#if JUCE_WINDOWS
        if (stored.startsWith("dpapi:"))
        {
            juce::MemoryBlock encrypted;
            if (!encrypted.fromBase64Encoding(stored.fromFirstOccurrenceOf("dpapi:", false, false))) return {};
            DATA_BLOB input { static_cast<DWORD>(encrypted.getSize()), static_cast<BYTE*>(encrypted.getData()) };
            DATA_BLOB output {};
            if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                   CRYPTPROTECT_UI_FORBIDDEN, &output))
            {
                const auto result = juce::String::fromUTF8(reinterpret_cast<const char*>(output.pbData),
                                                          static_cast<int>(output.cbData));
                LocalFree(output.pbData);
                return result;
            }
            return {};
        }
#endif
        return stored.startsWith("local:") ? stored.fromFirstOccurrenceOf("local:", false, false) : stored;
    }
    return {};
}

void AppSettings::setObsPassword(const juce::String& password)
{
    if (auto* settings = properties.getUserSettings())
    {
#if JUCE_WINDOWS
        const auto utf8 = password.toRawUTF8();
        DATA_BLOB input { static_cast<DWORD>(std::strlen(utf8)),
                          reinterpret_cast<BYTE*>(const_cast<char*>(utf8)) };
        DATA_BLOB output {};
        if (CryptProtectData(&input, L"Church Stream Processor OBS password", nullptr, nullptr, nullptr,
                             CRYPTPROTECT_UI_FORBIDDEN, &output))
        {
            juce::MemoryBlock encrypted(output.pbData, output.cbData);
            LocalFree(output.pbData);
            settings->setValue(obsPasswordKey, "dpapi:" + encrypted.toBase64Encoding());
            return;
        }
#endif
        settings->setValue(obsPasswordKey, "local:" + password);
    }
}

double AppSettings::getNumber(const juce::String& key, double defaultValue) const
{
    if (auto* settings = properties.getUserSettings())
        return settings->getDoubleValue(key, defaultValue);
    return defaultValue;
}

void AppSettings::setNumber(const juce::String& key, double value)
{
    if (auto* settings = properties.getUserSettings())
        settings->setValue(key, value);
}

juce::String AppSettings::getString(const juce::String& key, const juce::String& defaultValue) const
{
    if (auto* settings = properties.getUserSettings())
        return settings->getValue(key, defaultValue);
    return defaultValue;
}

void AppSettings::setString(const juce::String& key, const juce::String& value)
{
    if (auto* settings = properties.getUserSettings())
        settings->setValue(key, value);
}

juce::File AppSettings::getDataDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("ChurchStreamProcessor");
}

void AppSettings::flush()
{
    properties.saveIfNeeded();
}
} // namespace churchstream
