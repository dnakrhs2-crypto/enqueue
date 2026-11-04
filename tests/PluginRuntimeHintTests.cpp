#include "ui/PluginManager.h"
#include "ui/GoCueLookAndFeel.h"
#include "ui/PluginScanListComponent.h"
#include "../livemix/src/ui/PluginManagerWindow.h"
#include "../livemix/src/ui/LiveMixLookAndFeel.h"
#include <cmath>
#include "PluginRuntimeTestSupport.h"   // last: it brings in <windows.h>

namespace gocue::tests
{
namespace rt = runtime;
namespace
{
    template <typename T>
    T* childOfType (juce::Component& root)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* found = dynamic_cast<T*> (child)) return found;
            if (auto* found = childOfType<T> (*child)) return found;
        }
        return nullptr;
    }
    void descendants (juce::Component& root, juce::Array<juce::Component*>& list)
    {
        for (auto* child : root.getChildren()) { list.add (child); descendants (*child, list); }
    }
    juce::Array<juce::TextButton*> downloadButtons (PluginRuntimeHint& hint)
    {
        juce::Array<juce::Component*> children;
        descendants (hint, children);
        juce::Array<juce::TextButton*> buttons;
        for (auto* child : children)
            if (auto* button = dynamic_cast<juce::TextButton*> (child)) buttons.add (button);
        return buttons;
    }
    bool hasText (PluginRuntimeHint& hint, const juce::String& text)
    {
        juce::Array<juce::Component*> children;
        descendants (hint, children);
        for (auto* child : children)
        {
            if (child->getName() == text) return true;
            if (auto* label = dynamic_cast<juce::Label*> (child)) if (label->getText() == text) return true;
        }
        return false;
    }
    juce::TextButton* buttonNamed (juce::Component& content, const juce::String& text)
    {
        for (auto* child : content.getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*> (child)) if (button->getButtonText() == text) return button;
        return nullptr;
    }
    struct LookAndFeelScope
    {
        juce::LookAndFeel* previous = &juce::LookAndFeel::getDefaultLookAndFeel();
        explicit LookAndFeelScope (juce::LookAndFeel& next) { juce::LookAndFeel::setDefaultLookAndFeel (&next); }
        ~LookAndFeelScope() { juce::LookAndFeel::setDefaultLookAndFeel (previous); }
    };
}

