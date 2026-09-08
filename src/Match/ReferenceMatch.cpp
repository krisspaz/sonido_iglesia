#include "Match/ReferenceMatch.h"

#include <algorithm>
#include <cmath>

#include <juce_audio_formats/juce_audio_formats.h>

namespace churchstream
{
namespace
{
float dbToPower(float decibels) noexcept { return std::pow(10.0f, decibels * 0.1f); }

float powerToDb(float power) noexcept
{
    return power > 1.0e-12f ? 10.0f * std::log10(power) : -100.0f;
}

// Maps the analysed power spectrum into the 21 ANSI critical bands. The edges
// sit at the Bark midpoints between neighbouring band centres, so no bin is
// counted twice and every audible band is covered. Both the offline profile
// and the live comparison use this exact mapping, which is what makes a
// measured difference trustworthy.
std::array<float, psychoacoustics::criticalBandCount> criticalBandLevels(
    const SignalMetrics& metrics, double sampleRate)
{
    static const auto edges = [] {
        std::array<float, psychoacoustics::criticalBandCount + 1> value {};
        value[0] = 0.0f;
        for (int band = 1; band < psychoacoustics::criticalBandCount; ++band)
        {
            const auto previous = psychoacoustics::barkFromHertz(
                psychoacoustics::criticalBandCentresHz[static_cast<size_t>(band - 1)]);
            const auto centre = psychoacoustics::barkFromHertz(
                psychoacoustics::criticalBandCentresHz[static_cast<size_t>(band)]);
            value[static_cast<size_t>(band)] = psychoacoustics::hertzFromBark(0.5f * (previous + centre));
        }
        value[psychoacoustics::criticalBandCount] = 20000.0f;
        return value;
    }();

    std::array<float, psychoacoustics::criticalBandCount> levels {};
    std::array<double, psychoacoustics::criticalBandCount> power {};
    levels.fill(-100.0f);
    power.fill(0.0);

    for (int bin = 0; bin < spectrumBins; ++bin)
    {
        const auto decibels = metrics.spectrumDb[static_cast<size_t>(bin)];
        if (decibels <= -95.0f)
            continue;
        const auto frequency = static_cast<float>(bin * sampleRate / static_cast<double>(fftSize));
        int band = 0;
        for (int candidate = 0; candidate < psychoacoustics::criticalBandCount; ++candidate)
        {
            if (frequency >= edges[static_cast<size_t>(candidate)]
                && frequency < edges[static_cast<size_t>(candidate + 1)])
            {
                band = candidate;
                break;
            }
        }
        power[static_cast<size_t>(band)] += dbToPower(decibels);
    }

    for (int band = 0; band < psychoacoustics::criticalBandCount; ++band)
    {
        if (power[static_cast<size_t>(band)] > 1.0e-12)
            levels[static_cast<size_t>(band)] = powerToDb(static_cast<float>(power[static_cast<size_t>(band)]));
    }
    return levels;
}

juce::String formattedDb(float value)
{
    return juce::String::formatted("%.1f", std::abs(value));
}

juce::String describeConsistent(const ReferenceComparison& comparison)
{
    for (int zone = 0; zone < referenceZoneCount; ++zone)
        if (std::abs(comparison.zoneDifferenceDb[static_cast<size_t>(zone)]) >= 1.0f)
            return {};
    if (std::abs(comparison.loudnessDifferenceDb) >= 2.0f)
        return {};
    return "Tu stream suena muy parecido a tu referencia. No toques nada.";
}
} // namespace

ReferenceProfile ReferenceProfile::empty()
{
    ReferenceProfile profile;
    profile.bandLevelDb.fill(-100.0f);
    return profile;
}

juce::File ReferenceProfile::defaultDirectory()
{
    auto directory = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("ChurchStreamProcessor")
                         .getChildFile("references");
    directory.createDirectory();
    return directory;
}

ReferenceProfile ReferenceProfile::capture(const juce::File& audioFile, juce::String& error)
{
    ReferenceProfile profile = empty();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(audioFile));
    if (reader == nullptr)
    {
        error = "No se pudo leer el archivo (formato no compatible).";
        return profile;
    }

    const auto sampleRate = reader->sampleRate;
    if (sampleRate < 8000.0)
    {
        error = "El archivo no tiene una frecuencia de muestreo válida.";
        return profile;
    }

    constexpr int chunkSize = 8192;
    const auto totalFrames = reader->lengthInSamples;
    juce::AudioBuffer<float> buffer(2, chunkSize);
    AnalysisEngine analysis;
    analysis.prepareOffline(sampleRate);

