#pragma once
//==============================================================================
//  RtpMidiSession.h  -  RTP-MIDI (AppleMIDI network session) over the direct
//                       USB NCM link, for the DYSEKT standalone.
//
//  Purpose: forward DYSEKT's live incoming MIDI to an iPad/iPhone over the
//  wired 192.168.99.x (…102.x) Apple USB link, using the same network-MIDI
//  session protocol iOS apps already speak over Wi-Fi.
//
//  - Protocol: AppleMIDI session (IN/OK/NO/BY/CK) + RFC 6295 RTP-MIDI payload.
//  - DYSEKT is the session INITIATOR by default (it invites the iPad's session
//    at <peer>:5004). It also ACCEPTS an invitation from the configured peer if
//    the iPad app initiates instead (works when ports 5004/5005 could be bound).
//  - Sockets are bound to the local address that sits in the same /24 as the
//    peer (e.g. 192.168.99.1 for 192.168.99.2), so traffic can only use the
//    USB link, never Wi-Fi.
//  - Real-time safe producer side: sendMessage() only filters + pushes to a
//    lock-free FIFO. All socket work happens on the session thread.
//  - Not sent: SysEx, system-common, active sensing. Clock/start/stop/continue
//    only when setForwardRealtime (true).
//
//  Dependencies: juce_core, juce_audio_basics only.
//==============================================================================

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

class RtpMidiSession final : private juce::Thread
{
public:
    enum class State : int
    {
        Off = 0,          // not started
        WaitingForLink,   // started, but the USB link / peer subnet is not present yet
        Inviting,         // link present, negotiating the session with the peer
        Connected         // session established, MIDI is flowing
    };

    RtpMidiSession();
    ~RtpMidiSession() override;

    /** Process-wide session used by the standalone UI and the MIDI input forwarder.
        Created on first use; the destructor stops the session thread at exit. */
    static RtpMidiSession& shared()
    {
        static RtpMidiSession instance;
        return instance;
    }

    //==========================================================================
    //  Control (message thread)
    //==========================================================================

    /** Starts (or restarts) the session toward peerIPv4:peerControlPort.
        peerIPv4 must be a dotted IPv4 address, e.g. "192.168.99.2". */
    void start (const juce::String& peerIPv4, int peerControlPort = 5004);

    /** Stops the session. If connected, sends All Notes Off to the peer and a clean BY first. */
    void stop();

    bool  isRunning() const noexcept { return running.load(); }
    State getState()  const noexcept { return (State) state.load(); }

    juce::String getStatusText() const;     // human-readable, thread-safe copy
    juce::String getPeerName() const;       // session name reported by the peer ("" until known)
    juce::String getPeerAddress() const;    // address passed to start()
    int          getPeerPort() const;       // control port passed to start()
    juce::String getLocalAddress() const;   // local USB-link address in use ("" until bound)

    //==========================================================================
    //  Filters (any thread)
    //==========================================================================

    /** Bit n = MIDI channel n+1 is forwarded. Default 0xFFFF (all). */
    void setChannelMask (uint32_t mask) noexcept   { channelMask.store (mask & 0xFFFFu); }
    uint32_t getChannelMask() const noexcept       { return channelMask.load(); }

    /** Forward MIDI clock (F8), start (FA), continue (FB), stop (FC). Default off. */
    void setForwardRealtime (bool shouldForward) noexcept { forwardRealtime.store (shouldForward); }
    bool getForwardRealtime() const noexcept              { return forwardRealtime.load(); }

    //==========================================================================
    //  Live MIDI in (any thread, including the MIDI input callback thread)
    //==========================================================================

    /** Queues a message for the peer. Cheap, allocation-free, never blocks on the network.
        Silently ignored unless the session is Connected. */
    void sendMessage (const juce::MidiMessage& message) noexcept;

    /** Total messages handed to the network so far (for an activity indicator). */
    uint32_t getMessagesSent() const noexcept { return sentCount.load(); }

    //==========================================================================
    //  Pure helpers - exposed for unit tests
    //==========================================================================
    static bool isValidIPv4 (const juce::String& text);

    /** Builds an RTP-MIDI datagram (RTP header + MIDI command section) into dest.
        commandList is the already delta-encoded command list. Returns bytes written.
        dest must hold at least 12 + 2 + listLength bytes. */
    static int buildRtpMidiPacket (uint8_t* dest, uint16_t sequence, uint32_t timestamp,
                                   uint32_t ssrc, const uint8_t* commandList, int listLength) noexcept;

private:
    struct Slot { uint8_t length = 0; uint8_t data[3] = { 0, 0, 0 }; };

    enum class Role { None, Initiator, Responder };

    void run() override;

    // session thread only -----------------------------------------------------
    void openSockets();
    void closeSockets();
    void resetLink();
    bool localAddressStillPresent() const;
    juce::String findLocalAddress() const;

    void serviceSession (uint32_t nowMs);
    void pumpOutgoing();
    void pumpIncoming();
    void handlePacket (bool onDataSocket, const uint8_t* p, int n,
                       const juce::String& senderIp, int senderPort);

    void handleInvitation (bool onDataSocket, const uint8_t* p, int n, int senderPort);
    void handleOk         (bool onDataSocket, const uint8_t* p, int n);
    void handleClock      (const uint8_t* p, int n);
    void becomeConnected  (Role newRole);
    void closeSession     (bool sayGoodbye);

    void sendInvitation (bool viaData);
    void sendClockSync0();
    void sendBye();
    void sendAllNotesOff();
    void sendRtp (const uint8_t* commandList, int listLength);

    uint64_t now10kHz() const noexcept;
    void setStatus (State newState, const juce::String& text);

    // shared with other threads -----------------------------------------------
    std::atomic<bool>     running { false };
    std::atomic<int>      state { (int) State::Off };
    std::atomic<bool>     connectedFlag { false };
    std::atomic<uint32_t> channelMask { 0xFFFFu };
    std::atomic<bool>     forwardRealtime { false };
    std::atomic<uint32_t> sentCount { 0 };

    mutable juce::CriticalSection textLock;
    juce::String cfgPeer, statusText, peerName, localAddr;
    int          cfgPort = 5004;

    // FIFO: many producers (serialised by producerLock), one consumer (session thread)
    static constexpr int kFifoSize = 1024;
    std::array<Slot, kFifoSize> slots;
    juce::AbstractFifo          fifo { kFifoSize };
    juce::SpinLock              producerLock;

    // session thread state ----------------------------------------------------
    std::unique_ptr<juce::DatagramSocket> ctrl, data;
    juce::String peerIp;
    int          baseCtrlPort = 5004;   // port configured by start()
    int          peerCtrlPort = 5004;   // current control port (may follow the peer when it invites us)
    int          peerDataPort = 5005;

    Role     role = Role::None;
    int      stage = 0;               // initiator: 0 = inviting on control, 1 = inviting on data
    int      inviteTries = 0;
    uint32_t inviteDueMs = 0;
    uint32_t clockDueMs = 0;
    int      clockCount = 0;
    uint32_t lastRxMs = 0;
    uint32_t sessionToken = 0;        // initiator token of the current session
    uint32_t ssrc = 0;
    uint16_t sequence = 0;
    int64_t  startTicks = 0;
    uint32_t lastLinkCheckMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RtpMidiSession)
};
