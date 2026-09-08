#pragma once

#include "Biquad.h"

#include <array>
#include <cmath>

namespace churchstream
{
// The two-stage K-weighting curve of ITU-R BS.1770: a high shelf that stands in
// for the acoustic effect of a head, followed by the RLB high-pass that removes
// the low frequencies a listener does not weigh when judging loudness.
//
// The standard tabulates coefficients for 48 kHz only. The analog prototype
// behind them is a shelf at 1681.97 Hz with Q = 0.7071752 and +3.9998 dB, and a
// high-pass at 38.135 Hz with Q = 0.5003, so bilinear-transforming those at the
// running rate reproduces the published 48 kHz numbers and stays correct at
// 44.1 and 96 kHz instead of silently mis-weighting them.
//
// This exists because the programme leveller measured flat RMS. On worship the
// kick and the bass dominate a flat RMS, so the same voice ends up levelled
// differently depending on what the band is playing underneath it. Weighting the
// detector is what makes speech and music land in the same place.
class KWeighting final
{
public:
    static constexpr float lufsOffsetDb = -0.691f;

    void prepare(double sampleRate) noexcept
    {
        shelf.setHighShelf(sampleRate, 1681.974f, 0.7071752f, 3.999843f);
        highPass.setHighPass(sampleRate, 38.135471f, 0.5003270f);
        reset();
    }

    void reset() noexcept
    {
        shelf.reset();
        highPass.reset();
    }

    [[nodiscard]] float process(int channel, float sample) noexcept
    {
        return highPass.process(channel, shelf.process(channel, sample));
    }

private:
    Biquad shelf;
    Biquad highPass;
};
} // namespace churchstream
