// Renders a WAV through the real ProcessingEngine with the application's
// default settings, without an audio device or the GUI. It exists so a change
// to the dynamics can be judged on a recorded service instead of only on the
// synthetic signals the unit tests use:
//
//   ChurchStreamProcessorRender culto.wav culto-procesado.wav [--no-leveller]
//
// A per-second trace of the leveller and dynamics is written next to the
// output as <output>.csv.
#include "DSP/ProcessingEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: " << argv[0] << " input.wav output.wav [--no-leveller]\n";
        return 2;
    }

    const juce::File source { juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]) };
    const juce::File target { juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]) };
    const auto leveller = !(argc > 3 && juce::String(argv[3]) == "--no-leveller");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
    if (reader == nullptr)
    {
        std::cerr << "cannot read " << source.getFullPathName() << '\n';
        return 1;
    }

    constexpr int blockSize = 256;
    const auto sampleRate = reader->sampleRate;
    const auto channels = static_cast<int>(std::min<unsigned int>(2, reader->numChannels));

    auto engine = std::make_unique<churchstream::ProcessingEngine>();
    engine->prepare(sampleRate, blockSize, 2);
    // What the application stores on first run. The Smart Engine is not
    // running here, so its adaptive targets stay neutral.
    engine->getParameters().broadcastLevelerEnabled.store(leveller);

    target.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(target.createOutputStream());
    if (stream == nullptr)
    {
        std::cerr << "cannot write " << target.getFullPathName() << '\n';
        return 1;
    }
    auto options = juce::AudioFormatWriterOptions().withSampleRate(sampleRate)
        .withChannelLayout(juce::AudioChannelSet::stereo()).withBitsPerSample(24);
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr)
        return 1;

    juce::FileOutputStream trace(target.withFileExtension("csv"));
    trace.setPosition(0);
    trace.truncate();
    trace << "second,leveller_gain_db,programme_level_db,gate_open,compressor_db,limiter_db,"
             "tone_low_db,tone_body_db,tone_presence_db,tone_brilliance_db,tone_air_db\n";
    const auto blocksPerSecond = static_cast<int64_t>(std::llround(sampleRate / blockSize));

    juce::AudioBuffer<float> buffer(2, blockSize);
    const auto total = reader->lengthInSamples;
    auto maximumLimiter = 0.0f;
    double limiterSum = 0.0;
    double compressorSum = 0.0;
    int64_t blocks = 0;
    for (int64_t position = 0; position < total; position += blockSize)
    {
        const auto count = static_cast<int>(std::min<int64_t>(blockSize, total - position));
        buffer.clear();
        reader->read(&buffer, 0, count, position, true, channels > 1);
        if (channels == 1)
            buffer.copyFrom(1, 0, buffer, 0, 0, count);
        engine->process(buffer.getArrayOfWritePointers(), 2, count);
        writer->writeFromAudioSampleBuffer(buffer, 0, count);

        const auto& metrics = engine->getMetrics();
        maximumLimiter = std::max(maximumLimiter, metrics.limiterGainReductionDb.load());
        limiterSum += metrics.limiterGainReductionDb.load();
        compressorSum += metrics.compressorGainReductionDb.load();
        if (blocks % blocksPerSecond == 0)
            trace << juce::String(blocks / blocksPerSecond) << ","
                  << juce::String(metrics.broadcastLevelGainDb.load(), 2) << ","
                  << juce::String(metrics.programmeLevelDb.load(), 2) << ","
                  << (metrics.levelerGateOpen.load() ? "1" : "0") << ","
                  << juce::String(metrics.compressorGainReductionDb.load(), 2) << ","
                  << juce::String(metrics.limiterGainReductionDb.load(), 2) << ","
                  << juce::String(metrics.toneMatchGainDb[0].load(), 2) << ","
                  << juce::String(metrics.toneMatchGainDb[1].load(), 2) << ","
                  << juce::String(metrics.toneMatchGainDb[2].load(), 2) << ","
                  << juce::String(metrics.toneMatchGainDb[3].load(), 2) << ","
                  << juce::String(metrics.toneMatchGainDb[4].load(), 2) << "\n";
        ++blocks;
        if ((position / blockSize) % 100000 == 0)
            std::cerr << '\r' << static_cast<int>(100.0 * static_cast<double>(position) / static_cast<double>(total)) << "%";
    }
    std::cerr << "\r100%\n";
    std::cout << "average compressor reduction " << compressorSum / static_cast<double>(std::max<int64_t>(1, blocks))
              << " dB, average limiter reduction " << limiterSum / static_cast<double>(std::max<int64_t>(1, blocks))
              << " dB, maximum limiter reduction " << maximumLimiter << " dB\n";
    return 0;
}
