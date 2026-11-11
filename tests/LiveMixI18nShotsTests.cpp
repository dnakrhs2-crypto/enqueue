#include "LiveMixI18nShots.h"
#include "TestGainPlugin.h"
#include "ui/LiveMixLookAndFeel.h"
#include "../livemix/src/ui/MainComponent.h"
#include "ui/SettingsDialog.h"
#include "ui/CollabWindow.h"
#include "ui/RecordingWindow.h"
#include "ui/PluginManagerWindow.h"
#include "ui/PluginGroupsWindow.h"
#include "ui/LoudnessWindow.h"
#include "PluginPreset.h"
#include <regex>
#include <stdexcept>
#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace gocue::tests::i18nshots
{
namespace lm = gocue::livemix;
namespace
{
template<class Tag, typename Tag::Type value> struct Access { friend typename Tag::Type member (Tag) { return value; } };
struct About { using Type = void (lm::MainComponent::*)(); friend Type member (About); };
struct HyperlinkFont { using Type = juce::Font (juce::HyperlinkButton::*)() const; friend Type member (HyperlinkFont); };
struct GlyphFont { using Type = juce::Font juce::PositionedGlyph::*; friend Type member (GlyphFont); };
template struct Access<About, &lm::MainComponent::showAbout>;
template struct Access<HyperlinkFont, &juce::HyperlinkButton::getFontToUse>;
template struct Access<GlyphFont, &juce::PositionedGlyph::font>;

juce::Rectangle<int> visibleArea (juce::Component& root, juce::Component& c)
{
    auto area = c.getLocalBounds();
    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
    {
        if ((p != &root && ! p->isVisible()) || p->getAlpha() <= 0.0f) return {};
        area = area.getIntersection (c.getLocalArea (p, p->getLocalBounds()));
        if (p == &root) return area;
    }
    return {};
}
juce::String oneLine (const juce::String& text)
{
    return text.replace ("\r", "\\r").replace ("\n", "\\n").replace ("\t", "\\t");
}
juce::AlertWindow* currentAlert()
{
    for (int i = 0; i < juce::Component::getNumCurrentlyModalComponents(); ++i)
        if (auto* alert = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent (i))) return alert;
    return nullptr;
}
}

void dispatch()
{
    // The test target deliberately disables JUCE_MODAL_LOOPS_PERMITTED, like the production app.
    const auto drain = []
    {
       #if JUCE_WINDOWS
        MSG message {};
        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage (&message); DispatchMessageW (&message); }
       #endif
    };
    // Drain the whole queue: closing/resizing many real windows may queue more than a fixed message count.
    drain();
    juce::Timer::callPendingTimersSynchronously();
    drain();
}
juce::Component* findComponent (juce::Component& root, const std::function<bool (juce::Component&)>& matches)
{
    if (matches (root)) return &root;
    for (auto* child : root.getChildren())
        if (auto* found = findComponent (*child, matches)) return found;
    return nullptr;
}
juce::DialogWindow* dialogNamed (const juce::String& title)
{
    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* window = dynamic_cast<juce::DialogWindow*> (desktop.getComponent (i)); window != nullptr && window->getName() == title)
            return window;
    return nullptr;
}

Capture::Capture (juce::UnitTest& runner, const juce::File& destination) : test (runner), folder (destination)
{
    test.expect (folder.createDirectory().wasOk(), folder.getFullPathName());
    // Repeated opt-in runs replace only files named by this test's previous manifest.
    const auto old = juce::StringArray::fromLines (folder.getChildFile ("manifest.tsv").loadFileAsString());
    for (const auto& line : old)
    {
        const auto name = line.upToFirstOccurrenceOf ("\t", false, false);
        if (name.endsWith (".png") && ! name.containsAnyOf ("/\\:"))
            test.expect (folder.getChildFile (name).deleteFile());
    }
}

void Capture::measure (const juce::String& image, const juce::String& type, const juce::String& text,
                       const juce::Font& font, juce::Rectangle<int> area, bool multiline, float minimumScale, int maxLines)
{
    if (text.isEmpty() || area.isEmpty()) return;
    float width = 0;
    for (const auto& line : juce::StringArray::fromLines (text))
        width = juce::jmax (width, juce::GlyphArrangement::getStringWidth (font, line));
    const auto prefix = image + "\t" + type + "\t" + oneLine (text) + "\t";
    juce::GlyphArrangement fitted;
    fitted.addFittedText (font, text, 0, 0, (float) area.getWidth(), (float) area.getHeight(),
                         juce::Justification::centredLeft, juce::jmax (1, maxLines), minimumScale);
    float scale = 1.0f;
    for (int i = 0; i < fitted.getNumGlyphs(); ++i)
        scale = juce::jmin (scale, (fitted.getGlyph (i).*member (GlyphFont {})).getHorizontalScale() / font.getHorizontalScale());
    if (multiline)
    {
        juce::AttributedString attributed;
        attributed.setWordWrap (juce::AttributedString::byWord);
        attributed.append (text, font, juce::Colours::white);
        juce::TextLayout layout;
        layout.createLayout (attributed, (float) area.getWidth());
        if (layout.getHeight() > (float) area.getHeight() + 0.5f)
            overflow.add (prefix + "height=" + juce::String (layout.getHeight(), 2)
                          + "\tboxHeight=" + juce::String (area.getHeight()) + "\tboxWidth=" + juce::String (area.getWidth()));
        // A single token may still be wider than its wrapped line.
        if (layout.getWidth() <= (float) area.getWidth() + 0.5f)
        {
            // A Label may squeeze its letters even when a normal wrapped layout would fit.
            if (! type.startsWith ("Label") || scale >= 0.999f) return;
        }
        else width = layout.getWidth();
    }
    if (width <= (float) area.getWidth() + 0.5f) return;
    overflow.add (prefix + "width=" + juce::String (width, 2) + "\tboxWidth=" + juce::String (area.getWidth())
                  + "\tneededScale=" + juce::String ((float) area.getWidth() / width, 3)
                  + "\tfittedScale=" + juce::String (scale, 3) + "\tminimumScale=" + juce::String (minimumScale, 3));
}

