#include "AppleUsbNetworkTransport.h"

#if JUCE_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")
#endif


static bool appleUsbDevicePresent()
{
#if JUCE_WINDOWS
    const auto info = SetupDiGetClassDevsA (nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    for (DWORD i = 0; ; ++i)
    {
        SP_DEVINFO_DATA data {};
        data.cbSize = sizeof (data);
        if (! SetupDiEnumDeviceInfo (info, i, &data))
            break;

        char id[512] {};
        if (SetupDiGetDeviceInstanceIdA (info, &data, id, static_cast<DWORD> (sizeof (id)), nullptr))
        {
            std::string value = id;
            std::transform (value.begin(), value.end(), value.begin(),
                            [] (unsigned char ch) { return static_cast<char> (std::tolower (ch)); });
            if (value.rfind ("usb\\vid_05ac&", 0) == 0)
            {
                found = true;
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList (info);
    return found;
#else
    return false;
#endif
}

AppleUsbNetworkTransport::AppleUsbNetworkTransport()
{
    launcher = std::make_unique<AppleUsbShareLauncher> ([this] (const juce::String& message)
    {
        currentStatus = message;
        if (activityCallback)
            activityCallback (message);
    });
}

AppleUsbNetworkTransport::~AppleUsbNetworkTransport()
{
    stop();
}

std::vector<DeviceNetworkTransport::Device> AppleUsbNetworkTransport::enumerate()
{
    std::vector<DeviceNetworkTransport::Device> devices;
#if JUCE_WINDOWS
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) == ERROR_BUFFER_OVERFLOW)
    {
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) == NO_ERROR)
        {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next)
            {
                if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || ! adapter->AdapterName)
                    continue;
                const auto friendly = adapter->FriendlyName ? juce::String(adapter->FriendlyName) : juce::String();
                const auto description = adapter->Description ? juce::String(adapter->Description) : juce::String();
                const auto text = (friendly + " " + description).toLowerCase();
                if (! text.contains ("usbncm") && ! text.contains ("usb ncm"))
                    continue;

                Device device;
                device.kind = Kind::AppleUsb;
                device.id = juce::String::formatted ("%llX", static_cast<unsigned long long> (adapter->Luid.Value));
                device.name = friendly.isNotEmpty() ? friendly : description;
                device.interfaceName = device.name;
                device.connected = adapter->OperStatus == IfOperStatusUp;
                devices.push_back (device);
            }
        }
    }
#endif
    return devices;
}

bool AppleUsbNetworkTransport::start (const juce::String& deviceId)
{
    juce::ignoreUnused (deviceId);
    manualStop.store (false, std::memory_order_release);
    currentState.store (State::Starting, std::memory_order_release);
    currentStatus = "Starting USB sharing helper...";

    const bool launchStarted = launcher != nullptr && launcher->start();
    if (! launchStarted)
    {
        currentStatus = launcher != nullptr ? launcher->status() : juce::String ("Apple USB helper unavailable");
        currentState.store (State::Error, std::memory_order_release);
    }
    // On success we deliberately leave state() at Starting — poll() (driven
    // by the UI's existing 2Hz timer) is what promotes it to Connected once
    // a usbncm adapter actually comes up, or to Error if the helper exits
    // without one appearing. See AppleUsbShareLauncher::start()'s comment on
    // why this can't be resolved synchronously anymore (UAC can block
    // indefinitely; the USB mode-switch handshake takes several seconds).
    return launchStarted;
}

void AppleUsbNetworkTransport::stop()
{
    manualStop.store (true, std::memory_order_release);
    if (launcher != nullptr)
        launcher->stop();
    currentStatus = "USB network transport stopped";
    currentState.store (State::Stopped, std::memory_order_release);
}

