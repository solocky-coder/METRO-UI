#include "RtpMidiSession.h"

#include <cstring>

namespace
{
    constexpr uint32_t kProtocolVersion = 2;
    constexpr uint32_t kInviteRetryMs   = 1000;
    constexpr uint32_t kFastClockMs     = 500;      // first few clock syncs
    constexpr uint32_t kSlowClockMs     = 10000;    // steady-state clock sync
    constexpr uint32_t kTimeoutMs       = 35000;    // peer silent for this long => session lost
    constexpr int      kMaxList         = 120;      // MIDI command-list bytes per RTP packet
    constexpr int      kMaxPacket       = 1500;

    constexpr uint16_t kIN = 0x494E, kOK = 0x4F4B, kNO = 0x4E4F,
                       kBY = 0x4259, kCK = 0x434B, kRS = 0x5253;

    inline void put16 (uint8_t* p, uint32_t v) noexcept { p[0] = uint8_t (v >> 8); p[1] = uint8_t (v); }
    inline void put32 (uint8_t* p, uint32_t v) noexcept { put16 (p, v >> 16); put16 (p + 2, v & 0xFFFFu); }
    inline void put64 (uint8_t* p, uint64_t v) noexcept { put32 (p, uint32_t (v >> 32)); put32 (p + 4, uint32_t (v)); }

    inline uint32_t get32 (const uint8_t* p) noexcept
    {
        return (uint32_t (p[0]) << 24) | (uint32_t (p[1]) << 16) | (uint32_t (p[2]) << 8) | uint32_t (p[3]);
    }

    inline uint64_t get64 (const uint8_t* p) noexcept
    {
        return (uint64_t (get32 (p)) << 32) | uint64_t (get32 (p + 4));
    }

    inline bool timeReached (uint32_t now, uint32_t due) noexcept
    {
        return int32_t (now - due) >= 0;
    }

    uint32_t randomNonZero()
    {
        uint32_t v = 0;
        while (v == 0)
            v = (uint32_t) juce::Random::getSystemRandom().nextInt();
        return v;
    }

    /** Reads a NUL-terminated UTF-8 name that starts at p[offset] inside an n-byte packet. */
    juce::String readName (const uint8_t* p, int n, int offset)
    {
        if (n <= offset)
            return {};

        int len = 0;
        while (offset + len < n && p[offset + len] != 0)
            ++len;

        return juce::String::fromUTF8 (reinterpret_cast<const char*> (p + offset), len);
    }

    const char* const kSessionName = "DYSEKT";
}

//==============================================================================
RtpMidiSession::RtpMidiSession()
    : juce::Thread ("RTP-MIDI session")
{
}

RtpMidiSession::~RtpMidiSession()
{
    stop();
}

//==============================================================================
//  Helpers
//==============================================================================
bool RtpMidiSession::isValidIPv4 (const juce::String& text)
{
    juce::StringArray parts;
    parts.addTokens (text.trim(), ".", "");

    if (parts.size() != 4)
        return false;

    for (const auto& part : parts)
    {
        if (part.isEmpty() || part.length() > 3 || ! part.containsOnly ("0123456789"))
            return false;

        if (part.getIntValue() > 255)
            return false;
    }

    return true;
}

int RtpMidiSession::buildRtpMidiPacket (uint8_t* dest, uint16_t seq, uint32_t timestamp,
                                        uint32_t ssrcValue, const uint8_t* commandList, int listLength) noexcept
{
    dest[0] = 0x80;     // V=2, P=0, X=0, CC=0
    dest[1] = 0x61;     // M=0, payload type 97 (dynamic, as used by Apple's RTP-MIDI)
    put16 (dest + 2, seq);
    put32 (dest + 4, timestamp);
    put32 (dest + 8, ssrcValue);

    int pos = 12;

    // MIDI command section header: B (long form) | J=0 | Z=0 | P=0 | LEN
    if (listLength <= 15)
    {
        dest[pos++] = uint8_t (listLength);
    }
    else
    {
        dest[pos++] = uint8_t (0x80 | ((listLength >> 8) & 0x0F));
        dest[pos++] = uint8_t (listLength & 0xFF);
    }

    std::memcpy (dest + pos, commandList, (size_t) listLength);
    return pos + listLength;
}

uint64_t RtpMidiSession::now10kHz() const noexcept
{
    const auto elapsed = juce::Time::getHighResolutionTicks() - startTicks;
    return (uint64_t) (juce::Time::highResolutionTicksToSeconds (elapsed) * 10000.0);
}