void Capture::inspect (juce::Component& root, juce::Component& node, const juce::String& image)
{
    const auto clip = visibleArea (root, node);
    if (clip.isEmpty()) return;
    auto& look = node.getLookAndFeel();
    const auto id = node.getComponentID().isEmpty() ? juce::String() : "#" + node.getComponentID();
    auto check = [&] (const char* type, const juce::String& text, const juce::Font& font, juce::Rectangle<int> area,
                     bool multi = false, float scale = 1.0f, int lines = 1)
    {
        // A Viewport may intentionally show only part of a label. Measure its allocated text area, not that scroll crop.
        measure (image, juce::String (type) + id, text, font, area, multi, scale, lines);
    };

    if (auto* combo = dynamic_cast<juce::ComboBox*> (&node))
    {
        if (auto* label = dynamic_cast<juce::Label*> (findComponent (*combo, [] (auto& c) { return dynamic_cast<juce::Label*> (&c) != nullptr; })))
            check ("ComboBox", combo->getText().isNotEmpty() ? combo->getText() : combo->getTextWhenNothingSelected(),
                   look.getComboBoxFont (*combo), combo->getLocalArea (label, label->getBorderSize().subtractedFrom (label->getLocalBounds())));
        return; // its internal Label is the same visible text, not a second finding
    }
    if (auto* label = dynamic_cast<juce::Label*> (&node))
    {
        // AlertWindow has a transparent Label solely for screen readers; its visible title/message are custom paint.
        if (label->findColour (juce::Label::textColourId).isTransparent()) return;
        const auto font = look.getLabelFont (*label);
        const auto area = label->getBorderSize().subtractedFrom (label->getLocalBounds());
        const int lines = juce::jmax (1, (int) ((float) area.getHeight() / font.getHeight()));
        const auto text = dynamic_cast<lm::TwoLineLabel*> (label) != nullptr ? static_cast<lm::TwoLineLabel*> (label)->getShownText() : label->getText();
        check ("Label", text, font, area, lines > 1 || text.containsChar ('\n'), label->getMinimumHorizontalScale(), lines);
    }
    else if (auto* link = dynamic_cast<juce::HyperlinkButton*> (&node))
        check ("HyperlinkButton", link->getButtonText(), (link->*member (HyperlinkFont {}))(), link->getLocalBounds().reduced (1, 0));
    else if (auto* tab = dynamic_cast<juce::TabBarButton*> (&node))
    {
        auto area = tab->getTextArea();
        if (tab->getTabbedButtonBar().isVertical()) area = { area.getY(), area.getX(), area.getHeight(), area.getWidth() };
        check ("Tab", tab->getButtonText(), look.getTabButtonFont (*tab, (float) area.getHeight()), area);
    }
    else if (auto* toggle = dynamic_cast<juce::ToggleButton*> (&node))
    {
        const float height = juce::jmin (lm::pt (15.0f), (float) toggle->getHeight() * 0.75f);
        const auto area = toggle->getLocalBounds().withTrimmedLeft (juce::roundToInt (height * 1.1f) + 10).withTrimmedRight (2);
        check ("ToggleButton", toggle->getButtonText(), juce::Font (juce::FontOptions (height)), area,
               true, juce::Font::getDefaultMinimumHorizontalScaleFactor(), 10);
    }
    else if (auto* record = dynamic_cast<lm::RecordingButton*> (&node))
        check ("RecordingButton", record->getButtonText(), record->large ? lm::titleFont() : look.getTextButtonFont (*record, record->getHeight()),
               record->getLocalBounds().withTrimmedLeft (record->large ? 22 : 18));
    else if (auto* button = dynamic_cast<juce::TextButton*> (&node))
    {
        // These controls paint their own text. List them with the other custom paint controls in the report.
        const juce::String type (typeid (*button).name());
        if (dynamic_cast<lm::Chip*> (button) != nullptr || type.contains ("ChainChip") || type.contains ("FooterButton")) return;
        const auto font = look.getTextButtonFont (*button, button->getHeight());
        if (type.contains ("FxTabButton"))
        {
            check ("Tab", button->getButtonText(), font, button->getLocalBounds().reduced (10, 4));
            return;
        }
        const int corner = juce::jmin (button->getWidth(), button->getHeight()) / 2;
        const int indent = juce::roundToInt (font.getHeight() * 0.6f);
        const int left = juce::jmin (indent, 2 + corner / (button->isConnectedOnLeft() ? 4 : 2));
        const int right = juce::jmin (indent, 2 + corner / (button->isConnectedOnRight() ? 4 : 2));
        check ("TextButton", button->getButtonText(), font,
               button->getLocalBounds().reduced (0, juce::jmin (4, button->proportionOfHeight (0.3f))).withTrimmedLeft (left).withTrimmedRight (right),
               false, juce::Font::getDefaultMinimumHorizontalScaleFactor(), 2);
    }
    else if (auto* editor = dynamic_cast<juce::TextEditor*> (&node); editor != nullptr && editor->isMultiLine() && editor->isReadOnly())
        check ("TextEditor", editor->getText(), editor->getFont(), editor->getBorder().subtractedFrom (editor->getLocalBounds()).reduced (4, 4), true);

    if (auto* header = dynamic_cast<juce::TableHeaderComponent*> (&node))
        for (int i = 0; i < header->getNumColumns (true); ++i)
        {
            const int column = header->getColumnIdOfIndex (i, true);
            auto area = header->getColumnPosition (i).reduced (4, 0);
            if (header->getSortColumnId() == column) area.removeFromRight (header->getHeight() / 2);
            check ("TableHeader", header->getColumnName (column),
                   juce::Font (juce::FontOptions ((float) header->getHeight() * 0.5f, juce::Font::bold)), area,
                   false, juce::Font::getDefaultMinimumHorizontalScaleFactor());
        }
    for (auto* child : node.getChildren()) inspect (root, *child, image);
}

