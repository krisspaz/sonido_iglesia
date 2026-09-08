#include "AutoGroupRouter.h"

#include <algorithm>
#include <cmath>
#include <cctype>

namespace churchstream
{
namespace
{
float clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }

float closeness(float value, float target, float radius) noexcept
{
    return clamp01(1.0f - std::abs(value - target) / radius);
}

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool containsAny(const std::string& text, std::initializer_list<const char*> words)
{
    for (const auto* word : words)
        if (text.find(word) != std::string::npos) return true;
    return false;
}
}

void AutoGroupRouter::prepare(double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 1000.0 ? newSampleRate : 48000.0;
    reset();
}

void AutoGroupRouter::reset() noexcept
{
    candidates = {};
    analysedSamples = 0.0;
    evaluationCountdown = 0.0f;
    pending = {};
    pendingSeconds = 0.0f;
    phase.store(static_cast<int>(AutoRoutePhase::waitingForAudio), std::memory_order_release);
    pairCountPublished.store(0, std::memory_order_release);
    secondsPublished.store(0.0f, std::memory_order_release);
    for (int role = 0; role < 3; ++role)
    {
        routePairs[static_cast<size_t>(role)].store(-1, std::memory_order_release);
        routeConfidence[static_cast<size_t>(role)].store(0.0f, std::memory_order_release);
    }
    for (auto& pairHints : nameHints)
        for (auto& hint : pairHints)
            hint.store(0.0f, std::memory_order_release);
}

void AutoGroupRouter::process(const float* const* channels, int channelCount, int sampleCount) noexcept
{
    if (channels == nullptr || sampleCount <= 0) return;
    const auto pairCount = std::min(maxCandidatePairs, channelCount / 2);
    pairCountPublished.store(pairCount, std::memory_order_relaxed);
    if (pairCount < 3)
    {
        phase.store(static_cast<int>(AutoRoutePhase::waitingForAudio), std::memory_order_release);
        return;
    }

    const auto lowCoefficient = std::exp(-2.0 * 3.14159265358979323846 * 250.0 / sampleRate);
    const auto fullCoefficient = std::exp(-2.0 * 3.14159265358979323846 * 4000.0 / sampleRate);
    auto anyAudio = false;

    for (int pair = 0; pair < pairCount; ++pair)
    {
        auto& candidate = candidates[static_cast<size_t>(pair)];
        const auto* left = channels[pair * 2];
        const auto* right = channels[pair * 2 + 1];
        if (left == nullptr || right == nullptr) continue;

        double totalEnergy = 0.0, lowEnergy = 0.0, midEnergy = 0.0, highEnergy = 0.0;
        double sumEnergy = 0.0, differenceEnergy = 0.0;
        float peak = 0.0f;
        for (int sample = 0; sample < sampleCount; ++sample)
        {
            const float values[2] { left[sample], right[sample] };
            for (int side = 0; side < 2; ++side)
            {
                const auto value = static_cast<double>(values[side]);
                candidate.lowState[side] = lowCoefficient * candidate.lowState[side]
                    + (1.0 - lowCoefficient) * value;
                candidate.fullState[side] = fullCoefficient * candidate.fullState[side]
                    + (1.0 - fullCoefficient) * value;
                const auto low = candidate.lowState[side];
                const auto mid = candidate.fullState[side] - low;
                const auto high = value - candidate.fullState[side];
                totalEnergy += value * value;
                lowEnergy += low * low;
                midEnergy += mid * mid;
                highEnergy += high * high;
                peak = std::max(peak, std::abs(values[side]));
            }
            const auto sum = 0.5 * static_cast<double>(values[0] + values[1]);
            const auto difference = 0.5 * static_cast<double>(values[0] - values[1]);
            sumEnergy += sum * sum;
            differenceEnergy += difference * difference;
        }

        const auto denominator = std::max(totalEnergy, 1.0e-12);
        const auto rms = static_cast<float>(std::sqrt(totalEnergy / (2.0 * sampleCount)));
        const auto smoothing = 0.035f;
        candidate.level += smoothing * (clamp01((20.0f * std::log10(std::max(rms, 1.0e-8f)) + 60.0f) / 45.0f)
                                              - candidate.level);
        candidate.lowRatio += smoothing * (static_cast<float>(lowEnergy / denominator) - candidate.lowRatio);
        candidate.midRatio += smoothing * (static_cast<float>(midEnergy / denominator) - candidate.midRatio);
        candidate.highRatio += smoothing * (static_cast<float>(highEnergy / denominator) - candidate.highRatio);
        const auto width = static_cast<float>(differenceEnergy / std::max(sumEnergy + differenceEnergy, 1.0e-12));
        candidate.width += smoothing * (clamp01(width * 2.0f) - candidate.width);
        candidate.crest += smoothing * (std::clamp(peak / std::max(rms, 1.0e-6f), 1.0f, 8.0f) - candidate.crest);
        candidate.activity += smoothing * ((rms > 0.001f ? 1.0f : 0.0f) - candidate.activity);
        anyAudio = anyAudio || rms > 0.001f;
    }

    if (!anyAudio) return;
    analysedSamples += sampleCount;
    const auto seconds = static_cast<float>(analysedSamples / sampleRate);
    secondsPublished.store(seconds, std::memory_order_relaxed);
    // READY and UNCERTAIN are decisions published by evaluate(). Do not
    // overwrite them on every audio block, otherwise consumers observe READY
    // only for the single block that happened to run an evaluation.
    if (phase.load(std::memory_order_acquire) == static_cast<int>(AutoRoutePhase::waitingForAudio))
        phase.store(static_cast<int>(AutoRoutePhase::analysing), std::memory_order_release);

    evaluationCountdown -= static_cast<float>(sampleCount / sampleRate);
    if (evaluationCountdown <= 0.0f)
    {
        evaluationCountdown = 0.25f;
        evaluate(pairCount);
    }
}

