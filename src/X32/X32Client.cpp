#include "X32Client.h"

#include <algorithm>
#include <cmath>

namespace churchstream
{
namespace
{
// Every address the client is allowed to send. All of them are queries: the
// X32 answers with the current value and changes nothing.
const char* const allowedQueries[] {
    "/info",
    "/status",
    "/xremote",
    "/ch/",
    "/bus/",
    "/config/"
};

bool addressIsAllowed(const juce::String& address)
{
    for (const auto* allowed : allowedQueries)
        if (address.startsWith(allowed))
            return true;
    return false;
}

juce::String twoDigits(int value)
{
    return juce::String(value).paddedLeft('0', 2);
}
}

X32Client::X32Client() : Thread("CSP X32 Read-Only Client") {}
X32Client::~X32Client() { stop(); }

void X32Client::start(juce::String hostName, int portNumber)
{
    stop();
    const auto cleaned = hostName.trim();
    if (cleaned.isEmpty()) return;
    {
        const juce::ScopedLock lock(stateLock);
        state = {};
        state.host = cleaned;
    }
    host = cleaned;
    port.store(portNumber > 0 && portNumber < 65536 ? portNumber : defaultPort,
               std::memory_order_release);
    startThread(juce::Thread::Priority::background);
}

void X32Client::stop()
{
    signalThreadShouldExit();
    stopThread(2000);
    const juce::ScopedLock lock(stateLock);
    state.connected = false;
}

X32State X32Client::getState() const
{
    const juce::ScopedLock lock(stateLock);
    auto copy = state;
    copy.rejectedWrites = rejectedWrites.load(std::memory_order_relaxed);
    return copy;
}

bool X32Client::isReadOnlyQuery(const OscMessage& message) noexcept
{
    // An OSC message with arguments sets a value. Queries carry none, so the
    // rule is simple enough to be checked at the single point of transmission.
    if (message.getArgumentCount() != 0) return false;
    return addressIsAllowed(message.getAddress());
}

float X32Client::faderToDb(float normalised) noexcept
{
    const auto value = std::clamp(normalised, 0.0f, 1.0f);
    if (value <= 0.0f) return -90.0f;
    if (value >= 0.5f) return value * 40.0f - 30.0f;
    if (value >= 0.25f) return value * 80.0f - 50.0f;
    if (value >= 0.0625f) return value * 160.0f - 70.0f;
    return value * 480.0f - 90.0f;
}

int X32Client::indexFromAddress(const juce::String& address, const juce::String& prefix) noexcept
{
    if (!address.startsWith(prefix)) return -1;
    const auto remainder = address.substring(prefix.length());
    const auto number = remainder.upToFirstOccurrenceOf("/", false, false);
    if (number.length() != 2 || !number.containsOnly("0123456789")) return -1;
    return number.getIntValue() - 1;
}

void X32Client::run()
{
    juce::DatagramSocket socket(false);
    if (!socket.bindToPort(0))
    {
        const juce::ScopedLock lock(stateLock);
        state.lastError = "Could not open a local UDP port for the X32 client";
        return;
    }

    const auto targetPort = port.load(std::memory_order_acquire);
    auto subscriptionCountdown = 0.0;
    auto pollCountdown = 0.0;
    auto secondsSinceReply = 0.0;
    auto pollCursor = 0;
    auto lastTick = juce::Time::getMillisecondCounterHiRes();
    std::array<uint8_t, 2048> buffer {};

    while (!threadShouldExit())
    {
        if (subscriptionCountdown <= 0.0)
        {
            // /xremote expires after a few seconds; the console stops sending
            // updates unless it is renewed.
            sendQuery(socket, "/xremote");
            sendQuery(socket, "/info");
            sendQuery(socket, "/status");
            subscriptionCountdown = 6.0;
        }

        if (pollCountdown <= 0.0)
        {
            // Only a slice per tick: a full sweep of 32 channels in one burst
            // is the kind of traffic that makes a console drop packets.
            for (int offset = 0; offset < 8; ++offset)
            {
                const auto channel = (pollCursor + offset) % X32State::channelCount;
                sendQuery(socket, "/ch/" + twoDigits(channel + 1) + "/config/name");
                sendQuery(socket, "/ch/" + twoDigits(channel + 1) + "/mix/fader");
                sendQuery(socket, "/ch/" + twoDigits(channel + 1) + "/mix/on");
            }
            for (int offset = 0; offset < 4; ++offset)
            {
                const auto bus = (pollCursor + offset) % X32State::busCount;
                sendQuery(socket, "/bus/" + twoDigits(bus + 1) + "/config/name");
            }
            pollCursor = (pollCursor + 8) % X32State::channelCount;
            pollCountdown = 1.0;
        }

        auto received = false;
        while (socket.waitUntilReady(true, 20) == 1)
        {
            juce::String senderAddress;
            int senderPort = 0;
            const auto size = socket.read(buffer.data(), static_cast<int>(buffer.size()), false,
                                          senderAddress, senderPort);
            if (size <= 0) break;
            if (senderPort != targetPort) continue;

            OscMessage message;
            if (OscMessage::decode(buffer.data(), static_cast<size_t>(size), message))
            {
                handleMessage(message);
                received = true;
            }
            if (threadShouldExit()) break;
        }

        wait(100);
        // Measured, not assumed: draining the socket takes real time on top of
        // the wait, so counting a fixed tick made every timer here run at about
        // half speed - including the /xremote renewal, which the console
        // expires after ten seconds.
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto elapsed = std::clamp((now - lastTick) / 1000.0, 0.0, 1.0);
        lastTick = now;
        subscriptionCountdown -= elapsed;
        pollCountdown -= elapsed;
        secondsSinceReply = received ? 0.0 : secondsSinceReply + elapsed;

        const juce::ScopedLock lock(stateLock);
        state.connected = secondsSinceReply < 5.0;
    }

    const juce::ScopedLock lock(stateLock);
    state.connected = false;
}

bool X32Client::sendQuery(juce::DatagramSocket& socket, const juce::String& address)
{
    const OscMessage message(address);
    if (!isReadOnlyQuery(message))
    {
        rejectedWrites.fetch_add(1, std::memory_order_relaxed);
        jassertfalse;
        return false;
    }

    // The console answers to the source port, so queries must leave through the
    // same socket that is listening for replies.
    const auto data = message.encode();
    return socket.write(host, port.load(std::memory_order_acquire),
                        data.data(), static_cast<int>(data.size())) > 0;
}

void X32Client::handleMessage(const OscMessage& message)
{
    const auto& address = message.getAddress();
    const juce::ScopedLock lock(stateLock);

    if (address == "/info")
    {
        state.consoleName = message.getString(1, state.consoleName);
        state.model = message.getString(2, state.model);
        state.firmware = message.getString(3, state.firmware);
        return;
    }
    if (address == "/status")
    {
        state.consoleName = message.getString(2, state.consoleName);
        return;
    }

    const auto channel = indexFromAddress(address, "/ch/");
    if (channel >= 0 && channel < X32State::channelCount)
    {
        auto& info = state.channels[static_cast<size_t>(channel)];
        if (address.endsWith("/config/name"))
        {
            const auto name = message.getString(0).trim();
            if (!info.valid && name.isNotEmpty()) ++state.namedChannels;
            info.name = name;
            info.valid = info.valid || name.isNotEmpty();
        }
        else if (address.endsWith("/mix/fader"))
        {
            info.faderDb = faderToDb(message.getFloat(0, 0.0f));
            info.valid = true;
        }
        else if (address.endsWith("/mix/on"))
        {
            info.on = message.getInt(0, 0) != 0;
            info.valid = true;
        }
        return;
    }

    const auto bus = indexFromAddress(address, "/bus/");
    if (bus >= 0 && bus < X32State::busCount && address.endsWith("/config/name"))
        state.busNames[static_cast<size_t>(bus)] = message.getString(0).trim();
}
} // namespace churchstream
