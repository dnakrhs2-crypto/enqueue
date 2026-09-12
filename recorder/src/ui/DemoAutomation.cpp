#include "MainComponent.h"
#include "model/SafeFileWrite.h"
#include <algorithm>
#include <chrono>

namespace gocue::recorder
{
void MainComponent::startDemo(int iterations, const juce::File& devices, int asioIndex, const juce::File& report)
{
    if (demo) return; demo = std::make_unique<Demo>(); demo->iterations = iterations; demo->devices = devices; demo->asioIndex = asioIndex; demo->report = report;
    demo->folder = report.getParentDirectory().getChildFile("demo-" + juce::Uuid().toString());
    if (iterations < 1 || iterations > 1000 || asioIndex < 0 || !devices.existsAsFile()) { finishDemo("FAIL", "Invalid demo arguments"); return; }
    createProject(ko("녹화 시연"), demo->folder, 60);
}
void MainComponent::finishDemo(const juce::String& status, const juce::String& reason)
{
    auto& d = *demo; if (d.step == Demo::Step::finished) return;
    auto report = jsonObject(); jsonSet(report, "schemaVersion", 1); jsonSet(report, "status", status); jsonSet(report, "reason", reason);
    jsonSet(report, "source", "real MF camera + ASIO; application button handlers"); jsonSet(report, "iterationsRequested", d.iterations); jsonSet(report, "iterationsCompleted", d.iteration);
    jsonSet(report, "iterations", d.rows); jsonSet(report, "project", d.folder.getFullPathName()); jsonSet(report, "devices", d.devices.getFullPathName());
    jsonSet(report, "asioDeviceIndex", d.asioIndex); jsonSet(report, "asioDevice", session.deviceInfo().name); jsonSet(report, "sampleRate", session.deviceInfo().sampleRate);
    jsonSet(report, "bufferFrames", session.deviceInfo().bufferFrames); jsonSet(report, "appVersion", ProductIdentity::version()); jsonSet(report, "ffmpegVersion", RECORDER_FFMPEG_VERSION);
    jsonSet(report, "measurementDefinition", "QPC at the actual Stop button handler, including scheduled Nstop and finalization. Clip time is the first timeline paint containing the new take (not optical scanout). Video is successful D3D Present submission; audio is first timeline PCM block dispatched through the existing ASIO output. Audible time adds reported output latency, not DAC/loopback measurement. OS file cache is not purged.");
    for (const auto* key : {"stopToClipMs", "stopToFirstVideoMs", "stopToFirstAudioMs", "stopToFirstAudibleEstimateMs", "stopToFirstPlaybackMs"})
    {
        std::vector<double> values; for (const auto& row : d.rows) if (row[key].isDouble()) values.push_back(double(row[key]));
        auto statistics = jsonObject(); if (!values.empty()) { std::sort(values.begin(), values.end()); jsonSet(statistics, "max", values.back()); jsonSet(statistics, "p95", values[std::size_t(std::ceil(values.size() * .95)) - 1]); }
        jsonSet(statistics, "count", int(values.size())); jsonSet(report, key, statistics);
    }
    const auto path = d.report; d.returnCode = status == "PASS" ? 0 : status == "UNAVAILABLE" ? 2 : 1;
    d.step = Demo::Step::finished; d.writing = std::async(std::launch::async, [path, report] { return gocue::SafeFileWrite::writeTextVerified(path, juce::JSON::toString(report, false)); });
}
void MainComponent::demoTick()
{
    auto& d = *demo; const auto now = qpcNow(); const auto seconds = double(now - d.phaseQpc) / qpcFrequency();
    if (d.step == Demo::Step::finished)
    {
        if (d.writing.valid() && d.writing.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            if (d.writing.get().failed()) d.returnCode = 1;
            juce::JUCEApplication::getInstance()->setApplicationReturnValue(d.returnCode);
            requestClose([] { juce::JUCEApplication::getInstance()->quit(); });
        }
        return;
    }
    if (seconds > 45) { finishDemo(d.iteration == 0 && d.step <= Demo::Step::ready ? "UNAVAILABLE" : "FAIL", "Application stage timeout: " + juce::String(int(d.step))); return; }
    if (d.step == Demo::Step::opening && !fileWork.valid())
    {
        if (document.getFile() == juce::File()) { finishDemo("FAIL", banner); return; }
        try
        {
            const auto source = juce::JSON::parse(d.devices); const auto selected = source["selections"]["cam1"];
            if (int(source["schemaVersion"]) != 1 || selected["symbolicLink"].toString().isEmpty()) throw std::runtime_error("devices.json needs a selected cam1");
            const auto names = RecorderAudioEngine::deviceNames();
            if (d.asioIndex >= names.size()) { finishDemo("UNAVAILABLE", "Selected ASIO registry index is missing"); return; }
            UserSettings s; s.asioDeviceId = names[d.asioIndex]; s.cameraEnabled = {true, false};
            s.cameraDeviceIds[0] = selected["symbolicLink"].toString(); s.cameraModes[0] = selected["mode"].toString();
            s.output.mono = true; s.output.monoChannel = 0; // explicit mono probe output, valid on one-output devices
            s.physicalInputs = {0}; s.microphoneArmed[0] = true;
            const auto r = session.configure(s); if (r.failed()) { finishDemo("UNAVAILABLE", r.getErrorMessage()); return; }
            d.step = Demo::Step::configuring; d.phaseQpc = now;
        }
        catch (const std::exception& e) { finishDemo("FAIL", juce::String::fromUTF8(e.what())); }
    }
    else if (d.step == Demo::Step::configuring && !session.configuring())
    { if (session.error.isNotEmpty()) { finishDemo("UNAVAILABLE", session.error); return; } d.step = Demo::Step::ready; d.phaseQpc = now; }
    else if (d.step == Demo::Step::ready && session.readyToRecord())
    {
        setTimeline(false); recordView.startButton.onClick(); d.step = Demo::Step::recording; d.phaseQpc = now;
    }
    else if (d.step == Demo::Step::recording)
    {
        if (session.takeController().state() == TakeController::State::partialFailure) { finishDemo("FAIL", session.takeController().error()); return; }
        // Exercise live tab switching while recording, without disturbing capture/present ownership.
        if (session.elapsed() >= Sample(session.deviceInfo().sampleRate) * 5 && !timeline) setTimeline(true);
        if (session.takeController().state() == TakeController::State::recording && session.elapsed() >= Sample(session.deviceInfo().sampleRate) * 10)
        {
            recordView.stopButton.onClick(); d.stopQpc = lastStopButtonQpc; d.clipQpc = 0;
            latestClicked(); d.step = Demo::Step::waitingPlayback; d.phaseQpc = now;
        }
    }
    else if (d.step == Demo::Step::waitingPlayback)
    {
        if (!d.clipQpc && session.takeController().placementMetadata().ready && !document.getProject().media->takes.empty()
            && timelineView.lastPaintedTake == document.getProject().media->takes.back().takeId
            && timelineView.lastClipPaintQpc >= d.stopQpc) d.clipQpc = timelineView.lastClipPaintQpc;
        if (session.takeController().state() == TakeController::State::partialFailure) { finishDemo("FAIL", session.error); return; }
        if (d.clipQpc && session.firstPlaybackVideoQpc && session.firstPlaybackAudioQpc)
        {
            const auto ms = [&](std::int64_t qpc) { return double(qpc - d.stopQpc) * 1000 / qpcFrequency(); };
            auto row = jsonObject(); jsonSet(row, "iteration", ++d.iteration); jsonSet(row, "stopButtonQpc", d.stopQpc);
            jsonSet(row, "playButtonQpc", session.playbackButtonQpc); jsonSet(row, "stopToClipMs", ms(d.clipQpc));
            jsonSet(row, "stopToFirstVideoMs", ms(session.firstPlaybackVideoQpc)); jsonSet(row, "stopToFirstAudioMs", ms(session.firstPlaybackAudioQpc));
            jsonSet(row, "stopToFirstAudibleEstimateMs", ms(session.firstPlaybackAudibleQpc));
            jsonSet(row, "stopToFirstPlaybackMs", ms(std::max(session.firstPlaybackVideoQpc, session.firstPlaybackAudibleQpc)));
            jsonSet(row, "pass", ms(d.clipQpc) <= 250 && ms(std::max(session.firstPlaybackVideoQpc, session.firstPlaybackAudibleQpc)) <= 2000);
            jsonSet(row, "take", session.takeController().report()); d.rows.add(row); d.step = Demo::Step::showingPlayback; d.phaseQpc = now;
        }
        else if (seconds > 15) { finishDemo("FAIL", session.error.isEmpty() ? "No first video/audio/clip within 15 seconds" : session.error); }
    }
    else if (d.step == Demo::Step::showingPlayback && seconds >= 1)
    {
        timelineView.transport.stop.onClick(); setTimeline(false);
        if (d.iteration == d.iterations) { bool pass = true; for (const auto& row : d.rows) pass = pass && bool(row["pass"]); finishDemo(pass ? "PASS" : "FAIL", pass ? "All button-to-clip and button-to-playback bounds met" : "One or more timing bounds exceeded"); }
        else { d.step = Demo::Step::ready; d.phaseQpc = now; }
    }
}
}
