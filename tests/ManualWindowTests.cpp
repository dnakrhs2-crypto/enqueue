#include "ui/ManualWindow.h"
#include "ui/GoCueLookAndFeel.h"
#include "app/UiScale.h"

namespace gocue::tests
{
class ManualWindowTests : public juce::UnitTest
{
public:
    ManualWindowTests() : UnitTest ("Manual window", "Enqueue") {}

    void runTest() override
    {
        beginTest ("mobile remote tab contains the seven approved lines verbatim");
        const auto sections = ManualWindow::sections();
        int mobileIndex = -1;
        for (size_t i = 0; i < sections.size(); ++i)
            if (ko (sections[i].title) == ko ("모바일 연동"))
            {
                expectEquals (mobileIndex, -1, "one mobile tab");
                mobileIndex = static_cast<int> (i);
            }
        expect (mobileIndex >= 0);
        if (mobileIndex < 0) return;
        const auto approved = ko (R"(모바일 연동

  · 휴대폰·태블릿 브라우저로 앤큐를 조작합니다. 앱 설치는 필요 없습니다.
  · 메뉴 [모바일 연동 > 웹리모트 켜기]로 켜고, [QR 코드·주소 보기]의 QR을 휴대폰 카메라로 찍으면 연결됩니다. 주소를 브라우저에 직접 입력해도 됩니다.
  · PC와 휴대폰이 같은 공유기(와이파이)에 연결되어 있어야 합니다. 공유기가 없으면 사용할 수 없습니다. 손님용 와이파이처럼 기기끼리 연결을 막는 곳에서는 연결되지 않습니다.
  · 처음 켤 때 Windows 방화벽 창이 뜨면 [허용]을 누르세요.
  · 공유기나 네트워크가 바뀌면 주소가 바뀌니 QR을 다시 찍으세요. [QR 새로 만들기]를 누르면 연결된 기기가 모두 끊기고 예전 QR로는 들어올 수 없습니다.
  · 휴대폰에서 할 수 있는 것: GO, 일시정지, 페이드아웃, 전체 페이드 정지(두 번 누르면 즉시 정지), 재생 중인 큐의 일시정지·정지, 탭 바꾸기, 카트 실행, 큐를 눌러 다음 큐로 지정, 편집 모드에서 손잡이를 끌어 큐 순서 바꾸기, 쇼 모드·편집 모드 전환.
  · 휴대폰 화면이 꺼졌다 켜지면 연결을 확인하는 동안 잠깐 버튼이 잠깁니다. 공연 중에는 휴대폰 자동 잠금을 늘리거나 끄세요.)");
        expectEquals (ko (sections[static_cast<size_t> (mobileIndex)].text).replace ("\r\n", "\n"), approved.replace ("\r\n", "\n"));

        GoCueLookAndFeel theme;
        ManualWindow window (false);
        window.setLookAndFeel (&theme);
        expect (! window.isOnDesktop());
        auto* content = window.getContentComponent();
        juce::TabbedComponent* tabs = nullptr;
        for (auto* child : content->getChildren())
            if (auto* t = dynamic_cast<juce::TabbedComponent*> (child)) tabs = t;
        expect (tabs != nullptr);
        if (tabs == nullptr) { window.setLookAndFeel (nullptr); return; }
        expectEquals (tabs->getNumTabs(), static_cast<int> (sections.size()));
        auto& bar = tabs->getTabbedButtonBar();

        for (const int percent : { 100, 125, 150 })
            for (bool minimum : { false, true })
            {
                beginTest ("tabs and wrapped body at " + juce::String (percent) + "% " + (minimum ? "minimum" : "default") + " size without a native window");
                const float scale = static_cast<float> (percent) / 100.0f;
                window.setSize (minimum ? 640 : 960, minimum ? 420 : 720);
                UiScale::fitWindowIntoArea (window, { 1920 * 100 / percent, 1032 * 100 / percent });
                expect (content->getLocalBounds().contains (tabs->getBounds()));
                int visibleTabs = 0;
                for (int i = 0; i < tabs->getNumTabs(); ++i)
                {
                    auto* button = bar.getTabButton (i);
                    expect (button != nullptr);
                    if (button == nullptr) continue;
                    expectEquals (button->getButtonText(), ko (sections[static_cast<size_t> (i)].title));
                    if (button->isVisible())
                    {
                        ++visibleTabs;
                        expect (bar.getLocalBounds().contains (button->getBounds()));
                        const int textWidth = juce::GlyphArrangement::getStringWidthInt (Palette::font (Palette::tabSize, true), button->getButtonText());
                        expect (textWidth <= button->getWidth() - 26, "full tab label fits");
                        for (int j = 0; j < i; ++j)
                            if (auto* other = bar.getTabButton (j); other != nullptr && other->isVisible())
                                expect (! button->getBounds().intersects (other->getBounds()));
                    }
                    // The overflow menu selects a tab by this same public method.
                    tabs->setCurrentTabIndex (i, false);
                    auto* page = dynamic_cast<juce::TextEditor*> (tabs->getCurrentContentComponent());
                    expect (page != nullptr);
                    if (page != nullptr)
                    {
                        expect (page->isVisible() && page->isReadOnly());
                        expect (tabs->getLocalBounds().contains (page->getBounds()));
                        expect (page->getY() >= bar.getBottom());
                        expectEquals (page->getText().replace ("\r\n", "\n"), ko (sections[static_cast<size_t> (i)].text).replace ("\r\n", "\n"));
                        if (minimum && i == 0) // the existing long introduction still scrolls after adding a tab
                            for (auto* child : page->getChildren())
                                if (auto* viewport = dynamic_cast<juce::Viewport*> (child))
                                {
                                    expect (viewport->getVerticalScrollBar().isVisible());
                                    page->moveCaretToEnd (false);
                                    expect (viewport->getViewPositionY() > 0);
                                    page->moveCaretToTop (false);
                                }
                    }
                }
                expect (visibleTabs > 0);
                if (visibleTabs < bar.getNumTabs())
                {
                    juce::Button* extras = nullptr;
                    for (auto* child : bar.getChildren())
                        if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && dynamic_cast<juce::TabBarButton*> (b) == nullptr && b->isVisible()) extras = b;
                    expect (extras != nullptr, "hidden tabs remain reachable through the existing overflow button");
                    if (extras != nullptr) expect (bar.getLocalBounds().contains (extras->getBounds()));
                }
                tabs->setCurrentTabIndex (mobileIndex, false);
                auto* page = static_cast<juce::TextEditor*> (tabs->getCurrentContentComponent());
                juce::Viewport* viewport = nullptr;
                for (auto* child : page->getChildren())
                    if (auto* v = dynamic_cast<juce::Viewport*> (child)) viewport = v;
                expect (viewport != nullptr);
                if (viewport != nullptr)
                {
                    page->moveCaretToTop (false);
                    expectEquals (viewport->getViewPositionY(), 0);
                    expect (! viewport->getHorizontalScrollBar().isVisible(), "long lines wrap");
                    if (page->getTextHeight() > viewport->getViewHeight())
                    {
                        expect (viewport->getVerticalScrollBar().isVisible());
                        page->moveCaretToEnd (false);
                        expect (viewport->getViewPositionY() > 0, "the final mobile instruction is reachable");
                        page->moveCaretToTop (false);
                    }
                    else
                        expect (! viewport->getVerticalScrollBar().isVisible(), "all seven instructions fit without scrolling");
                }
                const auto image = content->createComponentSnapshot (content->getLocalBounds(), true, scale);
                expect (image.isValid());
                const auto output = juce::SystemStats::getEnvironmentVariable ("ENQUEUE_REMOTE_REVIEW_DIR", {});
                if (output.isNotEmpty())
                {
                    juce::File folder (output); folder.createDirectory();
                    auto stream = folder.getChildFile ("manual-mobile-" + juce::String (minimum ? "minimum-" : "default-") + juce::String (percent) + ".png").createOutputStream();
                    if (stream != nullptr) { stream->setPosition (0); stream->truncate(); juce::PNGImageFormat().writeImageToStream (image, *stream); }
                }
            }
        window.setLookAndFeel (nullptr);
    }
};
static ManualWindowTests manualWindowTests;
}
