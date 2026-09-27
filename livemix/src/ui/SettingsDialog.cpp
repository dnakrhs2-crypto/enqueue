#include "SettingsDialog.h"

#include "AudioBackends.h"
#include "DeviceFormatText.h"
#include "GlobalHotkeys.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
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
            styleCaption (typeCaption, ko ("장치 종류"));
            addAndMakeVisible (typeCaption);
            typeCombo.setComponentID ("device-type");
            typeCombo.setWantsKeyboardFocus (false);
            typeCombo.onChange = [this] { applyType(); };
            addAndMakeVisible (typeCombo);
            styleCaption (deviceCaption, ko ("ASIO 장치"));
            addAndMakeVisible (deviceCaption);
            deviceCombo.setComponentID ("device-input");
            deviceCombo.setWantsKeyboardFocus (false);
            deviceCombo.onChange = [this] { applySelection(); };
            addAndMakeVisible (deviceCombo);
            styleCaption (outputCaption, ko ("출력 (모니터)"));
            addAndMakeVisible (outputCaption);
            outputCombo.setComponentID ("device-output");
            outputCombo.setWantsKeyboardFocus (false);
            outputCombo.onChange = [this] { applySelection(); };
            addAndMakeVisible (outputCombo);
            styleCaption (rateCaption, ko ("샘플레이트"));
            addAndMakeVisible (rateCaption);
            rateCombo.setComponentID ("device-rate");
            rateCombo.setWantsKeyboardFocus (false);
            rateCombo.onChange = [this] { applySelection(); };
            addAndMakeVisible (rateCombo);
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
            styleCaption (bufferCaption, ko ("버퍼"));
            addAndMakeVisible (bufferCaption);
            bufferCombo.setComponentID ("device-buffer");
            bufferCombo.setWantsKeyboardFocus (false);
            bufferCombo.onChange = [this] { applySelection(); };
            addAndMakeVisible (bufferCombo);
            styleCaption (sharedBufferNote, ko ("버퍼: 윈도우가 정함 (보통 10 ms)"));
            sharedBufferNote.setFont (bodyFont (12.5f));
            addAndMakeVisible (sharedBufferNote);
            styleCaption (bitDepthCaption, ko ("비트뎁스"));
            addAndMakeVisible (bitDepthCaption);
            bitDepthCombo.setComponentID ("device-bitdepth");
            bitDepthCombo.setWantsKeyboardFocus (false);
            for (int id = 1; id <= 5; ++id) bitDepthCombo.addItem (DeviceFormatText::choiceName (id), id);
            bitDepthCombo.onChange = [this] { applySelection(); };
            addAndMakeVisible (bitDepthCombo);
            bitDepthDetail.setComponentID ("device-bitdepth-detail");
            bitDepthHint.setComponentID ("device-bitdepth-hint");
            bitDepthWarning.setComponentID ("device-bitdepth-warning");
            for (auto* label : { &bitDepthDetail, &bitDepthHint, &bitDepthWarning })
            {
                styleCaption (*label, {});
                label->setFont (bodyFont (12.5f));
                label->setJustificationType (juce::Justification::topLeft);
                addAndMakeVisible (*label);
            }
            bitDepthDetail.setColour (juce::Label::textColourId, Palette::text);
            bitDepthWarning.setColour (juce::Label::textColourId, Palette::meterYellow);
            soundSettingsButton.setComponentID ("windows-sound-settings");
            soundSettingsButton.setButtonText (ko ("윈도우 소리 설정..."));
            soundSettingsButton.onClick = []
            {
               #if JUCE_WINDOWS
                juce::File::getSpecialLocation (juce::File::windowsSystemDirectory).getChildFile ("control.exe").startAsProcess ("mmsys.cpl,,1");
               #endif
            };
            addAndMakeVisible (soundSettingsButton);
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
            updateContentSize();
            startTimer (500);
        }

        void refreshDevices()
        {
            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            const auto current = engine.getOpenDevice();
            types = AudioBackends::availableTypes (engine.getDeviceManager());
            typeCombo.clear (juce::dontSendNotification);
            for (int i = 0; i < types.size(); ++i)
                typeCombo.addItem (AudioBackends::label (types[i]), i + 1);
            // With no open device (including safe mode), offer the first available type without opening it.
            const int typeIndex = types.contains (current.type) ? types.indexOf (current.type) : (types.isEmpty() ? -1 : 0);
            typeCombo.setSelectedId (typeIndex + 1, juce::dontSendNotification);
            shownType = types[typeIndex];
            const bool asio = shownType.containsIgnoreCase ("ASIO");
            const bool shared = shownType == "Windows Audio";
            deviceCaption.setText (asio ? ko ("ASIO 장치") : ko ("입력 (마이크)"), juce::dontSendNotification);
            deviceCombo.clear (juce::dontSendNotification);
            names.clear();
            outputNames.clear();
            if (auto* type = findType (shownType))
            {
                type->scanForDevices();
                names = type->getDeviceNames (! asio);
                outputNames = type->getDeviceNames (false);
            }
            for (int i = 0; i < names.size(); ++i)
                deviceCombo.addItem (names[i], i + 1);
            deviceCombo.setSelectedId (names.indexOf (current.input) + 1, juce::dontSendNotification);
            deviceCombo.setTextWhenNothingSelected (asio ? ko ("ASIO 장치 없음") : ko ("입력 장치 없음"));
            outputCombo.clear (juce::dontSendNotification);
            outputCombo.addItem (ko ("없음 (OBS로만 보내기)"), 1);
            for (int i = 0; i < outputNames.size(); ++i) outputCombo.addItem (outputNames[i], i + 2);
            outputCombo.setSelectedId (current.output.isEmpty() ? 1 : outputNames.indexOf (current.output) + 2, juce::dontSendNotification);
            outputCaption.setVisible (! asio);
            outputCombo.setVisible (! asio);
            rateCaption.setVisible (! asio);
            rateCombo.setVisible (! asio);
            bufferCaption.setVisible (! shared);
            bufferCombo.setVisible (! shared);
            sharedBufferNote.setVisible (shared);
            bufferCombo.clear (juce::dontSendNotification);
            juce::Array<double> rates { 44100.0, 48000.0 };
            panelButton.setVisible (false);
            if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
            {
                const auto sizes = device->getAvailableBufferSizes();
                for (int i = 0; i < sizes.size(); ++i)
                    bufferCombo.addItem (juce::String (sizes[i]) + ko (" 샘플") + "  (" + juce::String (1000.0 * sizes[i] / juce::jmax (1.0, device->getCurrentSampleRate()), 1) + " ms)", sizes[i]);
                bufferCombo.setSelectedId (current.bufferSize, juce::dontSendNotification);
                for (auto rate : device->getAvailableSampleRates()) if (rate > 0.0) rates.addIfNotAlreadyThere (rate);
                if (current.sampleRate > 0.0) rates.addIfNotAlreadyThere (current.sampleRate);
                panelButton.setVisible (asio && device->hasControlPanel());
            }
            rates.sort();
            rateCombo.clear (juce::dontSendNotification);
            for (auto rate : rates) rateCombo.addItem (juce::String (juce::roundToInt (rate)) + " Hz", juce::roundToInt (rate));
            rateCombo.setSelectedId (juce::roundToInt (current.sampleRate > 0.0 ? current.sampleRate : 48000.0), juce::dontSendNotification);
            juce::String note = ko ("USB 마이크·헤드셋 같은 일반 장치를 씁니다. 마이크와 모니터가 서로 다른 장치면 샘플레이트 차이를 자동으로 맞춥니다 (모니터 지연이 조금 늘어납니다).");
            if (shownType == "Windows Audio (Low Latency Mode)") note += ko (" 지원하지 않는 장치면 일반 모드로 여세요.");
            if (shownType == "Windows Audio (Exclusive Mode)") note += ko (" 독점 모드에서는 OBS 등 다른 프로그램이 같은 마이크를 쓸 수 없습니다.");
            deviceNote.setText (asio ? ko ("ASIO 장치만 씁니다. 버퍼가 작을수록 지연이 짧고 끊길 위험이 큽니다 (128~256 권장).") : note, juce::dontSendNotification);
            bitDepthCombo.setSelectedId (DeviceFormatText::choiceId (current.sampleFormat), juce::dontSendNotification);
            refreshBitDepth();
            updateContentSize();
            resized();
        }

        juce::AudioIODeviceType* findType (const juce::String& typeName)
        {
            for (auto* type : engine.getDeviceManager().getAvailableDeviceTypes())
                if (type->getTypeName() == typeName) return type;
            return nullptr;
        }

        void applyType()
        {
            if (refreshing || typeCombo.getSelectedId() <= 0) return;
            const auto current = engine.getOpenDevice();
            MixDevice wanted;
            wanted.sampleFormat = current.sampleFormat;
            wanted.type = types[typeCombo.getSelectedId() - 1];
            wanted.bufferSize = 0;
            wanted.sampleRate = wanted.isAsio() ? 0.0 : 48000.0;
            if (auto* type = findType (wanted.type))
            {
                type->scanForDevices();
                const auto ins = type->getDeviceNames (! wanted.isAsio()), outs = type->getDeviceNames (false);
                wanted.input = ins.contains (current.input) ? current.input : ins[juce::jmax (0, type->getDefaultDeviceIndex (true))];
                wanted.output = wanted.isAsio() ? wanted.input
                    : (! current.isAsio() && current.output.isEmpty()) ? juce::String()
                    : outs.contains (current.output) ? current.output : outs[juce::jmax (0, type->getDefaultDeviceIndex (false))];
            }
            applyDevice (wanted);
        }

        void applySelection()
        {
            if (refreshing) return;
            auto wanted = engine.getOpenDevice();
            const bool hadDevice = wanted.input.isNotEmpty();
            wanted.type = shownType;
            wanted.input = deviceCombo.getSelectedId() > 0 ? deviceCombo.getText() : juce::String();
            wanted.output = wanted.isAsio() ? wanted.input : outputNames[outputCombo.getSelectedId() - 2];
            wanted.sampleRate = wanted.isAsio() ? (hadDevice ? wanted.sampleRate : 0.0) : (double) rateCombo.getSelectedId();
            wanted.bufferSize = shownType == "Windows Audio" ? 0 : bufferCombo.getSelectedId();
            if (shownType == "Windows Audio (Exclusive Mode)")
                wanted.sampleFormat = DeviceFormatText::choice (bitDepthCombo.getSelectedId());
            applyDevice (wanted);
        }

        void applyDevice (const MixDevice& wanted)
        {
            if (const auto error = engine.openDevice (wanted); error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("오디오 장치를 열지 못했습니다"), error, ko ("확인"));
            refreshDevices();
            if (onDeviceChanged)
                onDeviceChanged();
        }

        int deviceNoteHeight() const
        {
            juce::AttributedString text;
            text.append (deviceNote.getText(), deviceNote.getFont());
            juce::TextLayout layout;
            layout.createLayout (text, 510.0f);
            return juce::jmax (36, juce::roundToInt (std::ceil (layout.getHeight())) + 8);
        }

        static int textHeight (const juce::Label& label)
        {
            juce::AttributedString text;
            text.append (label.getText(), label.getFont());
            juce::TextLayout layout;
            layout.createLayout (text, 510.0f);
            return juce::jmax (22, (int) std::ceil (layout.getHeight()) + 8);
        }

        int bitDepthHeight() const
        {
            return 20 + (bitDepthCombo.isVisible() ? 38 : 0) + textHeight (bitDepthDetail)
                + (bitDepthWarning.isVisible() ? textHeight (bitDepthWarning) : 0)
                + (soundSettingsButton.isVisible() ? 38 : 0)
                + (bitDepthHint.isVisible() ? textHeight (bitDepthHint) : 0) + 8;
        }

        void refreshBitDepth()
        {
            const int previousHeight = bitDepthHeight();
            auto current = engine.getOpenDevice();
            auto format = engine.getDeviceFormat();
            if (! engine.isMonitorRunning()) current.output.clear();
            if (current.type != shownType) format = {};
            current.type = shownType;
            auto* device = engine.getDeviceManager().getCurrentAudioDevice();
            const auto text = DeviceFormatText::settings (format, current, device != nullptr && device->hasControlPanel());
            bitDepthCombo.setVisible (shownType == "Windows Audio (Exclusive Mode)");
            soundSettingsButton.setVisible (shownType == "Windows Audio" || shownType == "Windows Audio (Low Latency Mode)");
            bitDepthDetail.setText (text.detail, juce::dontSendNotification);
            bitDepthHint.setText (text.hint, juce::dontSendNotification);
            bitDepthHint.setVisible (text.hint.isNotEmpty());
            bitDepthWarning.setText (text.warning, juce::dontSendNotification);
            bitDepthWarning.setVisible (text.warning.isNotEmpty());
            for (int id = 1; id <= 5; ++id)
            {
                const auto item = DeviceFormatText::exclusiveItem (id, format, current.input.isNotEmpty(), current.output.isNotEmpty());
                bitDepthCombo.changeItemText (id, item.text);
                bitDepthCombo.setItemEnabled (id, item.enabled);
            }
            if (previousHeight != bitDepthHeight())
            {
                updateContentSize();
                resized();
            }
        }

        void updateContentSize()
        {
            const int deviceHeight = (shownType.containsIgnoreCase ("ASIO") ? 3 : 4) * 58 + bitDepthHeight() + deviceNoteHeight() + 12;
            setSize (560, 848 + MixSession::maxPluginGroups * 36 - 160 + deviceHeight);
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
            typeCaption.setBounds (area.removeFromTop (20));
            typeCombo.setBounds (area.removeFromTop (30));
            area.removeFromTop (8);
            deviceCaption.setBounds (area.removeFromTop (20));
            auto row = area.removeFromTop (30);
            if (panelButton.isVisible())
            {
                panelButton.setBounds (row.removeFromRight (250));
                row.removeFromRight (8);
            }
            deviceCombo.setBounds (row);
            area.removeFromTop (8);
            if (outputCombo.isVisible())
            {
                outputCaption.setBounds (area.removeFromTop (20));
                outputCombo.setBounds (area.removeFromTop (30));
                area.removeFromTop (8);
            }
            auto format = area.removeFromTop (50);
            if (rateCombo.isVisible())
            {
                auto rate = format.removeFromLeft (sharedBufferNote.isVisible() ? 210 : 250);
                rateCaption.setBounds (rate.removeFromTop (20));
                rateCombo.setBounds (rate);
                format.removeFromLeft (12);
            }
            if (sharedBufferNote.isVisible())
                sharedBufferNote.setBounds (format.withTrimmedTop (20));
            else
            {
                bufferCaption.setBounds (format.removeFromTop (20));
                bufferCombo.setBounds (format);
            }
            area.removeFromTop (8);
            bitDepthCaption.setBounds (area.removeFromTop (20));
            if (bitDepthCombo.isVisible())
            {
                bitDepthCombo.setBounds (area.removeFromTop (30));
                area.removeFromTop (8);
            }
            bitDepthDetail.setBounds (area.removeFromTop (textHeight (bitDepthDetail)));
            if (bitDepthWarning.isVisible()) bitDepthWarning.setBounds (area.removeFromTop (textHeight (bitDepthWarning)));
            if (soundSettingsButton.isVisible())
            {
                soundSettingsButton.setBounds (area.removeFromTop (30).withWidth (220));
                area.removeFromTop (8);
            }
            if (bitDepthHint.isVisible()) bitDepthHint.setBounds (area.removeFromTop (textHeight (bitDepthHint)));
            area.removeFromTop (8);
            deviceNote.setBounds (area.removeFromTop (deviceNoteHeight()));
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
        void timerCallback() override { refreshControlStatus(); refreshBitDepth(); }

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
        juce::StringArray types, names, outputNames;
        juce::String shownType;
        struct Row { juce::Label* label; HotkeyButton* button; juce::TextButton* clear; };
        std::vector<Row> rows;   // the hotkey rows in the order they are drawn
        juce::Label deviceCaption, bufferCaption, deviceNote, backupCaption, backupNote, hotkeyCaption, hotkeyNote, micHotkeyLabel, fxHotkeyLabel, windowHotkeyLabel;
        juce::Label typeCaption, outputCaption, rateCaption, sharedBufferNote;
        juce::Label bitDepthCaption, bitDepthDetail, bitDepthHint, bitDepthWarning;
        juce::Label groupHotkeyLabel[MixSession::maxPluginGroups];
        juce::Label controlCaption, controlNote, controlAddress, controlState;
        juce::HyperlinkButton controlHelp;
        HotkeyButton micHotkey, fxHotkey, windowHotkey;
        HotkeyButton groupHotkey[MixSession::maxPluginGroups];
        juce::TextButton micHotkeyClear { "x" }, fxHotkeyClear { "x" }, windowHotkeyClear { "x" };
        juce::TextButton groupHotkeyClear[MixSession::maxPluginGroups];
        juce::ComboBox typeCombo, deviceCombo, outputCombo, rateCombo, bufferCombo, bitDepthCombo;
        juce::TextButton panelButton, soundSettingsButton;
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
