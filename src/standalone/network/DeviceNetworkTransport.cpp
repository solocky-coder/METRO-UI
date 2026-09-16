#include "DeviceNetworkTransport.h"

juce::String DeviceNetworkTransport::kindName (Kind kind)
{
    switch (kind)
    {
        case Kind::WifiLan:    return "Wi-Fi / LAN";
        case Kind::AppleUsb:  return "USB device";
        case Kind::AndroidUsb:return "USB device";
        default:              return "Unknown";
    }
}