class PluginRuntimeHintTests : public juce::UnitTest
{
public:
    explicit PluginRuntimeHintTests (bool live)
        : UnitTest (live ? "PluginRuntimeCheck LiveMix window hints" : "PluginRuntimeCheck Enqueue window hints",
                    live ? "LiveMix" : "Enqueue"), liveMix (live) {}
    void runTest() override
    {
        rt::Fixture f;
        const auto shell = f.waves();
        juce::PropertiesFile::Options propertyOptions;
        propertyOptions.millisecondsBeforeSaving = -1;
        juce::PropertiesFile properties (f.root.getChildFile ("Enqueue/settings.xml"), propertyOptions);
        AppSettings enqueueSettings (properties);
        livemix::LiveMixSettings liveSettings (f.root.getChildFile ("LiveMix"));
        PluginHost host;
        for (int i = 0; i < 12; ++i)
        {
            juce::PluginDescription plugin;
            plugin.name = juce::StringArray { "Pro-Q 4", "Pro-C 2", "Pro-DS", "Pro-L 2", "LoudMax", "ValhallaRoom" }[i % 6];
            plugin.manufacturerName = "Fixture";
            plugin.pluginFormatName = "VST3";
            plugin.fileOrIdentifier = f.plugin (plugin.name + ".vst3").getFullPathName();
            plugin.uniqueId = i + 1;
            plugin.numInputChannels = plugin.numOutputChannels = 2;
            host.getKnownPlugins().addType (plugin);
        }
        juce::StringArray opened;
        PluginRuntimeHint::Options options;
        options.runtimeCheck = f.options;
        auto readOnMessageThread = std::make_shared<std::atomic<bool>> (false);
        options.runtimeCheck.onFileRead = [readOnMessageThread] (const juce::File&)
        {
            if (juce::MessageManager::getInstance()->isThisTheMessageThread()) *readOnMessageThread = true;
        };
        options.openDownload = [&] (const juce::String& url) { opened.add (url); };
        options.scanStateDirectory = f.root.getChildFile ("scan-state");
        GoCueLookAndFeel enqueueLook;
        livemix::LiveMixLookAndFeel liveLook;
        LookAndFeelScope look (liveMix ? static_cast<juce::LookAndFeel&> (liveLook) : static_cast<juce::LookAndFeel&> (enqueueLook));
        const auto makeWindow = [&] (PluginHost& pluginHost) -> std::unique_ptr<juce::DocumentWindow>
        {
            if (liveMix) return std::make_unique<livemix::PluginManagerWindow> (pluginHost, liveSettings, f.root.getChildFile ("Presets"), options);
            return std::make_unique<PluginManagerWindow> (pluginHost, enqueueSettings, options);
        };
        auto window = makeWindow (host);
        auto* content = window->getContentComponent();
        auto* hint = childOfType<PluginRuntimeHint> (*content);
        auto* table = childOfType<juce::TableListBox> (*content);
        expect (hint != nullptr && table != nullptr);
        if (hint == nullptr || table == nullptr) return;
        const int defaultWidth = liveMix ? 960 : 900, defaultHeight = liveMix ? 760 : 600;
        window->setContentComponentSize (defaultWidth, defaultHeight);

        beginTest ("no missing runtime keeps the hint hidden and preserves the existing table bounds");
        expect (! hint->isVisible());
        const auto unchangedTable = table->getBounds();
        hint->showResults ({ { shell.getFullPathName(), "VST3", {}, 0 } });
        expect (! hint->isVisible());
        expect (table->getBounds() == unchangedTable);
        // These are the pre-hint resized() calculations at the default content size.
        expect (unchangedTable == (liveMix ? juce::Rectangle<int> (20, 116, 920, 302)
                                          : juce::Rectangle<int> (18, 120, 864, 396)));

        beginTest ("one runtime uses the exact requested text and an injectable official download action");
        const auto one = f.check (shell);
        hint->showResults ({ one });
        expect (hint->isVisible());
        expect (hasText (*hint, rt::ko ("불러오지 못한 파일: WaveShell1-VST3 17.1_x64.vst3 (웨이브즈)")));
        expect (hasText (*hint, rt::ko ("이 PC에 Microsoft Visual C++ 2012 (x64) 런타임이 없습니다. 설치한 뒤 [VST3 스캔]을 다시 누르세요.")));
        auto buttons = downloadButtons (*hint);
        expectEquals (buttons.size(), 1);
        if (! buttons.isEmpty()) buttons[0]->onClick();
        expectEquals (opened.joinIntoString (""), pluginRuntimePackages()[1].url);
        auto* title = childOfType<juce::Label> (*hint);
        expect (title != nullptr);
        if (title != nullptr)
        {
            expectEquals (juce::StringArray::fromLines (title->getText()).size(), 1);
            expectEquals (title->getHeight(), (int) std::ceil (title->getFont().getHeight() * 1.25f));
            expectEquals (title->getText(), title->getTooltip());
        }
        const auto singleTitleBounds = title != nullptr ? title->getBounds() : juce::Rectangle<int>();
        const auto singleHintBounds = hint->getBounds();
        checkLayout (*content, *table, *hint, true);
        snapshot (*content, liveMix ? "livemix_rt1.png" : "enqueue_rt1.png");

        beginTest ("two packages have two rows and buttons followed by the exact retry line");
        auto two = one;
        two.packages.push_back (pluginRuntimePackages()[3]);
        hint->showResults ({ two });
        expect (hasText (*hint, rt::ko ("이 PC에 Microsoft Visual C++ 2012 (x64) 런타임이 없습니다.")));
        expect (hasText (*hint, rt::ko ("이 PC에 Microsoft Visual C++ 2015-2022 (x64) 런타임이 없습니다.")));
        expect (hasText (*hint, rt::ko ("설치한 뒤 [VST3 스캔]을 다시 누르세요.")));
        buttons = downloadButtons (*hint);
        expectEquals (buttons.size(), 2);
        opened.clear();
        for (auto* button : buttons) button->onClick();
        expect (opened == juce::StringArray { pluginRuntimePackages()[1].url, pluginRuntimePackages()[3].url });
        checkLayout (*content, *table, *hint, true);
        if (liveMix) snapshot (*content, "livemix_rt2.png");

        if (liveMix)
        {
            beginTest ("laptop work areas retain three complete plugin rows with one or two runtimes");
            for (int count : { 1, 2 })
            {
                hint->showResults ({ count == 1 ? one : two });
                for (int height : { 609, 657, 689, 760 })
                {
                    window->setContentComponentSize (960, height);
                    checkLayout (*content, *table, *hint, height == 760);
                    const int rows = table->getViewport()->getMaximumVisibleHeight() / table->getRowHeight();
                    expect (rows >= (height == 760 ? 6 : 3), "complete rows in the actual table viewport");
                    for (auto* child : content->getChildren())
                        if (auto* presets = dynamic_cast<juce::Viewport*> (child))
                        {
                            expect (presets->getHeight() >= 64);
                            // The original single-file hint is only 62px; do not enlarge it.
                            expect (hint->getHeight() >= juce::jmin (80, hint->preferredHeight (hint->getWidth())));
                            expect (hint->getBottom() <= presets->getY());
                            expect (! table->getBounds().intersects (presets->getBounds()));
                            logMessage ("layout: LiveMix 960x" + juce::String (height) + " runtimes=" + juce::String (count)
                                        + " table=" + juce::String (table->getHeight()) + " rows=" + juce::String (rows)
                                        + " hint=" + juce::String (hint->getHeight()) + " presets=" + juce::String (presets->getHeight()));
                        }
                    if (height == 609) snapshot (*content, "livemix_609_rt" + juce::String (count) + ".png");
                }
            }
        }

        beginTest ("default and minimum content sizes render at 100, 125 and 150 percent without overlapping controls");
        for (int count : { 1, 2, 4 })
        {
            auto multiple = one;
            if (count == 2) multiple = two;
            if (count == 4) multiple.packages = pluginRuntimePackages();
            hint->showResults ({ multiple });
            for (bool minimum : { false, true })
            {
                window->setContentComponentSize (minimum ? (liveMix ? 600 : 640) : defaultWidth,
                                                 minimum ? (liveMix ? 480 : 420) : defaultHeight);
                checkLayout (*content, *table, *hint, ! minimum && count <= 2);
                for (float scale : { 1.0f, 1.25f, 1.5f })
                {
                    const auto image = content->createComponentSnapshot (content->getLocalBounds(), true, scale);
                    expect (image.isValid());
                    expectEquals (image.getWidth(), juce::roundToInt (content->getWidth() * scale));
                }
            }
        }
        window->setContentComponentSize (defaultWidth, defaultHeight);

        beginTest ("several WaveShell files wrap at filename boundaries into two title lines");
        auto olderShell = one, vst2Shell = one, araShell = one;
        olderShell.file = f.plugin ("WaveShell1-VST3 16.2_x64.vst3").getFullPathName();
        vst2Shell.file = f.plugin ("WaveShell1-VST 17.1_x64.dll").getFullPathName();
        vst2Shell.format = "VST";
        araShell.file = f.plugin ("WaveShell1-VST3-ARA 17.1_x64.vst3").getFullPathName();
        hint->showResults ({ one, olderShell, vst2Shell, araShell });
        if (title != nullptr)
        {
            const auto lines = juce::StringArray::fromLines (title->getText());
            expectEquals (lines.size(), 2);
            expectEquals (title->getHeight(), 2 * singleTitleBounds.getHeight());
            for (const auto& result : { one, olderShell, vst2Shell, araShell })
            {
                const auto fileName = juce::File (result.file).getFileName() + rt::ko (" (웨이브즈)");
                bool wholeNameOnOneLine = false;
                for (const auto& line : lines) wholeNameOnOneLine |= line.contains (fileName);
                expect (wholeNameOnOneLine, "filename and Waves suffix stay together: " + fileName);
                expect (title->getTooltip().contains (fileName));
            }
            expect (! title->getText().containsChar ((juce::juce_wchar) 0x00a0));
            expect (! title->getText().contains (rt::ko ("…")));
        }
        checkLayout (*content, *table, *hint, true);
        snapshot (*content, liveMix ? "livemix_title2.png" : "enqueue_title2.png");

        beginTest ("long file lists stop at three title lines with an ellipsis and retain the full tooltip");
        auto longName = one;
        longName.file = f.plugin ("Vintage Compressor Extended Edition.dll").getFullPathName();
        longName.format = "VST";
        std::vector<PluginRuntimeResult> many { one, olderShell, vst2Shell, longName };
        for (int i = 1; i <= 8; ++i)
        {
            auto plugin = longName;
            plugin.file = f.plugin ("Classic Equalizer Edition " + juce::String (i) + ".dll").getFullPathName();
            many.push_back (plugin);
        }
        hint->showResults (many);
        if (title != nullptr)
        {
            const auto lines = juce::StringArray::fromLines (title->getText());
            expectEquals (lines.size(), 3);
            expectEquals (title->getHeight(), 3 * singleTitleBounds.getHeight());
            expect (lines[2].endsWith (rt::ko ("…")));
            expect (! lines[0].contains (rt::ko ("…")) && ! lines[1].contains (rt::ko ("…")));
            juce::StringArray fullNames;
            for (const auto& result : many)
            {
                auto fileName = juce::File (result.file).getFileName();
                if (fileName.startsWithIgnoreCase ("WaveShell")) fileName += rt::ko (" (웨이브즈)");
                fullNames.add (fileName);
            }
            expectEquals (title->getTooltip(), rt::ko ("불러오지 못한 파일: ") + fullNames.joinIntoString (", "));
            for (const auto& line : lines)
                expect (juce::GlyphArrangement::getStringWidth (title->getFont(), line) <= (float) title->getWidth());
        }
        checkLayout (*content, *table, *hint, true);
        snapshot (*content, liveMix ? "livemix_title3.png" : "enqueue_title3.png");
        expectEquals (downloadButtons (*hint).size(), 1);
        expect (hasText (*hint, rt::ko ("이 PC에 Microsoft Visual C++ 2012 (x64) 런타임이 없습니다. 설치한 뒤 ")
                               + (liveMix ? rt::ko ("[VST3 스캔]·[VST2 스캔]") : rt::ko ("[VST3 스캔]")) + rt::ko ("을 다시 누르세요.")));

        beginTest ("the plugin window owns a tooltip window that displays the full title");
        auto* tooltip = childOfType<juce::TooltipWindow> (*window);
        expect (tooltip != nullptr);
        if (tooltip != nullptr && title != nullptr)
        {
            expect (tooltip->getParentComponent() == window.get());
            tooltip->displayTip (title->getScreenBounds().getCentre(), title->getTooltip());
            expect (tooltip->isVisible());
            expect (window->getLocalBounds().contains (tooltip->getBounds()));
            tooltip->hideTip();
        }

        beginTest ("returning to one failed file preserves its original title and hint bounds");
        hint->showResults ({ one });
        if (title != nullptr)
        {
            expect (title->getBounds() == singleTitleBounds);
            expectEquals (title->getText(), title->getTooltip());
        }
        expect (hint->getBounds() == singleHintBounds);
        if (liveMix)
        {
            hint->showResults ({ longName });
            expect (hasText (*hint, rt::ko ("이 PC에 Microsoft Visual C++ 2012 (x64) 런타임이 없습니다. 설치한 뒤 [VST2 스캔]을 다시 누르세요.")));
        }

        beginTest ("starting a new scan hides the hint and cancelling the chooser leaves it hidden");
        auto* scan = buttonNamed (*content, liveMix ? rt::ko ("VST3 스캔") : rt::ko ("VST3 스캔..."));
        expect (scan != nullptr);
        if (scan != nullptr)
        {
            scan->onClick();
            expect (! hint->isVisible());
            expect (rt::pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() != nullptr; }));
            if (auto* chooser = juce::Component::getCurrentlyModalComponent()) chooser->exitModalState (0);
            auto* scanner = childOfType<PluginScanListComponent> (*content);
            expect (scanner != nullptr);
            if (scanner != nullptr) expect (rt::pumpUntil ([&] { return ! scanner->isScanning(); }));
        }
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        rt::pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
        window.reset();

        beginTest ("opening a new window restores the hint from the persisted blacklist in the background");
        host.getKnownPlugins().addToBlacklist (shell.getFullPathName());
        const auto saved = host.getKnownPlugins().createXml();
        if (liveMix) liveSettings.setPluginList (saved.get()); else enqueueSettings.setPluginList (saved.get());
        const auto restored = liveMix ? liveSettings.getPluginList() : enqueueSettings.getPluginList();
        PluginHost reopenedHost;
        expect (restored != nullptr);
        if (restored != nullptr) reopenedHost.getKnownPlugins().recreateFromXml (*restored);
        window = makeWindow (reopenedHost);
        content = window->getContentComponent();
        hint = childOfType<PluginRuntimeHint> (*content);
        expect (hint != nullptr);
        if (hint == nullptr) return;
        expect (rt::pumpUntil ([&] { return hint->isVisible(); }));
        expect (! readOnMessageThread->load(), "blacklist diagnostics must not read PE files on the message thread");
        expectEquals (downloadButtons (*hint).size(), 1);
        if (auto* live = dynamic_cast<livemix::PluginManagerWindow*> (window.get()))
        { live->open(); live->closeButtonPressed(); live->open(); }
        if (auto* enqueue = dynamic_cast<PluginManagerWindow*> (window.get()))
        { enqueue->open(); enqueue->closeButtonPressed(); enqueue->open(); }
        expect (hint->isVisible());

        beginTest ("confirmed list reset hides the hint and clears the blacklist");
        auto* reset = buttonNamed (*content, rt::ko ("목록 초기화"));
        expect (reset != nullptr);
        if (reset != nullptr)
        {
            reset->onClick();
            expect (rt::pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() != nullptr; }));
            if (auto* alert = juce::Component::getCurrentlyModalComponent()) alert->exitModalState (1);
            expect (rt::pumpUntil ([&] { return ! hint->isVisible(); }));
            expect (reopenedHost.getKnownPlugins().getBlacklistedFiles().isEmpty());
        }
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        rt::pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
    }