void Capture::take (juce::Component& component, const juce::String& name, juce::Component* overlay)
{
    dispatch();
    juce::Desktop::getInstance().getAnimator().cancelAllAnimations (true);
    auto bounds = component.getScreenBounds();
    if (overlay != nullptr) bounds = bounds.getUnion (overlay->getScreenBounds());
    juce::Image image (juce::Image::ARGB, bounds.getWidth(), bounds.getHeight(), true, juce::SoftwareImageType());
    {
        // Software image storage also works on the hidden desktop without a Direct2D swap chain.
        juce::Graphics graphics (image);
        graphics.fillAll (lm::Palette::background);
        graphics.drawImageAt (component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f, juce::SoftwareImageType()),
                              component.getScreenX() - bounds.getX(), component.getScreenY() - bounds.getY());
        if (overlay != nullptr)
            graphics.drawImageAt (overlay->createComponentSnapshot (overlay->getLocalBounds(), true, 1.0f, juce::SoftwareImageType()),
                                  overlay->getScreenX() - bounds.getX(), overlay->getScreenY() - bounds.getY());
    }
    const auto filename = name + "_" + juce::String (image.getWidth()) + "x" + juce::String (image.getHeight()) + ".png";
    auto output = folder.getChildFile (filename).createOutputStream();
    if (output != nullptr) { output->setPosition (0); output->truncate(); }
    test.expect (output != nullptr && juce::PNGImageFormat().writeImageToStream (image, *output), filename);
    bool rendered = false;
    for (int y = 0; y < image.getHeight(); y += juce::jmax (1, image.getHeight() / 20))
        for (int x = 0; x < image.getWidth(); x += juce::jmax (1, image.getWidth() / 20))
            rendered = rendered || image.getPixelAt (x, y) != image.getPixelAt (0, 0);
    test.expect (rendered, "Blank screenshot: " + filename);
    inspect (component, component, filename);
    if (overlay != nullptr) inspect (*overlay, *overlay, filename);
    manifest.add (filename + "\t" + oneLine (component.getName()));
    ++count;
}
void Capture::window (juce::DocumentWindow& window, const juce::String& name)
{
    window.setVisible (true);
    const auto size = window.getBounds();
    take (window, name + "_default");
    if (window.isResizable())
        if (auto* limits = window.getConstrainer())
        {
            juce::ignoreUnused (limits);
            // Uses the real native-frame-aware ClientLimits as well as ordinary window limits.
            window.setBoundsConstrained (size.withSize (1, 1));
            take (window, name + "_minimum");
        }
    window.setBounds (size);
}
void Capture::alert (const juce::String& name)
{
    dispatch();
    auto* alert = currentAlert();
    test.expect (alert != nullptr, "Missing alert: " + name);
    if (alert != nullptr)
    {
        take (*alert, name);
        alert->exitModalState (0);
        dispatch();
    }
}
void Capture::finish()
{
    test.expect (folder.getChildFile ("overflow.txt").replaceWithText (overflow.joinIntoString ("\n") + (overflow.isEmpty() ? "" : "\n")));
    test.expect (folder.getChildFile ("manifest.tsv").replaceWithText (manifest.joinIntoString ("\n") + "\n"));
    test.expect (folder.getChildFile ("menus.txt").replaceWithText (menus.joinIntoString ("\n") + "\n"));
    test.logMessage (folder.getFileName() + ": PNG=" + juce::String (count) + ", overflow=" + juce::String (overflow.size()));
}

