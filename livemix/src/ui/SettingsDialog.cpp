#include "SettingsDialog.h"

#include "Widgets.h"

namespace gocue::livemix
{

namespace
{
    class SettingsContent : public juce::Component
    {
    public:
        SettingsContent (MixEngine& e, LiveMixSettings& s, std::function<void()> deviceChanged)
            : engine (e), settings (s), onDeviceChanged (std::move (deviceChanged))
        {
            styleCaption (deviceCaption, ko ("ASIO 장치"));
            addAndMakeVisible (deviceCaption);
            deviceCombo.setWantsKeyboardFocus (false);
            deviceCombo.onChange = [this] { applyDevice(); };
            addAndMakeVisible (deviceCombo);
            panelButton.setButtonText (ko ("ASIO 제어판 (버퍼 크기)..."));
            panelButton.onClick = [this]
            {
                if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
                    if (device->hasControlPanel())
                        device->showControlPanel();
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
            toggle (closeToTray, ko ("닫기 버튼도 트레이로 (종료는 트레이 메뉴에서)"), settings.getCloseToTray(), [this] (bool on) { settings.setCloseToTray (on); });
            toggle (startWithWindows, ko ("Windows 시작할 때 LiveMix 실행"), settings.getStartWithWindows(), [this] (bool on)
            {
                settings.setStartWithWindows (on);
                SettingsDialog::setStartWithWindows (on);
            });

            styleCaption (autosaveCaption, ko ("자동 저장 (초)"));
            addAndMakeVisible (autosaveCaption);
            autosaveEditor.setInputRestrictions (4, "0123456789");
            autosaveEditor.setText (juce::String (settings.getAutosaveSeconds()));
            autosaveEditor.onFocusLost = [this] { settings.setAutosaveSeconds (autosaveEditor.getText().getIntValue()); };
            autosaveEditor.onReturnKey = [this] { settings.setAutosaveSeconds (autosaveEditor.getText().getIntValue()); };
            addAndMakeVisible (autosaveEditor);

            styleCaption (backupCaption, ko ("온라인 백업"));
            addAndMakeVisible (backupCaption);
            styleCaption (backupNote, ko ("위쪽 '온라인 백업' 버튼의 창에서 계정을 만들고 로그인합니다. 백업은 그 계정의 것만 보이고, 올리기·불러오기도 그 계정으로만 됩니다."));
            backupNote.setFont (bodyFont (12.5f));
            addAndMakeVisible (backupNote);

            refreshDevices();
            setSize (560, 540);
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
            closeToTray.setBounds (area.removeFromTop (28));
            startWithWindows.setBounds (area.removeFromTop (28));
            area.removeFromTop (8);
            row = area.removeFromTop (30);
            autosaveCaption.setBounds (row.removeFromLeft (110));
            autosaveEditor.setBounds (row.removeFromLeft (70));
            area.removeFromTop (16);
            backupCaption.setBounds (area.removeFromTop (20));
            backupNote.setBounds (area.removeFromTop (56));
        }

        void paint (juce::Graphics& g) override { g.fillAll (Palette::card); }

    private:
        MixEngine& engine;
        LiveMixSettings& settings;
        std::function<void()> onDeviceChanged;
        juce::StringArray names;
        juce::Label deviceCaption, bufferCaption, deviceNote, autosaveCaption, backupCaption, backupNote;
        juce::ComboBox deviceCombo, bufferCombo;
        juce::TextButton panelButton;
        juce::ToggleButton minimiseToTray, closeToTray, startWithWindows;
        juce::TextEditor autosaveEditor;
        bool refreshing = false;
    };

    juce::Component::SafePointer<juce::DialogWindow> openDialog;
}

void SettingsDialog::show (MixEngine& engine, LiveMixSettings& settings, juce::Component* centreAround, std::function<void()> onDeviceChanged)
{
    if (openDialog != nullptr)
    {
        openDialog->toFront (true);
        return;
    }

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new SettingsContent (engine, settings, std::move (onDeviceChanged)));
    options.dialogTitle = ko ("설정");
    options.dialogBackgroundColour = Palette::card;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.componentToCentreAround = centreAround;
    openDialog = options.launchAsync();
}

void SettingsDialog::closeIfOpen()
{
    if (openDialog != nullptr)
        openDialog->exitModalState (0);

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
