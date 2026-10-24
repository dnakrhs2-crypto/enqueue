#include "app/AppSettings.h"
#include "app/StartupAnnouncement.h"

namespace gocue::tests
{
class StartupAnnouncementTests : public juce::UnitTest
{
public:
    StartupAnnouncementTests() : UnitTest ("Mobile remote startup notice", "Enqueue") {}

    void runTest() override
    {
        const auto message = juce::String::fromUTF8 (R"(휴대폰과 태블릿 등에서 앤큐를 컨트롤 할 수 있는 기능이 추가 되었습니다. 메뉴 창에서 "모바일 연동"을 클릭해서 진행해보세요! (공유기가 없으면 불가능합니다))");
        const auto mobileTitle = juce::String::fromUTF8 ("모바일 연동");
        const auto updateTitle = juce::String::fromUTF8 ("업데이트 완료");
        const auto updateText = juce::String::fromUTF8 ("앤큐가 1.0 → 1.1(으)로 업데이트되었습니다.\n바뀐 점은 도움말 > 사용 설명서와 GitHub 릴리스 노트에 있습니다.");

        beginTest ("fresh install: one mobile notice with the exact text");
        const auto fresh = startupAnnouncement ({}, "1.1", false);
        expect (fresh.has_value());
        if (fresh)
        {
            expectEquals (fresh->title, mobileTitle);
            expectEquals (fresh->message, message);
            expect (fresh->includesMobileRemoteNotice);
        }

        beginTest ("update: one update notice, one blank line, then the exact mobile text");
        const auto update = startupAnnouncement ("1.0", "1.1", false);
        expect (update.has_value());
        if (update)
        {
            expectEquals (update->title, updateTitle);
            expectEquals (update->message, updateText + "\n\n" + message);
            expect (update->includesMobileRemoteNotice);
        }

        beginTest ("already seen: later updates retain only the original update notice");
        const auto seen = startupAnnouncement ("1.0", "1.1", true);
        expect (seen.has_value());
        if (seen)
        {
            expectEquals (seen->title, updateTitle);
            expectEquals (seen->message, updateText);
            expect (! seen->includesMobileRemoteNotice);
        }
        expect (! startupAnnouncement ({}, "1.1", true).has_value());

        beginTest ("same version relaunch: no notice after seeing it, but an unseen feature is still announced");
        expect (! startupAnnouncement ("1.1", "1.1", true).has_value());
        const auto unseen = startupAnnouncement ("1.1", "1.1", false);
        expect (unseen.has_value());
        if (unseen)
        {
            expectEquals (unseen->title, mobileTitle);
            expectEquals (unseen->message, message);
            expect (unseen->includesMobileRemoteNotice);
        }

        beginTest ("the shown flag defaults to false and persists to this PC's settings across relaunches");
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getNonexistentChildFile ("EnqueueMobileNotice", ".settings");
        juce::PropertiesFile::Options options;
        options.millisecondsBeforeSaving = -1;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        {
            juce::PropertiesFile storage (file, options);
            AppSettings settings (storage);
            expect (! settings.getMobileRemoteNoticeShown());
            expect (startupAnnouncement (settings.getLastRunVersion(), "1.1", settings.getMobileRemoteNoticeShown()).has_value());
            settings.setLastRunVersion ("1.1");
            settings.setMobileRemoteNoticeShown (true); // after the notice has been presented
            expect (settings.getMobileRemoteNoticeShown());
            expect (file.existsAsFile());
        }
        {
            juce::PropertiesFile storage (file, options);
            AppSettings settings (storage);
            expect (settings.getMobileRemoteNoticeShown());
            expectEquals (settings.getLastRunVersion(), juce::String ("1.1"));
            expect (! startupAnnouncement (settings.getLastRunVersion(), "1.1", settings.getMobileRemoteNoticeShown()).has_value());
            const auto later = startupAnnouncement (settings.getLastRunVersion(), "1.2", settings.getMobileRemoteNoticeShown());
            expect (later.has_value() && ! later->includesMobileRemoteNotice);
        }
        expect (file.deleteFile());
    }
};
static StartupAnnouncementTests startupAnnouncementTests;
}
