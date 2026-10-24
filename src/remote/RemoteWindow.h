#pragma once
#include "remote/RemoteService.h"
#include "ui/GoCueLookAndFeel.h"
#include "qrcodegen.hpp"

namespace gocue::remote
{
class Panel : public juce::Component, private juce::ChangeListener, private juce::ListBoxModel
{
public:
    Panel (Service&, std::function<void()> renew, std::function<void()> enabledChanged);
    ~Panel() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
private:
    void refresh();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    int getNumRows() override { return static_cast<int> (devices.size()); }
    juce::String getTooltipForRow (int row) override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    Service& service;
    std::function<void()> enabledChanged;
    juce::Label modeLabel, addressLabel, networkLabel, devicesLabel;
    juce::TextButton off, on, copy, renew;
    juce::TextEditor addressField;
    juce::ComboBox network;
    juce::ListBox deviceList;
    struct DeviceRow { juce::String name, time; };
    std::vector<DeviceRow> devices;
    juce::Rectangle<int> qrBounds, segmentBounds;
    juce::String qrLink;
    std::unique_ptr<qrcodegen::QrCode> qr;
    juce::Image qrImage;
};

class Window : public juce::DocumentWindow
{
public:
    Window (Service&, std::function<void()> renew, std::function<void()> enabledChanged);
    ~Window() override;
    void open();
    void closeButtonPressed() override { setVisible (false); }
    void resized() override;
private:
    GoCueLookAndFeel theme;
    bool fitting = false;
};
}