juce::String sourceBlock (const char* file, const char* begin, const char* end)
{
    const auto source = juce::File (LIVEMIX_TEST_SOURCE_ROOT).getChildFile (file).loadFileAsString();
    const int start = source.indexOf (juce::String::fromUTF8 (begin));
    const int finish = start < 0 ? -1 : source.indexOf (start + juce::String::fromUTF8 (begin).length(), juce::String::fromUTF8 (end));
    if (start < 0 || finish < 0) throw std::runtime_error ("Screenshot source anchor changed: " + std::string (file));
    return source.substring (start, finish);
}
juce::StringArray sourceKeys (const juce::String& source)
{
    const auto utf8 = source.toStdString();
    const std::regex key (R"re(\btr(?:f|In)?\s*\(\s*("(?:\\.|[^"\\])*"))re");
    juce::StringArray result;
    for (auto it = std::sregex_iterator (utf8.begin(), utf8.end(), key); it != std::sregex_iterator(); ++it)
    {
        const auto value = juce::JSON::fromString (juce::String::fromUTF8 ((*it)[1].str().c_str()));
        if (! value.isString() || value.toString().isEmpty()) throw std::runtime_error ("Unable to decode a production translation key");
        result.add (value.toString());
    }
    return result;
}

namespace
{
struct Scratch
{
    juce::File directory = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("LiveMixI18nShots-" + juce::Uuid().toString());
    Scratch() { directory.createDirectory(); }
    ~Scratch() { directory.deleteRecursively(); }
};
struct Theme
{
    lm::Language previousLanguage = lm::currentLanguage();
    UiTranslator previousHook = uiTranslator.load();
    juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
    std::unique_ptr<lm::LiveMixLookAndFeel> look;
    explicit Theme (lm::Language language)
    {
        lm::initialiseLanguage (language);
        look = std::make_unique<lm::LiveMixLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (look.get());
    }
    ~Theme()
    {
        juce::LookAndFeel::setDefaultLookAndFeel (previous);
        lm::initialiseLanguage (previousLanguage);
        uiTranslator.store (previousHook);
    }
};
// About uses JUCEApplication::getInstance(). No initialise(), device, updater, tray or network is started.
class ApplicationIdentity : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "LiveMix"; }
    const juce::String getApplicationVersion() override { return LIVEMIX_I18N_SHOTS_APP_VERSION; }
    void initialise (const juce::String&) override {}
    void shutdown() override {}
};
class FakeDevice : public juce::AudioIODevice
{
public:
    explicit FakeDevice (const juce::String& type) : AudioIODevice ("I18n interface", type) {}
    ~FakeDevice() override { close(); }
    juce::StringArray getInputChannelNames() override { return { "1", "2", "3", "4" }; }
    juce::StringArray getOutputChannelNames() override { return { "1", "2", "3", "4", "5", "6" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100, 48000, 96000 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 128, 256, 512 }; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double sr, int bs) override
    { inputs = ins; outputs = outs; rate = sr; buffer = bs; opened = true; return {}; }
    void close() override { stop(); opened = false; }
    bool isOpen() override { return opened; }
    void start (juce::AudioIODeviceCallback* cb) override { callback = cb; if (cb != nullptr) cb->audioDeviceAboutToStart (this); }
    void stop() override { if (callback != nullptr) callback->audioDeviceStopped(); callback = nullptr; }
    bool isPlaying() override { return opened && callback != nullptr; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return buffer; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 24; }
    juce::BigInteger getActiveInputChannels() const override { return inputs; }
    juce::BigInteger getActiveOutputChannels() const override { return outputs; }
    int getInputLatencyInSamples() override { return 48; }
    int getOutputLatencyInSamples() override { return 96; }
    bool hasControlPanel() const override { return getTypeName() == "ASIO"; }
    bool showControlPanel() override { return false; }
private:
    juce::BigInteger inputs, outputs;
    double rate = 48000;
    int buffer = 256;
    bool opened = false;
    juce::AudioIODeviceCallback* callback = nullptr;
};
class FakeType : public juce::AudioIODeviceType
{
public:
    explicit FakeType (const juce::String& type) : AudioIODeviceType (type) {}
    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool) const override { return { "I18n interface" }; }
    int getDefaultDeviceIndex (bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return getTypeName() != "ASIO"; }
    int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
    juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override { return new FakeDevice (getTypeName()); }
};
class DemoPlugin : public TestGainPlugin
{
public:
    explicit DemoPlugin (juce::String n) : TestGainPlugin (1.0f), title (std::move (n)) {}
    const juce::String getName() const override { return title; }
    void fillInPluginDescription (juce::PluginDescription& d) const override
    { TestGainPlugin::fillInPluginDescription (d); d.name = d.descriptiveName = title; d.manufacturerName = "Demo audio"; }
    juce::String title;
};
struct Fixture
{
    explicit Fixture (juce::UnitTest& test)
        : settings (scratch.directory.getChildFile ("settings")), engine ("LiveMixI18nShots-" + juce::Uuid().toString()), document (engine)
    {
        auto& manager = engine.getDeviceManager();
        while (! manager.getAvailableDeviceTypes().isEmpty()) manager.removeAudioDeviceType (manager.getAvailableDeviceTypes().getLast());
        manager.addAudioDeviceType (std::make_unique<FakeType> ("ASIO"));
        manager.addAudioDeviceType (std::make_unique<FakeType> ("Windows Audio"));
        settings.setLanguage (lm::currentLanguage());
        settings.setMicMuteHotkey ({}); settings.setFxMuteHotkey ({}); settings.setWindowHotkey ({});
        settings.setMicAllMuteHotkey ({}); settings.setFxAllMuteHotkey ({});
        for (int i = 1; i <= lm::MixSession::maxPluginGroups; ++i) settings.setPluginGroupHotkey (i, {});
        for (int i = 1; i <= lm::MixSession::maxChannels; ++i) settings.setMicOnHotkey (i, {});
        for (int i = 1; i <= lm::MixSession::maxFx; ++i) settings.setFxOnHotkey (i, {});
        settings.setBackupRememberPassword (false);
        settings.setBackupUser ({});
        settings.setCollabNick (lm::defaultRecordingTrackName (0));
        lm::recording::Options options;
        options.directoryUtf8 = scratch.directory.getChildFile (lm::defaultRecordingDirectory().getFileName()).getFullPathName().toStdString();
        settings.setRecordingOptions (options);

        auto& model = document.getSession();
        model = {};
        for (int i = 0; i < 3; ++i) model.addChannel();
        for (int i = 0; i < 2; ++i) model.addFx();
        model.channels[0].muteGroup = true;
        model.fx[0].muteGroup = true;
        model.sanitise();
        document.applyToEngine();
        test.expect (openDevice ("ASIO").isEmpty());
        auto& mic = *engine.getChannelChain (model.channels[0].id);
        auto add = [&] (PluginChain& chain, const juce::String& name, int id)
        {
            auto plugin = std::make_unique<DemoPlugin> (name);
            juce::PluginDescription description;
            plugin->fillInPluginDescription (description);
            description.pluginFormatName = "VST3"; description.fileOrIdentifier = "i18n-demo-" + juce::String (id);
            description.uniqueId = id; description.isInstrument = false;
            description.numInputChannels = description.numOutputChannels = 2;
            engine.getPluginHost().getKnownPlugins().addType (description);
            PluginSlotState slot; slot.name = name; slot.format = "VST3"; slot.fileOrIdentifier = description.fileOrIdentifier; slot.uniqueId = id;
            chain.addPlugin (std::move (plugin), slot);
        };
        add (mic, "Demo EQ", 101); add (mic, "Demo compressor", 102); add (mic, "Demo reverb", 103);
        add (engine.getMasterChain(), "Demo limiter", 104); add (engine.getMasterChain(), "Demo meter", 105);
        model.channels[0].chain = mic.getStates();
        model.master.chain = engine.getMasterChain().getStates();
        document.addPluginGroup (model.channels[0].id); document.addPluginGroup (model.channels[0].id);
        document.setPluginGroupMember (model.channels[0].id, 0, mic.getSlot (0).state.slotId, true);
        document.setPluginGroupMember (model.channels[0].id, 0, mic.getSlot (1).state.slotId, true);
        document.setPluginGroupMember (model.channels[0].id, 1, mic.getSlot (2).state.slotId, true);
        document.setSend (model.channels[0].id, model.fx[0].id, 0.35, true);
        document.setSend (model.channels[1].id, model.fx[1].id, 0.5, false);

        lm::ObsPluginActions obs;
        obs.roots = [root = scratch.directory]
        { return lm::ObsPluginInstaller::Roots { root.getChildFile ("program-data"), root.getChildFile ("obs"), root.getChildFile ("bundle"), [] { return false; }, {} }; };
        obs.elevate = [] (juce::String&) { return lm::ObsPluginInstaller::Result::failed; };
        main = std::make_unique<lm::MainComponent> (document, settings, obs);
        main->setVisible (true); main->setSize (1440, 900);
        main->hideNotice();
    }
    ~Fixture() { main.reset(); dispatch(); }
    juce::String openDevice (const juce::String& type)
    {
        lm::MixDevice device { type, "I18n interface", "I18n interface", 256, 48000 };
        return engine.openDevice (device);
    }
    Scratch scratch;
    lm::LiveMixSettings settings;
    lm::MixEngine engine;
    lm::MixDocument document;
    std::unique_ptr<lm::MainComponent> main;
};

