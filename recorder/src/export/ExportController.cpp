#include "ExportController.h"
#include "app/RecorderDocument.h"
#include "support/Platform.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace gocue::recorder
{
ExportController::ExportController(Runner r) : runner(std::move(r)) {}
ExportController::~ExportController() { cancel(); wait(); }
juce::File ExportController::resolveDestination(const juce::File& desired)
{
    exportRequire(desired != juce::File() && desired.getFileName().isNotEmpty(), "Choose an export destination folder");
    auto result = desired;
    for (unsigned number = 2; result.exists(); ++number)
    {
        exportRequire(number < 1000000, "Too many export destination collisions");
        result = desired.getSiblingFile(desired.getFileName() + " (" + juce::String(number) + ")");
    }
    return result;
}
juce::Result ExportController::start(const RecorderDocument& d, Request request, FileIoFaultAdapter* faults)
{
    return start(d.getProject(), d.getFile().getParentDirectory(), std::move(request), d.isRecordingStructureLocked(), faults);
}
juce::Result ExportController::start(const RecorderProject& p, const juce::File& directory, Request request, bool recording, FileIoFaultAdapter* faults)
{
    if (busy()) return juce::Result::fail(juce::String::fromUTF8("내보내기가 진행 중입니다."));
    if (recording || activity.current() == ExportActivity::State::recording)
        return juce::Result::fail(juce::String::fromUTF8("녹화와 마무리가 끝난 뒤 내보내기를 시작하세요."));
    wait();
    lastProject.reset();
    { std::lock_guard<std::mutex> lock(mutex); current = {}; }
    try
    {
        auto desired = request.destination;
        if (desired == juce::File()) desired = directory.getChildFile("exports").getChildFile("export");
        const auto destination = resolveDestination(desired);
        auto job = std::make_shared<ExportJob>(p, directory, destination, request.range, recording);
        // Detect the legacy constructor's UUID fallback if a directory appeared
        // between collision resolution and snapshot creation. Never silently nest.
        exportRequire(job->outputDirectory == destination, "Export destination appeared; retry to choose the next suffix");
        if (request.mode == Mode::materials) MaterialExporter::outputs(*job, request.materials);
        else FinalVideoExporter::validateSelection(*job, request.finalSource);
        auto lease = std::make_shared<ExportActivity::Lease>(activity);
        // The coordinator reserves the shared gate before spawning. The existing
        // core owns a separate local gate for its entire render transaction.
        auto coreGate = std::make_shared<ExportActivity>();
        control = std::make_shared<ExportControl>(*coreGate);
        lastProject = std::make_shared<const RecorderProject>(job->snapshot); lastDirectory = directory; lastRequest = request;
        {
            std::lock_guard<std::mutex> lock(mutex); current = {}; current.state = State::running; current.outputDirectory = destination;
            current.mode = request.mode; current.projectId = job->snapshot.projectId; current.editRevision = job->snapshot.editRevision;
            if (destination != desired) current.notice = juce::String::fromUTF8("같은 이름이 있어 다음 폴더에 저장합니다: ") + destination.getFileName();
        }
        const auto totalWorkFrames = double(job->range.frameCount) * (request.mode == Mode::materials
            ? double(MaterialExporter::outputs(*job, request.materials).size()) : 1.0);
        control->onProgress = [this, totalWorkFrames](const ExportProgress& p)
        {
            std::lock_guard<std::mutex> lock(mutex);
            current.progress = p;
            // A measured frame-equivalent rate includes WAV, preparation and
            // validation work. It is an ETA estimate, not encoder-only throughput.
            const auto rate = p.elapsedSeconds > .05 ? totalWorkFrames * p.fraction / p.elapsedSeconds : 0;
            current.estimatedEffectiveFps = std::isfinite(rate) ? rate : 0;
            current.progress.etaSeconds = rate > 0 ? std::optional<double>(totalWorkFrames * (1 - p.fraction) / rate) : std::nullopt;
        };
        active.store(true, std::memory_order_release);
        worker = std::thread([this, job, request, faults, lease = std::move(lease), coreGate, c = control]() mutable
        {
            State terminal = State::failed; juce::String error; juce::var result;
            try
            {
                ComApartment apartment;
                result = runner ? runner(*job, request, *c, faults) : request.mode == Mode::materials
                    ? MaterialExporter::run(*job, request.materials, *c, faults)
                    : FinalVideoExporter::run(*job, request.finalSource, *c, {}, faults);
                terminal = State::completed;
            }
            catch (const ExportCancelled&) { terminal = State::cancelled; }
            catch (const std::exception& e) { error = juce::String::fromUTF8(e.what()); }
            catch (...) { error = juce::String::fromUTF8("내보내기 중 알 수 없는 오류가 발생했습니다."); }
            lease.reset(); // completed/cancelled is a quiescent checkpoint
            {
                std::lock_guard<std::mutex> lock(mutex); current.state = terminal; current.error = error; current.manifest = result;
                if (terminal == State::completed) { current.progress.fraction = 1; current.progress.etaSeconds = 0.0; }
            }
            active.store(false, std::memory_order_release);
        });
        return juce::Result::ok();
    }
    catch (const std::exception& e)
    {
        active.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex); current.state = State::failed; current.error = juce::String::fromUTF8(e.what());
        return juce::Result::fail(current.error);
    }
}
juce::Result ExportController::retry(std::optional<juce::File> destination, FileIoFaultAdapter* faults)
{
    const auto state = status().state;
    if (!lastProject || busy() || (state != State::failed && state != State::cancelled))
        return juce::Result::fail(juce::String::fromUTF8("실패하거나 취소된 내보내기만 재시도할 수 있습니다."));
    auto request = lastRequest; if (destination) request.destination = *destination;
    auto snapshot = lastProject;
    return start(*snapshot, lastDirectory, std::move(request), false, faults);
}
void ExportController::cancel()
{
    if (!busy()) return;
    if (control) control->cancelled.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex);
    if (current.state == State::running) current.state = State::cancelling;
}
ExportController::Status ExportController::status() const
{ std::lock_guard<std::mutex> lock(mutex); auto copy = current; copy.manifest = current.manifest.clone(); return copy; }
bool ExportController::beforeRecording()
{
    if (busy())
    {
        cancel(); std::lock_guard<std::mutex> lock(mutex);
        current.notice = juce::String::fromUTF8("녹화를 시작하기 위해 내보내기를 멈추는 중입니다. 작업이 정지되면 녹화를 시작합니다.");
        return false;
    }
    wait();
    return activity.current() == ExportActivity::State::recording || activity.beginRecording();
}
void ExportController::endRecording() { activity.endRecording(); }
void ExportController::wait() { if (worker.joinable()) worker.join(); }
}