void AutoGroupRouter::evaluate(int pairCount) noexcept
{
    const auto scoreFor = [this](int pair, GroupRole role)
    {
        const auto roleIndex = static_cast<size_t>(role);
        const auto nameHint = nameHints[static_cast<size_t>(pair)][roleIndex].load(std::memory_order_acquire);
        return roleScore(candidates[static_cast<size_t>(pair)], role, nameHint);
    };

    Assignment best;
    for (int voice = 0; voice < pairCount; ++voice)
        for (int music = 0; music < pairCount; ++music)
            for (int ambience = 0; ambience < pairCount; ++ambience)
            {
                if (voice == music || voice == ambience || music == ambience) continue;
                const std::array<float, 3> scores {
                    scoreFor(voice, GroupRole::voice),
                    scoreFor(music, GroupRole::music),
                    scoreFor(ambience, GroupRole::ambience)
                };
                const auto total = scores[0] + scores[1] + scores[2];
                if (total > best.score)
                {
                    best.score = total;
                    best.pair = { voice, music, ambience };
                    best.confidence = scores;
                }
            }

    // Confidence also includes separation from the next-best candidate for
    // each role; this prevents arbitrary labels when every pair sounds alike.
    for (int role = 0; role < 3; ++role)
    {
        auto alternate = 0.0f;
        for (int pair = 0; pair < pairCount; ++pair)
            if (pair != best.pair[static_cast<size_t>(role)])
                alternate = std::max(alternate, scoreFor(pair, static_cast<GroupRole>(role)));
        const auto separation = clamp01((best.confidence[static_cast<size_t>(role)] - alternate + 0.18f) / 0.36f);
        best.confidence[static_cast<size_t>(role)] =
            clamp01(0.65f * best.confidence[static_cast<size_t>(role)] + 0.35f * separation);
    }

    if (best.pair == pending.pair)
        pendingSeconds += 0.25f;
    else
    {
        pending = best;
        pendingSeconds = 0.0f;
    }
    pending.confidence = best.confidence;
    pending.score = best.score;

    const auto enoughAudio = analysedSamples / sampleRate >= 4.0;
    const auto confident = *std::min_element(best.confidence.begin(), best.confidence.end()) >= 0.52f;
    const auto stable = pendingSeconds >= 2.0f;
    if (enoughAudio && confident && stable)
    {
        for (int role = 0; role < 3; ++role)
        {
            routePairs[static_cast<size_t>(role)].store(best.pair[static_cast<size_t>(role)], std::memory_order_release);
            routeConfidence[static_cast<size_t>(role)].store(best.confidence[static_cast<size_t>(role)], std::memory_order_release);
        }
        phase.store(static_cast<int>(AutoRoutePhase::ready), std::memory_order_release);
    }
    else if (enoughAudio && !confident)
        phase.store(static_cast<int>(AutoRoutePhase::uncertain), std::memory_order_release);
}

