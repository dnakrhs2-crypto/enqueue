#include "ui/PastePropertiesDialog.h"

#include "ui/UiUtils.h"

namespace gocue::PastePropertiesDialog
{

namespace
{
    class Content : public juce::Component
    {
    public:
        Content (const juce::String& sourceName, int targetCount, std::function<void (const Selection&)> apply)
            : onApply (std::move (apply))
        {
            info.setText (ko ("\"") + sourceName + ko ("\"의 속성을 선택한 큐 ") + juce::String (targetCount) + ko ("개에 붙여넣습니다."), juce::dontSendNotification);
            info.setColour (juce::Label::textColourId, Palette::dimText);
            info.setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (info);

            auto setup = [this] (juce::ToggleButton& t, const char* text, bool on)
            {
                t.setButtonText (ko (text));
                t.setToggleState (on, juce::dontSendNotification);
                t.setColour (juce::ToggleButton::textColourId, Palette::text);
                t.setColour (juce::ToggleButton::tickColourId, Palette::standby);
                addAndMakeVisible (t);
            };

            setup (basics, "기본 (색·깃발·비활성화·자동 로드·메모)", true);
            setup (timing, "시간 (프리웨이트·포스트웨이트·진행 모드)", true);
            setup (triggers, "트리거 (2차 트리거·시간 트리거·다른 큐 페이드 정지·덕)", true);
            setup (timeLoops, "재생 (트림·반복·속도·엔벨로프)", true);
            setup (levels, "레벨 (게인·정지 페이드)", true);
            setup (effects, "플러그인 (VST3 인서트 체인)", false);
            setup (fade, "페이드 / 디밴프 / 그룹 설정 (시간·상대·정지·레벨 목표·커브·파라미터·그룹 모드 — 대상은 제외)", true);

            applyButton.setButtonText (ko ("붙여넣기"));
            applyButton.onClick = [this]
            {
                Selection s;
                s.basics = basics.getToggleState();
                s.timing = timing.getToggleState();
                s.triggers = triggers.getToggleState();
                s.timeLoops = timeLoops.getToggleState();
                s.levels = levels.getToggleState();
                s.effects = effects.getToggleState();
                s.fade = fade.getToggleState();

                if (onApply)
                    onApply (s);

                closeDialog();
            };
            addAndMakeVisible (applyButton);

            cancelButton.setButtonText (ko ("취소"));
            cancelButton.onClick = [this] { closeDialog(); };
            addAndMakeVisible (cancelButton);

            setSize (520, 330);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (16, 12);
            info.setBounds (area.removeFromTop (22));
            area.removeFromTop (6);

            for (auto* t : { &basics, &timing, &triggers, &timeLoops, &levels, &effects, &fade })
            {
                t->setBounds (area.removeFromTop (26));
                area.removeFromTop (2);
            }

            auto buttons = area.removeFromBottom (28);
            cancelButton.setBounds (buttons.removeFromRight (80));
            buttons.removeFromRight (8);
            applyButton.setBounds (buttons.removeFromRight (100));
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::panel); }

    private:
        void closeDialog()
        {
            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                dialog->exitModalState (0);
        }

        std::function<void (const Selection&)> onApply;
        juce::Label info;
        juce::ToggleButton basics, timing, triggers, timeLoops, levels, effects, fade;
        juce::TextButton applyButton, cancelButton;
    };
}

void show (juce::Component* centreAround, const juce::String& sourceName, int targetCount, std::function<void (const Selection&)> onApply)
{
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = ko ("큐 속성 붙여넣기");
    options.content.setOwned (new Content (sourceName, targetCount, std::move (onApply)));
    options.componentToCentreAround = centreAround;
    options.dialogBackgroundColour = Palette::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

} // namespace gocue::PastePropertiesDialog
