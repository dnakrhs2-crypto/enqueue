#include "SettingsDialog.h"

#include "GlobalHotkeys.h"
#include "Widgets.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace gocue::livemix
{

namespace
{
    class SettingsContent : public juce::Component,
                            private juce::Timer
    {
    public:
        SettingsContent (MixEngine& e, LiveMixSettings& s, std::function<void()> deviceChanged, std::function<void()> hotkeysChanged,
                         std::function<void (bool)> hotkeyCapture, std::function<ControlServer::Status()> controlStatus,
                         std::function<void (bool)> controlEnabled)
            : engine (e), settings (s), onDeviceChanged (std::move (deviceChanged)), onHotkeysChanged (std::move (hotkeysChanged)),
              onHotkeyCapture (std::move (hotkeyCapture)), getControlStatus (std::move (controlStatus)),
              onControlEnabled (std::move (controlEnabled))
        {
            styleCaption (deviceCaption, ko ("ASIO 장치"));
            addAndMakeVisible (deviceCaption);
            deviceCombo.setWantsKeyboardFocus (false);
            deviceCombo.onChange = [this] { applyDevice(); };
            addAndMakeVisible (deviceCombo);
            panelButton.setButtonText (ko ("ASIO 제어판 (버퍼 크기)..."));
            panelButton.onClick = [this]
            {
                auto* device = engine.getDeviceManager().getCurrentAudioDevice();

                if (device == nullptr || ! device->hasControlPanel())
                    return;

                if (device->showControlPanel())   // the driver asks for a restart (its buffer / rate changed)
                {
                    const auto error = engine.restartDevice();

                    if (error.isNotEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("장치를 다시 열지 못했습니다"), error, ko ("확인"));
                }

                refreshDevices();

                if (onDeviceChanged)
                    onDeviceChanged();
            };
            addAndMakeVisible (panelButton);
            styleCaption (bufferCaption, ko ("버퍼 크기"));
            addAndMakeVisible (bufferCaption);
            bufferCombo.setWantsKeyboardFocus (false);
            bufferCombo.onChange = [this] { applyBuffer(); };
            addAndMakeVisible (bufferCombo);
            styleCaption (deviceNote, ko ("ASIO 장치만 씁니다. 버퍼가 작을수록 지연이 짧고 끊길 위험이 큽니다 (128~256 권장)."));
            deviceNote.setFont (bodyFont (12.5f));
            addAndMakeVisible (deviceNote);

            auto toggle = [this] (juce::ToggleButton& t, const juce::String& text, bool value, std::function<void (bool)> apply)
            {
                t.setButtonText (text);
                t.setToggleState (value, juce::dontSendNotification);
                t.onClick = [&t, apply] { apply (t.getToggleState()); };
                addAndMakeVisible (t);
            };

            toggle (minimiseToTray, ko ("최소화하면 트레이로 (창은 사라지고 소리는 계속)"), settings.getMinimiseToTray(), [this] (bool on) { settings.setMinimiseToTray (on); });
            toggle (closeAsk, ko ("닫을 때 물어보기 (종료할지 트레이로 갈지)"), settings.getCloseAsk(), [this] (bool on) { settings.setCloseAsk (on); });
            toggle (closeToTray, ko ("물어보지 않을 때: 트레이로 (끄면 종료)"), settings.getCloseToTray(), [this] (bool on) { settings.setCloseToTray (on); });
            toggle (startWithWindows, ko ("Windows 시작할 때 LiveMix 실행"), settings.getStartWithWindows(), [this] (bool on)
            {
                settings.setStartWithWindows (on);
                SettingsDialog::setStartWithWindows (on);
            });
            toggle (skipWhenOff, ko ("OFF시 플러그인 OFF"), settings.getSkipPluginsWhenOff(), [this] (bool on)
            {
                settings.setSkipPluginsWhenOff (on);
                engine.setSkipChainWhenOff (on);
            });

            styleCaption (hotkeyCaption, ko ("전역 핫키"));
            addAndMakeVisible (hotkeyCaption);
            styleCaption (hotkeyNote, ko ("LiveMix가 최소화·트레이 상태여도 듣는 전역 핫키입니다. 그동안 다른 프로그램은 그 키를 받지 못하니 F 키(F9 등)나 Ctrl+Alt 조합을 권합니다. 뮤트 대상은 각 마이크 카드와 FX의 '뮤트그룹' 칩으로 고릅니다. 세 번째 핫키는 창을 트레이로 숨기거나 다시 불러옵니다. 플러그인 그룹 핫키는 그 번호의 그룹을 마이크 전체에서 한꺼번에 껐다 켭니다 (그룹은 채널의 '체인 열기' 옆에서 만듭니다)."));
            hotkeyNote.setFont (bodyFont (12.5f));
            addAndMakeVisible (hotkeyNote);

            // every row is the same three widgets; 'rows' keeps them in the order they are drawn and is what the
            // clash check walks, so a new hotkey is one hotkeyRow() call and nothing else
            auto hotkeyRow = [this] (juce::Label& label, const juce::String& text, HotkeyButton& button, juce::TextButton& clear,
                                     std::function<juce::String()> get, std::function<void (const juce::String&)> set)
            {
                label.setText (text, juce::dontSendNotification);
                label.setFont (bodyFont (14.0f));
                label.setColour (juce::Label::textColourId, Palette::text);
                addAndMakeVisible (label);
                button.setHotkey (get());
                button.validate = [this, &button] (const juce::KeyPress& key)
                {
                    if (const auto why = GlobalHotkeys::reasonToRefuse (key); why.isNotEmpty())
                        return why;

                    for (const auto& other : rows)
                        if (other.button != &button && other.button->getHotkey().isNotEmpty()
                            && key == juce::KeyPress::createFromDescription (other.button->getHotkey()))
                            return ko ("다른 핫키가 쓰는 키입니다.");

                    return juce::String();
                };
                button.onCaptureChanged = [this] (bool capturing) { if (onHotkeyCapture) onHotkeyCapture (capturing); };
                button.onHotkeyChanged = [this, set, &button] (const juce::String& description)
                {
                    set (description);
                    button.setHotkey (description);

                    if (onHotkeysChanged)
                        onHotkeysChanged();
                };
                addAndMakeVisible (button);
                clear.setButtonText ("x");
                clear.setTooltip (ko ("핫키 지우기"));
                clear.onClick = [this, set, &button]
                {
                    set ({});
                    button.setHotkey ({});

                    if (onHotkeysChanged)
                        onHotkeysChanged();
                };
                addAndMakeVisible (clear);
                rows.push_back ({ &label, &button, &clear });
            };

            auto& prefs = settings;   // 'settings' is a reference member; the lambdas below keep it by name
            hotkeyRow (micHotkeyLabel, ko ("마이크 뮤트그룹"), micHotkey, micHotkeyClear,
                       [&prefs] { return prefs.getMicMuteHotkey(); }, [&prefs] (const juce::String& d) { prefs.setMicMuteHotkey (d); });
            hotkeyRow (fxHotkeyLabel, ko ("FX 뮤트그룹"), fxHotkey, fxHotkeyClear,
                       [&prefs] { return prefs.getFxMuteHotkey(); }, [&prefs] (const juce::String& d) { prefs.setFxMuteHotkey (d); });
            hotkeyRow (windowHotkeyLabel, ko ("창 숨기기/불러오기"), windowHotkey, windowHotkeyClear,
                       [&prefs] { return prefs.getWindowHotkey(); }, [&prefs] (const juce::String& d) { prefs.setWindowHotkey (d); });

            for (int i = 0; i < MixSession::maxPluginGroups; ++i)
            {
                const int group = i + 1;   // the number on the card
                hotkeyRow (groupHotkeyLabel[i], ko ("플러그인 그룹 ") + juce::String (group), groupHotkey[i], groupHotkeyClear[i],
                           [&prefs, group] { return prefs.getPluginGroupHotkey (group); },
                           [&prefs, group] (const juce::String& d) { prefs.setPluginGroupHotkey (group, d); });
            }

            styleCaption (controlCaption, ko ("외부 제어 (Stream Deck)"));
            addAndMakeVisible (controlCaption);
            toggle (externalControl, ko ("외부 제어 사용"), settings.getExternalControlEnabled(), [this] (bool on)
            {
                if (onControlEnabled) onControlEnabled (on);
                refreshControlStatus();   // the bound address is visible on the very first click
            });
            styleCaption (controlNote, ko ("이 PC의 Stream Deck 등에서 마이크와 FX를 조절합니다."));
            controlNote.setFont (bodyFont (12.5f));
            addAndMakeVisible (controlNote);
            for (auto* label : { &controlAddress, &controlState })
            {
                label->setFont (bodyFont (14.0f));
                label->setMinimumHorizontalScale (1.0f);
                label->setEditable (false, false, false);
                addAndMakeVisible (*label);
            }
            controlHelp.setButtonText (ko ("설치 방법"));
            controlHelp.setColour (juce::HyperlinkButton::textColourId, Palette::accent);
            controlHelp.setFont (bodyFont (12.5f), false, juce::Justification::centredLeft);
            controlHelp.onClick = [] { juce::URL (ko ("https://곰튀김.com/livemix/#streamdeck")).launchInDefaultBrowser(); };
            addAndMakeVisible (controlHelp);

            styleCaption (backupCaption, ko ("온라인 백업"));
            addAndMakeVisible (backupCaption);
            styleCaption (backupNote, ko ("위쪽 '온라인 백업' 버튼의 창에서 계정을 만들고 로그인합니다. 백업은 그 계정의 것만 보이고, 올리기·불러오기도 그 계정으로만 됩니다."));
            backupNote.setFont (bodyFont (12.5f));
            addAndMakeVisible (backupNote);

            refreshDevices();
            refreshControlStatus();
            setSize (560, 848 + MixSession::maxPluginGroups * 36);
            startTimer (500);
        }

        void refreshDevices()
        {
            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            deviceCombo.clear (juce::dontSendNotification);
            names.clear();

            for (auto* type : engine.getDeviceManager().getAvailableDeviceTypes())
            {
                if (! type->getTypeName().containsIgnoreCase ("ASIO"))
                    continue;

                type->scanForDevices();
                names = type->getDeviceNames (false);
            }

            for (int i = 0; i < names.size(); ++i)
                deviceCombo.addItem (names[i], i + 1);

            bufferCombo.clear (juce::dontSendNotification);

            if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
            {
                deviceCombo.setSelectedId (names.indexOf (device->getName()) + 1, juce::dontSendNotification);
                const auto sizes = device->getAvailableBufferSizes();

                for (int i = 0; i < sizes.size(); ++i)
                    bufferCombo.addItem (juce::String (sizes[i]) + ko (" 샘플") + "  (" + juce::String (1000.0 * sizes[i] / juce::jmax (1.0, device->getCurrentSampleRate()), 1) + " ms)", sizes[i]);

                bufferCombo.setSelectedId (device->getCurrentBufferSizeSamples(), juce::dontSendNotification);
                panelButton.setEnabled (device->hasControlPanel());
            }
            else
            {
                deviceCombo.setTextWhenNothingSelected (ko ("ASIO 장치 없음"));
                panelButton.setEnabled (false);
            }
        }

        void applyDevice()
        {
            if (refreshing || deviceCombo.getSelectedId() <= 0)
                return;

            // through the engine: the ASIO type, every channel and the callback (safe mode never opened anything)
            if (const auto error = engine.openDevice (deviceCombo.getText()); error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("장치를 열지 못했습니다"), error, ko ("확인"));

            refreshDevices();

            if (onDeviceChanged)
                onDeviceChanged();
        }

        void applyBuffer()
        {
            if (refreshing || bufferCombo.getSelectedId() <= 0)
                return;

            if (const auto error = engine.setBufferSize (bufferCombo.getSelectedId()); error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("버퍼 크기를 바꾸지 못했습니다"), error, ko ("확인"));

            refreshDevices();

            if (onDeviceChanged)
                onDeviceChanged();
        }

        ~SettingsContent() override
        {
            stopTimer();   // no timer can refer to the labels once SettingsWindow deletes this content
            const bool capturing = std::any_of (rows.begin(), rows.end(), [] (const Row& r) { return r.button->isCapturing(); });

            if (capturing && onHotkeyCapture)
                onHotkeyCapture (false);   // the dialog went away mid-capture: the hotkeys come back
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (20, 16);
            deviceCaption.setBounds (area.removeFromTop (20));
            auto row = area.removeFromTop (30);
            panelButton.setBounds (row.removeFromRight (200));
            row.removeFromRight (8);
            deviceCombo.setBounds (row);
            area.removeFromTop (8);
            bufferCaption.setBounds (area.removeFromTop (20));
            bufferCombo.setBounds (area.removeFromTop (30).withWidth (260));
            area.removeFromTop (4);
            deviceNote.setBounds (area.removeFromTop (36));
            area.removeFromTop (12);
            minimiseToTray.setBounds (area.removeFromTop (28));
            closeAsk.setBounds (area.removeFromTop (28));
            closeToTray.setBounds (area.removeFromTop (28));
            startWithWindows.setBounds (area.removeFromTop (28));
            skipWhenOff.setBounds (area.removeFromTop (28));
            area.removeFromTop (16);
            hotkeyCaption.setBounds (area.removeFromTop (20));
            hotkeyNote.setBounds (area.removeFromTop (90));
            area.removeFromTop (4);

            int labelWidth = 0;

            for (const auto& hotkey : rows)
                labelWidth = juce::jmax (labelWidth, labelWidthForText (*hotkey.label, hotkey.label->getText()));

            for (const auto& hotkey : rows)
            {
                auto r = area.removeFromTop (30);
                hotkey.label->setBounds (r.removeFromLeft (labelWidth));
                hotkey.clear->setBounds (r.removeFromRight (34));
                r.removeFromRight (6);
                hotkey.button->setBounds (r);
                area.removeFromTop (6);
            }

            area.removeFromTop (10);
            controlCaption.setBounds (area.removeFromTop (20));
            externalControl.setBounds (area.removeFromTop (28));
            controlNote.setBounds (area.removeFromTop (36));
            auto controlRow = area.removeFromTop (30);
            controlAddress.setBounds (controlRow.removeFromLeft (185));
            controlState.setBounds (controlRow);
            controlHelp.setBounds (area.removeFromTop (28).withWidth (100));
            area.removeFromTop (16);
            backupCaption.setBounds (area.removeFromTop (20));
            backupNote.setBounds (area.removeFromTop (56));
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

    private:
        void timerCallback() override { refreshControlStatus(); }

        void refreshControlStatus()
        {
            const auto status = getControlStatus ? getControlStatus() : ControlServer::Status {};
            externalControl.setToggleState (status.enabled, juce::dontSendNotification);
            controlAddress.setText (status.enabled ? status.address() : ko ("사용 안 함"), juce::dontSendNotification);
            const auto text = ! status.enabled ? ko ("꺼짐")
                : status.error.isNotEmpty() ? ko ("연결 오류 — 다시 켜 보세요")
                : status.starting ? ko ("시작 중")
                : status.connectedCount == 0 ? ko ("연결 대기 중")
                : ko ("연결됨 ") + juce::String (status.connectedCount) + ko ("개");
            controlState.setText (text, juce::dontSendNotification);
            controlState.setColour (juce::Label::textColourId, status.error.isNotEmpty() ? Palette::danger : Palette::dimText);
        }

        MixEngine& engine;
        LiveMixSettings& settings;
        std::function<void()> onDeviceChanged, onHotkeysChanged;
        std::function<void (bool)> onHotkeyCapture;
        std::function<ControlServer::Status()> getControlStatus;
        std::function<void (bool)> onControlEnabled;
        juce::StringArray names;
        struct Row { juce::Label* label; HotkeyButton* button; juce::TextButton* clear; };
        std::vector<Row> rows;   // the hotkey rows in the order they are drawn
        juce::Label deviceCaption, bufferCaption, deviceNote, backupCaption, backupNote, hotkeyCaption, hotkeyNote, micHotkeyLabel, fxHotkeyLabel, windowHotkeyLabel;
        juce::Label groupHotkeyLabel[MixSession::maxPluginGroups];
        juce::Label controlCaption, controlNote, controlAddress, controlState;
        juce::HyperlinkButton controlHelp;
        HotkeyButton micHotkey, fxHotkey, windowHotkey;
        HotkeyButton groupHotkey[MixSession::maxPluginGroups];
        juce::TextButton micHotkeyClear { "x" }, fxHotkeyClear { "x" }, windowHotkeyClear { "x" };
        juce::TextButton groupHotkeyClear[MixSession::maxPluginGroups];
        juce::ComboBox deviceCombo, bufferCombo;
        juce::TextButton panelButton;
        juce::ToggleButton minimiseToTray, closeAsk, closeToTray, startWithWindows, skipWhenOff, externalControl;
        bool refreshing = false;
    };

    juce::Component::SafePointer<juce::DialogWindow> openDialog;
}

namespace
{
    /** The settings' own window: closing it (the title bar, Esc) deletes it - not modal, so the mics stay usable meanwhile. */
    class SettingsWindow : public juce::DialogWindow
    {
    public:
        SettingsWindow() : DialogWindow (ko ("설정"), Palette::card, true, true) {}

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([] { SettingsDialog::closeIfOpen(); });   // not from inside its own callback
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key == juce::KeyPress (juce::KeyPress::escapeKey))
            {
                closeButtonPressed();   // DialogWindow's own Esc would only hide it
                return true;
            }

            return DialogWindow::keyPressed (key);
        }
    };
}

