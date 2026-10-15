#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// Minimal DNS-SD/mDNS advertiser for the AppleMIDI service type used by
// macOS Audio MIDI Setup and compatible RTP-MIDI clients. It deliberately
// advertises only while the Wi-Fi/LAN host session is enabled.
class WifiMidiAdvertiser final : private juce::Thread
{
public:
    WifiMidiAdvertiser() : juce::Thread ("Wi-Fi MIDI Bonjour advertiser") {}
    ~WifiMidiAdvertiser() override { stop(); }

    void start (int controlPort)
    {
        stop();
        port.store (juce::jlimit (1, 65534, controlPort));
        startThread();
    }

    void stop()
    {
        signalThreadShouldExit();
        stopThread (1500);
    }

private:
    static void put16 (std::vector<uint8_t>& out, uint16_t value)
    {
        out.push_back (uint8_t (value >> 8));
        out.push_back (uint8_t (value));
    }

    static void put32 (std::vector<uint8_t>& out, uint32_t value)
    {
        put16 (out, uint16_t (value >> 16));
        put16 (out, uint16_t (value));
    }

    static void putName (std::vector<uint8_t>& out, const juce::String& name)
    {
        juce::StringArray labels;
        labels.addTokens (name, ".", "");
        for (const auto& label : labels)
        {
            const auto utf8 = label.toRawUTF8();
            const auto length = (int) std::strlen (utf8);
            if (length <= 0 || length > 63)
                continue;
            out.push_back ((uint8_t) length);
            for (int i = 0; i < length; ++i)
                out.push_back ((uint8_t) utf8[i]);
        }
        out.push_back (0);
    }

    static void recordHeader (std::vector<uint8_t>& out, const juce::String& owner,
                              uint16_t type, uint32_t ttl, uint16_t dataLength, bool cacheFlush = true)
    {
        putName (out, owner);
        put16 (out, type);
        put16 (out, (uint16_t) (0x0001 | (cacheFlush ? 0x8000 : 0))); // IN
        put32 (out, ttl);
        put16 (out, dataLength);
    }

    static void addPtr (std::vector<uint8_t>& out, const juce::String& instance)
    {
        std::vector<uint8_t> rdata;
        putName (rdata, instance);
        recordHeader (out, "_apple-midi._udp.local", 12, 120, (uint16_t) rdata.size(), false);
        out.insert (out.end(), rdata.begin(), rdata.end());
    }

    static void addSrv (std::vector<uint8_t>& out, const juce::String& instance,
                        const juce::String& host, int servicePort)
    {
        std::vector<uint8_t> rdata;
        put16 (rdata, 0); put16 (rdata, 0);
        put16 (rdata, (uint16_t) servicePort);
        putName (rdata, host);
        recordHeader (out, instance, 33, 120, (uint16_t) rdata.size());
        out.insert (out.end(), rdata.begin(), rdata.end());
    }

    static void addTxt (std::vector<uint8_t>& out, const juce::String& instance)
    {
        constexpr char txt[] = "txtvers=1";
        recordHeader (out, instance, 16, 120, (uint16_t) (sizeof (txt) - 1 + 1));
        out.push_back ((uint8_t) (sizeof (txt) - 1));
        for (size_t i = 0; i < sizeof (txt) - 1; ++i)
            out.push_back ((uint8_t) txt[i]);
    }

    static bool parseIPv4 (const juce::String& text, std::array<uint8_t, 4>& bytes)
    {
        juce::StringArray parts;
        parts.addTokens (text, ".", "");
        if (parts.size() != 4)
            return false;
        for (int i = 0; i < 4; ++i)
        {
            if (parts[i].isEmpty() || ! parts[i].containsOnly ("0123456789"))
                return false;
            const int value = parts[i].getIntValue();
            if (value < 0 || value > 255)
                return false;
            bytes[(size_t) i] = (uint8_t) value;
        }
        return true;
    }

    void run() override
    {
        juce::DatagramSocket socket (true);
        socket.setEnablePortReuse (true);
        if (! socket.bindToPort (5353))
            return;
        socket.joinMulticast ("224.0.0.251");

        const juce::String serviceInstance = "DYSEKT-SF._apple-midi._udp.local";
        const juce::String hostName = "dysekt-sf.local";

        while (! threadShouldExit())
        {
            std::vector<std::array<uint8_t, 4>> addresses;
            for (const auto& address : juce::IPAddress::getAllAddresses (false))
            {
                const auto text = address.toString();
                std::array<uint8_t, 4> bytes {};
                // Apple USB Share reserves these subnets; do not advertise its
                // point-to-point addresses as LAN/Wi-Fi destinations.
                const bool appleUsbAddress = text.startsWith ("192.168.99.")
                                          || text.startsWith ("192.168.100.")
                                          || text.startsWith ("192.168.101.")
                                          || text.startsWith ("192.168.102.");
                if (text.startsWith ("127.") || text == "0.0.0.0"
                    || text.startsWith ("169.254.") || appleUsbAddress || ! parseIPv4 (text, bytes))
                    continue;
                if (std::find (addresses.begin(), addresses.end(), bytes) == addresses.end())
                    addresses.push_back (bytes);
            }

            if (! addresses.empty())
            {
                std::vector<uint8_t> packet;
                packet.reserve (512);
                put16 (packet, 0);       // transaction ID
                put16 (packet, 0x8400);  // authoritative unsolicited response
                put16 (packet, 0);       // questions
                put16 (packet, 1);       // answers: PTR
                put16 (packet, 0);       // authority
                put16 (packet, (uint16_t) (2 + addresses.size())); // SRV, TXT, A records
                addPtr (packet, serviceInstance);
                addSrv (packet, serviceInstance, hostName, port.load());
                addTxt (packet, serviceInstance);
                for (const auto& address : addresses)
                {
                    recordHeader (packet, hostName, 1, 120, 4);
                    packet.insert (packet.end(), address.begin(), address.end());
                }
                socket.write ("224.0.0.251", 5353, packet.data(), (int) packet.size());
            }

            // Refresh before the DNS-SD TTL expires, and answer browse queries
            // promptly instead of making clients wait for the next announcement.
            for (int i = 0; i < 150 && ! threadShouldExit(); ++i)
            {
                if (socket.waitUntilReady (true, 100) != 1)
                    continue;

                uint8_t query[1500];
                juce::String senderIp;
                int senderPort = 0;
                const int n = socket.read (query, (int) sizeof (query), false, senderIp, senderPort);

                // DNS responses have QR=1; ignore our own multicast announcement
                // and any other responses to avoid an announcement loop.
                if (n >= 12 && (query[2] & 0x80) == 0)
                    break; // rebuild and send the service announcement immediately
            }
        }
    }

    std::atomic<int> port { 5004 };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WifiMidiAdvertiser)
};
