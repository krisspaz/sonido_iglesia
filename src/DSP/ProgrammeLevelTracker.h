#pragma once

#include "KWeighting.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
// Gated estimate of programme loudness, in approximate LUFS.
//
// This is the measuring half of the broadcast leveller, kept apart from the
// gain it drives so the same estimate can steer gain at more than one point of
// the chain. It only measures: what to do with the estimate is the caller's
// decision.
//
// The detector is K-weighted and summed across channels the way BS.1770 does,
// so the level it reports is comparable to the LUFS number an operator sets as
// a target. It is a one-pole rather than 400 ms blocks, so it approximates
// loudness rather than measuring it conformantly.
class ProgrammeLevelTracker final
{
public:
    // Kalman tuning, expressed as the time constant each steady-state gain
    // corresponds to, because seconds are reviewable and covariances are not.
    static constexpr double kalmanSteadySeconds = 2.00;
    static constexpr double kalmanSectionChangeSeconds = 0.05;
    // The gate, the speech variation and silence are judged on this fast
    // detector: they have to see a pause or a stop as it happens.
    static constexpr double kalmanDetectorSeconds = 0.05;
    // The estimate itself follows the 400 ms momentary window of BS.1770. The
    // Kalman averages in dB, and an average of logarithms sits below the
    // logarithm of the average by an amount that grows with how much the level
    // moves. On the 50 ms detector that was 4-5 dB on a sermon and 2 dB on
    // worship, measured on recorded services: speech read several dB quieter
    // than it was, so the leveller either pushed it above the music or ran into
    // its recovery limit trying. Integrating power over the momentary window
    // first leaves too little movement for that bias to matter.
    static constexpr double momentaryDetectorSeconds = 0.40;
    static constexpr double kalmanInnovationSeconds = 0.50;
    // Measurement noise in dB^2. Programme RMS wanders by a few dB between
    // syllables even when nothing about the mix has changed.
    static constexpr float kalmanMeasurementNoise = 9.0f;
    // Innovation this small is ordinary programme variation; this large is a
    // section change. Between them the filter speeds up proportionally.
    static constexpr float kalmanInnovationFloorDb = 4.0f;
    static constexpr float kalmanInnovationCeilingDb = 12.0f;
    static constexpr float kalmanInitialCovariance = 400.0f;
    // Gating, in the shape of BS.1770: an absolute floor plus a relative gate
    // below the running programme level. The relative gate does the real work of
    // excluding prayer pauses; the absolute one only catches true silence.
    static constexpr double loudnessAverageSeconds = 30.0;
    static constexpr float absoluteGateDb = -60.0f;
    static constexpr float relativeGateDb = 10.0f;
    // A quieter section settles near the relative gate rather than far below it,
    // and without hysteresis it sits there flickering the gate open and shut. That
    // costs more than a few wasted branches: the hold that decides pause from
    // section restarts on every flicker, so the section is never recognised at all.
    static constexpr float gateHysteresisDb = 2.0f;
    // Long enough to sit through a pause for prayer, short enough that a genuinely
    // quieter section is picked up while it is still the same thought.
    static constexpr double gateHoldSeconds = 2.50;
    // A preacher starting after the worship needs no such wait: the hold is
    // the stretch where the sermon plays at the music's gain, 10-20 dB too
    // quiet. Speech announces itself with syllable onsets, which a reverb tail
    // (a steady fall) and room tone (no movement) never produce, so a few of
    // them settle the question well before the full hold.
    static constexpr double speechHoldSeconds = 0.80;
    // Onsets are counted as a leaky activity, so only recent ones count: speech
    // keeps it high, a single door or cough fades out of it in a second or two.
    static constexpr double onsetActivitySeconds = 1.50;
    static constexpr float speechOnsetActivity = 2.2f;
    // Even the full hold needs recent onsets. Variation alone read a reverb tail
    // as speech -- a steady fall leaves the level far from its lagging trend --
    // and accepted the room tone after it as a section, levelling it up.
    static constexpr float sectionOnsetActivity = 0.8f;
    // Syllables inside a word dip only about 8 dB on the fast detector and rise
    // over some 100 ms, so the valley may only creep up slowly or it erases
    // them. One steep attack, like a door, still counts once: the next onset
    // needs the level to have turned down from its peak first.
    static constexpr float onsetRiseDb = 5.0f;
    static constexpr float onsetRearmDb = 3.0f;
    static constexpr float valleyRiseDbPerSecond = 20.0f;
    // How long the estimate is allowed to move quickly after a section has been
    // accepted. Long enough to cover the step, short enough that the next fall is
    // judged on its own merits.
    static constexpr double sectionRebaseSeconds = 3.00;
    static constexpr double gateIntegrationSeconds = 0.40;
    static constexpr double variationSeconds = 0.70;
    // Speech modulates its own level by several dB just by being speech. Steady
    // noise sits far below this even before the step that produced it has settled.
    static constexpr float speechVariationDb = 2.0f;
    // How long silence must last before the estimate stands down, and how quickly
    // it then slides to the stand-down level. Nothing is audible while it
    // happens, by definition.
    static constexpr double silenceReleaseSeconds = 1.50;
    static constexpr double silenceSlideSeconds = 0.60;