private:
    void checkLayout (juce::Component& content, juce::TableListBox& table, PluginRuntimeHint& hint, bool sixRows)
    {
        expect (content.getLocalBounds().contains (hint.getBounds()));
        expect (content.getLocalBounds().contains (table.getBounds()));
        expect (! table.getBounds().intersects (hint.getBounds()));
        expect (table.getBottom() <= hint.getY());
        expect (table.getHeight() >= 30);
        if (sixRows) expect (table.getHeight() >= 212, "six complete plugin rows plus the header and borders");
        for (auto* child : content.getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*> (child))
            {
                expect (! button->getBounds().intersects (hint.getBounds()));
                expect (! button->getBounds().intersects (table.getBounds()));
                expect (content.getLocalBounds().contains (button->getBounds()));
                expect (button->getBottom() <= hint.getY());
            }
        const auto buttons = downloadButtons (hint);
        for (auto* button : buttons)
        {
            expectEquals (button->getHeight(), 32);
            expectEquals (button->getButtonText(), rt::ko ("설치 파일 받기"));
            const auto font = button->getLookAndFeel().getTextButtonFont (*button, 32);
            expect (button->getWidth() >= juce::GlyphArrangement::getStringWidthInt (font, button->getButtonText()) + 20);
            expect (button->getParentComponent()->getLocalBounds().contains (button->getBounds()));
            for (auto* sibling : button->getParentComponent()->getChildren())
                if (sibling != button) expect (! sibling->getBounds().intersects (button->getBounds()));
        }
    }
    void snapshot (juce::Component& content, const juce::String& name)
    {
        const auto directory = juce::SystemStats::getEnvironmentVariable ("GOCUE_SNAPSHOT_DIR", {});
        if (directory.isEmpty()) return;
        const auto file = juce::File (directory).getChildFile (name);
        file.getParentDirectory().createDirectory();
        const auto image = content.createComponentSnapshot (content.getLocalBounds());
        juce::FileOutputStream output (file);
        expect (output.openedOk());
        expect (juce::PNGImageFormat().writeImageToStream (image, output));
        logMessage ("snapshot: " + file.getFullPathName());
    }
    bool liveMix;
};

