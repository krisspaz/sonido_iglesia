#include "PresetManager.h"

namespace churchstream
{
namespace
{
void setNumber(juce::DynamicObject& object, const char* key, float value)
{
    object.setProperty(key, static_cast<double>(value));
}

float readFloat(const juce::DynamicObject& object, const char* key, float fallback, float low, float high)
{
    const auto value = object.getProperty(key);
    return value.isDouble() || value.isInt() || value.isInt64()
        ? juce::jlimit(low, high, static_cast<float>(value)) : fallback;
}

bool readBool(const juce::DynamicObject& object, const char* key, bool fallback)
{
    const auto value = object.getProperty(key);
    return value.isBool() ? static_cast<bool>(value) : fallback;
}
}

PresetManager::PresetManager()
    : directory(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                    .getChildFile("ChurchStreamProcessor").getChildFile("presets"))
{
    directory.createDirectory();
}

juce::Array<juce::File> PresetManager::findUserPresets() const
{
    juce::Array<juce::File> files;
    directory.findChildFiles(files, juce::File::findFiles, false, "*.cspreset");
    files.sort();
    return files;
}

juce::Result PresetManager::save(const juce::String& name, const DspParameters& p) const
{
    auto cleanName = juce::File::createLegalFileName(name.trim());
    if (cleanName.isEmpty()) return juce::Result::fail("Preset name is empty");

    auto object = juce::DynamicObject::Ptr(new juce::DynamicObject());
    object->setProperty("format", "ChurchStreamProcessorPreset");
    object->setProperty("version", 1);
    object->setProperty("smartProcessing", p.smartProcessing.load());
    object->setProperty("operatingMode", juce::jlimit(0, 2, p.operatingMode.load()));
    setNumber(*object, "clean", p.clean.load());
    setNumber(*object, "punch", p.punch.load());
    setNumber(*object, "clarity", p.clarity.load());
    setNumber(*object, "dynamics", p.dynamics.load());
    setNumber(*object, "warmth", p.warmth.load());
    setNumber(*object, "loudnessTarget", p.loudnessTarget.load());
    object->setProperty("rumbleEnabled", p.rumbleEnabled.load());
    object->setProperty("adaptiveEqEnabled", p.adaptiveEqEnabled.load());
    object->setProperty("compressorEnabled", p.compressorEnabled.load());
    object->setProperty("saturationEnabled", p.saturationEnabled.load());
    object->setProperty("limiterEnabled", p.limiterEnabled.load());

    const auto destination = directory.getChildFile(cleanName + ".cspreset");
    return destination.replaceWithText(juce::JSON::toString(juce::var(object.get()), true))
        ? juce::Result::ok() : juce::Result::fail("Could not write preset file");
}

juce::Result PresetManager::load(const juce::File& file, DspParameters& p) const
{
    if (!file.isAChildOf(directory) || !file.existsAsFile())
        return juce::Result::fail("Preset file is outside the local preset directory");
    const auto value = juce::JSON::parse(file.loadFileAsString());
    const auto* object = value.getDynamicObject();
    if (object == nullptr || object->getProperty("format").toString() != "ChurchStreamProcessorPreset")
        return juce::Result::fail("Invalid Church Stream Processor preset");

    p.smartProcessing.store(readBool(*object, "smartProcessing", p.smartProcessing.load()));
    p.operatingMode.store(static_cast<int>(readFloat(*object, "operatingMode",
                                                     static_cast<float>(p.operatingMode.load()), 0.0f, 2.0f)));
    p.clean.store(readFloat(*object, "clean", p.clean.load(), 0.0f, 1.0f));
    p.punch.store(readFloat(*object, "punch", p.punch.load(), 0.0f, 1.0f));
    p.clarity.store(readFloat(*object, "clarity", p.clarity.load(), 0.0f, 1.0f));
    p.dynamics.store(readFloat(*object, "dynamics", p.dynamics.load(), 0.0f, 1.0f));
    p.warmth.store(readFloat(*object, "warmth", p.warmth.load(), 0.0f, 1.0f));
    p.loudnessTarget.store(readFloat(*object, "loudnessTarget", p.loudnessTarget.load(), -18.0f, -10.0f));
    p.rumbleEnabled.store(readBool(*object, "rumbleEnabled", p.rumbleEnabled.load()));
    p.adaptiveEqEnabled.store(readBool(*object, "adaptiveEqEnabled", p.adaptiveEqEnabled.load()));
    p.compressorEnabled.store(readBool(*object, "compressorEnabled", p.compressorEnabled.load()));
    p.saturationEnabled.store(readBool(*object, "saturationEnabled", p.saturationEnabled.load()));
    p.limiterEnabled.store(readBool(*object, "limiterEnabled", p.limiterEnabled.load()));
    return juce::Result::ok();
}
} // namespace churchstream
