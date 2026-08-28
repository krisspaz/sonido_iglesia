#include "DSP/ProcessingEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

int main(int argc, char** argv)
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;
    const auto simulatedSeconds = argc > 1 ? std::max(10, std::atoi(argv[1])) : 600;
    const auto totalBlocks = static_cast<long long>(std::ceil(simulatedSeconds * sampleRate / blockSize));

    churchstream::ProcessingEngine engine;
    engine.prepare(sampleRate, blockSize, 2);
    std::vector<float> left(blockSize), right(blockSize);
    float* channels[] { left.data(), right.data() };
    double phaseA = 0.0, phaseB = 0.0;
    const auto stepA = 2.0 * 3.14159265358979323846 * 83.0 / sampleRate;
    const auto stepB = 2.0 * 3.14159265358979323846 * 997.0 / sampleRate;
    auto maximum = 0.0f;
    double checksum = 0.0;

    const auto start = std::chrono::steady_clock::now();
    for (long long block = 0; block < totalBlocks; ++block)
    {
        if ((block % 2000) == 0)
        {
            auto& parameters = engine.getParameters();
            const auto position = static_cast<float>((block / 2000) % 11) / 10.0f;
            parameters.clean.store(position);
            parameters.punch.store(1.0f - position);
            parameters.clarity.store(position * 0.8f);
            parameters.dynamics.store(0.3f + position * 0.6f);
            parameters.warmth.store(position * 0.5f);
            parameters.bypass.store(((block / 2000) % 17) == 16);
        }

        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto transient = ((block * blockSize + sample) % 24000) < 12 ? 0.35f : 0.0f;
            left[static_cast<size_t>(sample)] = 0.32f * static_cast<float>(std::sin(phaseA))
                + 0.20f * static_cast<float>(std::sin(phaseB)) + transient;
            right[static_cast<size_t>(sample)] = 0.30f * static_cast<float>(std::sin(phaseA * 1.001))
                + 0.18f * static_cast<float>(std::sin(phaseB * 0.997)) + transient;
            phaseA += stepA;
            phaseB += stepB;
        }

        engine.process(channels, 2, blockSize);
        for (int sample = 0; sample < blockSize; sample += 31)
        {
            const auto l = left[static_cast<size_t>(sample)];
            const auto r = right[static_cast<size_t>(sample)];
            if (!std::isfinite(l) || !std::isfinite(r))
            {
                std::cerr << "FAIL non-finite output at block " << block << '\n';
                return 2;
            }
            maximum = std::max({ maximum, std::abs(l), std::abs(r) });
            checksum += static_cast<double>(l) + r;
        }
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto realtimeCpuEquivalent = elapsed / static_cast<double>(simulatedSeconds) * 100.0;
    const auto realtimeFactor = static_cast<double>(simulatedSeconds) / elapsed;

    std::cout << "simulated_seconds=" << simulatedSeconds << '\n'
              << "wall_seconds=" << elapsed << '\n'
              << "realtime_factor=" << realtimeFactor << "x\n"
              << "single_core_realtime_equivalent=" << realtimeCpuEquivalent << "%\n"
              << "sampled_maximum=" << maximum << '\n'
              << "checksum=" << checksum << '\n';

    if (maximum > 1.0001f || !std::isfinite(checksum) || realtimeFactor < 1.0)
    {
        std::cerr << "FAIL benchmark stability/performance requirement\n";
        return 1;
    }
    return 0;
}