    double peak = 0.0;
    double squareSum = 0.0;
    double leftSquare = 0.0;
    double rightSquare = 0.0;
    double crossSum = 0.0;
    juce::int64 position = 0;

    while (position < totalFrames)
    {
        const auto count = static_cast<int>(juce::jmin<juce::int64>(totalFrames - position, chunkSize));
        if (!reader->read(&buffer, 0, count, position, true, true))
        {
            error = "El archivo no se pudo decodificar por completo.";
            return profile;
        }
        const auto* left = buffer.getReadPointer(0);
        const auto* right = buffer.getReadPointer(1);
        for (int sample = 0; sample < count; ++sample)
        {
            const auto leftSample = static_cast<double>(left[sample]);
            const auto rightSample = static_cast<double>(right[sample]);
            peak = std::max({ peak, std::abs(leftSample), std::abs(rightSample) });
            squareSum += 0.5 * (leftSample * leftSample + rightSample * rightSample);
            leftSquare += leftSample * leftSample;
            rightSquare += rightSample * rightSample;
            crossSum += leftSample * rightSample;
        }
        analysis.pushOffline(left, right, left, right, count);
        position += count;
    }

    const auto snapshot = analysis.finishOfflineUpdate(0.0);
    profile.bandLevelDb = criticalBandLevels(snapshot.processed, sampleRate);
    profile.lufsIntegrated = snapshot.processed.lufsIntegrated;
    profile.loudnessRange = snapshot.processed.loudnessRange;
    profile.durationSeconds = position > 0
        ? static_cast<float>(static_cast<double>(position) / sampleRate) : 0.0f;
    profile.sourceName = audioFile.getFileNameWithoutExtension();

    const auto rms = position > 0 ? std::sqrt(squareSum / static_cast<double>(position)) : 0.0;
    const auto peakDb = peak > 1.0e-5 ? static_cast<float>(20.0 * std::log10(peak)) : -100.0f;
    const auto rmsDb = rms > 1.0e-5 ? static_cast<float>(20.0 * std::log10(rms)) : -100.0f;
    profile.crestFactorDb = std::clamp(peakDb - rmsDb, 0.0f, 40.0f);

    const auto midEnergy = std::max(1.0e-12, 0.5 * (leftSquare + rightSquare + 2.0 * crossSum));
    const auto sideEnergy = std::max(0.0, 0.5 * (leftSquare + rightSquare - 2.0 * crossSum));
    profile.stereoWidth = std::clamp(static_cast<float>(std::sqrt(sideEnergy / midEnergy)), 0.0f, 2.0f);

    profile.valid = profile.lufsIntegrated > -60.0f;
    if (!profile.valid)
        error = "El archivo no tiene audio suficiente para usarse como referencia.";
    return profile;
}

bool ReferenceProfile::save(const juce::File& destination) const
{
    auto object = juce::DynamicObject::Ptr(new juce::DynamicObject());
    object->setProperty("format", "ChurchStreamProcessorReference");
    object->setProperty("version", 1);
    object->setProperty("sourceName", sourceName);
    object->setProperty("lufsIntegrated", static_cast<double>(lufsIntegrated));
    object->setProperty("loudnessRange", static_cast<double>(loudnessRange));
    object->setProperty("crestFactorDb", static_cast<double>(crestFactorDb));
    object->setProperty("stereoWidth", static_cast<double>(stereoWidth));
    object->setProperty("durationSeconds", static_cast<double>(durationSeconds));

    juce::Array<juce::var> bands;
    for (const auto level : bandLevelDb)
        bands.add(static_cast<double>(level));
    object->setProperty("bands", juce::var(bands));

    return destination.replaceWithText(juce::JSON::toString(juce::var(object.get()), true));
}

