#include <winsock2.h>
#include "CollabUpnp.h"
#include <miniupnpc.h>
#include <upnpcommands.h>

#ifndef MINIUPNP_STATICLIB
 #error LiveMix must link the static miniupnpc client
#endif

namespace gocue::livemix::collab
{
namespace
{
class MiniUpnp final : public CollabUpnp
{
public:
    MiniUpnp() { WSADATA startup {}; winsock = WSAStartup (MAKEWORD (2, 2), &startup) == 0; }
    ~MiniUpnp() override
    {
        FreeUPNPUrls (&urls);
        if (winsock) WSACleanup();
    }

    Result discover (int boundPort) override
    {
        removeMapping();
        FreeUPNPUrls (&urls); urls = {}; data = {}; result = {}; permanentOnly = false;
        if (! winsock || juce::SystemStats::getEnvironmentVariable ("LIVEMIX_COLLAB_NO_UPNP", {}) == "1") return result;

        // Search all types together. miniupnpc can wait once more after the last reply,
        // so a one-second discovery window keeps SSDP discovery within two seconds.
        const char* types[] { "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "urn:schemas-upnp-org:service:WANPPPConnection:1", "upnp:rootdevice", nullptr };
        int error = 0;
        auto* devices = upnpDiscoverDevices (types, 1000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error, 1);
        if (devices == nullptr) return result;
        char lan[64] {}, wan[64] {};
        const int igd = UPNP_GetValidIGD (devices, &urls, &data, lan, sizeof (lan), wan, sizeof (wan));
        freeUPNPDevlist (devices);
        result.found = igd == UPNP_CONNECTED_IGD || igd == UPNP_PRIVATEIP_IGD || igd == UPNP_DISCONNECTED_IGD;
        if (! result.found) return result;
        result.lanIp = lan;
        char external[16] {};
        if (UPNP_GetExternalIPAddress (urls.controlURL, data.first.servicetype, external) == UPNPCOMMAND_SUCCESS)
            result.externalIp = external;
        port = juce::String (boundPort);
        result.mapped = renew();
        return result;
    }

    bool renew() override
    {
        if (! result.found || result.lanIp.isEmpty()) return false;
        const auto add = [this] (const char* lease)
        {
            return UPNP_AddPortMapping (urls.controlURL, data.first.servicetype, port.toRawUTF8(), port.toRawUTF8(),
                                        result.lanIp.toRawUTF8(), "LiveMix", "UDP", nullptr, lease);
        };
        int code = add (permanentOnly ? "0" : "3600");
        if (code == 725)   // OnlyPermanentLeasesSupported: such a router takes only lease 0; leave / quit still delete it
        {
            permanentOnly = true;
            code = add ("0");
        }
        const bool success = code == UPNPCOMMAND_SUCCESS;
        if (success) mappingOwned = true;
        return success;
    }

    void removeMapping() override
    {
        if (mappingOwned)
            UPNP_DeletePortMapping (urls.controlURL, data.first.servicetype, port.toRawUTF8(), "UDP", nullptr);
        mappingOwned = false;
    }

private:
    bool winsock = false, mappingOwned = false, permanentOnly = false;
    UPNPUrls urls {};
    IGDdatas data {};
    Result result;
    juce::String port;
};
}

std::unique_ptr<CollabUpnp> CollabUpnp::makeMiniUpnp() { return std::make_unique<MiniUpnp>(); }
}
