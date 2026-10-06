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
                         std::function<void (bool)> controlEnabled, SettingsDialog::AcceptedFormatsQuery acceptedFormats,
                         std::function<void()> openFailed)
            : engine (e), settings (s), onDeviceChanged (std::move (deviceChanged)), onOpenFailed (std::move (openFailed)),
              onHotkeysChanged (std::move (hotkeysChanged)), onHotkeyCapture (std::move (hotkeyCapture)),
              getControlStatus (std::move (controlStatus)), onControlEnabled (std::move (controlEnabled)),
              getAcceptedFormats (std::move (acceptedFormats))
        {
            styleCaption (typeCaption, ko ("장치 종류"));
            addAndMakeVisible (typeCaption);
            typeCombo.setComponentID ("device-type");
            typeCombo.setWantsKeyboardFocus (false);
            typeCombo.onChange = [this] { typeCombo.settle(); applyType(); };
            addAndMakeVisible (typeCombo);
            styleCaption (deviceCaption, ko ("ASIO 장치"));
            addAndMakeVisible (deviceCaption);
            deviceCombo.setComponentID ("device-input");
            deviceCombo.setWantsKeyboardFocus (false);
            deviceCombo.onChange = [this] { deviceCombo.settle(); applySelection(); };
            deviceCombo.onRepick = [this] { reopenIfStopped(); };
            addAndMakeVisible (deviceCombo);
            styleCaption (outputCaption, ko ("출력 (모니터)"));
            addAndMakeVisible (outputCaption);
            outputCombo.setComponentID ("device-output");
            outputCombo.setWantsKeyboardFocus (false);
            outputCombo.onChange = [this] { outputCombo.settle(); applySelection(); };
            outputCombo.onRepick = [this] { reopenIfStopped(); };
            addAndMakeVisible (outputCombo);
            styleCaption (rateCaption, ko ("샘플레이트"));
            addAndMakeVisible (rateCaption);
            rateCombo.setComponentID ("device-rate");
            rateCombo.setWantsKeyboardFocus (false);
            rateCombo.onChange = [this] { rateCombo.settle(); applySelection (true); };
            addAndMakeVisible (rateCombo);
            panelButton.setComponentID ("asio-panel");
            panelButton.setButtonText (ko ("ASIO 제어판 (버퍼 크기)..."));
            panelButton.onClick = [this]
            {
                auto* device = engine.getDeviceManager().getCurrentAudioDevice();

                if (device == nullptr || ! device->hasControlPanel())
                    return;

                if (! device->showControlPanel())   // no restart asked for: nothing changed
                {
                    refreshDevices();
                    return;
                }

                // the driver asks for a restart (its buffer / rate changed): what it opens is the operator's choice; a
                // restart that failed leaves the session asking for its own device, like any open from here
                const auto error = engine.restartDevice();

                if (error.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("장치를 다시 열지 못했습니다"), error, ko ("확인"));

                refreshDevices();
                auto& notify = error.isEmpty() ? onDeviceChanged : onOpenFailed;
                if (notify)
                    notify();
            };
            addAndMakeVisible (panelButton);
            styleCaption (bufferCaption, ko ("버퍼"));
            addAndMakeVisible (bufferCaption);
            bufferCombo.setComponentID ("device-buffer");
            bufferCombo.setWantsKeyboardFocus (false);
            bufferCombo.onChange = [this] { bufferCombo.settle(); applySelection(); };
            addAndMakeVisible (bufferCombo);
            styleCaption (sharedBufferNote, ko ("버퍼: 윈도우가 정함 (보통 10 ms)"));
            sharedBufferNote.setFont (bodyFont (12.5f));
            addAndMakeVisible (sharedBufferNote);
            styleCaption (bitDepthCaption, ko ("비트뎁스"));
            addAndMakeVisible (bitDepthCaption);
            bitDepthCombo.setComponentID ("device-bitdepth");
            bitDepthCombo.setWantsKeyboardFocus (false);
            for (int id = 1; id <= 5; ++id) bitDepthCombo.addItem (DeviceFormatText::choiceName (id), id);
            bitDepthCombo.onChange = [this] { bitDepthCombo.settle(); applySelection(); };
            addAndMakeVisible (bitDepthCombo);
            // a refill skipped in the middle of an earlier pick (the type changed by arrow key in its open list) is done
            // before the next list shows: never the previous type's devices under the new type
            for (auto* box : { &typeCombo, &deviceCombo, &outputCombo, &rateCombo, &bufferCombo, &bitDepthCombo })
                box->beforeListOpens = [this] { if (refreshWaiting && ! anyListBusy()) refreshDevices(); };
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
            toggle (sendTransport, ko ("재생 신호 보내기 (학습 플러그인 사용)"), settings.getSendTransport(), [this] (bool on)
            {
                settings.setSendTransport (on);
                engine.setSendTransport (on);
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
            // Not in the middle of a pick (a list open, or a pick whose change is on its way): a refill (after a pick by
            // arrow key, a device unplugged) would give it to another item or lose it. Caught up once the pick is through
            // (the timer), or as a list is about to open.
            refreshWaiting = anyListBusy();
            if (refreshWaiting)
                return;

            const juce::ScopedValueSetter<bool> guard (refreshing, true);
            const auto current = engine.getOpenDevice();
            shownDevice = current;
            types = AudioBackends::availableTypes (engine.getDeviceManager());
            // a type whose device is being chosen stays up until one of its devices runs (or the type is gone)
            if ((current.input.isNotEmpty() && current.type == choosingType) || ! types.contains (choosingType))
                choosingType = {};
            const bool choosing = choosingType.isNotEmpty();
            typeCombo.clear (juce::dontSendNotification);
            for (int i = 0; i < types.size(); ++i)
                typeCombo.addItem (AudioBackends::label (types[i]), i + 1);
            // With no open device (including safe mode), offer the first available type without opening it.
            const auto showing = choosing ? choosingType : current.type;
            const int typeIndex = types.contains (showing) ? types.indexOf (showing) : (types.isEmpty() ? -1 : 0);
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
                deviceCombo.addRepickableItem (names[i], i + 1);
            deviceCombo.setSelectedId (choosing ? 0 : names.indexOf (current.input) + 1, juce::dontSendNotification);
            deviceCombo.setTextWhenNothingSelected (! asio ? ko ("입력 장치 없음") : names.isEmpty() ? ko ("ASIO 장치 없음") : ko ("ASIO 장치를 고르세요"));
            outputCombo.clear (juce::dontSendNotification);
            outputCombo.addRepickableItem (ko ("없음 (OBS로만 보내기)"), 1);
            for (int i = 0; i < outputNames.size(); ++i) outputCombo.addRepickableItem (outputNames[i], i + 2);
            outputCombo.setSelectedId (current.output.isEmpty() ? 1 : outputNames.indexOf (current.output) + 2, juce::dontSendNotification);
            outputCaption.setVisible (! asio);
            outputCombo.setVisible (! asio);
            rateCaption.setVisible (true);   // ASIO too: some drivers' own panels cannot change the rate (2026-09-29)
            rateCombo.setVisible (true);
            bufferCaption.setVisible (! shared);
            bufferCombo.setVisible (! shared);
            sharedBufferNote.setVisible (shared);
            bufferCombo.clear (juce::dontSendNotification);
            // Windows audio converts, so 44.1k / 48k are always offered; an ASIO driver offers only its own rates (JUCE
            // would open a rate it does not list at another one), within 44.1 - 192 kHz: FlexASIO claims all 21 from 8k
            // to 768k. The rate it runs at is shown even outside. With nothing open getOpenDevice() says 48 kHz, but no
            // ASIO rate is known then.
            juce::Array<double> rates;
            if (! asio)
                rates.addArray ({ 44100.0, 48000.0 });
            const double shownRate = asio && (current.input.isEmpty() || choosing) ? 0.0 : current.sampleRate;
            panelButton.setVisible (false);
            // while another type's device plays, its rates, buffers and panel are not the chosen type's
            if (auto* device = engine.getDeviceManager().getCurrentAudioDevice(); device != nullptr && ! choosing)
            {
                const auto sizes = device->getAvailableBufferSizes();
                for (int i = 0; i < sizes.size(); ++i)
                    bufferCombo.addItem (juce::String (sizes[i]) + ko (" 샘플") + "  (" + juce::String (1000.0 * sizes[i] / juce::jmax (1.0, device->getCurrentSampleRate()), 1) + " ms)", sizes[i]);
                bufferCombo.setSelectedId (current.bufferSize, juce::dontSendNotification);
                for (auto rate : device->getAvailableSampleRates())   // a closed ASIO device (a failed reset) offers none
                    if (rate > 0.0 && (! asio || (device->isOpen() && rate >= 44100.0 && rate <= 192000.0))) rates.addIfNotAlreadyThere (rate);
                panelButton.setVisible (asio && device->hasControlPanel());
            }
            if (shownRate > 0.0)
                rates.addIfNotAlreadyThere (shownRate);
            rates.sort();
            rateCombo.clear (juce::dontSendNotification);
            for (auto rate : rates) rateCombo.addItem (juce::String (juce::roundToInt (rate)) + " Hz", juce::roundToInt (rate));
            rateCombo.setTextWhenNothingSelected (ko ("장치 없음"));
            rateCombo.setSelectedId (juce::roundToInt (shownRate > 0.0 ? shownRate : (asio ? 0.0 : 48000.0)), juce::dontSendNotification);
            for (auto* box : { &typeCombo, &deviceCombo, &outputCombo, &rateCombo, &bufferCombo })
                box->settle();   // selected from here, nothing for an onChange to hand over
            juce::String note = ko ("USB 마이크·헤드셋 같은 일반 장치를 씁니다. 마이크와 모니터가 서로 다른 장치면 샘플레이트 차이를 자동으로 맞춥니다 (모니터 지연이 조금 늘어납니다).");
            if (shownType == "Windows Audio (Low Latency Mode)") note += ko (" 지원하지 않는 장치면 일반 모드로 여세요.");
            if (shownType == "Windows Audio (Exclusive Mode)") note += ko (" 독점 모드에서는 OBS 등 다른 프로그램이 같은 마이크를 쓸 수 없습니다.");
            deviceNote.setText (asio ? ko ("ASIO 장치만 씁니다. 버퍼가 작을수록 지연이 짧고 끊길 위험이 큽니다 (128~256 권장).") : note, juce::dontSendNotification);
            bitDepthCombo.setSelectedId (DeviceFormatText::choiceId (current.sampleFormat), juce::dontSendNotification);
            bitDepthCombo.settle();
            syncedChoiceId = DeviceFormatText::choiceId (current.sampleFormat);
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
            pendingRate = 0.0;
            const auto current = engine.getOpenDevice();
            MixDevice wanted;
            wanted.sampleFormat = current.sampleFormat;
            wanted.type = types[typeCombo.getSelectedId() - 1];
            const bool backToRunning = choosingType.isNotEmpty() && current.input.isNotEmpty() && wanted.type == current.type;
            choosingType = {};
            if (backToRunning)   // left the type being chosen for the one that plays
            {
                // nothing to reopen while it plays whole; a part stopped meanwhile (a separate monitor output that did
                // not come back from an unplug) is reopened as it was
                if (engine.isRunningWhole())
                    refreshDevices();
                else
                    applyDevice (current);
                return;
            }
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
            if (wanted.isAsio())
            {
                // The first driver listed is tried. One whose hardware is not plugged in (Ableton's driver, listed before a
                // TOPPING interface's: "No device is connected to the PC." - 2026-10-06) leaves the ASIO list up to pick the
                // right one, the running device kept - quietly, as nobody picked that driver.
                choosingType = wanted.type;
                applyDevice (wanted, true);
                return;
            }
            applyDevice (wanted);
        }

        /** The device or monitor output already selected, picked again: the device that runs is reopened as it is when
            part of it stopped (the status line sends the operator here once it is plugged back in); one that plays whole
            is left. One no longer open (closed under a list left open) is not reopened from what that list showed: the
            refresh that follows lists what there is to pick afresh. */
        void reopenIfStopped()
        {
            if (refreshing || choosingType.isNotEmpty() || engine.isRunningWhole()) return;
            const auto current = engine.getOpenDevice();
            if (current.input.isEmpty() || current.type != shownType || current.input != deviceCombo.getText()) return;
            applyDevice (current);
        }

        /** 'rateChosen': the sample rate box changed. For ASIO that rate is asked of the driver; another change (device,
            buffer) keeps the rate it runs at, as before. */
        void applySelection (bool rateChosen = false)
        {
            if (refreshing) return;
            auto wanted = engine.getOpenDevice();
            const bool hadDevice = wanted.input.isNotEmpty() && choosingType.isEmpty();   // a device of the shown type plays
            wanted.type = shownType;
            wanted.input = deviceCombo.getSelectedId() > 0 ? deviceCombo.getText() : juce::String();
            wanted.output = wanted.isAsio() ? wanted.input : outputNames[outputCombo.getSelectedId() - 2];
            if (wanted.isAsio())
                wanted.sampleRate = rateChosen && rateCombo.getSelectedId() > 0 ? (double) rateCombo.getSelectedId()
                                                                                 : (hadDevice ? wanted.sampleRate : 0.0);
            else
                wanted.sampleRate = (double) rateCombo.getSelectedId();
            wanted.bufferSize = shownType == "Windows Audio" ? 0 : bufferCombo.getSelectedId();
            if (shownType == "Windows Audio (Exclusive Mode)")
                wanted.sampleFormat = DeviceFormatText::choice (bitDepthCombo.getSelectedId());
            pendingRate = 0.0;
            const bool opened = applyDevice (wanted);

            // a driver may list a rate and still not switch to it (an external clock, a fixed rate): say what it runs at.
            // A failed open has said so already (and rolled back). One that switched can still be undone by the reset
            // many drivers ask for after a rate change: followDevice() watches for that a few seconds.
            if (opened && rateChosen && wanted.isAsio() && wanted.sampleRate > 0.0)
                if (auto* device = engine.getDeviceManager().getCurrentAudioDevice(); device != nullptr && device->isOpen())
                {
                    if (juce::roundToInt (device->getCurrentSampleRate()) != juce::roundToInt (wanted.sampleRate))
                    {
                        showRateNotApplied (wanted.sampleRate, device->getCurrentSampleRate());
                    }
                    else
                    {
                        pendingRate = wanted.sampleRate;
                        pendingSince = juce::Time::getMillisecondCounter();
                        pendingOpenCount = engine.getOpenCount();
                    }
                }
        }

        void showRateNotApplied (double wantedRate, double actualRate)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("샘플레이트를 바꾸지 못했습니다"),
                ko ("ASIO 드라이버가 ") + juce::String (juce::roundToInt (wantedRate)) + ko (" Hz로 바꾸지 않아 지금 ")
                    + juce::String (juce::roundToInt (actualRate)) + ko (" Hz로 동작합니다. 장치의 클럭(외부 동기) 설정이나 ASIO 제어판을 확인하세요."),
                ko ("확인"));
        }

        /** The device can change under the open dialog (a driver reset, a session opened from Explorer): show what runs,
            unless a list is open. A rate asked for moments ago that a reset then left is said like a refused one. */
        void followDevice()
        {
            const auto now = engine.getOpenDevice();

            // a reopen the app asked for since (a session, another choice) is no reset; after 3 s neither is anything
            if (pendingRate > 0.0 && (engine.getOpenCount() != pendingOpenCount || juce::Time::getMillisecondCounter() - pendingSince > 3000))
                pendingRate = 0.0;

            if (pendingRate > 0.0 && now.isAsio() && now.input.isNotEmpty() && juce::roundToInt (now.sampleRate) != juce::roundToInt (pendingRate))
            {
                showRateNotApplied (pendingRate, now.sampleRate);
                pendingRate = 0.0;
            }

            // queued behind what is already waiting: a choice just made in a list posts its onChange, which goes first
            if (deviceChangedUnderDialog())
                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<SettingsContent> (this)]
                {
                    if (safe != nullptr && safe->deviceChangedUnderDialog())
                        safe->refreshDevices();
                });
        }

        bool deviceChangedUnderDialog()
        {
            const auto now = engine.getOpenDevice();
            const bool changed = now.type != shownDevice.type || now.input != shownDevice.input || now.output != shownDevice.output
                                 || now.bufferSize != shownDevice.bufferSize || juce::roundToInt (now.sampleRate) != juce::roundToInt (shownDevice.sampleRate);
            return changed && ! anyListBusy();
        }

        /** The operator is in the middle of a pick from a list whose selection refreshDevices() sets (open, or a pick
            whose change is on its way). */
        bool anyListBusy() const
        {
            const RepickComboBox* boxes[] { &typeCombo, &deviceCombo, &outputCombo, &rateCombo, &bufferCombo, &bitDepthCombo };
            return std::any_of (std::begin (boxes), std::end (boxes), [] (const RepickComboBox* box) { return box->busy(); });
        }

        /** 'quietIfKept': a failure that left the running device as it was says nothing (a driver tried, not picked). */
        bool applyDevice (const MixDevice& wanted, bool quietIfKept = false)
        {
            const auto before = engine.getOpenDevice();
            const auto error = engine.openDevice (wanted);
            const auto after = engine.getOpenDevice();
            const bool kept = after.type == before.type && after.input == before.input && after.output == before.output
                              && engine.isDeviceRunning() == before.input.isNotEmpty();
            if (error.isNotEmpty() && ! (quietIfKept && kept))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, ko ("오디오 장치를 열지 못했습니다"), error, ko ("확인"));
            refreshDevices();
            // only a device that opened is the operator's choice: a failure leaves the session asking for its own device
            auto& notify = error.isEmpty() ? onDeviceChanged : onOpenFailed;
            if (notify)
                notify();
            return error.isEmpty();
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
            if (getAcceptedFormats)
            {
                const auto accepted = getAcceptedFormats();
                format.inputAccepted = accepted.first;
                format.outputAccepted = accepted.second;
            }
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
            // Changing an item's text leaves the old text shown and getSelectedId() at 0, so a choice is selected again
            // afterwards: the running device's, unless the operator has just picked another one whose asynchronous
            // onChange has not run yet (it differs from what was last synced) - that pick is kept for its onChange.
            const int runningChoice = DeviceFormatText::choiceId (engine.getOpenDevice().sampleFormat);
            const int shownChoice = bitDepthCombo.getSelectedId();
            const int choice = shownChoice > 0 && shownChoice != syncedChoiceId ? shownChoice : runningChoice;
            for (int id = 1; id <= 5; ++id)
            {
                const auto item = DeviceFormatText::exclusiveItem (id, format, current.input.isNotEmpty(), current.output.isNotEmpty());
                bitDepthCombo.changeItemText (id, item.text);
                bitDepthCombo.setItemEnabled (id, item.enabled);
            }
            bitDepthCombo.setSelectedId (choice, juce::dontSendNotification);
            if (choice == runningChoice)   // no pick of the operator's on its way: what is selected is handled
            {
                syncedChoiceId = runningChoice;
                bitDepthCombo.settle();
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
            setSize (560, 848 + 28 + MixSession::maxPluginGroups * 36 - 160 + deviceHeight);
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
            sendTransport.setBounds (area.removeFromTop (28));
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
        void timerCallback() override
        {
            refreshControlStatus();
            if (refreshWaiting && ! anyListBusy())   // a refill skipped in the middle of a pick, now that it is through
                refreshDevices();
            followDevice();
            refreshBitDepth();
        }

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
        std::function<void()> onDeviceChanged, onOpenFailed, onHotkeysChanged;
        std::function<void (bool)> onHotkeyCapture;
        std::function<ControlServer::Status()> getControlStatus;
        std::function<void (bool)> onControlEnabled;
        SettingsDialog::AcceptedFormatsQuery getAcceptedFormats;
        juce::StringArray types, names, outputNames;
        juce::String shownType;
        juce::String choosingType;      // shown while another type's device plays: switched to, its first driver did not open
        bool refreshWaiting = false;    // refreshDevices() skipped in the middle of a pick
        MixDevice shownDevice;          // what refreshDevices() last showed; followDevice() compares the running one
        double pendingRate = 0.0;       // an ASIO rate that just opened, watched for a reset that leaves it (3 s)
        juce::uint32 pendingSince = 0;
        int pendingOpenCount = 0;       // engine.getOpenCount() then: any later openDevice() is no reset
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
        RepickComboBox typeCombo, deviceCombo, outputCombo, rateCombo, bufferCombo, bitDepthCombo;
        juce::TextButton panelButton, soundSettingsButton;
        juce::ToggleButton minimiseToTray, closeAsk, closeToTray, startWithWindows, skipWhenOff, sendTransport, externalControl;
        bool refreshing = false;
        int syncedChoiceId = 0;   // the bit-depth choice the combo was last synced to from the running device
    };

    juce::Component::SafePointer<juce::DialogWindow> openDialog;
}

