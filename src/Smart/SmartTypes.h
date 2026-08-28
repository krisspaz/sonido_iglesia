#pragma once

#include "Analysis/AnalysisTypes.h"

#include <array>
#include <juce_core/juce_core.h>

namespace churchstream
{
enum class AutoTuneState : int { idle, analysing, complete };
enum class MixProfile : int { unknown, balanced, bright, dark, bassHeavy, dynamic, dense, vocalHeavy, acoustic };
enum class SmartScene : int { autoDetect, preaching, worship, fullBand, announcements, prayer, ambience };

struct QualityScores
{
    float overall = 0.0f;
    float tonalBalance = 0.0f;
    float dynamics = 0.0f;
    float loudness = 0.0f;
    float truePeak = 0.0f;
    float clarity = 0.0f;
    float stereo = 0.0f;
    float noise = 0.0f;
    float compression = 0.0f;
    float stability = 0.0f;
};

struct SmartProblem
{
    juce::String name;
    juce::String detail;
    float severity = 0.0f;
    bool warning = false;
};

struct SmartAction
{
    juce::String name;
    juce::String reason;
    float amountDb = 0.0f;
    float confidence = 0.0f;
    float frequencyHz = 0.0f;
    int priority = 4;
    bool active = false;
    bool rolledBack = false;
    juce::String result;
};

struct SmartState
{
    bool active = true;
    AutoTuneState autoTuneState = AutoTuneState::idle;
    float autoTuneProgress = 0.0f;
    MixProfile profile = MixProfile::unknown;
    MixContext context = MixContext::quiet;
    SmartScene scene = SmartScene::autoDetect;
    bool baselineReady = false;
    QualityScores quality;
    std::array<SmartProblem, 9> problems;
    int problemCount = 0;
    int rollbackCount = 0;
    juce::String churchName { "Mi Iglesia" };
    std::array<float, 5> bandState {};
    float rumbleState = 0.0f;
    float dynamicsState = 0.0f;
    float transientState = 0.0f;
    std::array<SmartAction, 8> actions;
    int actionCount = 0;
    double updateRateHz = 0.0;
};

inline juce::String profileName(MixProfile profile)
{
    switch (profile)
    {
        case MixProfile::balanced: return "BALANCED";
        case MixProfile::bright: return "BRIGHT";
        case MixProfile::dark: return "DARK";
        case MixProfile::bassHeavy: return "BASS HEAVY";
        case MixProfile::dynamic: return "DYNAMIC";
        case MixProfile::dense: return "DENSE";
        case MixProfile::vocalHeavy: return "VOCAL HEAVY";
        case MixProfile::acoustic: return "ACOUSTIC";
        case MixProfile::unknown: return "UNDETECTED";
    }
}

inline juce::String contextName(MixContext context)
{
    switch (context)
    {
        case MixContext::speech: return "SPEECH";
        case MixContext::music: return "MUSIC";
        case MixContext::fullBand: return "FULL BAND";
        case MixContext::denseMusic: return "DENSE MUSIC";
        case MixContext::ambience: return "AMBIENCE";
        case MixContext::worshipSoft: return "WORSHIP SOFT";
        case MixContext::soloVocal: return "SOLO VOCAL";
        case MixContext::quiet: return "QUIET";
    }
}

inline juce::String sceneName(SmartScene scene)
{
    switch (scene)
    {
        case SmartScene::autoDetect: return "AUTO SCENE";
        case SmartScene::preaching: return "PREACHING";
        case SmartScene::worship: return "WORSHIP";
        case SmartScene::fullBand: return "FULL BAND";
        case SmartScene::announcements: return "ANNOUNCEMENTS";
        case SmartScene::prayer: return "PRAYER";
        case SmartScene::ambience: return "AMBIENCE";
    }
}
} // namespace churchstream
