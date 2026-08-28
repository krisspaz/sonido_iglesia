#include "WindowsAudioEndpoint.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <functiondiscoverykeys_devpkey.h>
 #include <mmdeviceapi.h>
 #include <propvarutil.h>
#endif

namespace churchstream
{
juce::String WindowsAudioEndpoint::findCaptureEndpointId(const juce::String& friendlyName)
{
#if JUCE_WINDOWS
    const auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    juce::String result;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))
        && SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)))
    {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT index = 0; index < count && result.isEmpty(); ++index)
        {
            IMMDevice* device = nullptr;
            IPropertyStore* properties = nullptr;
            if (SUCCEEDED(collection->Item(index, &device))
                && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
            {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name))
                    && name.vt == VT_LPWSTR
                    && juce::String(name.pwszVal).containsIgnoreCase(friendlyName))
                {
                    LPWSTR identifier = nullptr;
                    if (SUCCEEDED(device->GetId(&identifier)))
                    {
                        result = juce::String(identifier);
                        CoTaskMemFree(identifier);
                    }
                }
                PropVariantClear(&name);
            }
            if (properties != nullptr) properties->Release();
            if (device != nullptr) device->Release();
        }
    }
    if (collection != nullptr) collection->Release();
    if (enumerator != nullptr) enumerator->Release();
    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
#else
    juce::ignoreUnused(friendlyName);
    return {};
#endif
}
} // namespace churchstream