void RtpMidiSession::setStatus (State newState, const juce::String& text)
{
    {
        const juce::ScopedLock sl (textLock);
        statusText = text;
    }
    state.store ((int) newState);
}

juce::String RtpMidiSession::getStatusText() const   { const juce::ScopedLock sl (textLock); return statusText; }
juce::String RtpMidiSession::getPeerName() const     { const juce::ScopedLock sl (textLock); return peerName; }
juce::String RtpMidiSession::getPeerAddress() const  { const juce::ScopedLock sl (textLock); return cfgPeer; }
int          RtpMidiSession::getPeerPort() const     { const juce::ScopedLock sl (textLock); return cfgPort; }
juce::String RtpMidiSession::getLocalAddress() const { const juce::ScopedLock sl (textLock); return localAddr; }

//==============================================================================
//  Control
//==============================================================================
void RtpMidiSession::start (const juce::String& peerIPv4, int peerControlPort)
{
    stop();

    {
        const juce::ScopedLock sl (textLock);
        cfgPeer = peerIPv4.trim();
        cfgPort = juce::jlimit (1, 65534, peerControlPort);
        peerName.clear();
        localAddr.clear();
    }

    setStatus (State::WaitingForLink, "Starting...");
    running.store (true);
    startThread();
}

void RtpMidiSession::stop()
{
    if (running.exchange (false))
        stopThread (3000);

    connectedFlag.store (false);
    setStatus (State::Off, "Off");
}

//==============================================================================
//  Producer side (any thread)
//==============================================================================
void RtpMidiSession::sendMessage (const juce::MidiMessage& message) noexcept
{
    if (! connectedFlag.load (std::memory_order_relaxed))
        return;

    const int size = message.getRawDataSize();
    if (size < 1 || size > 3)
        return;                                   // SysEx etc. are not forwarded

    const uint8_t* raw = message.getRawData();
    const uint8_t status = raw[0];

    if (status >= 0xF0)
    {
        const bool isRealtime = (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC);
        if (! isRealtime || ! forwardRealtime.load (std::memory_order_relaxed))
            return;                               // system common / active sensing are dropped
    }
    else
    {
        if (status < 0x80)
            return;

        if ((channelMask.load (std::memory_order_relaxed) & (1u << (status & 0x0F))) == 0)
            return;
    }

    const juce::SpinLock::ScopedLockType sl (producerLock);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 + size2 < 1)
        return;                                   // FIFO full: drop rather than block

    Slot& slot = slots[(size_t) (size1 > 0 ? start1 : start2)];
    slot.length = (uint8_t) size;
    for (int i = 0; i < size; ++i)
        slot.data[i] = raw[i];

    fifo.finishedWrite (1);
}

//==============================================================================
//  Session thread
//==============================================================================
juce::String RtpMidiSession::findLocalAddress() const
{
    const auto prefix = peerIp.upToLastOccurrenceOf (".", true, false);   // "192.168.99."

    for (const auto& address : juce::IPAddress::getAllAddresses (false))
    {
        const auto text = address.toString();
        if (text.startsWith (prefix) && text != peerIp)
            return text;
    }

    return {};
}

bool RtpMidiSession::localAddressStillPresent() const
{
    const auto current = getLocalAddress();

    for (const auto& address : juce::IPAddress::getAllAddresses (false))
        if (address.toString() == current)
            return true;

    return false;
}

void RtpMidiSession::resetLink()
{
    role = Role::None;
    connectedFlag.store (false);
    stage = 0;
    inviteTries = 0;
    inviteDueMs = juce::Time::getMillisecondCounter();
    clockCount = 0;
    sessionToken = randomNonZero();
    peerCtrlPort = baseCtrlPort;
    peerDataPort = baseCtrlPort + 1;

    const juce::ScopedLock sl (textLock);
    peerName.clear();
}

void RtpMidiSession::closeSockets()
{
    ctrl.reset();
    data.reset();

    const juce::ScopedLock sl (textLock);
    localAddr.clear();
}

void RtpMidiSession::openSockets()
{
    const auto local = findLocalAddress();

    if (local.isEmpty())
    {
        setStatus (State::WaitingForLink, "Waiting for USB link to " + peerIp);
        return;
    }

    auto tryBind = [&] (int controlPort) -> bool
    {
        auto c = std::make_unique<juce::DatagramSocket> (false);
        if (! c->bindToPort (controlPort, local))
            return false;

        auto d = std::make_unique<juce::DatagramSocket> (false);
        if (! d->bindToPort (c->getBoundPort() + 1, local))
            return false;

        ctrl = std::move (c);
        data = std::move (d);
        return true;
    };

    // Prefer the standard 5004/5005 pair (so a peer that initiates can find us),
    // otherwise fall back to any free consecutive pair - still fine for initiating.
    bool ok = tryBind (5004);
    for (int attempt = 0; attempt < 25 && ! ok; ++attempt)
        ok = tryBind (0);

    if (! ok)
    {
        setStatus (State::WaitingForLink, "Could not open UDP ports on " + local);
        return;
    }

    {
        const juce::ScopedLock sl (textLock);
        localAddr = local;
    }

    resetLink();
    setStatus (State::Inviting, "Connecting to " + peerIp + ":" + juce::String (baseCtrlPort) + " ...");
}