    void prepare(double sampleRate) noexcept
    {
        weighting.prepare(sampleRate);
        // A Kalman gain of 1/(fs*tau) behaves like a one-pole with that time
        // constant, and the steady-state relation K = sqrt(Q/R) inverts to give
        // the process noise the filter needs to settle there.
        kalmanSteadyGain = static_cast<float>(1.0 / (sampleRate * kalmanSteadySeconds));
        kalmanFastGain = static_cast<float>(1.0 / (sampleRate * kalmanSectionChangeSeconds));
        detectorCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * kalmanDetectorSeconds));
        momentaryCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * momentaryDetectorSeconds));
        innovationCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * kalmanInnovationSeconds));
        loudnessAverageCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * loudnessAverageSeconds));
        gateHoldSamples = static_cast<float>(sampleRate * gateHoldSeconds);
        speechHoldSamples = static_cast<float>(sampleRate * speechHoldSeconds);
        onsetActivityCoefficient = std::exp(-1.0f / static_cast<float>(sampleRate * onsetActivitySeconds));
        valleyRisePerSample = static_cast<float>(valleyRiseDbPerSecond / sampleRate);
        sectionRebaseLength = static_cast<float>(sampleRate * sectionRebaseSeconds);
        trendCoefficient = static_cast<float>(1.0 / (sampleRate * variationSeconds));
        gateLevelCoefficient = static_cast<float>(1.0 / (sampleRate * gateIntegrationSeconds));
        silenceReleaseSamples = static_cast<float>(sampleRate * silenceReleaseSeconds);
        silenceSlideGain = static_cast<float>(1.0 / (sampleRate * silenceSlideSeconds));
        reset();
    }

    void reset() noexcept
    {
        weighting.reset();
        lastPower = 0.0f;
        levelSquare = 0.0f;
        momentarySquare = 0.0f;
        // A large starting covariance is what lets the estimate lock onto the
        // first real programme within a fraction of a second instead of creeping
        // towards it with the steady-state time constant.
        estimateDb = -100.0f;
        covariance = kalmanInitialCovariance;
        innovationAverage = 0.0f;
        initialised = false;
        loudnessAverage = -100.0f;
        loudnessSampleCount = 0.0f;
        gateClosedSamples = 0.0f;
        sectionRebaseSamples = 0.0f;
        gateLevelDb = -100.0f;
        levelTrend = -100.0f;
        variationDb = 0.0f;
        valleyDb = -100.0f;
        onsetPeakDb = -100.0f;
        onsetArmed = true;
        onsetActivity = 0.0f;
        gateOpen = false;
        silenceSamples = 0.0f;
    }

    // One stereo frame. `standDownDb` is where the estimate drifts during real
    // silence; the caller chooses it so the gain it derives returns to unity.
    void process(const float* frame, int channels, float standDownDb) noexcept
    {
        auto power = 0.0f;
        for (int channel = 0; channel < channels; ++channel)
        {
            const auto weighted = weighting.process(channel, frame[channel]);
            power += weighted * weighted;
        }
        lastPower = power;
        levelSquare = detectorCoefficient * levelSquare + (1.0f - detectorCoefficient) * power;
        momentarySquare = momentaryCoefficient * momentarySquare + (1.0f - momentaryCoefficient) * power;
        update(toLoudnessDb(levelSquare), toLoudnessDb(momentarySquare), standDownDb);
    }

    [[nodiscard]] float getEstimateDb() const noexcept { return estimateDb; }
    // K-weighted power of the last frame, summed across channels.
    [[nodiscard]] float getLastPower() const noexcept { return lastPower; }
    [[nodiscard]] bool isGateOpen() const noexcept { return gateOpen; }
    [[nodiscard]] bool isInitialised() const noexcept { return initialised; }
    // True while there is a usable estimate the caller should follow: either the
    // gate is open, or real silence has lasted long enough that the estimate is
    // being walked back to its stand-down level.
    [[nodiscard]] bool isTracking() const noexcept
    {
        return initialised && (gateOpen || silenceSamples > silenceReleaseSamples);
    }
    [[nodiscard]] bool isInSilence() const noexcept { return silenceSamples > silenceReleaseSamples; }

