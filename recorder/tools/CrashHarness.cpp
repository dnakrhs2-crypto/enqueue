#include "CrashFixtures.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <thread>

using namespace gocue::recorder;
using namespace gocue::recorder::recovery;
namespace
{
const char* cases[]{"fragment-write", "wav-header-write", "chunk-replace", "final-moov-write", "checkpoint-replace",
    "take-before-commit", "take-after-commit", "recovery-before-commit", "recovery-after-commit"};
struct Options
{
    bool child = false, large = false; unsigned iterations = 5, caseOffset = 0, seconds = 6;
    juce::String selected = "all", event; juce::File report, root;
};
unsigned numberArgument(const char* p)
{
    unsigned n = 0; const auto end = p + std::strlen(p); const auto result = std::from_chars(p, end, n);
    require(result.ec == std::errc() && result.ptr == end, "Expected unsigned decimal argument"); return n;
}
Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String name(argv[i]);
        if (name == "--child") { o.child = true; continue; }
        if (name == "--include-large-files") { o.large = true; continue; }
        require(i + 1 < argc, "Missing option value"); const auto* v = argv[++i];
        if (name == "--iterations") o.iterations = numberArgument(v);
        else if (name == "--case-offset") o.caseOffset = numberArgument(v);
        else if (name == "--seconds") o.seconds = numberArgument(v);
        else if (name == "--crash-cases") o.selected = v;
        else if (name == "--ready-event") o.event = v;
        else if (name == "--report") o.report = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(v));
        else if (name == "--root") o.root = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(v));
        else throw std::invalid_argument("Unknown option: " + name.toStdString());
    }
    require(o.iterations > 0 && o.iterations <= 10000 && o.seconds >= 5 && o.seconds <= 10, "Use --iterations 1..10000 and --seconds 5..10");
    bool known = o.selected == "all"; for (const auto* c : cases) known |= o.selected == c; require(known, "Unknown crash case");
    require(o.child ? o.root != juce::File() && o.event.isNotEmpty() && o.selected != "all" : o.report != juce::File(), "Require --report, or --child --root --ready-event --crash-cases NAME"); return o;
}
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Gate : FileIoFaultAdapter
{
    const Options& options; std::atomic<bool> armed{false}, fired{false}; std::atomic<std::uint64_t> submitted{0};
    Id takeId; HANDLE ready;
    Gate(const Options& o, HANDLE event) : options(o), ready(event) {}
    void hit(const char* stage)
    {
        if (!armed.load() || options.selected != stage || fired.exchange(true)) return;
        auto v = object(); set(v, "case", stage); set(v, "submittedSamples", integer(std::int64_t(submitted.load()))); set(v, "takeId", takeId);
        set(v, "recordSeconds", double(submitted.load()) / 48000); set(v, "pid", int(GetCurrentProcessId()));
        writeJson(options.root.getChildFile("crash-stage.json"), v);
        require(SetEvent(ready) != 0, "Signal crash stage");
        // Only the dedicated child waits here. Parent times out and terminates it.
        for (;;) Sleep(20);
    }
    juce::Result beforeIo(FileIoOperation operation, const juce::File& file, std::uint64_t offset, std::size_t) override
    {
        if (file.hasFileExtension("wav") && operation == FileIoOperation::patchProgress && offset < 44) hit("wav-header-write");
        if (file.getFileName() == "000002.wav" && operation == FileIoOperation::open) hit("chunk-replace");
        return juce::Result::ok();
    }
};
// Build placement metadata from actual journal IDs and durable per-chunk ranges.
void place(RecorderDocument& document, const juce::File& root, const juce::Uuid& takeId, const Id& cameraId, Sample samples, crashFixture::TicketSink& sink)
{
    JournalReplay read; check(RecordingJournal::replay(root.getChildFile("journal"), read));
    const juce::var* start = nullptr; std::map<juce::String, JournalFilePosition> positions;
    for (const auto& record : read.records)
    {
        if (record.payload["takeId"].toString() != takeId.toDashedString()) continue;
        if (record.kind == JournalKind::TakeStarted) start = &record.payload;
        if (record.kind == JournalKind::Checkpoint) for (const auto& f : *record.payload["files"].getArray())
        { JournalFilePosition p; p.path = f["path"].toString(); p.firstSample = std::uint64_t(number(f["firstSample"])); p.validSamples = std::uint64_t(number(f["validSamples"])); positions[p.path] = p; }
    }
    require(start != nullptr, "Placement missing TakeStarted");
    Take take; take.takeId = takeId.toString(); take.number = 7; take.createdAt = "2026-09-09T00:00:06Z"; take.logicalLength = samples; take.cam1AssetId = cameraId; take.state = TakeState::finalising;
    std::vector<MediaAsset> assets;
    for (const auto& f : *(*start)["files"].getArray())
    {
        MediaAsset a; a.assetId = juce::Uuid(f["assetId"].toString()).toString(); a.logicalLength = samples; a.availableRanges = {{0, samples}}; a.contentIdentity = "synthetic:" + a.assetId;
        if (f["path"].toString().endsWith(".mp4"))
        { a.relativePath = f["path"].toString(); a.originalFormat.codec = "h264"; a.originalFormat.width = 1920; a.originalFormat.height = 1080; a.sourceUnitsNumerator = 30; a.sourceUnitsDenominator = 48000; }
        else
        {
            a.kind = AssetKind::mic; a.originalFormat.codec = "pcm_s24le"; a.originalFormat.sampleRate = 48000; a.originalFormat.channels = 1; a.originalFormat.bitsPerSample = 24;
            const auto prefix = f["path"].toString().upToLastOccurrenceOf("/", true, false);
            for (const auto& p : positions) if (p.first.startsWith(prefix) && p.second.validSamples) a.chunks.push_back({p.first, {Sample(p.second.firstSample), Sample(p.second.validSamples)}});
            take.capture.physicalInputs.push_back(int(take.microphoneAssetIds.size())); take.microphoneAssetIds.push_back(a.assetId);
        }
        assets.push_back(a);
    }
    document.setJournalSink(&sink); check(document.placeTake(take, assets));
}
int child(const Options& o)
{
    Handle event; event.value = OpenEventW(EVENT_MODIFY_STATE, FALSE, o.event.toWideCharPointer()); require(event.value != nullptr, "Open parent crash event");
    auto project = crashFixture::baseline(o.root); Gate gate(o, event.value);
    auto config = crashFixture::wavConfig(o.root); config.faults = &gate; config.testChunkFrames = 5 * 48000;
    const juce::Uuid cameraId; gate.takeId = config.takeId.toString();
    const auto prefix = "media/takes/" + config.takeId.toDashedString() + "/cam1";
    config.additionalFiles.push_back({cameraId.toDashedString(), prefix + ".recording.mp4", prefix + ".mp4"});
    auto camera = std::make_unique<crashFixture::Camera>(o.root.getChildFile(prefix + ".mp4"), &gate, [&](const char* stage) { gate.hit(stage); });
    WavTrackWriter wav(config); check(wav.start()); const auto begin = std::chrono::steady_clock::now();
    const auto frames = int(o.seconds * 30 + (o.seconds == 5 && o.selected == "chunk-replace" ? 1u : 0u));
    for (int frame = 0; frame < frames; ++frame)
    {
        if (frame >= 149) gate.armed.store(true);
        while (gate.fired.load()) Sleep(20);
        std::this_thread::sleep_until(begin + std::chrono::microseconds((std::int64_t(frame) + 1) * 1000000 / 30));
        camera->frame(); crashFixture::push(wav, std::uint64_t(frame) * 1600, 1600, 8); gate.submitted.store(std::uint64_t(frame + 1) * 1600);
    }
    const auto samples = Sample(frames) * 1600; const juce::Uuid placement;
    check(wav.stop(samples, placement));
    RecorderDocument document; CheckpointInfo info; check(document.adopt(project, o.root.getChildFile("project.recorder"), info));
    crashFixture::TicketSink sink; place(document, o.root, config.takeId, cameraId.toString(), samples, sink);
    // The take stop record and edit journal use the same stable transaction ID.
    sink.ticket.transactionId = placement.toString();
    RecoveryScanner::appendEdit(o.root.getChildFile("journal/edits-000001.log"), sink.ticket, document.getProject(), &gate,
        [&](const char* stage) { gate.hit(juce::String(stage) == "journal-before-commit" ? "take-before-commit" : "take-after-commit"); });
    if (o.selected.startsWith("recovery-"))
    {
        camera.reset(); RecoveryReport report; RecoveryScanner scanner({&gate, [&](const char* s) { gate.hit(s); }}); check(scanner.run(o.root, report));
    }
    else
    {
        camera->finish();
        RecoveryScanner::writeCheckpoint(o.root.getChildFile("project.recorder"), document.getProject(), &gate, [&](const char* s) { gate.hit(s); });
    }
    throw std::runtime_error("Requested crash stage was never reached");
}
juce::var hashRows(const std::map<juce::String, juce::String>& before, const std::map<juce::String, juce::String>& after)
{
    juce::Array<juce::var> result;
    for (const auto& entry : before)
    { auto v = object(); set(v, "path", entry.first); set(v, "beforeSha256", entry.second); const auto found = after.find(entry.first); set(v, "afterSha256", found == after.end() ? "MISSING" : found->second); result.add(v); }
    return result;
}
juce::var iteration(const Options& options, unsigned iterationNumber, const char* stage, const juce::File& root)
{
    check(root.createDirectory()); const auto eventName = "Local\\RecorderCrash-" + juce::Uuid().toString(); Handle event;
    event.value = CreateEventW(nullptr, TRUE, FALSE, eventName.toWideCharPointer()); require(event.value != nullptr, "Create child stage event");
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto command = exe.getFullPathName().quoted() + " --child --root " + root.getFullPathName().quoted() + " --ready-event " + eventName.quoted()
        + " --crash-cases " + juce::String(stage) + " --seconds " + juce::String(options.seconds);
    auto wide = command.toWideCharPointer(); std::vector<wchar_t> cmd(wide, wide + command.length() + 1);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE; PROCESS_INFORMATION process{};
    require(CreateProcessW(exe.getFullPathName().toWideCharPointer(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0, "Create crash child");
    Handle childProcess, childThread; childProcess.value = process.hProcess; childThread.value = process.hThread;
    const HANDLE handles[]{event.value, childProcess.value}; const auto wait = WaitForMultipleObjects(2, handles, FALSE, (options.seconds + 20) * 1000);
    if (wait != WAIT_OBJECT_0)
    {
        if (wait != WAIT_OBJECT_0 + 1) { TerminateProcess(childProcess.value, 0xdead); WaitForSingleObject(childProcess.value, 5000); }
        throw std::runtime_error("Child failed/timed out before " + std::string(stage) + ": " + root.getChildFile("child-error.txt").loadFileAsString().toStdString());
    }
    require(TerminateProcess(childProcess.value, 0xdead) != 0, "TerminateProcess child"); require(WaitForSingleObject(childProcess.value, 5000) == WAIT_OBJECT_0, "Wait for terminated child");
    DWORD exit = 0; require(GetExitCodeProcess(childProcess.value, &exit) && exit == 0xdead, "Unexpected child exit code");
    const auto progress = juce::JSON::parse(root.getChildFile("crash-stage.json").loadFileAsString()); require(progress["case"].toString() == stage, "Crash stage handshake mismatch");
    const auto before = crashFixture::hashes(root, true); RecoveryReport first; RecorderDocument document;
    RecoveryScanner scanner; check(scanner.run(root, first, &document));
    const auto after = crashFixture::hashes(root, true); const auto allBefore = crashFixture::hashes(root);
    RecoveryReport second; check(scanner.run(root, second)); const auto allAfter = crashFixture::hashes(root);
    const auto takeId = progress["takeId"].toString(); const auto* take = first.project.media->findTake(takeId); require(take != nullptr, "Crashed take was not recovered");
    const auto submitted = number(progress["submittedSamples"]); bool ranges = true, pcm = true; double maxLoss = 0; juce::Array<juce::var> rangesReport;
    for (const auto& asset : first.project.media->assets)
    {
        if (asset.assetId != take->cam1AssetId && std::find(take->microphoneAssetIds.begin(), take->microphoneAssetIds.end(), asset.assetId) == take->microphoneAssetIds.end()) continue;
        Sample validEnd = 0; for (const auto& r : asset.availableRanges) validEnd = std::max(validEnd, r.start + r.length);
        const double loss = double(std::max<Sample>(0, submitted - validEnd)) / 48000; maxLoss = std::max(maxLoss, loss); ranges &= loss <= 2.0 && validEnd > 0;
        if (asset.kind == AssetKind::mic) pcm &= crashFixture::verifyPcm(root, asset, unsigned(std::find(take->microphoneAssetIds.begin(), take->microphoneAssetIds.end(), asset.assetId) - take->microphoneAssetIds.begin()));
        auto v = object(); set(v, "assetId", asset.assetId); set(v, "kind", asset.kind == AssetKind::mic ? "mic" : "camera"); set(v, "sourceSamples", integer(submitted)); set(v, "validEnd", integer(validEnd)); set(v, "tailLossSeconds", loss); rangesReport.add(v);
    }
    bool baseline = false; for (const auto& t : first.project.media->takes) if (t.number == 6 && t.state == TakeState::complete) baseline = true;
    const bool edit = first.project.markers.size() == 1 && first.project.markers[0].name == "saved-before-crash";
    const bool idempotent = second.changedTakes == 0 && second.addedClips == 0 && allBefore == allAfter && RecorderSerializer::toJson(first.project) == RecorderSerializer::toJson(second.project);
    const bool adopted = RecorderSerializer::toJson(document.getProject()) == RecorderSerializer::toJson(first.project);
    const bool passed = before == after && baseline && edit && ranges && pcm && idempotent && adopted && take->microphoneAssetIds.size() == 8;
    auto result = object(); set(result, "iteration", int(iterationNumber)); set(result, "case", stage); set(result, "status", passed ? "PASS" : "FAIL");
    set(result, "project", root.getFullPathName()); set(result, "childPid", int(process.dwProcessId)); set(result, "terminateExitCode", integer(exit)); set(result, "crashStage", progress);
    set(result, "completedTakePreserved", baseline); set(result, "durableEditPreserved", edit); set(result, "originalHashesUnchanged", before == after); set(result, "originalHashes", hashRows(before, after));
    set(result, "normalIoTailWithin2Seconds", ranges); set(result, "maxTailLossSeconds", maxLoss); set(result, "ranges", rangesReport); set(result, "pcmOracleMatch", pcm);
    set(result, "secondRunNoChanges", idempotent); set(result, "documentAdoptedAfterCommit", adopted); set(result, "firstRecovery", first.toJson()); set(result, "secondRecovery", second.toJson()); return result;
}
int parent(const Options& options)
{
    require(!options.report.exists(), "Report already exists; use a new path to retain previous evidence");
    check(options.report.getParentDirectory().createDirectory());
    const auto runRoot = options.report.getParentDirectory().getChildFile("crash-media-" + juce::Uuid().toString()); check(runRoot.createDirectory());
    auto report = object(); set(report, "schemaVersion", 1); set(report, "ffmpegBuild", RECORDER_FFMPEG_VERSION);
    set(report, "startedUtc", juce::Time::getCurrentTime().toISO8601(true)); set(report, "source", "Synthetic CPU OpenH264 1920x1080p30 + AAC reference + 8 mono PCM24 WAV; actual Recorder writers");
    set(report, "powerLoss", juce::String::fromUTF8("미확인 → 스파이크 4")); set(report, "caseOffset", int(options.caseOffset)); set(report, "iterationsRequested", int(options.iterations));
    juce::Array<juce::var> results; unsigned failures = 0;
    for (unsigned i = 0; i < options.iterations; ++i)
    {
        const auto selected = options.selected == "all" ? juce::String(cases[(options.caseOffset + i) % std::size(cases)]) : options.selected;
        const auto root = runRoot.getChildFile(juce::String(i + 1).paddedLeft('0', 4) + "-" + selected);
        try { auto result = iteration(options, i + 1, selected.toRawUTF8(), root); if (result["status"].toString() != "PASS") ++failures; results.add(result); }
        catch (const std::exception& e) { auto result = object(); set(result, "iteration", int(i + 1)); set(result, "case", selected); set(result, "status", "FAIL"); set(result, "error", e.what()); set(result, "project", root.getFullPathName()); results.add(result); ++failures; }
        std::cout << (i + 1) << '/' << options.iterations << ' ' << selected << ' ' << results.getLast()["status"].toString() << std::endl;
    }
    if (options.large)
    {
        try { set(report, "largeFiles", crashFixture::largeFaultChecks(runRoot.getChildFile("large-fault-adapter"))); }
        catch (const std::exception& e) { set(report, "largeFilesError", e.what()); ++failures; }
    }
    set(report, "results", results); set(report, "failures", int(failures)); set(report, "status", failures ? "FAIL" : "PASS"); set(report, "endedUtc", juce::Time::getCurrentTime().toISO8601(true));
    juce::StringArray coverage, pending;
    for (const auto* c : cases) { bool covered = false; for (const auto& r : results) covered |= r["case"].toString() == c && r["status"].toString() == "PASS"; (covered ? coverage : pending).add(c); }
    set(report, "passedCases", juce::var(coverage)); set(report, "casesNotPassedInThisRun", juce::var(pending)); writeJson(options.report, report);
    std::cout << (failures ? "FAIL" : "PASS") << " report " << options.report.getFullPathName() << std::endl; return failures ? 1 : 0;
}
}
int main(int argc, char** argv)
{
    Options options;
    try { av_log_set_level(AV_LOG_ERROR); options = parse(argc, argv); return options.child ? child(options) : parent(options); }
    catch (const std::exception& e)
    {
        if (options.child && options.root != juce::File()) try { writeNew(options.root.getChildFile("child-error.txt"), e.what(), std::strlen(e.what())); } catch (...) {}
        std::cerr << e.what() << "\nRecorderCrashHarness --iterations 5 --crash-cases all --include-large-files --report PATH [--case-offset N] [--seconds 5..10]\n"; return 1;
    }
}
