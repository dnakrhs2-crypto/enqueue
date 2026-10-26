#include "../../tests/livemix017/r5/RecorderChecks.h"
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <iostream>
#include <iomanip>

using namespace gocue::livemix::recording;
namespace
{
int crashChild(const juce::File& directory, Format format)
{
    checks::Result result { "child", {} };
    Recorder recorder;
    auto request = checks::request(directory, true, format);
    request.people[1] = {}; request.people[2] = {};
    checks::start(result, recorder, request, 48000, 480);
    recorder.noteSeatOccupant(1, { 4, gocue::livemix::collab::contracts::ParticipantKind::native, "나중이름" });
    checks::Signal signal(48000, 48000);
    bool ready = false;
    for (unsigned block = 0;; ++block)
    {
        checks::feed(recorder, signal, static_cast<int>(block % 100) * 480, 480, 3, 480);
        const auto snapshot = recorder.snapshot();
        if (snapshot.state == State::error) return 3;
        if (!ready && snapshot.files[0].checkpointFrames > 0)
        {
            juce::String manifest;
            for (const auto& f : snapshot.files)
                manifest += juce::String::fromUTF8(f.pathUtf8.c_str()) + "\t"
                    + juce::String(static_cast<juce::int64>(f.checkpointFrames)) + "\n";
            if (!directory.getChildFile("checkpoint.txt").replaceWithText(manifest)) return 4;
            ready = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
void crashCheck(checks::Result& r, Format format)
{
    checks::Temp directory;
    struct Process
    {
        PROCESS_INFORMATION info {};
        ~Process()
        {
            if (info.hProcess)
            {
                if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(info.hProcess, 77);
                WaitForSingleObject(info.hProcess, 5000);
                CloseHandle(info.hProcess); CloseHandle(info.hThread);
            }
        }
    } child;
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto command = "\"" + exe.getFullPathName() + "\" --crash-child \"" + directory.path.getFullPathName()
        + "\" " + (format == Format::wav ? "wav" : "mp3");
    std::wstring mutableCommand(command.toWideCharPointer());
    STARTUPINFOW startup {}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    r.require(CreateProcessW(exe.getFullPathName().toWideCharPointer(), mutableCommand.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.info) != 0, "spawn headless recorder child");
    const auto checkpoint = directory.path.getChildFile("checkpoint.txt");
    r.require(checks::waitFor([&] { return checkpoint.existsAsFile() || WaitForSingleObject(child.info.hProcess, 0) == WAIT_OBJECT_0; }), "child reaches a checkpoint");
    r.require(checkpoint.existsAsFile(), "checkpoint manifest published by live child");
    const auto lines = juce::StringArray::fromLines(checkpoint.loadFileAsString());
    r.require(TerminateProcess(child.info.hProcess, 77) != 0, "force terminate process without destructors");
    r.require(WaitForSingleObject(child.info.hProcess, 5000) == WAIT_OBJECT_0, "child terminated");
    int files = 0;
    for (const auto& line : lines)
    {
        if (line.isEmpty()) continue;
        auto fields = juce::StringArray::fromTokens(line, "\t", "");
        r.require(fields.size() == 2, "checkpoint path and completed length");
        const juce::File file(fields[0]);
        const auto frames = static_cast<std::uint64_t>(fields[1].getLargeIntValue());
        if (format == Format::wav)
        {
            auto audio = checks::wave(r, file);
            r.require(audio.left.size() >= frames && audio.rate == 48000 && audio.bits == 24, "WAV opens through last completed checkpoint");
        }
        else
        {
            auto audio = checks::mp3(r, file, 48000, 320);
            r.require(audio.mpegFrames >= frames, "MP3 opens through last completed checkpoint");
        }
        ++files;
    }
    r.require(files == 4 && checks::paths(directory.path, format == Format::wav ? "*.wav" : "*.mp3").size() == 4, "crash never deletes empty seats");
    r.require(checks::named(directory.path, juce::String::fromUTF8("B_합방상대1") + (format == Format::wav ? ".wav" : ".mp3")).existsAsFile(), "crash never renames initial empty seat");
}
void cpuCheck(checks::Result& result, Format format)
{
    checks::Temp directory;
    Recorder recorder;
    constexpr unsigned rate = 48000;
    constexpr int seconds = 10;
    const auto wallStart = std::chrono::steady_clock::now();
    checks::start(result, recorder, checks::request(directory.path, true, format, 320), rate, 480);
    checks::Signal signal(rate, rate * seconds);
    auto deadline = std::chrono::steady_clock::now();
    for (int first = 0; first < signal.frames; first += 480)
    {
        checks::feed(recorder, signal, first, 480, 7, 480);
        deadline += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(deadline);
    }
    checks::finished(result, recorder, signal.frames);
    result.require(recorder.snapshot().fileCount == 4, "four simultaneous output files");
    const double cpu = recorder.writerCpuSeconds();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    std::cout << "CPU," << (format == Format::wav ? "WAV_PCM24_48000" : "MP3_320kbps_48000")
              << ",files=4,audio_s=" << seconds << ",writer_cpu_s=" << std::fixed << std::setprecision(6) << cpu
              << ",percent_one_core=" << cpu / seconds * 100.0 << ",wall_s=" << wall << std::endl;
}
}
int main(int argc, char** argv)
{
    if (argc == 4 && juce::String(argv[1]) == "--crash-child")
    {
        try { return crashChild(juce::File(juce::String::fromUTF8(argv[2])), juce::String(argv[3]) == "wav" ? Format::wav : Format::mp3); }
        catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 5; }
    }
    int passed = 0, failed = 0, assertions = 0;
    const checks::Report report = [&](const checks::Result& result)
    {
        (result.ok() ? passed : failed)++;
        assertions += static_cast<int>(result.assertions.size());
        std::cout << (result.ok() ? "PASS" : "FAIL") << " | " << result.name << " | checks=" << result.assertions.size() << std::endl;
        for (const auto& a : result.assertions) if (!a.first) std::cout << "  " << a.second << std::endl;
    };
    checks::recorderTests(report);
    checks::wavTests(report);
    checks::mp3Tests(report);
    checks::run(report, "WAV checkpoint survives process termination", [](auto& r) { crashCheck(r, Format::wav); });
    checks::run(report, "MP3 checkpoint survives process termination", [](auto& r) { crashCheck(r, Format::mp3); });
    checks::run(report, "CPU: four simultaneous 48k PCM24 WAV files", [](auto& r) { cpuCheck(r, Format::wav); });
    checks::run(report, "CPU: four simultaneous 48k 320kbps MP3 files", [](auto& r) { cpuCheck(r, Format::mp3); });
    std::cout << "RESULT," << passed << " passed," << failed << " failed," << assertions << " checks" << std::endl;
    return failed ? 1 : 0;
}