void SettingsDialog::show (MixEngine& engine, LiveMixSettings& settings, juce::Component* centreAround, std::function<void()> onDeviceChanged,
                           std::function<void()> onHotkeysChanged, std::function<void (bool capturing)> onHotkeyCapture,
                           std::function<ControlServer::Status()> controlStatus, std::function<void (bool)> controlEnabled)
{
    if (openDialog != nullptr)
    {
        openDialog->toFront (true);
        return;
    }

    auto* content = new SettingsContent (engine, settings, std::move (onDeviceChanged), std::move (onHotkeysChanged), std::move (onHotkeyCapture),
                                         std::move (controlStatus), std::move (controlEnabled));
    auto* scroller = new juce::Viewport();
    scroller->setViewedComponent (content, true);
    scroller->setScrollBarsShown (true, true);   // sideways only when a narrow display squeezed the window under the content's width
    scroller->setSize (content->getWidth() + scroller->getScrollBarThickness(), content->getHeight());

    auto* window = new SettingsWindow();
    window->setUsingNativeTitleBar (true);
    window->setContentOwned (scroller, true);   // the window may be shorter than the settings: they scroll
    window->setResizable (true, false);
    window->setResizeLimits (scroller->getWidth(), 320, scroller->getWidth(), content->getHeight() + 40);

    if (centreAround != nullptr)
        window->centreAroundComponent (centreAround, window->getWidth(), window->getHeight());   // on its display, inside it
    else
        window->centreWithSize (window->getWidth(), window->getHeight());

    window->setVisible (true);
    window->toFront (true);
    openDialog = window;
}

void SettingsDialog::closeIfOpen()
{
    openDialog.deleteAndZero();
}

void SettingsDialog::setStartWithWindows (bool on)
{
   #if JUCE_WINDOWS
    const juce::String key = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\LiveMix";

    if (on)
        juce::WindowsRegistry::setValue (key, "\"" + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName() + "\"");
    else
        juce::WindowsRegistry::deleteValue (key);
   #else
    juce::ignoreUnused (on);
   #endif
}

} // namespace gocue::livemix