float AutoGroupRouter::roleScore(const Candidate& candidate, GroupRole role, float nameHint) noexcept
{
    const auto crest = clamp01((candidate.crest - 1.0f) / 4.0f);
    const auto tonalTotal = std::max(candidate.lowRatio + candidate.midRatio + candidate.highRatio, 1.0e-5f);
    const auto low = candidate.lowRatio / tonalTotal;
    const auto mid = candidate.midRatio / tonalTotal;
    const auto high = candidate.highRatio / tonalTotal;
    if (role == GroupRole::voice)
        return clamp01(0.27f * closeness(mid, 0.62f, 0.55f)
                       + 0.22f * (1.0f - candidate.width)
                       + 0.16f * crest + 0.12f * (1.0f - low)
                       + 0.08f * candidate.activity + 0.15f * nameHint);
    if (role == GroupRole::music)
        return clamp01(0.20f * candidate.activity + 0.17f * candidate.level
                       + 0.17f * candidate.width + 0.14f * closeness(low, 0.24f, 0.24f)
                       + 0.12f * closeness(high, 0.20f, 0.28f)
                       + 0.20f * nameHint);
    return clamp01(0.25f * candidate.width + 0.18f * high
                   + 0.18f * (1.0f - candidate.level) + 0.10f * (1.0f - crest)
                   + 0.09f * candidate.activity + 0.20f * nameHint);
}

void AutoGroupRouter::setCandidateName(int pairIndex, const std::string& name)
{
    if (pairIndex < 0 || pairIndex >= maxCandidatePairs) return;
    auto& hints = nameHints[static_cast<size_t>(pairIndex)];
    hints[static_cast<size_t>(GroupRole::voice)].store(keywordScore(name, GroupRole::voice), std::memory_order_release);
    hints[static_cast<size_t>(GroupRole::music)].store(keywordScore(name, GroupRole::music), std::memory_order_release);
    hints[static_cast<size_t>(GroupRole::ambience)].store(keywordScore(name, GroupRole::ambience), std::memory_order_release);
}

float AutoGroupRouter::keywordScore(const std::string& original, GroupRole role)
{
    const auto text = lower(original);
    if (role == GroupRole::voice)
        return containsAny(text, { "vox", "vocal", "voice", "voz", "speech", "pastor", "pred" }) ? 1.0f : 0.0f;
    if (role == GroupRole::music)
        return containsAny(text, { "band", "music", "musica", "worship", "banda", "instr" }) ? 1.0f : 0.0f;
    return containsAny(text, { "amb", "room", "sala", "audience", "congreg", "crowd" }) ? 1.0f : 0.0f;
}

AutoRouteSnapshot AutoGroupRouter::getSnapshot() const noexcept
{
    AutoRouteSnapshot snapshot;
    snapshot.phase = static_cast<AutoRoutePhase>(phase.load(std::memory_order_acquire));
    snapshot.candidatePairs = pairCountPublished.load(std::memory_order_acquire);
    snapshot.analysedSeconds = secondsPublished.load(std::memory_order_acquire);
    std::array<GroupRoute*, 3> routes { &snapshot.routes.voice, &snapshot.routes.music, &snapshot.routes.ambience };
    for (int role = 0; role < 3; ++role)
    {
        const auto pair = routePairs[static_cast<size_t>(role)].load(std::memory_order_acquire);
        routes[static_cast<size_t>(role)]->leftChannel = pair >= 0 ? pair * 2 : -1;
        routes[static_cast<size_t>(role)]->rightChannel = pair >= 0 ? pair * 2 + 1 : -1;
        snapshot.confidence[static_cast<size_t>(role)] = routeConfidence[static_cast<size_t>(role)].load(std::memory_order_acquire);
    }
    return snapshot;
}

bool AutoGroupRouter::getResolvedRoutes(GroupRoutingConfig& destination) const noexcept
{
    const auto snapshot = getSnapshot();
    if (snapshot.phase != AutoRoutePhase::ready) return false;
    destination = snapshot.routes;
    return true;
}
} // namespace churchstream
