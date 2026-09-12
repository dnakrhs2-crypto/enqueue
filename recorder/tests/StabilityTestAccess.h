#pragma once
#include "ui/TimelineView.h"
#include "app/RecorderSession.h"
#include "ui/MainComponent.h"
#include <chrono>
#include <thread>

namespace gocue::recorder
{
// Test/automation-only access. No device discovery or production startup changes.
struct StabilityTestAccess
{
    static TimelineView& timeline(MainComponent& main) { return main.timelineView; }
    static RecorderSession& session(MainComponent& main) { return main.session; }
    static void tab(MainComponent& main, bool timeline) { main.setTimeline(timeline); }
    static void latest(MainComponent& main) { main.latestClicked(); }
    static void refresh(MainComponent& main) { main.refresh(); }
    static void failPreparation(RecorderSession& session, std::exception_ptr error)
    {
        session.wantPlay = session.pendingLatest = session.timeline = true;
        session.notice = "Preparing";
        std::promise<std::unique_ptr<RecorderSession::PreparedPlan>> promise;
        session.planWork = promise.get_future(); promise.set_exception(error);
    }
    static void planError(RecorderSession& session)
    {
        session.wantPlay = session.pendingLatest = session.timeline = true;
        session.notice = "Preparing";
        auto plan = std::make_unique<RecorderSession::PreparedPlan>();
        plan->project = session.document.getProject().projectId;
        plan->revision = session.document.getProject().editRevision;
        plan->generation = session.lifecycle->generation(); plan->error = "injected media failure";
        std::promise<std::unique_ptr<RecorderSession::PreparedPlan>> promise;
        session.planWork = promise.get_future(); promise.set_value(std::move(plan));
    }
    static bool preparing(const RecorderSession& session) { return session.pendingLatest || session.wantPlay || session.planWork.valid(); }
    static std::uint64_t derivedGeneration(RecorderSession& session) { return session.derivedGeneration.load(); }
    static void queueDerived(RecorderSession& session, const RecorderProject& origin, const Id& asset, std::uint64_t generation)
    {
        PeakSnapshot peaks; peaks.sampleRate = origin.Fs;
        const std::lock_guard<std::mutex> lock(session.derivedMutex);
        session.derivedResults.push_back({asset, {}, peaks, 0, generation, origin.projectId, origin.media->findAsset(asset)->mediaGeneration});
    }
    static void openAudio(RecorderSession& session)
    {
        auto& audio = session.audioEngine();
        const auto result = audio.openSynthetic(session.document.getProject().Fs, 480, 0, 2);
        if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString());
        OutputMapping mapping; mapping.left = 0; mapping.right = 1;
        const auto mapped = audio.setOutputMap(mapping);
        if (mapped.failed()) throw std::runtime_error(mapped.getErrorMessage().toStdString());
        session.device = audio.deviceInfo();
    }
    // Exercise the real async session preparation, including available-range and
    // decoded-length clipping. Only immutable media indexes and audio are synthetic.
    static std::vector<PlaybackVideoClip> prepareVideos(RecorderSession& session,
        const std::vector<std::shared_ptr<const VideoIndex>>& sources)
    {
        const auto& p = session.document.getProject();
        if (sources.size() != p.media->assets.size()) throw std::runtime_error("Video fixture source count");
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            const auto& asset = p.media->assets[i];
            session.videoIndexes[p.projectId + "/" + asset.assetId + "/" + juce::String(asset.mediaGeneration)] = sources[i];
        }
        openAudio(session);
        session.preparePlayback();
        if (!session.planWork.valid() || session.planWork.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            throw std::runtime_error("Session video preparation did not complete");
        auto plan = session.collectPreparedPlan();
        if (!plan) throw std::runtime_error(session.error.toStdString());
        if (plan->error.isNotEmpty()) throw std::runtime_error(plan->error.toStdString());
        return std::move(plan->videos);
    }
    static juce::Component& rows(TimelineView& view) { return view.rows; }
    static float x(TimelineView& view, Sample at) { return float(view.xFor(at)); }
    static Sample sample(TimelineView& view, double x) { return view.sampleFor(x); }
    static bool previewContains(TimelineView& view, const Id& id) { return view.previewIndex.find(id) != nullptr; }
    static void key(TimelineView& view, int code, int modifiers = 0)
    { view.keyPressed(juce::KeyPress(code, juce::ModifierKeys(modifiers), 0), &view.rows); }
    static void menu(TimelineView& view) { view.menuButton.onClick(); }
    static void snap(TimelineView& view, bool on) { view.snapButton.setToggleState(on, juce::dontSendNotification); }
    static void button(TimelineView& view, TimelineAction action)
    {
        // Legacy stress operations still cover model edits whose UI was removed.
        // Retained actions must continue to go through their real toolbar buttons.
        switch (action)
        {
            case TimelineAction::rippleAll: case TimelineAction::rippleAudio:
            case TimelineAction::earlier: case TimelineAction::later:
            case TimelineAction::unlink: case TimelineAction::link:
                view.invoke(action, view.edits.playhead()); return;
            default: view.buttons.at(action)->onClick();
        }
    }
    static void mouse(TimelineView& view, int phase, juce::Point<float> point, juce::Point<float> down, int modifiers = 0)
    {
        auto& target = view.rows;
        juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys(modifiers | (phase == 2 ? 0 : juce::ModifierKeys::leftButtonModifier)),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, juce::Time::getCurrentTime(), down,
            juce::Time::getCurrentTime(), 1, point != down);
        if (phase == 0) target.mouseDown(event);
        else if (phase == 1) target.mouseDrag(event);
        else target.mouseUp(event);
    }
    static void wheel(TimelineView& view, float delta, int modifiers)
    {
        juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), {300, 50}, juce::ModifierKeys(modifiers),
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &view.rows, &view.rows, juce::Time::getCurrentTime(), {300, 50}, juce::Time::getCurrentTime(), 0, false);
        juce::MouseWheelDetails wheel; wheel.deltaY = delta;
        view.rows.mouseWheelMove(event, wheel);
    }
};
class StabilityAudioClock
{
public:
    explicit StabilityAudioClock(RecorderSession& session) : audio(session.audioEngine()), rate(audio.deviceInfo().sampleRate)
    {
        worker = std::thread([this]
        {
            std::array<float, 480> left{}, right{}; float* outputs[]{left.data(), right.data()};
            auto next = std::chrono::steady_clock::now(); std::uint64_t sequence = 0;
            while (!stopping.load())
            {
                BlockStamp stamp{}; stamp.flags = samplePositionValid | latenciesValid;
                stamp.sampleRate = rate; stamp.numSamples = 480; stamp.sequence = sequence;
                stamp.samplePosition = Sample(sequence++) * 480; stamp.callbackQpc = qpcNow();
                audio.processBlock(stamp, nullptr, 0, nullptr, outputs, 2);
                ++callbacks;
                next += std::chrono::nanoseconds(480000000000ll / rate); std::this_thread::sleep_until(next);
            }
        });
    }
    ~StabilityAudioClock() { stopping.store(true); if (worker.joinable()) worker.join(); }
    std::atomic<unsigned> callbacks{0};
private:
    RecorderAudioEngine& audio;
    unsigned rate;
    std::atomic<bool> stopping{false};
    std::thread worker;
};
}