void RtpMidiSession::run()
{
    {
        const juce::ScopedLock sl (textLock);
        peerIp = cfgPeer;
        baseCtrlPort = cfgPort;
    }

    peerCtrlPort = baseCtrlPort;
    peerDataPort = baseCtrlPort + 1;
    startTicks   = juce::Time::getHighResolutionTicks();
    ssrc         = randomNonZero();
    sequence     = (uint16_t) juce::Random::getSystemRandom().nextInt (65536);
    role         = Role::None;

    if (! isValidIPv4 (peerIp))
        setStatus (State::WaitingForLink, "Enter a valid IPv4 address for the device (e.g. 192.168.99.2)");

    uint32_t lastBindTry = 0;
    bool firstBind = true;
    lastLinkCheckMs = juce::Time::getMillisecondCounter();

    while (! threadShouldExit())
    {
        const uint32_t now = juce::Time::getMillisecondCounter();

        if (! isValidIPv4 (peerIp))
        {
            wait (250);
            continue;
        }

        if (ctrl == nullptr)
        {
            if (firstBind || now - lastBindTry >= 1000)
            {
                firstBind = false;
                lastBindTry = now;
                openSockets();
            }

            if (ctrl == nullptr)
            {
                wait (50);
                continue;
            }
        }
        else if (now - lastLinkCheckMs >= 2000)
        {
            lastLinkCheckMs = now;

            if (! localAddressStillPresent())
            {
                closeSession (false);
                setStatus (State::WaitingForLink, "USB link down - waiting for " + peerIp);
                continue;
            }
        }

        serviceSession (now);
        pumpOutgoing();
        pumpIncoming();
    }

    closeSession (true);
}

//------------------------------------------------------------------------------
void RtpMidiSession::serviceSession (uint32_t nowMs)
{
    if (role == Role::None)
    {
        if (timeReached (nowMs, inviteDueMs))
        {
            // Restart the whole handshake now and then so a half-open attempt can't wedge.
            if (inviteTries > 0 && inviteTries % 8 == 0)
            {
                stage = 0;
                sessionToken = randomNonZero();
            }

            inviteDueMs = nowMs + kInviteRetryMs;
            ++inviteTries;
            sendInvitation (stage == 1);

            if (inviteTries == 3)
                setStatus (State::Inviting, "No reply from " + peerIp + ":" + juce::String (baseCtrlPort)
                                             + " - turn on a network MIDI session in the iPad app");
        }

        return;
    }

    if (role == Role::Initiator && timeReached (nowMs, clockDueMs))
    {
        sendClockSync0();
        ++clockCount;
        clockDueMs = nowMs + (clockCount < 4 ? kFastClockMs : kSlowClockMs);
    }

    if (nowMs - lastRxMs > kTimeoutMs)
    {
        resetLink();
        setStatus (State::Inviting, "Peer stopped responding - reconnecting to " + peerIp + " ...");
    }
}

void RtpMidiSession::pumpOutgoing()
{
    const int ready = fifo.getNumReady();
    if (ready <= 0)
        return;

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToRead (ready, start1, size1, start2, size2);

    if (connectedFlag.load())
    {
        uint8_t list[kMaxList + 8];
        int len = 0, count = 0;

        auto consume = [&] (const Slot& s)
        {
            const int needed = (count > 0 ? 1 : 0) + (int) s.length;

            if (len + needed > kMaxList)
            {
                sendRtp (list, len);
                len = 0;
                count = 0;
            }

            if (count > 0)
                list[len++] = 0;                       // delta time 0: play immediately

            std::memcpy (list + len, s.data, s.length);
            len += s.length;
            ++count;
            sentCount.fetch_add (1, std::memory_order_relaxed);
        };

        for (int i = 0; i < size1; ++i) consume (slots[(size_t) (start1 + i)]);
        for (int i = 0; i < size2; ++i) consume (slots[(size_t) (start2 + i)]);

        if (len > 0)
            sendRtp (list, len);
    }
    // else: not connected - the queued messages are stale, discard them

    fifo.finishedRead (size1 + size2);
}