void AppleUsbNetworkTransport::poll()
{
    const bool applePresent = appleUsbDevicePresent();
    const auto state = currentState.load (std::memory_order_acquire);

    if (applePresent)
        absentPolls.store (0, std::memory_order_relaxed);

    // Physical removal is the reset point for auto-start suppression. This
    // also tears down a live helper if the USB device disappears without the
    // normal stop path running first.
    if (! applePresent)
    {
        // ~4 s at the UI's 2 Hz poll: longer than a USB re-enumeration, far
        // shorter than a real unplug that the user would notice.
        constexpr int kAbsentGracePolls = 8;
        if (absentPolls.fetch_add (1, std::memory_order_relaxed) + 1 < kAbsentGracePolls)
            return;

        manualStop.store (false, std::memory_order_release);
        if (state != State::Stopped)
        {
            if (activityCallback)
                activityCallback ("Apple USB device gone for ~4 s; stopping helper.");
            if (launcher != nullptr)
                launcher->stop();
            currentStatus = "Waiting for iPhone or iPad over USB";
            currentState.store (State::Stopped, std::memory_order_release);
        }
        return;
    }

    if (state == State::Stopped)
    {
        if (! manualStop.load (std::memory_order_acquire))
        {
            currentStatus = "Apple USB device detected - starting USB sharing...";
            if (activityCallback)
                activityCallback ("Auto-start: Apple USB device present and no manual Stop.");
            if (! start (""))
                currentStatus = "Apple USB sharing could not be started";
        }
        return;
    }

    if (state == State::Error)
    {
        // Avoid repeated UAC/helper launches after a failure. Physical
        // removal resets the state so the next USB arrival gets a fresh try.
        if (! applePresent)
        {
            currentState.store (State::Stopped, std::memory_order_release);
            currentStatus = "Waiting for iPhone or iPad over USB";
        }
        return;
    }

    // isRunning() only becomes true once the background thread has registered
    // and started the helper, which takes longer than one 2 Hz poll tick.
    // Treating "not running yet" as failure flipped the state to Error
    // before the launch had even finished; only fail once the launch thread
    // is done AND the helper is not running.
    if (launcher != nullptr && ! launcher->isRunning() && ! launcher->isLaunching())
    {
        currentStatus = launcher->status();
        currentState.store (State::Error, std::memory_order_release);
        return;
    }

    // Connected used to mean "some usbncm adapter reports up". That stays true
    // after a session ends (Windows keeps the adapter), so a later run showed
    // "ready" while the helper had not even started and the phone never linked.
    // Now it requires the helper to have ACKed a DHCP lease to the phone in
    // THIS session (parsed from its ActivityLog.txt).
    const bool linkUp = launcher != nullptr && launcher->hasDhcpLease();

    currentState.store (linkUp ? State::Connected : State::Starting, std::memory_order_release);
    if (linkUp)
        currentStatus = "Direct USB link is up (isolated USB network, no Internet sharing)";
}

juce::String AppleUsbNetworkTransport::linkStatus() const
{
    const auto st = currentState.load (std::memory_order_acquire);
    if (st != State::Starting && st != State::Connected)
        return currentStatus;

    if (launcher != nullptr)
    {
        if (launcher->hasDhcpLease())
            return "Direct USB link is up (isolated USB network, no Internet sharing)";
        if (launcher->hasSeenDhcpRequest())
            return "DHCP request received - completing handshake...";
        if (launcher->isUsbNetworkReady())
            return "Waiting for Ios Device (no DHCP request yet)";
    }

    return "Starting Direct USB link...";
}

juce::String AppleUsbNetworkTransport::status() const
{
    return currentStatus;
}


namespace
{
#if JUCE_WINDOWS
struct SocketGuard { SOCKET s = INVALID_SOCKET; ~SocketGuard(){ if(s != INVALID_SOCKET) closesocket(s); } };

static bool sendAll(SOCKET s,const void* d,size_t n){auto p=(const char*)d;while(n){int k=send(s,p,(int)std::min(n,(size_t)INT_MAX),0);if(k<=0)return false;p+=k;n-=k;}return true;}
static bool recvAll(SOCKET s,void* d,size_t n){auto p=(char*)d;while(n){int k=recv(s,p,(int)std::min(n,(size_t)INT_MAX),0);if(k<=0)return false;p+=k;n-=k;}return true;}
static uint32_t le32(const unsigned char*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint32_t be32(const unsigned char*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void putle(unsigned char*p,uint32_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);p[2]=(unsigned char)(v>>16);p[3]=(unsigned char)(v>>24);}
static bool localConnect(SOCKET&s,uint16_t port){s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(s==INVALID_SOCKET)return false;int ms=1200;setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(char*)&ms,sizeof(ms));setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(char*)&ms,sizeof(ms));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);if(connect(s,(sockaddr*)&a,sizeof(a))!=0){closesocket(s);s=INVALID_SOCKET;return false;}return true;}
static std::string unescape(std::string v){const std::pair<const char*,const char*> r[]={{"&amp;","&"},{"&lt;","<"},{"&gt;",">"},{"&quot;","\""},{"&apos;","'"}};for(auto [a,b]:r){size_t p=0;while((p=v.find(a,p))!=std::string::npos){v.replace(p,strlen(a),b);p+=strlen(b);}}return v;}
static std::string plistString(const std::string&x,const char*k){auto n=std::string("<key>")+k+"</key>";auto p=x.find(n);if(p==std::string::npos)return{};auto a=x.find("<string>",p+n.size()),b=a==std::string::npos?std::string::npos:x.find("</string>",a+8);return a==std::string::npos||b==std::string::npos?std::string{}:unescape(x.substr(a+8,b-a-8));}

