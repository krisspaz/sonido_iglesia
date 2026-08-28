#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <juce_core/juce_core.h>
#include <vector>

namespace churchstream
{
// Minimal OSC 1.0 encoder/decoder covering exactly what the X32 remote
// protocol uses: int32, float32 and OSC-strings, every field padded to a
// four byte boundary and written big-endian.
struct OscArgument
{
    enum class Type : char { int32 = 'i', float32 = 'f', string = 's', blob = 'b' };

    Type type = Type::int32;
    int intValue = 0;
    float floatValue = 0.0f;
    juce::String stringValue;
};

class OscMessage final
{
public:
    OscMessage() = default;
    explicit OscMessage(juce::String addressToUse) : address(std::move(addressToUse)) {}

    [[nodiscard]] const juce::String& getAddress() const noexcept { return address; }
    [[nodiscard]] int getArgumentCount() const noexcept { return static_cast<int>(arguments.size()); }
    [[nodiscard]] const OscArgument& getArgument(int index) const { return arguments[static_cast<size_t>(index)]; }

    [[nodiscard]] int getInt(int index, int fallback = 0) const
    {
        if (index < 0 || index >= getArgumentCount()) return fallback;
        const auto& argument = arguments[static_cast<size_t>(index)];
        return argument.type == OscArgument::Type::int32 ? argument.intValue
             : argument.type == OscArgument::Type::float32 ? static_cast<int>(argument.floatValue)
             : fallback;
    }

    [[nodiscard]] float getFloat(int index, float fallback = 0.0f) const
    {
        if (index < 0 || index >= getArgumentCount()) return fallback;
        const auto& argument = arguments[static_cast<size_t>(index)];
        return argument.type == OscArgument::Type::float32 ? argument.floatValue
             : argument.type == OscArgument::Type::int32 ? static_cast<float>(argument.intValue)
             : fallback;
    }

    [[nodiscard]] juce::String getString(int index, juce::String fallback = {}) const
    {
        if (index < 0 || index >= getArgumentCount()) return fallback;
        const auto& argument = arguments[static_cast<size_t>(index)];
        return argument.type == OscArgument::Type::string ? argument.stringValue : fallback;
    }

    OscMessage& addInt(int value)
    {
        OscArgument argument;
        argument.type = OscArgument::Type::int32;
        argument.intValue = value;
        arguments.push_back(argument);
        return *this;
    }

    OscMessage& addFloat(float value)
    {
        OscArgument argument;
        argument.type = OscArgument::Type::float32;
        argument.floatValue = value;
        arguments.push_back(argument);
        return *this;
    }

    OscMessage& addString(juce::String value)
    {
        OscArgument argument;
        argument.type = OscArgument::Type::string;
        argument.stringValue = std::move(value);
        arguments.push_back(argument);
        return *this;
    }

    [[nodiscard]] std::vector<uint8_t> encode() const
    {
        std::vector<uint8_t> data;
        appendPaddedString(data, address);

        juce::String tags = ",";
        for (const auto& argument : arguments)
            tags += static_cast<juce::juce_wchar>(static_cast<char>(argument.type));
        appendPaddedString(data, tags);

        for (const auto& argument : arguments)
        {
            switch (argument.type)
            {
                case OscArgument::Type::int32: appendInt(data, argument.intValue); break;
                case OscArgument::Type::float32: appendFloat(data, argument.floatValue); break;
                case OscArgument::Type::string: appendPaddedString(data, argument.stringValue); break;
                case OscArgument::Type::blob: appendInt(data, 0); break;
            }
        }
        return data;
    }

    // Returns false on any malformed packet instead of reading past the end.
    // The console is on the same LAN as the streaming PC, so a truncated or
    // hostile datagram must never be able to walk memory.
    [[nodiscard]] static bool decode(const uint8_t* data, size_t size, OscMessage& destination)
    {
        destination = {};
        size_t position = 0;
        juce::String parsedAddress;
        if (!readPaddedString(data, size, position, parsedAddress)) return false;
        if (!parsedAddress.startsWithChar('/')) return false;
        destination.address = parsedAddress;

        juce::String tags;
        if (!readPaddedString(data, size, position, tags))
            return true; // Address-only packets are legal and used as queries.
        if (!tags.startsWithChar(',')) return false;

        for (int index = 1; index < tags.length(); ++index)
        {
            OscArgument argument;
            switch (tags[index])
            {
                case 'i':
                    argument.type = OscArgument::Type::int32;
                    if (!readInt(data, size, position, argument.intValue)) return false;
                    break;
                case 'f':
                    argument.type = OscArgument::Type::float32;
                    if (!readFloat(data, size, position, argument.floatValue)) return false;
                    break;
                case 's':
                    argument.type = OscArgument::Type::string;
                    if (!readPaddedString(data, size, position, argument.stringValue)) return false;
                    break;
                case 'b':
                {
                    argument.type = OscArgument::Type::blob;
                    int length = 0;
                    if (!readInt(data, size, position, length) || length < 0) return false;
                    const auto padded = static_cast<size_t>((length + 3) & ~3);
                    if (position + padded > size) return false;
                    position += padded;
                    break;
                }
                default:
                    return false;
            }
            destination.arguments.push_back(argument);
        }
        return true;
    }

private:
    static void appendInt(std::vector<uint8_t>& data, int value)
    {
        const auto unsignedValue = static_cast<uint32_t>(value);
        data.push_back(static_cast<uint8_t>((unsignedValue >> 24) & 0xFF));
        data.push_back(static_cast<uint8_t>((unsignedValue >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((unsignedValue >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>(unsignedValue & 0xFF));
    }

    static void appendFloat(std::vector<uint8_t>& data, float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendInt(data, static_cast<int>(bits));
    }

    static void appendPaddedString(std::vector<uint8_t>& data, const juce::String& value)
    {
        const auto utf8 = value.toRawUTF8();
        const auto length = std::strlen(utf8);
        data.insert(data.end(), utf8, utf8 + length);
        const auto padded = (length + 4) & ~static_cast<size_t>(3);
        data.insert(data.end(), padded - length, 0);
    }

    static bool readInt(const uint8_t* data, size_t size, size_t& position, int& value)
    {
        if (position + 4 > size) return false;
        value = static_cast<int>((static_cast<uint32_t>(data[position]) << 24)
                                 | (static_cast<uint32_t>(data[position + 1]) << 16)
                                 | (static_cast<uint32_t>(data[position + 2]) << 8)
                                 | static_cast<uint32_t>(data[position + 3]));
        position += 4;
        return true;
    }

    static bool readFloat(const uint8_t* data, size_t size, size_t& position, float& value)
    {
        int bits = 0;
        if (!readInt(data, size, position, bits)) return false;
        const auto unsignedBits = static_cast<uint32_t>(bits);
        std::memcpy(&value, &unsignedBits, sizeof(value));
        return true;
    }

    static bool readPaddedString(const uint8_t* data, size_t size, size_t& position,
                                 juce::String& value)
    {
        if (position >= size) return false;
        auto end = position;
        while (end < size && data[end] != 0) ++end;
        if (end >= size) return false;
        value = juce::String::fromUTF8(reinterpret_cast<const char*>(data + position),
                                       static_cast<int>(end - position));
        const auto length = end - position;
        position += (length + 4) & ~static_cast<size_t>(3);
        return position <= size;
    }

    juce::String address;
    std::vector<OscArgument> arguments;
};
} // namespace churchstream