void RtpMidiSession::pumpIncoming()
{
    uint8_t buffer[kMaxPacket];
    juce::String senderIp;
    int senderPort = 0;

    for (int pass = 0; pass < 2; ++pass)
    {
        juce::DatagramSocket* socket = (pass == 0 ? ctrl : data).get();
        if (socket == nullptr)
            return;

        int timeoutMs = (pass == 1 ? 1 : 0);          // the 1 ms wait is the loop's idle sleep

        for (int guard = 0; guard < 32 && socket->waitUntilReady (true, timeoutMs) == 1; ++guard)
        {
            const int n = socket->read (buffer, (int) sizeof (buffer), false, senderIp, senderPort);
            if (n <= 0)
                break;

            handlePacket (pass == 1, buffer, n, senderIp, senderPort);
            timeoutMs = 0;
        }
    }
}

void RtpMidiSession::handlePacket (bool onDataSocket, const uint8_t* p, int n,
                                   const juce::String& senderIp, int senderPort)
{
    if (senderIp != peerIp)
        return;                                       // only ever talk to the configured device

    lastRxMs = juce::Time::getMillisecondCounter();

    if (n >= 4 && p[0] == 0xFF && p[1] == 0xFF)
    {
        const uint16_t command = (uint16_t) ((p[2] << 8) | p[3]);

        switch (command)
        {
            case kIN:
                handleInvitation (onDataSocket, p, n, senderPort);
                break;

            case kOK:
                handleOk (onDataSocket, p, n);
                break;

            case kNO:
                if (role == Role::None && n >= 12 && get32 (p + 8) == sessionToken)
                {
                    stage = 0;
                    sessionToken = randomNonZero();
                    inviteDueMs = lastRxMs + 3000;
                    setStatus (State::Inviting, "The device refused the session - allow DYSEKT in the app's network MIDI settings");
                }
                break;

            case kBY:
                if (role != Role::None)
                {
                    resetLink();
                    inviteDueMs = lastRxMs + 1000;
                    setStatus (State::Inviting, "The device ended the session - reconnecting ...");
                }
                break;

            case kCK:
                if (onDataSocket && role != Role::None)
                    handleClock (p, n);
                break;

            case kRS:
            default:
                break;                                // receiver feedback: not needed without a journal
        }
    }
    // RTP data packets from the peer (it sending MIDI to us) are not used yet.
}

void RtpMidiSession::handleInvitation (bool onDataSocket, const uint8_t* p, int n, int senderPort)
{
    if (n < 16)
        return;

    const uint32_t token = get32 (p + 8);
    const auto name = readName (p, n, 16);

    uint8_t reply[64];
    put16 (reply, 0xFFFF);
    put16 (reply + 2, kOK);
    put32 (reply + 4, kProtocolVersion);
    put32 (reply + 8, token);
    put32 (reply + 12, ssrc);
    const int nameLen = (int) std::strlen (kSessionName) + 1;
    std::memcpy (reply + 16, kSessionName, (size_t) nameLen);

    if (! onDataSocket)
    {
        // The peer is initiating: follow its ports and drop any attempt of our own.
        resetLink();
        sessionToken = token;
        peerCtrlPort = senderPort;
        peerDataPort = senderPort + 1;

        {
            const juce::ScopedLock sl (textLock);
            peerName = name;
        }

        ctrl->write (peerIp, senderPort, reply, 16 + nameLen);
        setStatus (State::Inviting, "Invitation from " + (name.isNotEmpty() ? name : peerIp) + " ...");
    }
    else
    {
        if (token != sessionToken)
            return;

        peerDataPort = senderPort;
        data->write (peerIp, senderPort, reply, 16 + nameLen);
        becomeConnected (Role::Responder);
    }
}

void RtpMidiSession::handleOk (bool onDataSocket, const uint8_t* p, int n)
{
    if (n < 16 || role != Role::None || get32 (p + 8) != sessionToken)
        return;

    const auto name = readName (p, n, 16);

    if (name.isNotEmpty())
    {
        const juce::ScopedLock sl (textLock);
        peerName = name;
    }

    if (! onDataSocket && stage == 0)
    {
        stage = 1;                                    // control accepted: now open the data port
        inviteTries = 0;
        inviteDueMs = juce::Time::getMillisecondCounter();
    }
    else if (onDataSocket && stage == 1)
    {
        becomeConnected (Role::Initiator);
    }
}