ReferenceProfile ReferenceProfile::load(const juce::File& source)
{
    ReferenceProfile profile = empty();
    if (!source.existsAsFile())
        return profile;

    const auto value = juce::JSON::parse(source.loadFileAsString());
    const auto* object = value.getDynamicObject();
    if (object == nullptr || object->getProperty("format").toString() != "ChurchStreamProcessorReference")
        return profile;

    const auto readFloat = [object](const char* key, float fallback, float low, float high) {
        const auto field = object->getProperty(key);
        return field.isDouble() || field.isInt() || field.isInt64()
            ? juce::jlimit(low, high, static_cast<float>(field)) : fallback;
    };

    profile.sourceName = object->getProperty("sourceName").toString();
    profile.lufsIntegrated = readFloat("lufsIntegrated", -100.0f, -100.0f, 20.0f);
    profile.loudnessRange = readFloat("loudnessRange", 0.0f, 0.0f, 60.0f);
    profile.crestFactorDb = readFloat("crestFactorDb", 0.0f, 0.0f, 40.0f);
    profile.stereoWidth = readFloat("stereoWidth", 0.0f, 0.0f, 2.0f);
    profile.durationSeconds = std::max(0.0f, readFloat("durationSeconds", 0.0f, 0.0f, 86400.0f));

    const auto bands = object->getProperty("bands");
    if (bands.isArray())
    {
        const auto& array = *bands.getArray();
        const auto count = std::min(static_cast<int>(array.size()), psychoacoustics::criticalBandCount);
        for (int band = 0; band < count; ++band)
        {
            const auto& element = array[static_cast<int>(band)];
            profile.bandLevelDb[static_cast<size_t>(band)] =
                element.isDouble() || element.isInt() || element.isInt64()
                ? juce::jlimit(-140.0f, 20.0f, static_cast<float>(element))
                : -100.0f;
        }
    }

    profile.valid = profile.lufsIntegrated > -60.0f;
    return profile;
}

ReferenceComparison ReferenceMatchEngine::compare(const ReferenceProfile& profile,
                                                  const SignalMetrics& live,
                                                  double sampleRate, float liveSeconds) noexcept
{
    ReferenceComparison result;
    result.liveSeconds = liveSeconds;
    result.bandDifferenceDb.fill(0.0f);
    result.zoneDifferenceDb.fill(0.0f);
    if (!profile.valid)
        return result;

    result.active = true;
    result.liveLufsShortTerm = live.lufsShortTerm;
    result.meaningful = live.rmsDb > -60.0f && live.lufsShortTerm > -50.0f;
    if (!result.meaningful)
    {
        result.title = "Esperando audio en vivo para comparar...";
        return result;
    }

    const auto referenceLoudness = profile.lufsIntegrated > -90.0f ? profile.lufsIntegrated : 0.0f;
    const auto liveLoudness = live.lufsShortTerm > -90.0f ? live.lufsShortTerm : referenceLoudness;
    const auto liveBands = criticalBandLevels(live, sampleRate);

    std::array<float, referenceZoneCount> zoneTotal {};
    std::array<int, referenceZoneCount> zoneCount {};
    zoneTotal.fill(0.0f);
    zoneCount.fill(0);

    for (int band = 0; band < psychoacoustics::criticalBandCount; ++band)
    {
        const auto difference = (liveBands[static_cast<size_t>(band)] - liveLoudness)
                              - (profile.bandLevelDb[static_cast<size_t>(band)] - referenceLoudness);
        result.bandDifferenceDb[static_cast<size_t>(band)] = difference;
        const auto zone = referenceZoneForBand(band);
        zoneTotal[static_cast<size_t>(zone)] += difference;
        ++zoneCount[static_cast<size_t>(zone)];
    }
    for (int zone = 0; zone < referenceZoneCount; ++zone)
    {
        if (zoneCount[static_cast<size_t>(zone)] > 0)
            result.zoneDifferenceDb[static_cast<size_t>(zone)] =
                zoneTotal[static_cast<size_t>(zone)] / static_cast<float>(zoneCount[static_cast<size_t>(zone)]);
    }

    result.loudnessDifferenceDb = live.lufsShortTerm > -90.0f ? live.lufsShortTerm - profile.lufsIntegrated : 0.0f;
    result.crestDifferenceDb = live.crestFactorDb - profile.crestFactorDb;
    result.widthDifference = live.stereoWidth - profile.stereoWidth;

    result.title = "Alineación con tu referencia de sonido";
    juce::StringArray& lines = result.lines;
    if (profile.sourceName.isNotEmpty())
        lines.add("Referencia: " + profile.sourceName);

    for (int zone = 0; zone < referenceZoneCount; ++zone)
    {
        const auto& info = referenceZones[static_cast<size_t>(zone)];
        const auto difference = result.zoneDifferenceDb[static_cast<size_t>(zone)];
        if (std::abs(difference) < 1.0f)
            continue;
        lines.add("Tu stream tiene " + formattedDb(difference)
                  + " dB " + (difference > 0.0f ? "más" : "menos")
                  + " de " + juce::String(info.name) + " (" + juce::String(info.range)
                  + ") que tu referencia.");
    }

    const auto loudnessDifference = result.loudnessDifferenceDb;
    if (std::abs(loudnessDifference) < 1.0f)
        lines.add("Tu volumen está alineado con tu referencia.");
    else
        lines.add("Tu volumen está " + formattedDb(loudnessDifference)
                  + " LUFS " + (loudnessDifference > 0.0f ? "por encima" : "por debajo")
                  + " de tu referencia.");

    if (std::abs(result.crestDifferenceDb) >= 3.0f)
        lines.add("Dinámica: tu crest difiere " + formattedDb(result.crestDifferenceDb)
                  + " dB de tu referencia.");
    if (std::abs(result.widthDifference) > 0.15f)
        lines.add("Estéreo: tu stream suena " + juce::String(result.widthDifference > 0.0f ? "más ancho" : "más angosto")
                  + " que tu referencia.");

    const auto consistent = describeConsistent(result);
    if (consistent.isNotEmpty())
        lines.add(consistent);

    return result;
}

