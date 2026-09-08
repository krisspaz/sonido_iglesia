#pragma once

#include "Biquad.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace churchstream
{
// One band of dynamic EQ.
//
// The band is extracted with a constant-peak-gain band-pass, which is unity at
// its centre, so
//
//     output = input + (gain - 1) * bandPass(input)
//
// is a peaking cut of `gain` at the centre with no coefficient recomputation.
// That is the whole reason for this shape: the gain moves per sample, and
// rebuilding a peaking biquad at audio rate is both expensive and ill defined,
// because the filter state is carried across the coefficient change.
//
// What the band is judged against is deliberately left to the caller. A
// sustained problem such as low-mid buildup has to be judged against the
// broadband programme, because a detector that adapts to the band's own average
// would treat the buildup as normal within a second and stop correcting it. A
// transient problem such as sibilance is the opposite: it has to be judged
// against the band's own recent average, otherwise the de-esser stops working
// the moment a bass-heavy song raises the broadband reference.
class DynamicBand final
{
public:
    void prepare(double sampleRateToUse, float centreHz, float q,
                 double attackSeconds, double releaseSeconds) noexcept
    {
        sampleRate = sampleRateToUse;
        filter.setBandPass(sampleRateToUse, centreHz, q);
        attackCoefficient = coefficientFor(attackSeconds);
        releaseCoefficient = coefficientFor(releaseSeconds);
        // Slow enough that a sibilant never moves it, which is what makes it a
        // usable reference for one.
        averageCoefficient = coefficientFor(1.0);
        // Two milliseconds only removes the staircase left by the envelope; the
        // timing itself belongs to the attack and release above.
        gainCoefficient = coefficientFor(0.002);
        reset();
    }

    void reset() noexcept
    {
        filter.reset();
        envelope = 0.0f;
        averageEnvelope = 0.0f;
        gain = 1.0f;
        reductionDb = 0.0f;
    }

    void setResponse(float thresholdOffsetDbToUse, float ratioToUse, float maximumReductionDbToUse) noexcept
    {
        thresholdOffsetDb = thresholdOffsetDbToUse;
        ratio = std::max(1.0f, ratioToUse);
        maximumReductionDb = std::max(0.0f, maximumReductionDbToUse);
    }

    // Must be called once per channel per sample, before updateGain.
    [[nodiscard]] float extract(int channel, float input) noexcept
    {
        return filter.process(channel, input);
    }

    // `detector` is the peak of the extracted band across the active channels.
    // `referenceDb` is whatever this band is being judged against.
    void updateGain(float detector, float referenceDb, bool enabled) noexcept
    {
        const auto coefficient = detector > envelope ? attackCoefficient : releaseCoefficient;
        envelope = coefficient * envelope + (1.0f - coefficient) * detector;
        averageEnvelope = averageCoefficient * averageEnvelope + (1.0f - averageCoefficient) * detector;

        auto targetReduction = 0.0f;
        if (enabled && maximumReductionDb > 0.0f)
        {
            const auto bandDb = levelDb(envelope);
            const auto excess = bandDb - (referenceDb + thresholdOffsetDb);
            if (excess > 0.0f)
                targetReduction = std::min(maximumReductionDb, excess * (1.0f - 1.0f / ratio));
        }

        reductionDb = gainCoefficient * reductionDb + (1.0f - gainCoefficient) * targetReduction;
        // Most samples of most services need no reduction at all, and this runs
        // three times per sample. Skipping the exponential when there is
        // nothing to convert is worth more than any cleverness inside it.
        if (targetReduction <= 0.0f && reductionDb < 1.0e-4f)
        {
            reductionDb = 0.0f;
            gain = 1.0f;
            return;
        }
        // exp with a folded constant rather than pow with a literal base: the
        // compiler cannot assume anything about pow's first argument.
        gain = std::exp(-reductionDb * 0.11512925f);
        if (!std::isfinite(gain))
        {
            gain = 1.0f;
            reductionDb = 0.0f;
        }
    }

    [[nodiscard]] float getGain() const noexcept { return gain; }
    [[nodiscard]] float getReductionDb() const noexcept { return reductionDb; }
    // The band's own running average, in dB. This is the reference a transient
    // detector such as the de-esser judges itself against.
    [[nodiscard]] float getAverageLevelDb() const noexcept { return levelDb(averageEnvelope); }

    [[nodiscard]] static float levelDb(float linear) noexcept
    {
        return linear > 1.0e-9f ? 20.0f * std::log10(linear) : -180.0f;
    }

private:
    [[nodiscard]] float coefficientFor(double seconds) const noexcept
    {
        return static_cast<float>(std::exp(-1.0 / std::max(1.0, sampleRate * seconds)));
    }

    Biquad filter;
    double sampleRate = 48000.0;
    float attackCoefficient = 0.0f;
    float releaseCoefficient = 0.0f;
    float averageCoefficient = 0.0f;
    float gainCoefficient = 0.0f;
    float envelope = 0.0f;
    float averageEnvelope = 0.0f;
    float gain = 1.0f;
    float reductionDb = 0.0f;
    float thresholdOffsetDb = 0.0f;
    float ratio = 2.0f;
    float maximumReductionDb = 0.0f;
};
} // namespace churchstream