static std::vector<std::pair<uint32_t,std::string>> muxDevices()
{
    std::vector<std::pair<uint32_t,std::string>> out; SocketGuard g; if(!localConnect(g.s,27015))return out;
    static constexpr char xml[]="<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist version=\"1.0\"><dict><key>MessageType</key><string>ListDevices</string><key>ClientVersionString</key><string>DYSEKT-SF</string><key>ProgName</key><string>DYSEKT-SF</string></dict></plist>";
    unsigned char h[16]{};putle(h,sizeof(h)+sizeof(xml)-1);putle(h+4,1);putle(h+8,8);putle(h+12,1);
    if(!sendAll(g.s,h,sizeof(h))||!sendAll(g.s,xml,sizeof(xml)-1))return out;
    if(!recvAll(g.s,h,sizeof(h)))return out;auto len=le32(h);if(len<16||len>1024*1024)return out;
    std::string body(len-16,'\0');if(!recvAll(g.s,body.data(),body.size()))return out;
    size_t p=0;while((p=body.find("<dict>",p))!=std::string::npos){auto e=body.find("</dict>",p);if(e==std::string::npos)break;auto d=body.substr(p,e+7-p);auto q=d.find("<key>DeviceID</key><integer>");auto serial=plistString(d,"SerialNumber");if(q!=std::string::npos&&!serial.empty()){auto a=q+strlen("<key>DeviceID</key><integer>"),b=d.find("</integer>",a);if(b!=std::string::npos){try{out.emplace_back((uint32_t)std::stoul(d.substr(a,b-a)),serial);}catch(...){}}}p=e+7;}return out;
}

