#include "TimelineView.h"
#include "TimelineView.automation.h"
#include "model/SafeFileWrite.h"
#include <cmath>

namespace gocue::recorder
{
std::unique_ptr<juce::DocumentWindow> createTimelineUxWindow(const juce::var&, std::function<void(int)>);
namespace
{
class TimelineScenarioWindow : public juce::DocumentWindow, private juce::Timer
{
public:
    TimelineScenarioWindow(juce::var config, std::function<void(int)> done)
        : juce::DocumentWindow(ko("TimelineView · 독립 오디오 컷 검증"), Palette::background, juce::DocumentWindow::closeButton),
          view(document), scenario(view.edits), configuration(std::move(config)), completion(std::move(done))
    {
        setUsingNativeTitleBar(true); setContentNonOwned(&view, true); setResizable(true, false); setResizeLimits(960, 440, 8192, 8192); centreWithSize(1180, 620);
        scenario.dispatch = [this](TimelineAction a, Sample at, bool exact) { return view.invoke(a, at, exact); };
        for (const auto& asset : document.getProject().media->assets) if (asset.kind == AssetKind::mic)
        {
            PeakSnapshot peaks; peaks.sampleRate = document.getProject().Fs; peaks.channels = 1; peaks.samples = std::uint64_t(asset.logicalLength); peaks.samplesPerBin = 480; peaks.complete = true;
            peaks.bins.resize(std::size_t(peaks.samples / peaks.samplesPerBin));
            for (unsigned i = 0; i < peaks.bins.size(); ++i) { const auto level = float(.2 + .6 * std::abs(std::sin(i * .17))); peaks.bins[i][0] = {-level, level}; }
            view.setLoadedPeaks(asset.assetId, std::move(peaks), 0);
        }
        view.refresh(false, 5 * 48000, {}); view.zoomToFit(); setVisible(true); startTimer(200);
    }
    ~TimelineScenarioWindow() override { stopTimer(); clearContentComponent(); }
    void closeButtonPressed() override { writeResult(true); }
private:
    void timerCallback() override
    {
        if (finished) { writeResult(false); return; }
        finished = scenario.advance(); view.refresh(false, view.edits.playhead(), {});
    }
    void writeResult(bool cancelled)
    {
        stopTimer(); auto report = scenario.report(); auto* r = report.getDynamicObject();
        r->setProperty("mode", "real-window"); r->setProperty("runId", configuration["runId"]); r->setProperty("rowPaintCount", int(view.rowPaintCount));
        r->setProperty("visibleWindowPainted", view.rowPaintCount > 0); r->setProperty("osInputInjected", false);
        if (cancelled || view.rowPaintCount == 0) { r->setProperty("status", "FAIL"); r->setProperty("reason", cancelled ? "Window closed before completion" : "Timeline did not paint"); }
        const auto written = gocue::SafeFileWrite::writeTextVerified(juce::File(configuration["report"].toString()), juce::JSON::toString(report, false));
        completion(written.wasOk() && report["status"].toString() == "PASS" ? 0 : 1);
    }
    RecorderDocument document;
    TimelineView view;
    IndependentAudioCutsScenario scenario;
    juce::var configuration;
    std::function<void(int)> completion;
    bool finished = false;
};
}
std::unique_ptr<juce::DocumentWindow> createTimelineAutomationWindow(const juce::File& file, std::function<void(int)> completion)
{
    try
    {
        if (!file.existsAsFile() || file.getSize() > 65536) throw std::invalid_argument("Invalid scenario file");
        const auto config = juce::JSON::parse(file);
        if (int(config["schemaVersion"]) == 1 && config["scenario"].toString() == "timeline-ux" && juce::File::isAbsolutePath(config["report"].toString()))
            return createTimelineUxWindow(config, completion);
        if (int(config["schemaVersion"]) != 1 || config["scenario"].toString() != "independent-audio-cuts" || config["runId"].toString().isEmpty()
            || !juce::File::isAbsolutePath(config["report"].toString())) throw std::invalid_argument("Invalid timeline scenario");
        return std::make_unique<TimelineScenarioWindow>(config, completion);
    }
    catch (const std::exception& e) { file.withFileExtension("error.txt").replaceWithText(juce::String::fromUTF8(e.what())); completion(2); return {}; }
}
}
