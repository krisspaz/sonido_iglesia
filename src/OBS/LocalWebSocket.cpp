#include "LocalWebSocket.h"

#include <array>
#include <random>

namespace churchstream
{
LocalWebSocket::LocalWebSocket(Listener& listenerToUse)
    : Thread("CSP OBS WebSocket"), listener(listenerToUse)
{
}

LocalWebSocket::~LocalWebSocket() { stop(); }

void LocalWebSocket::start(juce::String hostName, int portNumber)
{
    host = std::move(hostName);
    port = portNumber;
    if (!isThreadRunning())
        startThread(juce::Thread::Priority::low);
}

void LocalWebSocket::stop()
{
    signalThreadShouldExit();
    closeSocket();
    stopThread(2000);
    const juce::ScopedLock lock(socketLock);
    socket.reset();
}

bool LocalWebSocket::sendText(const juce::String& message)
{
    const auto utf8 = message.toRawUTF8();
    return sendFrame(utf8, static_cast<size_t>(message.getNumBytesAsUTF8()), 0x1);
}

bool LocalWebSocket::isConnected() const noexcept
{
    return connected.load(std::memory_order_acquire);
}

void LocalWebSocket::run()
{
    while (!threadShouldExit())
    {
        if (!connectAndHandshake())
        {
            wait(2500);
            continue;
        }

        connected.store(true, std::memory_order_release);
        listener.localWebSocketConnected();
        juce::String reason = "OBS WebSocket closed";
        juce::MemoryBlock fragmentedMessage;
        auto fragmentedOpcode = 0;

        while (!threadShouldExit() && isConnected())
        {
            juce::MemoryBlock payload;
            int opcode = 0;
            bool finalFrame = true;
            if (!readFrame(payload, opcode, finalFrame))
                break;
            if (opcode == 0x1)
            {
                if (finalFrame)
                    listener.localWebSocketMessage(juce::String::fromUTF8(
                        static_cast<const char*>(payload.getData()), static_cast<int>(payload.getSize())));
                else
                {
                    fragmentedMessage = payload;
                    fragmentedOpcode = opcode;
                }
            }
            else if (opcode == 0x0 && fragmentedOpcode == 0x1)
            {
                fragmentedMessage.append(payload.getData(), payload.getSize());
                if (finalFrame)
                {
                    listener.localWebSocketMessage(juce::String::fromUTF8(
                        static_cast<const char*>(fragmentedMessage.getData()),
                        static_cast<int>(fragmentedMessage.getSize())));
                    fragmentedMessage.reset();
                    fragmentedOpcode = 0;
                }
            }
            else if (opcode == 0x8)
            {
                reason = "OBS WebSocket sent close";
                break;
            }
            else if (opcode == 0x9)
                sendFrame(payload.getData(), payload.getSize(), 0xA);
        }

        connected.store(false, std::memory_order_release);
        closeSocket();
        listener.localWebSocketDisconnected(reason);
        if (!threadShouldExit())
            wait(2500);
    }
}

bool LocalWebSocket::connectAndHandshake()
{
    auto candidate = std::make_unique<juce::StreamingSocket>();
    if (!candidate->connect(host, port, 1200))
        return false;

    std::array<uint8_t, 16> nonce {};
    std::random_device random;
    for (auto& byte : nonce)
        byte = static_cast<uint8_t>(random());
    const auto key = juce::Base64::toBase64(nonce.data(), nonce.size());
    const auto request = "GET / HTTP/1.1\r\nHost: " + host + ":" + juce::String(port)
        + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key
        + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (candidate->write(request.toRawUTF8(), static_cast<int>(request.getNumBytesAsUTF8())) <= 0)
        return false;

    juce::MemoryOutputStream response;
    while (response.getDataSize() < 8192)
    {
        if (candidate->waitUntilReady(true, 1200) <= 0)
            return false;
        char byte = 0;
        if (candidate->read(&byte, 1, true) != 1)
            return false;
        response.writeByte(byte);
        if (response.toString().contains("\r\n\r\n"))
            break;
    }
    if (!response.toString().startsWithIgnoreCase("HTTP/1.1 101"))
        return false;

    const juce::ScopedLock lock(socketLock);
    socket = std::move(candidate);
    return true;
}

bool LocalWebSocket::readFrame(juce::MemoryBlock& payload, int& opcode, bool& finalFrame)
{
    uint8_t header[2] {};
    if (!readExact(header, 2, 1000))
        return false;
    finalFrame = (header[0] & 0x80U) != 0;
    opcode = header[0] & 0x0f;
    const auto masked = (header[1] & 0x80U) != 0;
    uint64_t payloadSize = header[1] & 0x7fU;
    if (payloadSize == 126)
    {
        uint8_t extended[2] {};
        if (!readExact(extended, 2, 1000)) return false;
        payloadSize = (static_cast<uint64_t>(extended[0]) << 8U) | extended[1];
    }
    else if (payloadSize == 127)
    {
        uint8_t extended[8] {};
        if (!readExact(extended, 8, 1000)) return false;
        payloadSize = 0;
        for (auto byte : extended)
            payloadSize = (payloadSize << 8U) | byte;
    }
    if (payloadSize > 2U * 1024U * 1024U)
        return false;

    std::array<uint8_t, 4> mask {};
    if (masked && !readExact(mask.data(), 4, 1000))
        return false;
    payload.setSize(static_cast<size_t>(payloadSize), true);
    if (payloadSize > 0 && !readExact(payload.getData(), static_cast<int>(payloadSize), 2000))
        return false;
    if (masked)
        for (size_t index = 0; index < payload.getSize(); ++index)
            static_cast<uint8_t*>(payload.getData())[index] ^= mask[index & 3U];
    return true;
}

bool LocalWebSocket::sendFrame(const void* data, size_t size, int opcode)
{
    if (!isConnected() && opcode != 0x1)
        return false;
    juce::MemoryOutputStream frame;
    frame.writeByte(static_cast<char>(0x80 | (opcode & 0x0f)));
    if (size < 126)
        frame.writeByte(static_cast<char>(0x80U | static_cast<uint8_t>(size)));
    else if (size <= 0xffffU)
    {
        frame.writeByte(static_cast<char>(0x80U | 126U));
        frame.writeByte(static_cast<char>((size >> 8U) & 0xffU));
        frame.writeByte(static_cast<char>(size & 0xffU));
    }
    else
    {
        frame.writeByte(static_cast<char>(0x80U | 127U));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.writeByte(static_cast<char>((size >> static_cast<unsigned>(shift)) & 0xffU));
    }

    const auto seed = static_cast<uint32_t>(juce::Time::getHighResolutionTicks());
    const std::array<uint8_t, 4> mask { static_cast<uint8_t>(seed), static_cast<uint8_t>(seed >> 8U),
                                        static_cast<uint8_t>(seed >> 16U), static_cast<uint8_t>(seed >> 24U) };
    frame.write(mask.data(), mask.size());
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
        frame.writeByte(static_cast<char>(bytes[index] ^ mask[index & 3U]));

    const juce::ScopedLock lock(socketLock);
    return socket != nullptr
        && socket->write(frame.getData(), static_cast<int>(frame.getDataSize())) == static_cast<int>(frame.getDataSize());
}

bool LocalWebSocket::readExact(void* destination, int bytes, int timeoutMs)
{
    auto* output = static_cast<char*>(destination);
    auto completed = 0;
    while (completed < bytes && !threadShouldExit())
    {
        juce::StreamingSocket* current = nullptr;
        {
            const juce::ScopedLock lock(socketLock);
            current = socket.get();
        }
        if (current == nullptr)
            return false;
        const auto ready = current->waitUntilReady(true, timeoutMs);
        if (ready < 0)
            return false;
        if (ready == 0)
            continue; // A quiet OBS connection is healthy; do not reconnect on idle timeout.
        const auto count = current->read(output + completed, bytes - completed, true);
        if (count <= 0) return false;
        completed += count;
    }
    return completed == bytes;
}

void LocalWebSocket::closeSocket()
{
    const juce::ScopedLock lock(socketLock);
    if (socket != nullptr) socket->close();
    connected.store(false, std::memory_order_release);
}
} // namespace churchstream