template<class T> T* find (juce::Component& root)
{ return dynamic_cast<T*> (findComponent (root, [] (auto& c) { return dynamic_cast<T*> (&c) != nullptr; })); }
juce::TextButton* buttonNamed (juce::Component& root, const juce::String& text)
{
    return dynamic_cast<juce::TextButton*> (findComponent (root, [&] (auto& c)
    { auto* b = dynamic_cast<juce::TextButton*> (&c); return b != nullptr && b->getButtonText() == text; }));
}
void listMenu (Capture& shots, const juce::PopupMenu& menu, const juce::String& indent = {})
{
    for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
    {
        const auto& item = it.getItem();
        if (item.isSeparator) { shots.menus.add (indent + "---"); continue; }
        shots.menus.add (indent + item.text + (item.isEnabled ? "" : " [disabled]") + (item.isTicked ? " [checked]" : ""));
        if (item.subMenu != nullptr) listMenu (shots, *item.subMenu, indent + "  ");
    }
}
void captureMenus (Capture& shots, Fixture& f)
{
    const auto names = f.main->getMenuBarNames();
    for (int i = 0; i < names.size(); ++i)
    {
        shots.menus.add ("[" + names[i] + "]");
        listMenu (shots, f.main->getMenuForIndex (i, names[i]));
    }
    shots.menus.add ("[Tray - application source]");
    const auto tray = sourceKeys (sourceBlock ("livemix/src/Main.cpp", "menu.addItem (1, tr (", "menu.showMenuAsync"));
    shots.test.expectEquals (tray.size(), 7);
    for (const auto& key : tray) shots.menus.add (lm::tr (key.toRawUTF8()));

    // Calling showAddMenu would inspect the user's Documents/LiveMix preset directory.
    // Preserve its exact production keys here, plus the same KnownPluginList submenu from our demo host.
    shots.menus.add ("[+ Add - production source; empty-list and populated-list branches]");
    const auto keys = sourceKeys (sourceBlock ("livemix/src/ui/ChainDrawer.cpp", "void ChainDrawer::showAddMenu (juce::Rectangle<int>", "juce::Component::SafePointer<ChainDrawer> safeThis"));
    shots.test.expectEquals (keys.size(), 5);
    for (const auto& key : keys) shots.menus.add (lm::tr (key.toRawUTF8()));
    juce::PopupMenu plugins;
    juce::KnownPluginList::addToMenu (plugins, f.engine.getPluginHost().getEffectTypes(), juce::KnownPluginList::sortByManufacturer);
    listMenu (shots, plugins, "  ");
}