class PluginRuntimePersistenceTests : public juce::UnitTest
{
public:
    explicit PluginRuntimePersistenceTests (bool live)
        : UnitTest (live ? "PluginRuntimeCheck LiveMix zero-result persistence" : "PluginRuntimeCheck Enqueue zero-result persistence",
                    live ? "LiveMix" : "Enqueue"), liveMix (live) {}

    void runTest() override
    {
        beginTest ("a zero-type scan shows the hint and saves the ordinary plugin path outside the blacklist");
        rt::Fixture f;
        const auto plugin = f.root.getChildFile (rt::ko ("ordinary-plugins/일반 플러그인.vst3"));
        expect (rt::write (plugin, rt::pe ({ "MSVCR110.dll" })));
        juce::PropertiesFile::Options propertyOptions;
        propertyOptions.millisecondsBeforeSaving = -1;
        juce::PropertiesFile properties (f.root.getChildFile ("Enqueue/settings.xml"), propertyOptions);
        AppSettings enqueueSettings (properties);
        livemix::LiveMixSettings liveSettings (f.root.getChildFile ("LiveMix"));
        PluginRuntimeHint::Options options;
        options.runtimeCheck = f.options;
        options.openDownload = [] (const juce::String&) {};
        options.scanStateDirectory = f.root.getChildFile ("LiveMix/scan-state");
        options.scanExecutable = juce::File (liveMix ? LM_SCAN_WORKER_EXE : ENQUEUE_SCAN_WORKER_EXE);
        const auto log = liveMix ? options.scanStateDirectory.getChildFile ("logs/plugin-scan.log")
                                : enqueueSettings.getPluginScanLogFile();
        const auto stateFile = log.getSiblingFile ("plugin-scan-runtime-missing.txt");
        GoCueLookAndFeel enqueueLook;
        livemix::LiveMixLookAndFeel liveLook;
        LookAndFeelScope look (liveMix ? static_cast<juce::LookAndFeel&> (liveLook) : static_cast<juce::LookAndFeel&> (enqueueLook));
        PluginHost host, restartedHost;
        const auto makeWindow = [&] (PluginHost& h) -> std::unique_ptr<juce::DocumentWindow>
        {
            if (liveMix) return std::make_unique<livemix::PluginManagerWindow> (h, liveSettings, f.root.getChildFile ("Presets"), options);
            return std::make_unique<PluginManagerWindow> (h, enqueueSettings, options);
        };
        auto window = makeWindow (host);
        expect (scanEmptyPlugin (*window, plugin));
        expect (host.getKnownPlugins().getBlacklistedFiles().isEmpty());
        expectEquals (host.getKnownPlugins().getNumTypes(), 0);
        expect (log.loadFileAsString().contains ("\t" + plugin.getFullPathName() + rt::ko ("\t0개\t")));
        expectEquals (stateFile.loadFileAsString(), plugin.getFullPathName() + "\n");

        beginTest ("closing and constructing a fresh window restores a zero-type hint without a blacklist");
        window->closeButtonPressed();
        window.reset();
        window = makeWindow (restartedHost);
        auto* hint = childOfType<PluginRuntimeHint> (*window->getContentComponent());
        expect (hint != nullptr);
        if (hint == nullptr) return;
        expect (rt::pumpUntil ([&] { return hint->isVisible(); }));
        expect (hasText (*hint, rt::ko ("불러오지 못한 파일: 일반 플러그인.vst3")));
        expect (restartedHost.getKnownPlugins().getBlacklistedFiles().isEmpty());

        beginTest ("installing the runtime before reopening suppresses the remembered hint");
        window.reset();
        const auto runtime = options.runtimeCheck.environment.windowsSystemDirectory.getChildFile ("msvcr110.dll");
        expect (rt::touch (runtime));
        window = makeWindow (restartedHost);
        expectNoHintAfterDiagnosis (*window);
        expect (stateFile.existsAsFile());
        window.reset();
        expect (runtime.deleteFile());
        window = makeWindow (restartedHost);
        hint = childOfType<PluginRuntimeHint> (*window->getContentComponent());
        if (hint == nullptr) { expect (false); return; }
        expect (rt::pumpUntil ([&] { return hint->isVisible(); }));

        beginTest ("starting a new scan immediately hides the hint and deletes the path file");
        auto* chooser = startScan (*window);
        expect (! hint->isVisible());
        expect (! stateFile.exists());
        if (chooser != nullptr) chooser->exitModalState (0);
        waitForScan (*window);
        expect (! hint->isVisible());

        beginTest ("confirmed list reset deletes the path file along with the visible hint");
        expect (scanEmptyPlugin (*window, plugin));
        expect (stateFile.existsAsFile());
        auto* reset = buttonNamed (*window->getContentComponent(), rt::ko ("목록 초기화"));
        expect (reset != nullptr);
        if (reset != nullptr)
        {
            reset->onClick();
            expect (rt::pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() != nullptr; }));
            if (auto* alert = juce::Component::getCurrentlyModalComponent()) alert->exitModalState (1);
            expect (rt::pumpUntil ([&] { return ! hint->isVisible(); }));
            expect (! stateFile.exists());
            expect (restartedHost.getKnownPlugins().getBlacklistedFiles().isEmpty());
        }
        dismissModals();

        beginTest ("a remembered plugin removed from disk does not restore a hint");
        expect (scanEmptyPlugin (*window, plugin));
        window.reset();
        expect (plugin.deleteFile());
        window = makeWindow (restartedHost);
        expectNoHintAfterDiagnosis (*window);
        window.reset();
        dismissModals();
    }