namespace
{
    constexpr int shortestHeight = 320;   // its resize limit
    constexpr int tallestOpening = 640;   // the device and the window switches show; the hotkeys and below scroll

    struct WindowLimits { SettingsDialog::ClientLimits limits; };   // a base before the window's: alive through all of its teardown

    /** The settings' own window: closing it (the title bar, Esc) deletes it - not modal, so the mics stay usable meanwhile. */
    class SettingsWindow : private WindowLimits,
                           public juce::DialogWindow
    {
    public:
        SettingsWindow() : DialogWindow (ko ("설정"), Palette::card, true, true)
        {
            limits.frameNow = [this] { return getPeer() != nullptr ? getPeer()->getFrameSize() : juce::BorderSize<int>(); };
        }

        ~SettingsWindow() override { limits.frameNow = nullptr; }   // the window's own teardown must not ask a half-gone window

        void useClientLimits (const SettingsDialog::Placement& place)
        {
            limits.setClientLimits (place.minWidth, place.maxWidth, place.minHeight, place.maxHeight);
            setConstrainer (&limits);
        }

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
                           std::function<ControlServer::Status()> controlStatus, std::function<void (bool)> controlEnabled,
                           AcceptedFormatsQuery acceptedFormats, std::function<void()> onOpenFailed)
{
    if (openDialog != nullptr)
    {
        openDialog->toFront (true);
        return;
    }

    auto* content = new SettingsContent (engine, settings, std::move (onDeviceChanged), std::move (onHotkeysChanged), std::move (onHotkeyCapture),
                                         std::move (controlStatus), std::move (controlEnabled), std::move (acceptedFormats),
                                         std::move (onOpenFailed));
    auto* scroller = new juce::Viewport();
    scroller->setViewedComponent (content, true);
    scroller->setScrollBarsShown (true, true);   // sideways only when a narrow display squeezed the window under the content's width
    scroller->setSize (content->getWidth() + scroller->getScrollBarThickness(), content->getHeight());

    auto* window = new SettingsWindow();
    window->setUsingNativeTitleBar (true);
    window->setContentOwned (scroller, true);   // the window is shorter than the settings: they scroll
    window->setResizable (true, false);

    // centred on the app (none: on the main screen), on its screen title bar and all; the limits are for the inside
    // (setResizeLimits holds the whole window, frame included: the bare width squeezed the settings 16 px narrower)
    const auto frame = window->getPeer() != nullptr ? window->getPeer()->getFrameSize() : juce::BorderSize<int> (31, 8, 8, 8);
    const auto& displays = juce::Desktop::getInstance().getDisplays();
    const auto around = centreAround != nullptr ? centreAround->getScreenBounds() : juce::Rectangle<int>();
    const auto* display = around.isEmpty() ? displays.getPrimaryDisplay() : displays.getDisplayForRect (around);
    const auto screen = display != nullptr ? display->userBounds.toNearestInt() : juce::Rectangle<int> (0, 0, 1920, 1032);
    const auto place = placement (content->getWidth() + scroller->getScrollBarThickness(), content->getHeight(),
                                  around.isEmpty() ? screen.getCentre() : around.getCentre(), screen, frame);
    window->useClientLimits (place);
    window->setBounds (place.bounds);

    window->setVisible (true);
    window->toFront (true);
    openDialog = window;
}

void SettingsDialog::closeIfOpen()
{
    openDialog.deleteAndZero();
}

SettingsDialog::Placement SettingsDialog::placement (int width, int contentHeight, juce::Point<int> centre, juce::Rectangle<int> screen,
                                                     juce::BorderSize<int> frame)
{
    const auto room = screen.reduced (12);
    const int tallest = juce::jmin (tallestOpening, juce::jmax (shortestHeight, juce::roundToInt (screen.getHeight() * 0.7)));
    const int w = juce::jmax (1, juce::jmin (width, room.getWidth() - frame.getLeftAndRight()));   // narrower only on a screen narrower than the settings: they scroll sideways
    const int h = juce::jmax (1, juce::jmin (contentHeight, tallest, room.getHeight() - frame.getTopAndBottom()));
    Placement p;
    p.bounds = frame.subtractedFrom (frame.addedTo (juce::Rectangle<int> (w, h)).withCentre (centre).constrainedWithin (room));
    p.minWidth = w;
    p.maxWidth = width;
    p.minHeight = juce::jmin (shortestHeight, h);   // a tiny screen's fitted height, not pushed back off it
    p.maxHeight = juce::jmax (contentHeight, h);
    return p;
}

SettingsDialog::ClientLimits::ClientLimits()
{
    setMinimumOnscreenAmounts (0x10000, 16, 24, 16);   // as ResizableWindow's own constrainer: the title bar stays reachable
}

void SettingsDialog::ClientLimits::setClientLimits (int minWidth, int maxWidth, int minHeight, int maxHeight)
{
    minW = minWidth;
    maxW = maxWidth;
    minH = minHeight;
    maxH = maxHeight;
}

void SettingsDialog::ClientLimits::checkBounds (juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previous, const juce::Rectangle<int>& limits,
                                                bool stretchingTop, bool stretchingLeft, bool stretchingBottom, bool stretchingRight)
{
    const auto frame = frameNow ? frameNow() : juce::BorderSize<int>();
    setSizeLimits (minW + frame.getLeftAndRight(), minH + frame.getTopAndBottom(), maxW + frame.getLeftAndRight(), maxH + frame.getTopAndBottom());
    ComponentBoundsConstrainer::checkBounds (bounds, previous, limits, stretchingTop, stretchingLeft, stretchingBottom, stretchingRight);
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