void captureMain (Capture& shots, Fixture& f)
{
    auto* status = dynamic_cast<juce::Label*> (findComponent (*f.main, [] (auto& c) { return c.getComponentID() == "device-status"; }));
    shots.test.expect (status != nullptr);
    if (status == nullptr) return;
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
    while (status->getText().isEmpty() && juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        dispatch();
        if (status->getText().isNotEmpty()) break;
       #if JUCE_WINDOWS
        MsgWaitForMultipleObjectsEx (0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
       #else
        juce::Thread::sleep (1);
       #endif
    }
    shots.test.expect (status->getText().isNotEmpty(), "Top bar audio status must be populated before the first screenshot");
    if (status->getText().isEmpty()) return;
    for (const auto size : { juce::Point<int> (1440, 900), { 1220, 800 }, { 1000, 760 }, { 760, 900 }, { 460, 1000 } })
    {
        f.main->setSize (size.x, size.y); shots.take (*f.main, "main");
    }
    auto* card = find<lm::ChannelCard> (*f.main);
    auto* top = find<lm::TopBar> (*f.main);
    shots.test.expect (card != nullptr && top != nullptr);
    if (card == nullptr || top == nullptr) return;
    card->onOpenChain (f.document.getSession().channels[0].id);
    for (const auto size : { juce::Point<int> (1440, 900), { 760, 900 } })
    { f.main->setSize (size.x, size.y); shots.take (*f.main, "main_chain"); }
    top->onFxPanel();
    for (const auto size : { juce::Point<int> (1440, 900), { 760, 900 } })
    { f.main->setSize (size.x, size.y); shots.take (*f.main, "main_fx"); }
    top->onFxPanel();
    f.main->setSize (1440, 900);
    f.main->toggleMuteAll (lm::MuteGroups::Group::mic);
    f.main->toggleMuteAll (lm::MuteGroups::Group::fx);
    shots.take (*f.main, "main_all_muted");
    f.main->toggleMuteAll (lm::MuteGroups::Group::mic);
    f.main->toggleMuteAll (lm::MuteGroups::Group::fx);

    const auto notes = sourceKeys (sourceBlock ("livemix/src/Main.cpp", "main.setStartupNote (tr (", "Updater::Callbacks callbacks;"));
    shots.test.expectEquals (notes.size(), 2);
    if (notes.size() == 2)
    {
        f.main->setSize (1000, 760);
        f.engine.getDeviceManager().closeAudioDevice();
        f.main->setSafeMode (true);
        f.main->setStartupNote (lm::tr (notes[0].toRawUTF8()), false, true);
        shots.take (*f.main, "notice_safe_mode");
        f.main->setSafeMode (false);
        f.main->setStartupNote (formatUiText (lm::tr (notes[1].toRawUTF8()), { "I18n device unavailable" }), true, false);
        shots.take (*f.main, "notice_device_failure");
        f.main->hideNotice();
        shots.test.expect (f.openDevice ("ASIO").isEmpty());
        f.main->deviceChanged();
    }
    captureMenus (shots, f);
}

juce::DialogWindow* openSettings (Fixture& f)
{
    lm::SettingsDialog::show (f.engine, f.settings, f.main.get(), {}, {}, {}, {}, {}, {}, {},
                             [&] { return lm::TrackHotkeys::tracks (f.document.getSession()); }, [] {});
    dispatch();
    return dialogNamed (lm::tr ("설정"));
}
void captureSettings (Capture& shots, Fixture& f)
{
    for (const auto* backend : { "ASIO", "Windows Audio" })
    {
        shots.test.expect (f.openDevice (backend).isEmpty());
        auto* window = openSettings (f);
        shots.test.expect (window != nullptr);
        if (window == nullptr) continue;
        const auto name = juce::String (backend) == "ASIO" ? "settings_asio" : "settings_windows";
        shots.window (*window, name);
        auto* viewport = dynamic_cast<juce::Viewport*> (window->getContentComponent());
        shots.test.expect (viewport != nullptr && viewport->getViewedComponent() != nullptr);
        if (viewport != nullptr && viewport->getViewedComponent() != nullptr)
            shots.take (*viewport->getViewedComponent(), juce::String (name) + "_full");
        if (juce::String (backend) == "ASIO")
        {
            auto* combo = dynamic_cast<juce::ComboBox*> (findComponent (*window, [] (auto& c) { return c.getComponentID() == "language"; }));
            shots.test.expect (combo != nullptr);
            if (combo != nullptr)
            {
                combo->showPopup(); dispatch();
                auto* popup = juce::Component::getCurrentlyModalComponent();
                shots.test.expect (popup != nullptr);
                shots.take (*window, "settings_language_open", popup);
                juce::PopupMenu::dismissAllActiveMenus(); dispatch();
            }
        }
        lm::SettingsDialog::closeIfOpen(); dispatch();
    }
}

void capturePluginWindows (Capture& shots, Fixture& f)
{
    const auto presets = f.scratch.directory.getChildFile ("presets");
    lm::PluginPreset preset; preset.name = "Demo voice"; preset.plugins = f.document.getSession().channels[0].chain;
    shots.test.expect (preset.save (lm::PluginPreset::fileFor (preset.name, presets)).wasOk());
    lm::PluginManagerWindow manager (f.engine.getPluginHost(), f.settings, presets);
    manager.open(); shots.window (manager, "plugin_manager");
    const auto keys = sourceKeys (sourceBlock ("livemix/src/ui/PluginManagerWindow.cpp", "button (newPresetButton,", "newPresetButton.setColour"));
    shots.test.expectEquals (keys.size(), 1);
    auto* create = keys.isEmpty() ? nullptr : buttonNamed (manager, lm::tr (keys[0].toRawUTF8()));
    shots.test.expect (create != nullptr);
    if (create != nullptr)
    {
        create->onClick(); dispatch();
        auto* builder = dialogNamed (lm::tr ("새 플러그인 프리셋"));
        shots.test.expect (builder != nullptr);
        if (builder != nullptr) shots.window (*builder, "preset_builder");
    }
    manager.closeButtonPressed(); dispatch();
    lm::PluginGroupsWindow groups (f.document);
    groups.open (f.document.getSession().channels[0].id);
    shots.window (groups, "plugin_groups"); groups.closeButtonPressed();
    lm::LoudnessWindow loudness (f.engine, f.settings);
    loudness.open(); shots.window (loudness, "loudness"); loudness.closeButtonPressed();
}

void showSourceAlert (Capture& shots, const juce::StringArray& keys, const juce::String& name)
{
    shots.test.expect (keys.size() >= 3);
    if (keys.size() < 3) return;
    auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
        .withTitle (lm::tr (keys[0].toRawUTF8())).withMessage (lm::tr (keys[1].toRawUTF8()));
    for (int i = 2; i < keys.size(); ++i) options = options.withButton (lm::tr (keys[i].toRawUTF8()));
    juce::AlertWindow::showAsync (options, [] (int) {});
    shots.alert (name);
}
void captureAlerts (Capture& shots, Fixture& f)
{
    const auto close = sourceKeys (sourceBlock ("livemix/src/Main.cpp", ".withTitle (tr (\"LiveMix 닫기\"))", "[safe] (int result)"));
    shots.test.expectEquals (close.size(), 5);
    showSourceAlert (shots, close, "confirm_close_or_tray");
    if (auto* card = find<lm::ChannelCard> (*f.main))
    { card->onRemove (f.document.getSession().channels[0].id); shots.alert ("confirm_delete_channel"); }
    const auto vst2 = sourceKeys (sourceBlock ("livemix/src/ui/MainComponent.cpp", ".withTitle (on ? tr (", "[safeThis] (int result)"));
    shots.test.expectEquals (vst2.size(), 6);
    if (vst2.size() == 6)
        for (int state = 0; state < 2; ++state)
        {
            const auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                .withTitle (lm::tr (vst2[state].toRawUTF8()))
                .withMessage (formatUiText (lm::tr (vst2[state + 2].toRawUTF8()), { "1" }))
                .withButton (lm::tr (vst2[4].toRawUTF8())).withButton (lm::tr (vst2[5].toRawUTF8()));
            juce::AlertWindow::showAsync (options, [] (int) {});
            shots.alert (state == 0 ? "confirm_fill_vst2" : "confirm_remove_vst2");
        }
    f.document.markDirty();
    f.main->withSessionSecured ([] {}); shots.alert ("confirm_unsaved_session");
    f.main->askSaveBeforeUpdate ([] (bool) {}); shots.alert ("confirm_update_unsaved");
    shots.test.expect (f.document.save (f.scratch.directory.getChildFile (lm::defaultSessionName() + ".livemix")).wasOk());
    f.document.setChannelPan (f.document.getSession().channels[0].id, 0.25);
    f.main->askSaveBeforeUpdate ([] (bool) {}); shots.alert ("confirm_update_saved");
    (f.main.get()->*member (About {}))(); shots.alert ("about");
}

void captureCollab (Capture& shots, Fixture& f)
{
    namespace collab = lm::collab;
    namespace contracts = collab::contracts;
    using State = collab::CollabSession::State;
    using Snapshot = collab::CollabSession::Snapshot;
    lm::CollabPanel::Actions actions;
    actions.outputs = []
    {
        lm::CollabPanel::OutputPairs pairs;
        pairs.names = { "1", "2", "3", "4", "5", "6" };
        pairs.asio = pairs.hasOutput = pairs.running = true; pairs.numOutputs = 6;
        return pairs;
    };
    // The production CollabWindow contains this exact viewport. Snapshot injection keeps all room actions offline.
    auto take = [&] (const Snapshot& snapshot, const juce::String& name, bool web = false, bool enteredCode = false)
    {
        lm::CollabViewport viewport (f.settings, actions);
        viewport.setName (lm::tr ("합방")); viewport.setVisible (true);
        viewport.setSnapshot (snapshot);
        if (enteredCode)
            if (auto* entry = find<lm::CollabCodeEntry> (viewport))
                if (auto* digit = find<juce::TextEditor> (*entry)) digit->insertTextAtCaret ("4827");
        viewport.pushMeter ({ 0.18f, 0.14f }, false);
        for (const auto size : { juce::Point<int> (440, web ? 800 : 430), { 400, web ? 640 : 400 } })
        {
            viewport.setSize (size.x, size.y);
            shots.take (viewport, "collab_" + name);
        }
    };
    Snapshot base;
    base.myNick = lm::defaultRecordingTrackName (0);
    base.peerNick = lm::defaultRecordingTrackName (1);
    base.code = "4827";
    base.estimatedOneWayMs = 18.4; base.bufferMs = 5; base.lossPercent = 0.04;
    const std::pair<State, const char*> states[] {
        { State::off, "off" }, { State::checkingHost, "creating" }, { State::hostWaiting, "host_waiting" },
        { State::joining, "joining" }, { State::connected, "connected" }, { State::disconnected, "disconnected" }
    };
    for (const auto& state : states)
    {
        auto snapshot = base; snapshot.state = state.first; snapshot.isHost = state.first != State::joining;
        take (snapshot, state.second);
    }
    take (base, "join_input", false, true);
    int reason = 0;
    for (const auto& upnp : { collab::CollabUpnp::Result {}, collab::CollabUpnp::Result { true, true, "10.0.0.1", {} },
                              collab::CollabUpnp::Result { true, true, "203.0.113.8", {} } })
    {
        const auto check = collab::CollabHostCheck::evaluate (false, upnp, "203.0.113.8");
        auto snapshot = base; snapshot.state = State::hostUnavailable; snapshot.isHost = true;
        snapshot.message = check.message(); snapshot.reason = check.displayReason();
        take (snapshot, "host_unavailable_" + juce::String (++reason));
    }
    for (int status : { 0, 404, 410, 425, 409, 503 })
    {
        auto snapshot = base; snapshot.state = State::error; snapshot.message = collab::CollabRoom::errorMessage (status);
        take (snapshot, "error_" + juce::String (status));
    }
    for (const auto route : { collab::CollabAudio::Route::direct, collab::CollabAudio::Route::obs })
    {
        auto snapshot = base; snapshot.state = State::connected; snapshot.route = route; snapshot.routeFirst = 2;
        take (snapshot, route == collab::CollabAudio::Route::direct ? "direct" : "obs");
    }

    auto room = base;
    room.state = State::hostWaiting; room.isHost = true; room.selfId = 11;
    room.roomMode = contracts::RoomMode::v2; room.nativeAvailable = room.signalingConnected = true;
    room.personCount = 1; room.people[0].participantId = 11; room.people[0].isSelf = room.people[0].isHost = true;
    room.people[0].nickname = base.myNick; room.people[0].kind = contracts::ParticipantKind::native;
    room.people[0].state = contracts::ConnectionState::connected;
    room.canCreateWebInvite = true;
    take (room, "web_creating_invite", true);
    room.webInviteUrl = "https://example.invalid/join/" + juce::String::repeatedString ("a", 64);
    room.webInviteQrUrl = room.webInviteUrl; room.inviteGeneration = 1;
    take (room, "web_invite", true);
    room.webRequestCount = 1;
    room.webRequests[0] = { "demo-request", lm::defaultRecordingTrackName (2), juce::Time::currentTimeMillis() + 60000, true, true };
    take (room, "web_request", true);
    room.webRequestCount = room.webRequests.size();
    for (size_t i = 1; i < room.webRequestCount; ++i)
        room.webRequests[i] = { "demo-request-" + juce::String ((int) i), "Guest " + juce::String ((int) i + 1),
                                juce::Time::currentTimeMillis() + 60000, true, true };
    take (room, "web_requests_scroll", true);
    room.webRequestCount = 0;
    room.state = State::connected; room.personCount = 3; room.kickableWebId = 33;
    for (size_t i = 1; i < room.personCount; ++i)
    {
        auto& person = room.people[i]; person.participantId = i == 1 ? 22 : 33; person.slot = (int) i;
        person.nickname = lm::defaultRecordingTrackName ((int) i);
        person.kind = i == 1 ? contracts::ParticipantKind::native : contracts::ParticipantKind::web;
        person.state = contracts::ConnectionState::connected; person.estimatedOneWayMs = i == 1 ? 18 : 46;
    }
    take (room, "web_connected", true);
    room.webRequestCount = 1; room.webRequests[0].canAccept = false;
    take (room, "web_full_request", true);
    room.webRequestCount = 0; room.isHost = false; room.selfId = 22;
    room.people[0].isSelf = false; room.people[1].isSelf = true;
    room.kickableWebId = 0; room.canCreateWebInvite = false; room.webInviteUrl.clear();
    take (room, "web_guest_view", true);
}

void captureRecording (Capture& shots, Fixture& f)
{
    // RecordingWindow is fixed-size; its production panel is also used for states without opening any files.
    lm::RecordingWindow window (f.engine, f.settings);
    window.open(); shots.window (window, "recording_idle_wav"); window.closeButtonPressed();
    const std::pair<lm::recording::State, const char*> states[] {
        { lm::recording::State::idle, "idle_mp3" }, { lm::recording::State::starting, "starting" },
        { lm::recording::State::recording, "recording" }, { lm::recording::State::stopping, "stopping" },
        { lm::recording::State::error, "error" }
    };
    auto options = f.settings.getRecordingOptions(); options.format = lm::recording::Format::mp3;
    f.settings.setRecordingOptions (options);
    for (const auto& state : states)
    {
        lm::RecordingPanel panel (f.settings, {});
        panel.setVisible (true); panel.setName (lm::tr ("녹음"));
        lm::recording::Snapshot snapshot; snapshot.state = state.first; snapshot.options = options;
        snapshot.elapsedSeconds = 3661; snapshot.fileCount = 4; snapshot.collabAtStart = true;
        snapshot.files[0].pathUtf8 = f.scratch.directory.getChildFile ("2026-11-11 14-30-00")
            .getChildFile ("A_" + lm::defaultRecordingTrackName (0) + ".mp3").getFullPathName().toStdString();
        if (state.first == lm::recording::State::error) snapshot.error = lm::recording::Error::openFailed;
        panel.setSnapshot (snapshot); shots.take (panel, "recording_" + juce::String (state.second));
    }
}

void captureRestart (Capture& shots, lm::Language picked)
{
    const auto running = picked == lm::Language::ko ? lm::Language::en : lm::Language::ko;
    Theme theme (running);
    Fixture f (shots.test);
    if (auto* window = openSettings (f))
    {
        auto* combo = dynamic_cast<juce::ComboBox*> (findComponent (*window, [] (auto& c) { return c.getComponentID() == "language"; }));
        shots.test.expect (combo != nullptr);
        if (combo != nullptr)
        {
            combo->setSelectedId (1 + (int) picked, juce::sendNotificationSync);
            shots.alert ("restart_from_" + juce::String (lm::languageCode (running)));
            shots.test.expectEquals (f.settings.getLanguage(), juce::String (lm::languageCode (picked)));
            shots.test.expect (lm::currentLanguage() == running);
        }
        lm::SettingsDialog::closeIfOpen(); dispatch();
    }
    else shots.test.expect (false, "Settings did not open for language change");
}
}