private:
    static void dismissModals()
    {
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        rt::pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() == nullptr; });
    }
    juce::AlertWindow* startScan (juce::DocumentWindow& window)
    {
        auto* scan = buttonNamed (*window.getContentComponent(), liveMix ? rt::ko ("VST3 스캔") : rt::ko ("VST3 스캔..."));
        expect (scan != nullptr);
        if (scan == nullptr) return nullptr;
        scan->onClick();
        expect (rt::pumpUntil ([] { return juce::Component::getCurrentlyModalComponent() != nullptr; }));
        auto* chooser = dynamic_cast<juce::AlertWindow*> (juce::Component::getCurrentlyModalComponent());
        expect (chooser != nullptr);
        return chooser;
    }
    void waitForScan (juce::DocumentWindow& window)
    {
        auto* scanner = childOfType<PluginScanListComponent> (*window.getContentComponent());
        expect (scanner != nullptr);
        if (scanner != nullptr) expect (rt::pumpUntil ([&] { return ! scanner->isScanning(); }, 20000));
        dismissModals();
    }
    bool scanEmptyPlugin (juce::DocumentWindow& window, const juce::File& plugin)
    {
        auto* chooser = startScan (window);
        if (chooser == nullptr) return false;
        auto* paths = dynamic_cast<juce::FileSearchPathListComponent*> (chooser->getCustomComponent (0));
        expect (paths != nullptr);
        if (paths == nullptr) { chooser->exitModalState (0); waitForScan (window); return false; }
        paths->setPath (juce::FileSearchPath (plugin.getParentDirectory().getFullPathName()));
        chooser->exitModalState (1);
        waitForScan (window);
        auto* hint = childOfType<PluginRuntimeHint> (*window.getContentComponent());
        return hint != nullptr && rt::pumpUntil ([&] { return hint->isVisible(); });
    }
    void expectNoHintAfterDiagnosis (juce::DocumentWindow& window)
    {
        auto* hint = childOfType<PluginRuntimeHint> (*window.getContentComponent());
        expect (hint != nullptr);
        if (hint == nullptr) return;
        bool finished = false;
        const auto original = hint->onChanged;
        hint->onChanged = [&] { if (original) original(); finished = true; };
        expect (rt::pumpUntil ([&] { return finished; }));
        hint->onChanged = original;
        expect (! hint->isVisible());
    }
    bool liveMix;
};

static PluginRuntimeHintTests liveMixPluginRuntimeHintTests (true);
static PluginRuntimeHintTests enqueuePluginRuntimeHintTests (false);
static PluginRuntimePersistenceTests liveMixPluginRuntimePersistenceTests (true);
static PluginRuntimePersistenceTests enqueuePluginRuntimePersistenceTests (false);
} // namespace gocue::tests
