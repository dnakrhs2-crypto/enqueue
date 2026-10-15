#include "CollabHostCheck.h"
#include "ui/UiUtils.h"

namespace gocue::livemix::collab
{
CollabHostCheck::Result CollabHostCheck::evaluate (bool received, const CollabUpnp::Result& upnp, const juce::String& publicIp)
{
    if (received) return { true, Cause::none, {} };
    if (! upnp.found || ! upnp.mapped)
        return { false, Cause::upnpDisabled, ko ("공유기 자동 포트 열기(UPnP)가 꺼져 있습니다") };

    const auto parts = juce::StringArray::fromTokens (upnp.externalIp, ".", "");
    const int a = parts.size() == 4 ? parts[0].getIntValue() : -1;
    const int b = parts.size() == 4 ? parts[1].getIntValue() : -1;
    const bool shared = a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168)
                        || (a == 100 && b >= 64 && b <= 127);
    if (shared || (upnp.externalIp.isNotEmpty() && upnp.externalIp != publicIp))
        return { false, Cause::sharedAddress, ko ("공유기가 두 대 연결돼 있거나 통신사 공유 IP라 바깥에서 들어올 수 없습니다") };
    return { false, Cause::firewall, ko ("윈도우 방화벽이나 보안 프로그램이 막고 있을 수 있습니다") };
}

juce::String CollabHostCheck::Result::message() const
{
    return reachable ? juce::String() : ko ("이 PC는 공유기 포트가 열리지 않아 방장을 할 수 없습니다. 상대방이 방을 만들어 주세요.")
                                       + "\n" + reason;
}
}
