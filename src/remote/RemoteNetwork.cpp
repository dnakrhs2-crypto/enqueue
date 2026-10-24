#include "remote/RemoteNetwork.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>

namespace gocue::remote
{
namespace
{
std::string utf8 (const wchar_t* value)
{
    if (value == nullptr) return {};
    const int size = WideCharToMultiByte (CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result (static_cast<size_t> (size), '\0');
    WideCharToMultiByte (CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    if (! result.empty()) result.pop_back();
    return result;
}
}
bool isPrivateIPv4 (const std::string& address)
{
    IN_ADDR parsed {};
    if (InetPtonA (AF_INET, address.c_str(), &parsed) != 1) return false;
    const auto ip = ntohl (parsed.s_addr);
    return (ip & 0xff000000u) == 0x0a000000u
        || (ip & 0xfff00000u) == 0xac100000u
        || (ip & 0xffff0000u) == 0xc0a80000u;
}
std::vector<NetworkAddress> networkAddresses()
{
    ULONG bytes = 16 * 1024;
    std::vector<unsigned char> buffer (bytes);
    ULONG result;
    do
    {
        buffer.resize (bytes);
        result = GetAdaptersAddresses (AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                       nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.data()), &bytes);
    } while (result == ERROR_BUFFER_OVERFLOW);
    std::vector<NetworkAddress> addresses;
    if (result != NO_ERROR) return addresses;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.data()); adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const auto name = utf8 (adapter->FriendlyName);
        auto description = utf8 (adapter->Description) + " " + name;
        std::transform (description.begin(), description.end(), description.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
        bool virtualAdapter = adapter->IfType == IF_TYPE_TUNNEL;
        MIB_IF_ROW2 interfaceInfo {};
        interfaceInfo.InterfaceLuid = adapter->Luid;
        if (GetIfEntry2 (&interfaceInfo) == NO_ERROR)
            virtualAdapter |= ! interfaceInfo.InterfaceAndOperStatusFlags.HardwareInterface;
        for (const char* term : { "virtual", "vpn", "hyper-v", "vmware", "virtualbox", "vethernet", "tap-", "tun", "tailscale", "zerotier", "wireguard", "loopback" })
            virtualAdapter |= description.find (term) != std::string::npos;
        bool gateway = false;
        for (auto* g = adapter->FirstGatewayAddress; g != nullptr; g = g->Next)
            if (g->Address.lpSockaddr != nullptr && g->Address.lpSockaddr->sa_family == AF_INET)
                gateway |= reinterpret_cast<sockaddr_in*> (g->Address.lpSockaddr)->sin_addr.s_addr != 0;
        for (auto* address = adapter->FirstUnicastAddress; address != nullptr; address = address->Next)
        {
            if (address->Address.lpSockaddr == nullptr || address->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* ip = reinterpret_cast<sockaddr_in*> (address->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN] {};
            if (InetNtopA (AF_INET, const_cast<IN_ADDR*> (&ip->sin_addr), text, sizeof (text)) != nullptr && isPrivateIPv4 (text))
                addresses.push_back ({ adapter->AdapterName, name, text, gateway && ! virtualAdapter });
        }
    }
    std::stable_sort (addresses.begin(), addresses.end(), [] (const auto& a, const auto& b) { return a.recommended > b.recommended; });
    return addresses;
}
std::string randomKey()
{
    std::array<unsigned char, 32> bytes {};
    if (BCryptGenRandom (nullptr, bytes.data(), static_cast<ULONG> (bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string key;
    unsigned int bits = 0;
    int count = 0;
    for (auto byte : bytes)
    {
        bits = (bits << 8) | byte;
        count += 8;
        while (count >= 6) { count -= 6; key += alphabet[(bits >> count) & 63]; }
    }
    if (count > 0) key += alphabet[(bits << (6 - count)) & 63];
    return key;
}
}
