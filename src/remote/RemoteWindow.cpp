#include "remote/RemoteWindow.h"
#include "app/UiScale.h"
#include <cmath>

namespace gocue::remote
{
Panel::Panel (Service& s, std::function<void()> newKey, std::function<void()> onEnabled)
    : service (s), enabledChanged (std::move (onEnabled))
{
    const auto label = [this] (juce::Label& l, const char* text)
    {
        l.setText (ko (text), juce::dontSendNotification);
        l.setFont (Palette::font (Palette::fieldLabelSize, true));
        l.setColour (juce::Label::textColourId, Palette::dimText);
        l.setBorderSize (juce::BorderSize<int> (0));
        addAndMakeVisible (l);
    };
    label (modeLabel, "웹리모트"); label (addressLabel, "주소"); label (networkLabel, "네트워크"); label (devicesLabel, "연결된 기기 0");
    const auto button = [this] (juce::TextButton& b, const char* text)
    { b.setButtonText (ko (text)); addAndMakeVisible (b); };
    button (off, "꺼짐"); button (on, "켜짐"); button (copy, "복사"); button (renew, "QR 새로 만들기");
    for (auto* b : { &off, &on })
    {
        b->getProperties().set ("slateSegment", true);
        b->setColour (juce::TextButton::buttonColourId, Palette::panel);
        b->setColour (juce::TextButton::buttonOnColourId, Palette::accent);
        b->setColour (juce::TextButton::textColourOffId, Palette::muted);
        b->setColour (juce::TextButton::textColourOnId, Palette::background);
    }
    off.setConnectedEdges (juce::Button::ConnectedOnRight);
    on.setConnectedEdges (juce::Button::ConnectedOnLeft);
    off.onClick = [this] { service.setEnabled (false); if (enabledChanged) enabledChanged(); };
    on.onClick = [this] { service.setEnabled (true); if (enabledChanged) enabledChanged(); };
    copy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (service.link()); };
    renew.onClick = std::move (newKey);
    addressField.setReadOnly (true);
    addressField.setFont (Palette::monoFont (Palette::fieldValueSize));
    addressField.setMultiLine (false);
    addressField.setJustification (juce::Justification::centredLeft);
    addressField.setCaretVisible (false);
    addAndMakeVisible (addressField);
    network.onChange = [this] { service.selectNetwork (network.getSelectedItemIndex()); };
    addAndMakeVisible (network);
    deviceList.setModel (this);
    deviceList.setRowHeight (40);
    deviceList.setColour (juce::ListBox::backgroundColourId, Palette::transparent);
    deviceList.setOutlineThickness (0);
    deviceList.setWantsKeyboardFocus (false);
    deviceList.getViewport()->setScrollBarThickness (Palette::scrollBarWidth);
    addAndMakeVisible (deviceList);
    service.addChangeListener (this);
    refresh();
    setSize (820, 470);
}
Panel::~Panel() { service.removeChangeListener (this); deviceList.setModel (nullptr); }
void Panel::refresh()
{
    off.setToggleState (! service.isEnabled(), juce::dontSendNotification);
    on.setToggleState (service.isEnabled(), juce::dontSendNotification);
    addressField.setText (service.failedToOpenPort() ? ko ("포트를 열 수 없습니다") : service.address(), false);
    copy.setEnabled (service.link().isNotEmpty());
    network.clear (juce::dontSendNotification);
    int index = 1;
    for (const auto& address : service.networks())
        network.addItem (juce::String (address.name) + ko (" · ") + juce::String (address.ip) + (address.recommended ? ko (" (추천)") : juce::String()), index++);
    network.setSelectedItemIndex (service.selectedNetwork(), juce::dontSendNotification);
    network.setTooltip (network.getText());
    devices.clear();
    for (const auto& entry : service.devices())
        devices.push_back ({ entry.second.name, entry.second.since.formatted ("%H:%M") + ko (" 연결") });
    devicesLabel.setText (ko ("연결된 기기 ") + juce::String (devices.size()), juce::dontSendNotification);
    deviceList.updateContent();
    deviceList.repaint();
    const auto nextLink = service.link();
    if (qrLink != nextLink)
    {
        qrLink = nextLink;
        qr.reset();
        qrImage = {};
        if (qrLink.isNotEmpty())
        {
            qr = std::make_unique<qrcodegen::QrCode> (qrcodegen::QrCode::encodeText (qrLink.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM));
            qrImage = juce::Image (juce::Image::RGB, qr->getSize(), qr->getSize(), false);
            for (int y = 0; y < qr->getSize(); ++y)
                for (int x = 0; x < qr->getSize(); ++x)
                    qrImage.setPixelAt (x, y, qr->getModule (x, y) ? Palette::background : Palette::accentInk);
        }
    }
    repaint();
}
void Panel::resized()
{
    auto area = getLocalBounds().reduced (24);
    const int tile = juce::jlimit (220, 320, (area.getWidth() - 28) * 42 / 100);
    qrBounds = area.removeFromLeft (tile).withHeight (tile);
    area.removeFromLeft (28);
    modeLabel.setBounds (area.removeFromTop (20));
    segmentBounds = area.removeFromTop (40).withWidth (144);
    off.setBounds (segmentBounds.withWidth (72));
    on.setBounds (segmentBounds.withTrimmedLeft (72));
    area.removeFromTop (18);
    addressLabel.setBounds (area.removeFromTop (20));
    auto address = area.removeFromTop (42);
    copy.setBounds (address.removeFromRight (76));
    address.removeFromRight (8);
    addressField.setBounds (address);
    area.removeFromTop (16);
    networkLabel.setBounds (area.removeFromTop (20));
    network.setBounds (area.removeFromTop (38));
    area.removeFromTop (16);
    devicesLabel.setBounds (area.removeFromTop (20));
    renew.setBounds (area.removeFromBottom (42).removeFromRight (136));
    area.removeFromBottom (16);
    deviceList.setBounds (area);
}
void Panel::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);
    g.setColour (Palette::accentInk.withAlpha (qr != nullptr ? 1.0f : Palette::disabledAlpha));
    g.fillRoundedRectangle (qrBounds.toFloat(), Palette::cornerRadius);
    if (qr != nullptr)
    {
        constexpr int quiet = 4;
        const int cells = qr->getSize() + quiet * 2;
        const float scale = static_cast<float> (qrBounds.getWidth()) / static_cast<float> (cells);
        // Nearest-neighbour bitmap scaling keeps joined modules solid at 125/150% DPI.
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (qrImage, qrBounds.toFloat().reduced (quiet * scale));
    }
    g.setColour (Palette::panel);
    g.fillRoundedRectangle (deviceList.getBounds().toFloat(), Palette::cornerRadius);
}
void Panel::paintOverChildren (juce::Graphics& g)
{
    g.setColour (Palette::outline);
    g.drawRoundedRectangle (segmentBounds.toFloat().reduced (0.5f), Palette::cornerRadius, Palette::borderWidth);
}
void Panel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (row, static_cast<int> (devices.size()))) return;
    const auto& device = devices[static_cast<size_t> (row)];
    g.setColour (Palette::playing);
    g.fillEllipse (12.0f, static_cast<float> (height / 2 - 3), 6.0f, 6.0f);
    g.setColour (Palette::text);
    g.setFont (Palette::font());
    g.drawText (device.name, 26, 0, juce::jmax (0, width - 118), height, juce::Justification::centredLeft, true);
    g.setColour (Palette::dimText);
    g.setFont (Palette::font (Palette::fileSize));
    g.drawText (device.time, width - 88, 0, 76, height, juce::Justification::centredRight, true);
    if (row + 1 < static_cast<int> (devices.size()))
    { g.setColour (Palette::outline); g.fillRect (12, height - 1, juce::jmax (0, width - 24), 1); }
}
juce::String Panel::getTooltipForRow (int row)
{
    if (! juce::isPositiveAndBelow (row, static_cast<int> (devices.size()))) return {};
    const auto& device = devices[static_cast<size_t> (row)];
    return device.name + "\n" + device.time;
}
Window::Window (Service& service, std::function<void()> renew, std::function<void()> enabledChanged)
    : DocumentWindow (ko ("외부기기 연동"), Palette::background, DocumentWindow::closeButton, false)
{
    setLookAndFeel (&theme);
    setUsingNativeTitleBar (false);
    setTitleBarHeight (Palette::menuBarHeight);
    setContentOwned (new Panel (service, std::move (renew), std::move (enabledChanged)), true);
    setResizable (true, false);
    setResizeLimits (720, 480, Palette::windowMaxSize, Palette::windowMaxSize);
    centreWithSize (820, 504);
}
Window::~Window() { clearContentComponent(); setLookAndFeel (nullptr); }
void Window::open()
{
    if (! isOnDesktop()) addToDesktop (getDesktopWindowStyleFlags());
    UiScale::fitWindowIntoDisplay (*this);
    setVisible (true);
    toFront (true);
}
void Window::resized() { DocumentWindow::resized(); UiScale::fitOnResized (*this, fitting); }
}