class LiveMixI18nShotsTests : public juce::UnitTest
{
public:
    LiveMixI18nShotsTests() : UnitTest ("LiveMix i18n screenshots", "LiveMix") {}
    void runTest() override
    {
        beginTest ("optional production UI screenshots and text overflow");
        const auto destination = juce::SystemStats::getEnvironmentVariable ("LIVEMIX_I18N_SHOTS_DIR", {});
        if (destination.isEmpty()) { expect (true); return; }
        const auto originalLanguage = lm::currentLanguage();
        auto* originalLook = &juce::LookAndFeel::getDefaultLookAndFeel();
        std::unique_ptr<ApplicationIdentity> application;
        if (juce::JUCEApplication::getInstance() == nullptr) application = std::make_unique<ApplicationIdentity>();
        const juce::File root (destination);
        for (const auto language : { lm::Language::ko, lm::Language::en, lm::Language::ja })
        {
            beginTest ("capture " + juce::String (lm::languageCode (language)));
            Capture shots (*this, root.getChildFile (lm::languageCode (language)));
            try
            {
                {
                    Theme theme (language);
                    Fixture fixture (*this);
                    captureMain (shots, fixture);
                    captureSettings (shots, fixture);
                    capturePluginWindows (shots, fixture);
                    captureCollab (shots, fixture);
                    captureRecording (shots, fixture);
                    captureBackup (shots, fixture.document, fixture.settings);
                    captureAlerts (shots, fixture);
                }
                captureRestart (shots, language);
            }
            catch (const std::exception& error) { expect (false, error.what()); }
            shots.finish();
        }
        expect (lm::currentLanguage() == originalLanguage);
        expect (&juce::LookAndFeel::getDefaultLookAndFeel() == originalLook);
    }
};
static LiveMixI18nShotsTests liveMixI18nShotsTests;
}
