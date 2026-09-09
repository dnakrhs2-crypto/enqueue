#include "playback/VideoPlaybackEngine.h"
#include "playback/TimelineTransport.h"
#include "record/WavTrackWriter.h"
#include "record/Ffmpeg.h"
#include "diagnostics/CaptureTelemetry.h"
#include <juce_events/juce_events.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace gocue::recorder
{
namespace
{
struct Options
{
    std::map<juce::String, juce::String> values;
    bool stopToPlay = false;
    bool indexOnly = false;
    juce::Array<juce::var> command;
    juce::String get(const char* key, const char* fallback = "") const
    { const auto it = values.find(key); return it == values.end() ? juce::String(fallback) : it->second; }
};
int number(const juce::String& s, int low, int high)
{
    if (s.isEmpty() || !s.containsOnly("0123456789")) throw std::invalid_argument("Expected nonnegative integer option");
    const auto value = s.getLargeIntValue();
    if (value < low || value > high) throw std::invalid_argument("Playback option out of range"); return static_cast<int>(value);
}
juce::File path(const juce::String& text) { return juce::File::getCurrentWorkingDirectory().getChildFile(text); }
void check(const juce::Result& r) { if (r.failed()) throw std::runtime_error(r.getErrorMessage().toStdString()); }
struct Window
{
    HWND window = nullptr;
    std::array<HWND, 2> hosts{};
    std::array<HWND, 2> placeholders{};
    std::array<bool, 2> gaps{{true, true}};
    bool closed = false;
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l)
    {
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_CLOSE) { self->closed = true; return 0; }
        if (self && message == WM_SIZE)
        {
            const auto width = LOWORD(l), height = HIWORD(l);
            if (self->hosts[0]) MoveWindow(self->hosts[0], 0, 0, width / 2, height, TRUE);
            if (self->hosts[1]) MoveWindow(self->hosts[1], width / 2, 0, width - width / 2, height, TRUE);
            for (unsigned i = 0; i < 2; ++i) if (self->placeholders[i])
                MoveWindow(self->placeholders[i], 0, height / 2 - 12, width / 2, 24, TRUE);
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
    Window()
    {
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"RecorderR11PlaybackProbe"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register playback probe window failed");
        window = CreateWindowExW(0, cls.lpszClassName, L"Recorder playback | cam1 / cam2", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 480, nullptr, nullptr, cls.hInstance, this);
        if (!window) throw std::runtime_error("Create playback probe window failed");
        for (unsigned i = 0; i < 2; ++i)
            hosts[i] = CreateWindowExW(0, L"STATIC", i == 0 ? L"cam1" : L"영상 없음", WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
                i * 620, 0, 620, 440, window, nullptr, cls.hInstance, nullptr);
        if (!hosts[0] || !hosts[1]) throw std::runtime_error("Create playback HWND hosts failed");
        for (unsigned i = 0; i < 2; ++i)
            placeholders[i] = CreateWindowExW(0, L"STATIC", L"영상 없음", WS_CHILD | WS_VISIBLE | SS_CENTER,
                0, 208, 620, 24, hosts[i], nullptr, cls.hInstance, nullptr);
    }
    ~Window() { if (window) DestroyWindow(window); }
    void pump()
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { if (message.message == WM_QUIT) closed = true; TranslateMessage(&message); DispatchMessageW(&message); }
        if (closed) throw std::runtime_error("Playback probe cancelled by user");
    }
    void updateGaps(const VideoPlaybackEngine& engine)
    {
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto gap = engine.displaySelection(i).gap;
            if (gaps[i] != gap) { gaps[i] = gap; ShowWindow(placeholders[i], gap ? SW_SHOWNOACTIVATE : SW_HIDE); }
        }
    }
};
Sample metadataDuration(const juce::File& file, std::uint32_t rate)
{
    struct Input { AVFormatContext* p = nullptr; ~Input() { avformat_close_input(&p); } } input;
    ffCheck(avformat_open_input(&input.p, file.getFullPathName().toRawUTF8(), nullptr, nullptr), "Read fixture duration");
    ffCheck(avformat_find_stream_info(input.p, nullptr), "Read fixture video metadata");
    const auto index = av_find_best_stream(input.p, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0); ffCheck(index, "Find fixture video");
    const auto* stream = input.p->streams[index];
    if (stream->duration <= 0 || stream->duration == AV_NOPTS_VALUE) throw std::invalid_argument("Fixture has no video duration");
    return av_rescale_q(stream->duration, stream->time_base, {1, static_cast<int>(rate)});
}
struct Fixture
{
    juce::File root;
    juce::Uuid take;
};
Fixture synthesizeWav(Sample length, std::uint32_t rate)
{
    Fixture fixture{juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("gocue-rec-r11-fixture-" + juce::Uuid().toString()), juce::Uuid()};
    WavTrackWriter::Config c; c.projectDirectory = fixture.root; c.takeId = fixture.take;
    c.sampleRate = rate; c.mics = 1; c.framesPerBlock = 4096;
    c.devices = {{"synthetic-r11", "440 Hz synthetic WAV, 0.10 peak", 1, 0, 0}};
    WavTrackWriter writer(c); check(writer.start());
    std::vector<std::int32_t> pcm(c.framesPerBlock);
    for (Sample at = 0; at < length;)
    {
        const auto frames = static_cast<std::uint32_t>((std::min)(Sample(c.framesPerBlock), length - at));
        for (std::uint32_t i = 0; i < frames; ++i)
            pcm[i] = static_cast<std::int32_t>(std::sin(6.283185307179586 * 440.0 * static_cast<double>(at + i) / rate) * 838860.0);
        while (writer.queueFrames() + frames > writer.queueCapacityFrames())
        { check(writer.status()); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        if (!writer.tryPush(pcm.data(), frames, static_cast<std::uint64_t>(at))) throw std::runtime_error("Synthetic WAV fixture queue failed");
        at += frames;
    }
    check(writer.stop(length, juce::Uuid())); return fixture;
}
double percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end()); return values[static_cast<std::size_t>(std::ceil(values.size() * p)) - 1];
}
}
int runPlaybackProbe(int argc, wchar_t** argv)
{
    auto report = jsonObject(); Options options;
    juce::File reportFile; std::string stage = "arguments"; bool beganPlayback = false;
    const auto hz = qpcFrequency();
    jsonSet(report, "result", "FAIL"); jsonSet(report, "probe", "playback");
    jsonSet(report, "ffmpegVersion", av_version_info()); jsonSet(report, "startedAt", utcNowIso8601());
    try
    {
        for (int i = 0; i < argc; ++i) options.command.add(juce::String(argv[i]));
        jsonSet(report, "command", options.command);
        const std::set<juce::String> valued{"--media-dir", "--report", "--seek-storm", "--asio-device", "--asio-outputs", "--buffer-size", "--sample-rate"};
        for (int i = 2; i < argc; ++i)
        {
            const juce::String arg(argv[i]);
            if (arg == "--stop-to-play") { options.stopToPlay = true; continue; }
            if (arg == "--index-only") { options.indexOnly = true; continue; }
            if (!valued.count(arg) || i + 1 == argc || options.values.count(arg)) throw std::invalid_argument("Unknown, duplicate or missing playback option");
            options.values[arg] = juce::String(argv[++i]);
            if (arg == "--report") reportFile = path(options.values[arg]);
        }
        if (options.get("--media-dir").isEmpty() || reportFile == juce::File()) throw std::invalid_argument("playback requires --media-dir DIR --report FILE");
        const auto directory = path(options.get("--media-dir")); const auto videoFile = directory.getChildFile("cam1.mp4");
        const auto storm = number(options.get("--seek-storm", "0"), 0, 10000);
        std::uint32_t rate = static_cast<std::uint32_t>(number(options.get("--sample-rate", "48000"), 8000, 768000));
        const bool manifest = directory.getChildFile("take.json").existsAsFile();
        if (manifest)
        {
            const auto m = juce::JSON::parse(directory.getChildFile("take.json"));
            const auto fs = m["sampleRate"].toString();
            rate = static_cast<std::uint32_t>(number(fs, 8000, 768000));
            if (!options.get("--sample-rate").isEmpty() && options.get("--sample-rate").getLargeIntValue() != rate)
                throw std::invalid_argument("Requested rate differs from WAV manifest");
        }
        stage = "fixture";
        const auto lengthHint = metadataDuration(videoFile, rate);
        Fixture fixture;
        if (!manifest)
        {
            const auto wavs = directory.findChildFiles(juce::File::findFiles, true, "*.wav");
            if (!wavs.isEmpty()) throw std::invalid_argument("Existing WAV chunks require a complete schema-1 playback take.json; samples will not be guessed");
            fixture = synthesizeWav(lengthHint, rate);
        }
        jsonSet(report, "mediaDirectory", directory.getFullPathName()); jsonSet(report, "audioSource", manifest ? "manifest PCM24 WAV" : "synthetic PCM24 WAV via round-06 WavTrackWriter");
        if (!manifest) { jsonSet(report, "fixtureProject", fixture.root.getFullPathName()); jsonSet(report, "fixtureTakeId", fixture.take.toDashedString()); }
        jsonSet(report, "fixturePreparation", "Before measured stop marker; metadata duration read may warm OS cache; input MP4 unchanged");
        jsonSet(report, "measurementDefinition", "Synthetic Stop marker with finalized/renamed fixture already available -> successful DXGI Present and ASIO first PCM callback; audible time also estimated with driver output latency. No TakeController/finalize duration included.");
        jsonSet(report, "physicalDisplayAndDac", "UNAVAILABLE: Present success is not a photon timestamp; driver latency is not a measured DAC timestamp");
        jsonSet(report, "stopToPlayRequested", options.stopToPlay);
        jsonSet(report, "finalizeMs", juce::var());
        if (options.indexOnly)
        {
            stage = "media-index"; const auto start = qpcNow(); MediaIndex index;
            const auto indexed = index.openVideo(videoFile, rate);
            const auto sources = manifest ? index.openWavManifest(directory) : index.openWavJournal(fixture.root, fixture.take);
            jsonSet(report, "videoPackets", indexed->packets.size()); jsonSet(report, "idrCount", indexed->idrs.size());
            jsonSet(report, "timelineSamples", indexed->length); jsonSet(report, "wavTracks", sources.size());
            jsonSet(report, "indexMs", 1000.0 * (qpcNow() - start) / hz);
            jsonSet(report, "stage", stage); jsonSet(report, "result", "PASS");
            jsonSet(report, "reason", "Offline final-file index validation only; no ASIO device, GPU decoder or playback opened");
            CaptureTelemetry::writeJson(reportFile, report); std::cout << juce::JSON::toString(report, false) << '\n'; return 0;
        }
        juce::ScopedJuceInitialiser_GUI runtime;
        Window window;
        auto output = makeAsioPlaybackOutput(); AudioOutputConfig config;
        config.sampleRate = rate; config.deviceIndex = number(options.get("--asio-device", "0"), 0, 1024);
        config.bufferFrames = number(options.get("--buffer-size", "0"), 0, 262144);
        const auto map = juce::StringArray::fromTokens(options.get("--asio-outputs", "1:2"), ":", "");
        if (map.size() < 1 || map.size() > 2) throw std::invalid_argument("--asio-outputs expects one-based L:R or mono channel");
        config.leftPhysical = number(map[0], 1, 1024) - 1;
        config.rightPhysical = map.size() == 2 ? number(map[1], 1, 1024) - 1 : config.leftPhysical;
        stage = "device"; const auto info = output->open(config);
        jsonSet(report, "asioDriver", info.driver); jsonSet(report, "sampleRate", info.sampleRate); jsonSet(report, "blockFrames", info.blockFrames);
        jsonSet(report, "outputLatencySamples", info.outputLatency); jsonSet(report, "leftPhysicalChannel", info.leftPhysical + 1); jsonSet(report, "rightPhysicalChannel", info.rightPhysical + 1);
        const auto stoppedAt = qpcNow(); stage = "media-index";
        jsonSet(report, "stopMarkerQpc", stoppedAt); jsonSet(report, "qpcFrequency", hz);
        MediaIndex index;
        const auto video = index.openVideo(videoFile, rate);
        const auto audioSources = manifest ? index.openWavManifest(directory) : index.openWavJournal(fixture.root, fixture.take);
        std::vector<PlaybackVideoClip> videoClips;
        const auto addVideo = [&](unsigned camera, std::shared_ptr<const VideoIndex> source)
        {
            PlaybackVideoClip c; c.camera = camera; c.source = std::move(source); c.mapping.clipId = newId();
            c.mapping.assetId = newId(); c.mapping.mediaGeneration = static_cast<Sample>(c.source->generation); c.mapping.lengthSamples = c.source->length;
            videoClips.push_back(std::move(c));
        };
        addVideo(0, video);
        if (directory.getChildFile("cam2.mp4").existsAsFile()) addVideo(1, index.openVideo(directory.getChildFile("cam2.mp4"), rate));
        const unsigned cameras = static_cast<unsigned>(videoClips.size());
        Sample timelineEnd = video->length;
        for (const auto& c : videoClips) timelineEnd = (std::max)(timelineEnd, c.mapping.lengthSamples);
        std::vector<PlaybackAudioTrack> tracks;
        for (const auto& source : audioSources)
        {
            PlaybackAudioTrack track; track.trackId = source->trackId;
            PlaybackAudioClip c; c.source = source; c.mapping.clipId = newId(); c.mapping.trackId = track.trackId;
            c.mapping.mediaGeneration = static_cast<Sample>(source->generation); c.mapping.lengthSamples = source->length;
            timelineEnd = (std::max)(timelineEnd, source->length);
            if (source->length) track.clips.push_back(std::move(c)); tracks.push_back(std::move(track));
        }
        jsonSet(report, "timelineSamples", timelineEnd); jsonSet(report, "videoPackets", video->packets.size()); jsonSet(report, "idrCount", video->idrs.size());
        jsonSet(report, "indexMs", 1000.0 * (qpcNow() - stoppedAt) / hz);
        TimelineAudioRenderer audio(rate, info.blockFrames); audio.setPlan(std::move(tracks), timelineEnd);
        VideoPlaybackEngine engine; engine.prepare(std::move(videoClips));
        TimelineTransport transport(rate, hz, audio.queue(), timelineEnd);
        // Callback barrier must run before transport/PCM destructors, including all
        // startup exceptions. ASIO ownership itself was established before Stop.
        struct CloseOutput { IAudioOutput& output; ~CloseOutput() { output.close(); } } close{*output};
        stage = "playback"; output->start(transport);
        for (unsigned camera = 0; camera < cameras; ++camera) engine.attachPlaybackView(camera, window.hosts[camera]);
        transport.seek(0); transport.play();
        auto pump = [&]
        {
            window.pump(); transport.service(audio, engine, *output, qpcNow()); check(transport.status());
            window.updateGaps(engine);
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
        };
        std::array<std::int64_t, 2> firstVideo{}; TransportSnapshot firstAudio;
        const auto deadline = stoppedAt + hz * 15;
        while (qpcNow() < deadline)
        {
            pump();
            for (unsigned camera = 0; camera < cameras; ++camera)
            {
                const auto p = engine.lastPresentation(camera);
                if (!firstVideo[camera] && p.generation == transport.generation() && p.qpc) firstVideo[camera] = p.qpc;
            }
            firstAudio = transport.snapshot();
            if (firstAudio.firstBlockQpc && firstVideo[0] && (cameras == 1 || firstVideo[1])) break;
        }
        if (!firstAudio.firstBlockQpc || !firstVideo[0] || (cameras == 2 && !firstVideo[1])) throw std::runtime_error("First synchronized playback timed out");
        beganPlayback = true;
        const auto videoMs = 1000.0 * ((std::max)(firstVideo[0], firstVideo[1]) - stoppedAt) / hz;
        const auto blockMs = 1000.0 * (firstAudio.firstBlockQpc - stoppedAt) / hz;
        const auto audibleMs = 1000.0 * (firstAudio.firstAudibleQpc - stoppedAt) / hz;
        jsonSet(report, "firstVideoPresentMs", videoMs); jsonSet(report, "firstAudioBlockOutputMs", blockMs);
        jsonSet(report, "firstAudioAudibleEstimateMs", audibleMs); jsonSet(report, "stopToPlayMs", (std::max)(videoMs, audibleMs));
        jsonSet(report, "stopToPlayLimitMs", 2000);
        const auto runningUntil = qpcNow() + hz; while (qpcNow() < runningUntil) pump();
        std::vector<double> seekTimes; juce::Array<juce::var> seeks;
        double cold = 0; std::uint64_t random = 0x11c0ffee;
        if (storm) transport.pause();
        for (int i = 0; i < storm; ++i)
        {
            // Every release creates fresh decoders and discards previous seek work.
            // Seven drag requests plus final release also exercise coalescing.
            for (int drag = 0; drag < 7; ++drag)
            {
                random = random * 6364136223846793005ull + 1;
                transport.scrub(static_cast<Sample>((random >> 1) % static_cast<std::uint64_t>(video->length)), false, qpcNow());
            }
            random = random * 6364136223846793005ull + 1;
            const auto target = i == 0 ? video->length * 3 / 4 : static_cast<Sample>((random >> 1) % static_cast<std::uint64_t>(video->length));
            const auto release = qpcNow(); transport.scrub(target, true, release); const auto gen = transport.generation();
            std::int64_t exactAt = 0;
            while (qpcNow() - release < hz * 10)
            {
                pump(); bool exact = true;
                for (unsigned camera = 0; camera < cameras; ++camera)
                {
                    const auto p = engine.lastPresentation(camera);
                    exact &= p.generation == gen && p.begin <= target && target < p.end;
                    if (exact) exactAt = (std::max)(exactAt, p.qpc);
                }
                if (exact) break;
                exactAt = 0;
            }
            if (!exactAt) throw std::runtime_error("Exact seek presentation timed out");
            const auto ms = 1000.0 * (exactAt - release) / hz; seekTimes.push_back(ms); if (!i) cold = ms;
            auto attempt = jsonObject(); jsonSet(attempt, "sample", target); jsonSet(attempt, "generation", gen);
            jsonSet(attempt, "releaseToExactPresentMs", ms); jsonSet(attempt, "decodedFrameCache", "miss: new seek generation");
            jsonSet(attempt, "firstLargeColdSeek", i == 0); seeks.add(attempt);
        }
        jsonSet(report, "seeks", seeks); jsonSet(report, "seekCount", storm);
        if (storm)
        {
            jsonSet(report, "seekP95Ms", percentile(seekTimes, .95)); jsonSet(report, "coldSeekMs", cold);
            jsonSet(report, "seekDefinition", "Programmatic mouse-release proxy -> exact containing frame successfully submitted to DXGI in all cameras; decoder/frame-cache cold on every generation; OS filesystem cache NOT flushed");
        }
        const auto final = transport.snapshot(); jsonSet(report, "underruns", final.underruns); jsonSet(report, "video", engine.telemetry());
        jsonSet(report, "audibleCursorSample", TimelineTransport::audibleCursor(final, rate, hz, qpcNow()));
        output->close(); audio.stopWorker(); engine.stop();
        const bool pass = (std::max)(videoMs, audibleMs) <= 2000 && !final.underruns
            && (!storm || (percentile(seekTimes, .95) <= 250 && cold <= 500));
        jsonSet(report, "result", pass ? "PASS" : "FAIL");
        jsonSet(report, "reason", pass ? "Observed finalized-fixture playback/seek software timing targets met; physical A/V gate remains unverified"
                                       : "Observed latency or underrun target missed; inspect measurements");
    }
    catch (const std::invalid_argument& e) { jsonSet(report, "result", "FAIL"); jsonSet(report, "reason", e.what()); }
    catch (const std::exception& e)
    {
        const bool unavailable = !beganPlayback && (stage == "device" || stage == "playback");
        jsonSet(report, "result", unavailable ? "UNAVAILABLE" : "FAIL"); jsonSet(report, "reason", e.what());
    }
    jsonSet(report, "stage", stage);
    try { if (reportFile != juce::File()) CaptureTelemetry::writeJson(reportFile, report); }
    catch (const std::exception& e) { std::cerr << "Playback report write failed: " << e.what() << '\n'; return 1; }
    std::cout << juce::JSON::toString(report, false) << '\n';
    return report["result"].toString() == "PASS" ? 0 : report["result"].toString() == "UNAVAILABLE" ? 2 : 1;
}
}
