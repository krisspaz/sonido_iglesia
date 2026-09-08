#pragma once

#include "Groups/AutoGroupRouter.h"
#include "X32/X32Client.h"
#include "Theme.h"

#include <array>
#include <juce_gui_basics/juce_gui_basics.h>

namespace churchstream
{
// Read-only console group panel. It turns the X32 link and the group router
// into visible information: whether the console is connected, which buses the
// operator has named, and which card channels currently feed each stem. It
// never sends anything to the console.
class X32Panel final : public juce::Component
{
public:
    X32Panel();

    void setState(const X32State& state, const AutoRouteSnapshot& routing);
    void paint(juce::Graphics&) override;

private:
    [[nodiscard]] static juce::String roleLabel(GroupRole role);

    X32State state;
    AutoRouteSnapshot routing;
};
} // namespace churchstream