void RtpMidiSession::handleClock (const uint8_t* p, int n)
{
    if (n < 36)
        return;

    const uint8_t count = p[8];
    uint8_t reply[36];
    std::memset (reply, 0, sizeof (reply));
    put16 (reply, 0xFFFF);
    put16 (reply + 2, kCK);
    put32 (reply + 4, ssrc);

    if (count == 0)                                   // peer's CK0 -> answer CK1
    {
        reply[8] = 1;
        put64 (reply + 12, get64 (p + 12));
        put64 (reply + 20, now10kHz());
    }
    else if (count == 1)                              // answer to our CK0 -> send CK2
    {
        reply[8] = 2;
        put64 (reply + 12, get64 (p + 12));
        put64 (reply + 20, get64 (p + 20));
        put64 (reply + 28, now10kHz());
    }
    else
    {
        return;
    }

    data->write (peerIp, peerDataPort, reply, (int) sizeof (reply));
}

void RtpMidiSession::becomeConnected (Role newRole)
{
    role = newRole;
    connectedFlag.store (true);

    const auto now = juce::Time::getMillisecondCounter();
    lastRxMs = now;
    clockCount = 0;
    clockDueMs = now;                                 // initiator sends CK0 right away

    // Drop anything queued before the session existed.
    const int stale = fifo.getNumReady();
    if (stale > 0)
    {
        int a, b, c, d;
        fifo.prepareToRead (stale, a, b, c, d);
        fifo.finishedRead (b + d);
    }

    const auto name = getPeerName();
    setStatus (State::Connected, "Connected to " + (name.isNotEmpty() ? name : peerIp)
                                  + (newRole == Role::Responder ? " (device-initiated)" : ""));
}

void RtpMidiSession::closeSession (bool sayGoodbye)
{
    if (sayGoodbye && role != Role::None && ctrl != nullptr && data != nullptr)
    {
        sendAllNotesOff();
        sendBye();
    }

    role = Role::None;
    connectedFlag.store (false);
    closeSockets();
}

//------------------------------------------------------------------------------
//  Outgoing packets
//------------------------------------------------------------------------------
void RtpMidiSession::sendInvitation (bool viaData)
{
    uint8_t buf[64];
    put16 (buf, 0xFFFF);
    put16 (buf + 2, kIN);
    put32 (buf + 4, kProtocolVersion);
    put32 (buf + 8, sessionToken);
    put32 (buf + 12, ssrc);
    const int nameLen = (int) std::strlen (kSessionName) + 1;
    std::memcpy (buf + 16, kSessionName, (size_t) nameLen);

    if (viaData)
        data->write (peerIp, baseCtrlPort + 1, buf, 16 + nameLen);
    else
        ctrl->write (peerIp, baseCtrlPort, buf, 16 + nameLen);
}

void RtpMidiSession::sendClockSync0()
{
    uint8_t buf[36];
    std::memset (buf, 0, sizeof (buf));
    put16 (buf, 0xFFFF);
    put16 (buf + 2, kCK);
    put32 (buf + 4, ssrc);
    buf[8] = 0;
    put64 (buf + 12, now10kHz());
    data->write (peerIp, peerDataPort, buf, (int) sizeof (buf));
}

void RtpMidiSession::sendBye()
{
    uint8_t buf[16];
    put16 (buf, 0xFFFF);
    put16 (buf + 2, kBY);
    put32 (buf + 4, kProtocolVersion);
    put32 (buf + 8, sessionToken);
    put32 (buf + 12, ssrc);

    ctrl->write (peerIp, peerCtrlPort, buf, (int) sizeof (buf));
    data->write (peerIp, peerDataPort, buf, (int) sizeof (buf));
}

void RtpMidiSession::sendAllNotesOff()
{
    uint8_t list[kMaxList];
    int len = 0;
    const uint32_t mask = channelMask.load();

    for (int channel = 0; channel < 16; ++channel)
    {
        if ((mask & (1u << channel)) == 0)
            continue;

        if (len > 0)
            list[len++] = 0;

        list[len++] = uint8_t (0xB0 | channel);       // control change
        list[len++] = 123;                            // all notes off
        list[len++] = 0;
    }

    if (len > 0)
        sendRtp (list, len);
}

void RtpMidiSession::sendRtp (const uint8_t* commandList, int listLength)
{
    if (listLength <= 0 || listLength > kMaxList + 8 || data == nullptr)
        return;

    uint8_t packet[12 + 2 + kMaxList + 8];
    const int size = buildRtpMidiPacket (packet, sequence++, (uint32_t) (now10kHz() & 0xFFFFFFFFu),
                                         ssrc, commandList, listLength);

    data->write (peerIp, peerDataPort, packet, size);
}