ReferenceMatchEngine::ReferenceMatchEngine(AnalysisEngine& analysisToUse)
    : Thread("CSP Reference Match Thread"), analysis(analysisToUse)
{
}

ReferenceMatchEngine::~ReferenceMatchEngine()
{
    stop();
}

void ReferenceMatchEngine::start()
{
    if (!isThreadRunning())
        startThread(juce::Thread::Priority::low);
}

void ReferenceMatchEngine::stop()
{
    signalThreadShouldExit();
    stopThread(1500);
}

void ReferenceMatchEngine::captureFromFile(const juce::File& audioFile)
{
    if (audioFile == juce::File())
        return;
    const juce::ScopedLock lock(captureLock);
    pendingCaptureFile = audioFile.getFullPathName();
}

juce::Result ReferenceMatchEngine::loadProfile(const juce::File& profileFile)
{
    const auto loaded = ReferenceProfile::load(profileFile);
    if (!loaded.valid)
        return juce::Result::fail("No se pudo leer el perfil de referencia.");
    setProfile(loaded);
    return juce::Result::ok();
}

void ReferenceMatchEngine::clearProfile()
{
    const juce::ScopedLock lock(profileLock);
    profile = ReferenceProfile::empty();
    const juce::ScopedLock comparisonGuardLock(comparisonLock);
    comparison = {};
}

void ReferenceMatchEngine::setProfile(const ReferenceProfile& profileToUse)
{
    const juce::ScopedLock lock(profileLock);
    profile = profileToUse;
}

ReferenceProfile ReferenceMatchEngine::getProfile() const
{
    const juce::ScopedLock lock(profileLock);
    return profile;
}

ReferenceComparison ReferenceMatchEngine::getComparison() const
{
    const juce::ScopedLock lock(comparisonLock);
    return comparison;
}

ReferenceMatchEngine::CaptureState ReferenceMatchEngine::getCaptureState() const
{
    const juce::ScopedLock lock(captureLock);
    return captureState;
}

void ReferenceMatchEngine::processSnapshotForTesting(const AnalysisSnapshot& snapshot)
{
    update(snapshot);
}

void ReferenceMatchEngine::run()
{
    while (!threadShouldExit())
    {
        wait(125);
        if (threadShouldExit())
            break;

        juce::String pending;
        {
            const juce::ScopedLock lock(captureLock);
            pending = pendingCaptureFile;
            pendingCaptureFile = {};
        }
        if (pending.isNotEmpty())
        {
            juce::String error;
            const auto captured = ReferenceProfile::capture(juce::File(pending), error);
            if (captured.valid)
            {
                setProfile(captured);
                (void)captured.save(ReferenceProfile::defaultDirectory()
                                       .getChildFile("ref-" + juce::File::createLegalFileName(captured.sourceName) + ".cspref"));
            }
            {
                const juce::ScopedLock lock(captureLock);
                captureState.running = false;
                captureState.complete = true;
                captureState.ok = captured.valid;
                captureState.file = juce::File(pending).getFileName();
                captureState.error = captured.valid ? juce::String() : error;
            }
        }

        update(analysis.getSnapshot());
    }
}

void ReferenceMatchEngine::update(const AnalysisSnapshot& snapshot)
{
    ReferenceProfile current;
    {
        const juce::ScopedLock lock(profileLock);
        current = profile;
    }

    if (snapshot.analyzedFrames > previousFrames)
    {
        const auto rate = snapshot.sampleRate > 8000.0 ? snapshot.sampleRate : 48000.0;
        liveSeconds += static_cast<float>(
            static_cast<double>(snapshot.analyzedFrames - previousFrames) / rate);
        previousFrames = snapshot.analyzedFrames;
    }

    ReferenceComparison next;
    if (current.valid)
        next = compare(current, snapshot.processed, snapshot.sampleRate, liveSeconds);

    const juce::ScopedLock lock(comparisonLock);
    comparison = next;
}
} // namespace churchstream