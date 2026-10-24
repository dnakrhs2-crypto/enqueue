#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace gocue
{
struct StartupAnnouncement
{
    juce::String title, message;
    bool includesMobileRemoteNotice = false;
};

/** Decide the one version/feature notice without opening a window or changing settings. */
inline std::optional<StartupAnnouncement> startupAnnouncement (const juce::String& previousVersion,
                                                               const juce::String& currentVersion,
                                                               bool mobileRemoteNoticeShown)
{
    const bool updated = previousVersion.isNotEmpty() && previousVersion != currentVersion;
    if (! updated && mobileRemoteNoticeShown)
        return std::nullopt;

    StartupAnnouncement notice;
    notice.includesMobileRemoteNotice = ! mobileRemoteNoticeShown;
    if (updated)
    {
        notice.title = juce::String::fromUTF8 ("업데이트 완료");
        notice.message = juce::String::fromUTF8 ("앤큐가 ") + previousVersion + " → " + currentVersion
            + juce::String::fromUTF8 ("(으)로 업데이트되었습니다.\n바뀐 점은 도움말 > 사용 설명서와 GitHub 릴리스 노트에 있습니다.");
    }
    else
        notice.title = juce::String::fromUTF8 ("모바일 연동");

    if (notice.includesMobileRemoteNotice)
    {
        if (notice.message.isNotEmpty()) notice.message += "\n\n";
        notice.message += juce::String::fromUTF8 (R"(휴대폰과 태블릿 등에서 앤큐를 컨트롤 할 수 있는 기능이 추가 되었습니다. 메뉴 창에서 "모바일 연동"을 클릭해서 진행해보세요! (공유기가 없으면 불가능합니다))");
    }
    return notice;
}
}
