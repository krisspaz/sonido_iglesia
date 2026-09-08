#pragma once

#include <functional>
#include <juce_core/juce_core.h>

namespace churchstream
{
class LocalWebSocket final : private juce::Thread
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void localWebSocketConnected() = 0;
        virtual void localWebSocketDisconnected(const juce::String& reason) = 0;
        virtual void localWebSocketMessage(const juce::String& message) = 0;
    };

    explicit LocalWebSocket(Listener& listenerToUse);
    ~LocalWebSocket() override;

    void start(juce::String hostName = "127.0.0.1", int portNumber = 4455);
    void stop();
    bool sendText(const juce::String& message);
    [[nodiscard]] bool isConnected() const noexcept;

private:
    void run() override;
    bool connectAndHandshake();
    bool readFrame(juce::MemoryBlock& payload, int& opcode, bool& finalFrame);
    bool sendFrame(const void* data, size_t size, int opcode);
    bool readExact(void* destination, int bytes, int timeoutMs);
    void closeSocket();

    Listener& listener;
    juce::String host { "127.0.0.1" };
    int port = 4455;
    std::unique_ptr<juce::StreamingSocket> socket;
    mutable juce::CriticalSection socketLock;
    std::atomic<bool> connected { false };
};
} // namespace churchstream