static std::string deviceName(uint32_t id)
{
    SocketGuard g;
    if (! localConnect (g.s, 27015))
        return {};

    static std::atomic<uint32_t> tag { 100 };
    const auto currentTag = tag.fetch_add (1, std::memory_order_relaxed);

    const auto muxPort = htons (0xf27e);
    const auto portNumber = static_cast<unsigned int> (muxPort);
    const auto request = std::string (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><dict>"
        "<key>MessageType</key><string>Connect</string>"
        "<key>ClientVersionString</key><string>DYSEKT-SF</string>"
        "<key>ProgName</key><string>DYSEKT-SF</string>"
        "<key>DeviceID</key><integer>") + std::to_string (id)
        + "</integer><key>PortNumber</key><integer>"
        + std::to_string (portNumber)
        + "</integer></dict></plist>";

    unsigned char header[16] {};
    putle (header, static_cast<uint32_t> (sizeof (header) + request.size()));
    putle (header + 4, 1);
    putle (header + 8, 8);
    putle (header + 12, currentTag);

    if (! sendAll (g.s, header, sizeof (header))
        || ! sendAll (g.s, request.data(), request.size()))
        return {};

    unsigned char replyHeader[16] {};
    if (! recvAll (g.s, replyHeader, sizeof (replyHeader)))
        return {};

    const auto replyLength = le32 (replyHeader);
    if (replyLength < sizeof (replyHeader) || replyLength > 1024 * 1024)
        return {};

    std::string reply (replyLength - sizeof (replyHeader), '\0');
    if (! reply.empty() && ! recvAll (g.s, reply.data(), reply.size()))
        return {};

    const auto number = plistString (reply, "Number");
    if (number != "0")
        return {};

    static constexpr char lockdownRequest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<plist version=\"1.0\"><dict>"
        "<key>Key</key><string>DeviceName</string>"
        "<key>Request</key><string>GetValue</string>"
        "</dict></plist>";

    const auto requestSize = static_cast<uint32_t> (sizeof (lockdownRequest) - 1);
    const auto beLength = htonl (requestSize);
    if (! sendAll (g.s, &beLength, sizeof (beLength))
        || ! sendAll (g.s, lockdownRequest, requestSize))
        return {};

    unsigned char lengthBytes[4] {};
    if (! recvAll (g.s, lengthBytes, sizeof (lengthBytes)))
        return {};

    const auto responseSize = be32 (lengthBytes);
    if (responseSize == 0 || responseSize > 1024 * 1024)
        return {};

    std::string lockdownReply (responseSize, '\0');
    if (! recvAll (g.s, lockdownReply.data(), lockdownReply.size()))
        return {};

    return plistString (lockdownReply, "Value");
}
static std::string pnpIdForAdapter(const char*name)
{
    if(!name||!*name)return{};std::string guid=name;if(guid.front()=='{')guid.erase(0,1);if(!guid.empty()&&guid.back()=='}')guid.pop_back();
    HKEY k=nullptr;auto path=std::string("SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\{")+guid+"}\\Connection";if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,path.c_str(),0,KEY_READ,&k)!=ERROR_SUCCESS)return{};
    char v[1024]{};DWORD t=0,n=sizeof(v);auto e=RegQueryValueExA(k,"PnpInstanceID",nullptr,&t,(BYTE*)v,&n);RegCloseKey(k);return e==ERROR_SUCCESS&&(t==REG_SZ||t==REG_EXPAND_SZ)?v:std::string{};
}
static std::string serialFromPnp(std::string id){auto p=id.find_last_of('\\');if(p==std::string::npos||p+1>=id.size())return{};auto s=id.substr(p+1);return s.find('&')==std::string::npos?s:std::string{};}
static std::string lower(std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return(char)std::tolower(c);});return s;}
static bool peerForAdapter(const IP_ADAPTER_ADDRESSES*a,juce::String&peer){for(auto*u=a->FirstUnicastAddress;u;u=u->Next){if(!u->Address.lpSockaddr||u->Address.lpSockaddr->sa_family!=AF_INET)continue;auto*sa=(sockaddr_in*)u->Address.lpSockaddr;auto x=ntohl(sa->sin_addr.s_addr),sub=(x>>8)&0xFFFFFFu;if(sub>=0xC0A863u&&sub<=0xC0A866u){peer="192.168."+juce::String((sub>>8)&255u)+".2";return true;}}return false;}
#endif
}
std::vector<std::pair<juce::String, juce::String>> AppleUsbNetworkTransport::liveDirectPeerNames() const
{
    std::vector<std::pair<juce::String, juce::String>> result;
#if JUCE_WINDOWS
    const auto devices=muxDevices(); if(devices.empty()) return result;
    ULONG size=0;if(GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_PREFIX,nullptr,nullptr,&size)!=ERROR_BUFFER_OVERFLOW)return result;
    std::vector<unsigned char> buffer(size);auto* adapters=(IP_ADAPTER_ADDRESSES*)buffer.data();
    if(GetAdaptersAddresses(AF_INET,GAA_FLAG_INCLUDE_PREFIX,nullptr,adapters,&size)!=NO_ERROR)return result;
    for(auto*a=adapters;a;a=a->Next){
        if(a->IfType!=IF_TYPE_ETHERNET_CSMACD||a->OperStatus!=IfOperStatusUp)continue;
        auto friendly=a->FriendlyName?juce::String(a->FriendlyName):juce::String(),desc=a->Description?juce::String(a->Description):juce::String();
        auto text=(friendly+" "+desc).toLowerCase();if(!text.contains("usbncm")&&!text.contains("usb ncm"))continue;
        juce::String peer;if(!peerForAdapter(a,peer))continue;
        auto serial=lower(serialFromPnp(pnpIdForAdapter(a->AdapterName)));if(serial.empty())continue;
        for(const auto&[id,muxSerial]:devices)if(lower(muxSerial)==serial){auto name=deviceName(id);if(!name.empty())result.emplace_back(peer,juce::String::fromUTF8(name.data(),(int)name.size()));break;}
    }
#endif
    return result;
}