private:
    [[nodiscard]] static float toLoudnessDb(float meanSquare) noexcept
    {
        return meanSquare > 1.0e-12f
            ? std::max(-100.0f, 10.0f * std::log10(meanSquare)) + KWeighting::lufsOffsetDb
            : -100.0f;
    }

    void update(float measuredDb, float momentaryDb, float standDownDb) noexcept
    {
        gateLevelDb += gateLevelCoefficient * (measuredDb - gateLevelDb);
        sectionRebaseSamples = std::max(0.0f, sectionRebaseSamples - 1.0f);

        // Tracked whether the gate is open or not: deciding what a closed gate
        // is looking at is exactly what this is for.
        levelTrend += trendCoefficient * (measuredDb - levelTrend);
        variationDb += trendCoefficient * (std::abs(measuredDb - levelTrend) - variationDb);
        onsetActivity *= onsetActivityCoefficient;
        if (onsetArmed)
        {
            valleyDb = std::min(measuredDb, valleyDb + valleyRisePerSample);
            if (measuredDb - valleyDb > onsetRiseDb)
            {
                onsetActivity += 1.0f;
                onsetArmed = false;
                onsetPeakDb = measuredDb;
            }
        }
        else
        {
            onsetPeakDb = std::max(onsetPeakDb, measuredDb);
            if (measuredDb < onsetPeakDb - onsetRearmDb)
            {
                onsetArmed = true;
                valleyDb = measuredDb;
            }
        }

        // Gate first. A closed gate freezes the estimate, so a pause cannot drag
        // the programme level down and then be levelled back up as room noise
        // when the speaker stops.
        if (gateLevelDb > absoluteGateDb)
        {
            silenceSamples = 0.0f;
            loudnessSampleCount += 1.0f;
            // Cumulative mean while the window is still filling, exponential
            // afterwards. BS.1770 averages every gated block for the same
            // reason: the relative gate is meaningless until the reference it
            // is relative to actually reflects the programme.
            const auto alpha = std::max(1.0f / loudnessSampleCount, 1.0f - loudnessAverageCoefficient);
            loudnessAverage += alpha * (gateLevelDb - loudnessAverage);
            // Both sides of the comparison are integrated. Syllables swing about
            // 12 dB peak to valley, wider than the 10 dB relative gate, so
            // gating on the fast detector makes every loud phrase close the gate
            // on its own quiet half. BS.1770 gates on 400 ms blocks for exactly
            // this reason.
            const auto relativeThreshold = loudnessAverage - relativeGateDb
                + (gateOpen ? 0.0f : gateHysteresisDb);
            gateOpen = gateLevelDb > relativeThreshold;
        }
        else
        {
            silenceSamples += 1.0f;
            gateOpen = false;
        }

        if (gateOpen)
        {
            gateClosedSamples = 0.0f;
        }
        else if (silenceSamples <= 0.0f)
        {
            // Only counted while there is still programme present. Real silence
            // is handled by the release path below, not by rebasing onto it.
            if (gateClosedSamples <= 0.0f)
            {
                // The measurement window starts here. The step that closed the
                // gate is itself a large deviation, and letting it into the
                // variation would make every pause look like speech for the
                // first few seconds -- which is precisely the span being judged.
                levelTrend = measuredDb;
                variationDb = 0.0f;
                onsetActivity = 0.0f;
            }
            gateClosedSamples += 1.0f;
            // Duration alone cannot separate a quieter section from room tone:
            // four seconds of air conditioning outlasts any sensible hold. What
            // separates them is that speech moves and a room does not.
            const auto heldLongEnough
                = (gateClosedSamples > gateHoldSamples && onsetActivity >= sectionOnsetActivity)
                || (gateClosedSamples > speechHoldSamples && onsetActivity >= speechOnsetActivity);
            if (heldLongEnough && variationDb > speechVariationDb)
            {
                loudnessAverage = gateLevelDb;
                loudnessSampleCount = 1.0f;
                gateOpen = true;
                gateClosedSamples = 0.0f;
                // The gate has just concluded this is a new section. Making the
                // filter rediscover that from its own innovation would waste the
                // second it takes to build up, and the conclusion is already in
                // hand: hand it over.
                innovationAverage = kalmanInnovationCeilingDb;
                sectionRebaseSamples = sectionRebaseLength;
            }
        }

        if (gateOpen)
        {
            if (!initialised)
            {
                estimateDb = momentaryDb;
                initialised = true;
            }

            const auto innovation = momentaryDb - estimateDb;
            innovationAverage = innovationCoefficient * innovationAverage
                + (1.0f - innovationCoefficient) * std::abs(innovation);
            // Sustained innovation means the programme really moved, not that a
            // syllable was loud. Only then is it worth abandoning the slow
            // estimate, which is the whole anti-pumping argument. Rising
            // programme is unambiguous -- the mix got louder and the gain has to
            // come down now -- so it may always accelerate. Falling programme
            // may not, unless the gate has already accepted it as a section
            // rather than a pause.
            const auto mayAccelerate = innovation > 0.0f || sectionRebaseSamples > 0.0f;
            const auto sectionChange = mayAccelerate
                ? std::clamp((innovationAverage - kalmanInnovationFloorDb)
                                 / (kalmanInnovationCeilingDb - kalmanInnovationFloorDb), 0.0f, 1.0f)
                : 0.0f;
            const auto steadyStateGain = kalmanSteadyGain + sectionChange * (kalmanFastGain - kalmanSteadyGain);
            const auto processNoise = steadyStateGain * steadyStateGain * kalmanMeasurementNoise;

            const auto predictedCovariance = covariance + processNoise;
            const auto kalmanGain = predictedCovariance / (predictedCovariance + kalmanMeasurementNoise);
            estimateDb += kalmanGain * innovation;
            covariance = (1.0f - kalmanGain) * predictedCovariance;
        }
        else if (initialised && silenceSamples > silenceReleaseSamples)
        {
            // Standing down is expressed as the estimate drifting, not as an
            // override on the gain: one state variable stays in charge, so when
            // programme returns the gain is already continuous and the Kalman
            // simply picks up from where it is.
            estimateDb += silenceSlideGain * (standDownDb - estimateDb);
            // Nothing has been measured for over a second, so the estimate is
            // worth very little. Saying so is what lets it re-lock quickly.
            covariance = kalmanInitialCovariance;
            innovationAverage = 0.0f;
        }
    }

    KWeighting weighting;
    float lastPower = 0.0f;
    float levelSquare = 0.0f;
    float momentarySquare = 0.0f;

    // One-dimensional Kalman estimate of programme level in dB. The point is
    // not the maths but the adaptive gain: while the level is stable the
    // covariance collapses and the estimate barely moves, which is what stops a
    // leveller from breathing between phrases. When the innovation stays large
    // -- worship ending, a pastor starting to pray -- the process noise is
    // raised and the same filter turns into a fast one for as long as the
    // section change lasts.
    float estimateDb = -100.0f;
    float covariance = kalmanInitialCovariance;
    float innovationAverage = 0.0f;
    bool initialised = false;
    // Running programme level the relative gate hangs from. Counts gated
    // samples so it starts as a true cumulative mean and only then degrades into
    // a 30 s window; a plain exponential average needs minutes to become
    // meaningful, and until it does the relative gate excludes nothing.
    float loudnessAverage = -100.0f;
    float loudnessSampleCount = 0.0f;
    // A pause and a quieter section look identical to the relative gate: both
    // drop below it. What separates them is how long they last, and whether the
    // level keeps moving the way speech does.
    float gateClosedSamples = 0.0f;
    float gateHoldSamples = 0.0f;
    // Counts down after the gate accepts a new section, and is the only thing
    // that lets the estimate accelerate downwards. Speeding up on any fall
    // means a pause is chased for as long as the gate takes to classify it, and
    // that fall is exactly the room tone nobody wants amplified.
    float sectionRebaseSamples = 0.0f;
    float sectionRebaseLength = 0.0f;
    // The level the gate decides on, integrated over roughly the 400 ms block
    // BS.1770 gates on. The Kalman still measures the fast 50 ms detector: a
    // gate driven by that detector opens and closes on every syllable.
    float gateLevelDb = -100.0f;
    float gateLevelCoefficient = 0.0f;
    float levelTrend = -100.0f;
    float variationDb = 0.0f;
    float trendCoefficient = 0.0f;
    // Recent syllable onsets since the gate closed: rises of the fast
    // detector well above its recent valley.
    float valleyDb = -100.0f;
    float onsetPeakDb = -100.0f;
    float valleyRisePerSample = 0.0f;
    bool onsetArmed = true;
    float onsetActivity = 0.0f;
    float onsetActivityCoefficient = 0.0f;
    float speechHoldSamples = 0.0f;
    bool gateOpen = false;
    // A closed gate freezes the estimate, which is right for a prayer pause but
    // wrong for the end of a service: holding a large gain armed means the next
    // thing through the microphone gets amplified.
    float silenceSamples = 0.0f;
    float silenceReleaseSamples = 0.0f;
    float silenceSlideGain = 0.0f;
    float kalmanSteadyGain = 0.0f;
    float kalmanFastGain = 0.0f;
    float innovationCoefficient = 0.0f;
    float loudnessAverageCoefficient = 0.0f;
    float detectorCoefficient = 0.0f;
    float momentaryCoefficient = 0.0f;
};
} // namespace churchstream
