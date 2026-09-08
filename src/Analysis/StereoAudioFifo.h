#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace churchstream
{
template <size_t Capacity>
class StereoAudioFifo final
{
    static_assert((Capacity & (Capacity - 1)) == 0, "FIFO capacity must be a power of two");

public:
    int push(const float* const* channels, int numChannels, int numSamples) noexcept
    {
        if (channels == nullptr || numChannels <= 0 || numSamples <= 0)
            return 0;

        const auto write = writeCounter.load(std::memory_order_relaxed);
        const auto read = readCounter.load(std::memory_order_acquire);
        const auto available = Capacity - static_cast<size_t>(write - read);
        const auto count = std::min(static_cast<size_t>(numSamples), available);
        const auto* left = channels[0];
        const auto* right = numChannels > 1 && channels[1] != nullptr ? channels[1] : left;

        if (left == nullptr)
            return 0;

        for (size_t sample = 0; sample < count; ++sample)
        {
            const auto index = static_cast<size_t>(write + sample) & (Capacity - 1);
            data[0][index] = left[sample];
            data[1][index] = right[sample];
        }

        writeCounter.store(write + count, std::memory_order_release);
        if (count < static_cast<size_t>(numSamples))
            droppedSamples.fetch_add(static_cast<uint64_t>(numSamples) - count, std::memory_order_relaxed);
        return static_cast<int>(count);
    }

    int pop(float* left, float* right, int maximumSamples) noexcept
    {
        if (left == nullptr || right == nullptr || maximumSamples <= 0)
            return 0;

        const auto read = readCounter.load(std::memory_order_relaxed);
        const auto write = writeCounter.load(std::memory_order_acquire);
        const auto count = std::min(static_cast<size_t>(maximumSamples), static_cast<size_t>(write - read));

        for (size_t sample = 0; sample < count; ++sample)
        {
            const auto index = static_cast<size_t>(read + sample) & (Capacity - 1);
            left[sample] = data[0][index];
            right[sample] = data[1][index];
        }

        readCounter.store(read + count, std::memory_order_release);
        return static_cast<int>(count);
    }

    void reset() noexcept
    {
        const auto write = writeCounter.load(std::memory_order_acquire);
        readCounter.store(write, std::memory_order_release);
        droppedSamples.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t getDroppedSamples() const noexcept
    {
        return droppedSamples.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::array<std::array<float, Capacity>, 2> data {};
    alignas(64) std::atomic<uint64_t> writeCounter { 0 };
    alignas(64) std::atomic<uint64_t> readCounter { 0 };
    std::atomic<uint64_t> droppedSamples { 0 };
};
} // namespace churchstream

