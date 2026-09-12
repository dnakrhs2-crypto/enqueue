#include "app/ProductIdentity.h"
#include "app/RecorderDocument.h"
#include "app/RecorderSettings.h"
#include "app/RecorderUpdater.h"
#include "support/CrashHandler.h"
#include "ui/MainComponent.h"
#include "model/SafeFileWrite.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <charconv>

namespace gocue::recorder
{
namespace
{
int roundtrip(const juce::File& testRoot, const juce::File& folder)
{
    const auto target = folder.getChildFile(ProductIdentity::projectFileName());
    juce::Result result = juce::Result::ok();
    bool bytesEqual = false;
    if (target.exists()) result = juce::Result::fail(ko("이미 프로젝트가 있는 폴더입니다. 새 폴더를 지정하세요."));
    else
    {
        RecorderDocument created; created.newProject(folder.getFileName());
        result = created.saveCheckpoint(target);
        if (result.wasOk())
        {
            juce::MemoryBlock first, second;
            if (!target.loadFileAsData(first)) result = juce::Result::fail(ko("저장한 프로젝트를 읽을 수 없습니다."));
            RecorderDocument reopened;
            if (result.wasOk()) result = reopened.openCheckpoint(target);
            if (result.wasOk()) result = reopened.saveCheckpoint(target);
            if (result.wasOk())
            {
                bytesEqual = target.loadFileAsData(second) && first == second;
                if (!bytesEqual) result = juce::Result::fail(ko("프로젝트 왕복 후 바이트가 다릅니다."));
            }
        }
        if (result.wasOk())
        {
            RecorderSettings settings(testRoot); result = settings.load();
            if (result.wasOk()) { settings.rememberProject(target); result = settings.save().get(); }
        }
    }
    auto* report = new juce::DynamicObject();
    report->setProperty("status", result.wasOk() ? "PASS" : "FAIL"); report->setProperty("byteIdentical", bytesEqual);
    report->setProperty("project", target.getFullPathName()); report->setProperty("error", result.getErrorMessage());
    const auto written = gocue::SafeFileWrite::writeTextVerified(testRoot.getChildFile("roundtrip.json"), juce::JSON::toString(juce::var(report), false) + "\n");
    return result.wasOk() && written.wasOk() ? 0 : 1;
}
}
class RecorderApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return ProductIdentity::displayName(); }
    const juce::String getApplicationVersion() override { return ProductIdentity::version(); }
    bool moreThanOneInstanceAllowed() override { return getCommandLineParameters().contains("--test-root") || getCommandLineParameters().contains("--self-test-record") || getCommandLineParameters().startsWith("--automation "); }
    void unhandledException(const std::exception* exception, const juce::String& sourceFilename, int lineNumber) override
    {
        CrashHandler::handleException(exception, sourceFilename, lineNumber, [this](const juce::File& report)
        {
            lastExceptionReport = report; exceptionReported = true;
            if (window) { window->content().showUnhandledException(report); if (report != juce::File()) CrashHandler::markSeen(report); }
        });
    }
    void initialise(const juce::String& commandLine) override
    {
        CrashHandler::install();
        auto args = juce::StringArray::fromTokens(commandLine.trim(), true); for (auto& arg : args) arg = arg.unquoted();
        if (args.contains("--crash-test")) { volatile int* nowhere = nullptr; *nowhere = 1; } // diagnostic: proves the crash reporter on this PC
        if (args.size() == 2 && args[0] == "--automation")
        {
            lookAndFeel = std::make_unique<RecorderLookAndFeel>(); juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
            timelineAutomation = createTimelineAutomationWindow(juce::File::getCurrentWorkingDirectory().getChildFile(args[1]), [this](int result) { setApplicationReturnValue(result); quit(); });
            return;
        }
        juce::String rootPath, projectPath, openPath, demoDevices, demoReport, demoTimeline;
        int demoIterations = 0, demoAsio = -1; bool automation = false, demoArguments = false, invalid = false;
        for (int i = 0; i < args.size(); ++i)
        {
            const auto flag = args[i];
            if (flag == "--automation") { automation = true; continue; }
            if ((flag == "--self-test-record" || flag == "--devices" || flag == "--asio-device" || flag == "--report" || flag == "--demo-timeline") && i + 1 < args.size())
            {
                demoArguments = true;
                const auto value = args[++i];
                if (flag == "--self-test-record" || flag == "--asio-device")
                {
                    const auto number = value.toStdString(); auto& destination = flag == "--self-test-record" ? demoIterations : demoAsio;
                    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), destination);
                    if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) invalid = true;
                }
                else if (flag == "--devices") demoDevices = value; else if (flag == "--demo-timeline") demoTimeline = value; else demoReport = value;
                continue;
            }
            if ((flag == "--test-root" || flag == "--new-project" || flag == "--open-project") && i + 1 < args.size())
            {
                auto& destination = flag == "--test-root" ? rootPath : flag == "--new-project" ? projectPath : openPath;
                if (destination.isNotEmpty()) invalid = true; destination = args[++i];
                if (!juce::File::isAbsolutePath(destination) || destination.startsWith("\\\\") || destination.startsWith("//")) invalid = true;
            }
            else if (args.size() == 1 && juce::File::isAbsolutePath(flag) && flag.endsWithIgnoreCase(ProductIdentity::projectExtension())) openPath = flag;
            else invalid = true;
        }
        if (projectPath.isNotEmpty() && !automation && !demoArguments)
        {
            int result = 2;
            if (!invalid && rootPath.isNotEmpty() && projectPath.isNotEmpty() && openPath.isEmpty())
            {
                try { result = roundtrip(juce::File(rootPath), juce::File(projectPath)); }
                catch (const std::exception&) { result = 1; }
            }
            setApplicationReturnValue(result); quit(); return; // no window, device, tray or updater
        }
        if (invalid) { setApplicationReturnValue(2); quit(); return; }
        lookAndFeel = std::make_unique<RecorderLookAndFeel>(); juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel.get());
        if ((automation || demoArguments) && (demoIterations < 1 || demoIterations > 1000 || demoAsio < 0 || demoDevices.isEmpty() || demoReport.isEmpty() || openPath.isNotEmpty() || projectPath.isNotEmpty())) { setApplicationReturnValue(2); quit(); return; }
        if (demoIterations && rootPath.isEmpty()) rootPath = juce::File::getCurrentWorkingDirectory().getChildFile(demoReport).getParentDirectory().getChildFile("demo-settings-" + juce::Uuid().toString()).getFullPathName();
        settings = std::make_unique<RecorderSettings>(rootPath.isEmpty() ? juce::File() : juce::File(rootPath)); const auto loaded = settings->load();
        document = std::make_unique<RecorderDocument>(); window = std::make_unique<MainWindow>(*document, *settings);
        if (exceptionReported) { window->content().showUnhandledException(lastExceptionReport); if (lastExceptionReport != juce::File()) CrashHandler::markSeen(lastExceptionReport); }
        if (demoIterations) { window->content().startDemo(demoIterations, juce::File::getCurrentWorkingDirectory().getChildFile(demoDevices), demoAsio, juce::File::getCurrentWorkingDirectory().getChildFile(demoReport), demoTimeline); return; }
        if (rootPath.isNotEmpty())
        {
            // Isolated GUI inspection; --test-root + --new-project still uses the headless roundtrip above.
            if (loaded.failed()) window->content().showError(loaded.getErrorMessage());
            window->content().initialiseProject(openPath.isEmpty() ? juce::File() : juce::File(openPath), false, false);
            return; // no startup prompt, devices, updater or previous-crash dialog
        }
        const auto lifecycle = window->content().lifecycleState();
        const juce::Component::SafePointer<MainComponent> content(&window->content());
        RecorderUpdater::initialise({[lifecycle] { return lifecycle->canShutdown(); },
            [content] { if (content) content->updateShutdownRequested(); },
            [content] { if (content) content->updateShutdownBlocked(); }});
        if (loaded.failed()) window->content().showError(loaded.getErrorMessage());
        if (const auto report = CrashHandler::latestUnseenReport(); report != juce::File())
        {
            // A dialog, not the banner: the banner is cleared as soon as the devices connect. The folder button reveals the report.
            CrashHandler::markSeen(report);
            juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon).withTitle(ko("비정상 종료 보고"))
                .withMessage(ko("이전 실행이 비정상 종료됐습니다. 아래 보고 파일과 같은 이름의 .dmp를 함께 보내주시면 원인을 찾을 수 있습니다.\n") + report.getFileName())
                .withButton(ko("폴더 열기")).withButton(ko("확인")).withAssociatedComponent(&window->content()),
                [report](int result) { if (result == 1) report.revealToUser(); });
        }
        window->content().initialiseProject(openPath.isEmpty() ? juce::File() : juce::File(openPath), true);
    }
    void shutdown() override
    {
        RecorderUpdater::shutdown(); // deactivate queued thunks, join WinSparkle before host destruction
        timelineAutomation.reset(); window.reset(); document.reset(); settings.reset(); juce::LookAndFeel::setDefaultLookAndFeel(nullptr); lookAndFeel.reset();
    }
    void systemRequestedQuit() override
    {
        if (window == nullptr) { quit(); return; }
        auto next = settings->get(); next.windowState = window->getWindowStateAsString(); settings->set(std::move(next));
        window->content().requestClose([] { if (auto* app = juce::JUCEApplication::getInstance()) app->quit(); });
    }
    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        if (window == nullptr) return;
        window->setVisible(true); window->toFront(true);
        const auto path = commandLine.unquoted();
        if (juce::File::isAbsolutePath(path) && path.endsWithIgnoreCase(ProductIdentity::projectExtension())) window->content().openProject(juce::File(path));
    }
private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(RecorderDocument& document, RecorderSettings& settings)
            : juce::DocumentWindow(ProductIdentity::displayName(), Palette::background, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true); setContentOwned(new MainComponent(document, settings), true);
            setResizable(true, false); setResizeLimits(960, 640, 8192, 8192);
            if (!restoreWindowStateFromString(settings.get().windowState)) centreWithSize(1180, 780);
            setVisible(true);
        }
        MainComponent& content() { return *static_cast<MainComponent*>(getContentComponent()); }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<RecorderLookAndFeel> lookAndFeel;
    std::unique_ptr<RecorderSettings> settings;
    std::unique_ptr<RecorderDocument> document;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<juce::DocumentWindow> timelineAutomation;
    juce::File lastExceptionReport;
    bool exceptionReported = false;
};
}
START_JUCE_APPLICATION(gocue::recorder::RecorderApplication)
