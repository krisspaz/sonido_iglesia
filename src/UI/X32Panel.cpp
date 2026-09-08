#include "X32Panel.h"

#include <cmath>

namespace churchstream
{
X32Panel::X32Panel()
{
    setInterceptsMouseClicks(false, false);
}

juce::String X32Panel::roleLabel(GroupRole role)
{
    switch (role)
    {
    case GroupRole::voice: return "VOICE";
    case GroupRole::music: return "MUSIC";
    case GroupRole::ambience: return "AMBIENCE";
    }
    return "?";
}

void X32Panel::setState(const X32State& newState, const AutoRouteSnapshot& newRouting)
{
    state = newState;
    routing = newRouting;
    repaint();
}

void X32Panel::paint(juce::Graphics& graphics)
{
    auto bounds = getLocalBounds().toFloat();

    // Header: connection status.
    auto header = bounds.removeFromTop(22.0f);
    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(10.0f).withStyle("Bold")));
    graphics.drawText("X32 CONSOLE", header.removeFromLeft(110.0f), juce::Justification::centredLeft);

    const auto connected = state.connected;
    graphics.setColour(connected ? Colours::primary : Colours::danger);
    auto status = header.removeFromLeft(150.0f);
    graphics.drawText(connected ? "CONNECTED" : "DISCONNECTED", status, juce::Justification::centredLeft);
    graphics.setColour(Colours::mutedText);
    const auto model = (state.model.isNotEmpty() ? state.model : juce::String("X32"))
        + (state.firmware.isNotEmpty() ? "  " + state.firmware : juce::String());
    graphics.drawText(model, header, juce::Justification::centredLeft);

    if (!connected)
    {
        bounds.removeFromTop(4.0f);
        auto empty = bounds.removeFromTop(20.0f);
        graphics.setColour(Colours::mutedText);
        graphics.setFont(juce::Font(juce::FontOptions(10.0f).withStyle("Regular")));
        graphics.drawText("Console link is read-only. Enable it under ADVANCED to name groups.",
                          empty, juce::Justification::centredLeft);
    }

    // Named buses.
    bounds.removeFromTop(4.0f);
    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(9.0f).withStyle("Bold")));
    auto busHeader = bounds.removeFromTop(14.0f);
    graphics.drawText("NAMED BUSES", busHeader.removeFromLeft(120.0f), juce::Justification::centredLeft);

    int shown = 0;
    const auto rowHeight = 17.0f;
    for (int bus = 0; bus < X32State::busCount; ++bus)
    {
        const auto name = state.busNames[static_cast<size_t>(bus)].trim();
        if (name.isEmpty())
            continue;
        if (shown >= 4)
            break;
        auto row = bounds.removeFromTop(rowHeight);
        graphics.setColour(Colours::mutedText);
        graphics.setFont(juce::Font(juce::FontOptions(9.0f).withStyle("Bold")));
        graphics.drawText(juce::String(bus + 1) + "  ", row.removeFromLeft(26.0f),
                          juce::Justification::centredLeft);
        graphics.setColour(Colours::text);
        graphics.drawText(name, row, juce::Justification::centredLeft);
        ++shown;
    }
    if (shown == 0)
    {
        auto row = bounds.removeFromTop(rowHeight);
        graphics.setColour(Colours::mutedText);
        graphics.setFont(juce::Font(juce::FontOptions(9.0f).withStyle("Regular")));
        graphics.drawText(connected ? "No buses named on the console yet." : "--",
                          row, juce::Justification::centredLeft);
    }

    // Group routing derived from audio.
    bounds.removeFromTop(4.0f);
    graphics.setColour(Colours::mutedText);
    graphics.setFont(juce::Font(juce::FontOptions(9.0f).withStyle("Bold")));
    auto routeHeader = bounds.removeFromTop(14.0f);
    graphics.drawText("GROUP ROUTING", routeHeader.removeFromLeft(120.0f), juce::Justification::centredLeft);

    const auto phaseReady = routing.phase == AutoRoutePhase::ready
        || routing.phase == AutoRoutePhase::analysing;
    if (!phaseReady)
    {
        auto row = bounds.removeFromTop(rowHeight);
        graphics.setColour(Colours::mutedText);
        graphics.setFont(juce::Font(juce::FontOptions(9.0f).withStyle("Regular")));
        graphics.drawText("Waiting for a distinct stereo mix to resolve voices/music.",
                          row, juce::Justification::centredLeft);
    }
    else
    {
        const auto confidence = routing.confidence;
        const auto routes = routing.routes;
        const auto roleText = [&](GroupRole role, const GroupRoute& route) -> juce::String
        {
            const auto conf = confidence[static_cast<size_t>(role)];
            const auto prefix = roleLabel(role);
            if (route.validFor(X32State::channelCount))
                return prefix + "  <-  Ch " + juce::String(route.leftChannel + 1) + " / "
                    + juce::String(route.rightChannel + 1) + "   "
                    + juce::String(static_cast<int>(std::round(conf * 100.0f))) + "%";
            return prefix + "  (not assigned)";
        };
        const std::array<GroupRole, 3> roles { GroupRole::voice, GroupRole::music, GroupRole::ambience };
        for (const auto role : roles)
        {
            auto row = bounds.removeFromTop(rowHeight);
            graphics.setColour(Colours::mutedText);
            graphics.setFont(juce::Font(juce::FontOptions(8.5f).withStyle("Bold")));
            graphics.drawText(roleLabel(role), row.removeFromLeft(86.0f), juce::Justification::centredLeft);
            GroupRoute route { -1, -1 };
            switch (role)
            {
            case GroupRole::voice: route = routes.voice; break;
            case GroupRole::music: route = routes.music; break;
            case GroupRole::ambience: route = routes.ambience; break;
            }
            graphics.setColour(routing.phase == AutoRoutePhase::ready ? Colours::text : Colours::warning);
            graphics.setFont(juce::Font(juce::FontOptions(8.5f).withStyle("Regular")));
            graphics.drawText(roleText(role, route), row, juce::Justification::centredLeft);
        }
    }
}
} // namespace churchstream
