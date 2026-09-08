#pragma once

#include <juce_cryptography/juce_cryptography.h>

namespace churchstream
{
struct OBSAuthentication final
{
    static juce::String create(const juce::String& password,
                               const juce::String& salt,
                               const juce::String& challenge)
    {
        // Keep the owning String alive while JUCE reads its UTF-8 buffer.
        const juce::String salted = password + salt;
        const juce::SHA256 secretHash(salted.toUTF8());
        const auto secretRaw = secretHash.getRawData();
        const auto secret = juce::Base64::toBase64(secretRaw.getData(), secretRaw.getSize());
        const juce::String challenged = secret + challenge;
        const juce::SHA256 authenticationHash(challenged.toUTF8());
        const auto authenticationRaw = authenticationHash.getRawData();
        return juce::Base64::toBase64(authenticationRaw.getData(), authenticationRaw.getSize());
    }
};
} // namespace churchstream
