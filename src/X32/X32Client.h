#pragma once

#include "OscMessage.h"

#include <array>
#include <atomic>
#include <juce_core/juce_core.h>

namespace churchstream
{
struct X32ChannelInfo
{
    juce::String name;
    float faderDb = -90.0f;
    bool on = false;
    bool valid = false;
};

struct X32State
{
    bool connected = false;
    juce::String host;
    juce::String consoleName;
    juce::String model;
    juce::String firmware;
    static constexpr int channelCount = 32;
    static constexpr int busCount = 16;
    std::array<X32ChannelInfo, channelCount> channels;
    std::array<juce::String, busCount> busNames;
    int namedChannels = 0;
    int rejectedWrites = 0;
    juce::String lastError;
};

// Read-only X32 remote client. It exists to let the application understand how
// the console is built - channel names, faders, buses - so its suggestions can
// be specific. It never changes console state: every outgoing message is
// checked against an allowlist of query addresses and rejected if it carries
// arguments, because an argument is what turns an OSC query into a write.
//
// Console control is a separate, later decision and would need an explicit
// allowlist of writable addresses plus operator confirmation.
class X32Client final : private juce::Thread
{
public:
    static constexpr int defaultPort = 10023;

    X32Client();
    ~X32Client() override;

    void start(juce::String hostName, int portNumber = defaultPort);
    void stop();
    [[nodiscard]] X32State getState() const;
    [[nodiscard]] bool isRunning() const noexcept { return isThreadRunning(); }

    // Exposed so the read-only guarantee is directly testable.
    [[nodiscard]] static bool isReadOnlyQuery(const OscMessage& message) noexcept;
    [[nodiscard]] static float faderToDb(float normalised) noexcept;

private:
    void run() override;
    bool sendQuery(juce::DatagramSocket& socket, const juce::String& address);
    void handleMessage(const OscMessage& message);
    static int indexFromAddress(const juce::String& address, const juce::String& prefix) noexcept;

    mutable juce::CriticalSection stateLock;
    X32State state;
    juce::String host;
    std::atomic<int> port { defaultPort };
    std::atomic<int> rejectedWrites { 0 };
};
} // namespace churchstream
