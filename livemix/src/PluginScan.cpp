#include "PluginScan.h"
#include <condition_variable>
#include <windows.h>
#include <werapi.h>

namespace gocue::livemix
{
namespace
{
    constexpr auto workerId = "plugin-scan-worker";
    constexpr auto workerPrefix = "--plugin-scan-worker:"; // JUCE appends the pipe name after ':'

    juce::String outcomeText (const PluginScanCoordinator::Result& r)
    {
        using Outcome = PluginScanCoordinator::Outcome;
        switch (r.outcome)
        {
            case Outcome::completed: return juce::String (r.numTypes) + juce::String::fromUTF8 ("개");
            case Outcome::crashed: return juce::String::fromUTF8 ("튕김(종료 코드 0x") + juce::String::toHexString (r.exitCode) + ")";
            case Outcome::timedOut: return juce::String::fromUTF8 ("시간 초과");
            case Outcome::cancelled: return juce::String::fromUTF8 ("취소");
            case Outcome::workerFailed: return juce::String::fromUTF8 ("워커 시작/통신 실패");
        }
        return {};
    }
}

class PluginScanCoordinator::Connection final : private juce::InterprocessConnection,
                                               private juce::Thread
{
public:
    // JUCE ChildProcessWorker's framing and control messages (juce_ConnectedChildProcess.cpp).
    // Owning the pipe/process here allows disconnect before teardown: ChildProcessCoordinator's
    // killWorkerProcess writes to the dead pipe and can wait its full 8 s reconnect timeout.
    Connection() : InterprocessConnection (false, 0x712baf04), Thread ("LiveMix scan ping") {}
    ~Connection() override
    {
        signalThreadShouldExit();
        disconnect (1000, juce::InterprocessConnection::Notify::no);
        stopThread (1000);
        if (process != nullptr)
        {
            if (WaitForSingleObject (process, 0) == WAIT_TIMEOUT)
                TerminateProcess (process, ERROR_CANCELLED);
            WaitForSingleObject (process, 5000);
        }
        if (process != nullptr) CloseHandle (process);
        if (job != nullptr) CloseHandle (job);
    }
    bool launch (const juce::File& executable)
    {
        job = CreateJobObjectW (nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job == nullptr || ! SetInformationJobObject (job, JobObjectExtendedLimitInformation, &limits, sizeof (limits)))
            return false;
        const auto pipeName = "livemix-scan-" + juce::Uuid().toString();
        if (! createPipe (pipeName, 8000)) return false;
        const auto command = executable.getFullPathName().quoted() + " " + workerPrefix + pipeName;
        std::wstring mutableCommand (command.toWideCharPointer());
        STARTUPINFOW startup {};
        startup.cb = sizeof (startup);
        PROCESS_INFORMATION info {};
        // Assign the job before any worker code runs, including a failed/hung startup.
        if (! CreateProcessW (executable.getFullPathName().toWideCharPointer(), mutableCommand.data(), nullptr, nullptr,
                              FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info)) return false;
        process = info.hProcess;
        pid = info.dwProcessId;
        const bool assigned = AssignProcessToJobObject (job, process) != FALSE;
        const bool resumed = assigned && ResumeThread (info.hThread) != (DWORD) -1;
        CloseHandle (info.hThread);
        if (! resumed) return false;
        return startThread();
    }
    bool sendRequest (const juce::String& format, const juce::String& file)
    {
        juce::MemoryBlock block;
        juce::MemoryOutputStream stream (block, false);
        stream.writeString (format);
        stream.writeString (file);
        return send (block);
    }
    std::unique_ptr<juce::XmlElement> waitForMessage()
    {
        std::unique_lock<std::mutex> lock (mutex);
        condition.wait_for (lock, std::chrono::milliseconds (20), [this] { return message != nullptr || lost; });
        return std::move (message);
    }
    bool hasExited()
    {
        if (process != nullptr && WaitForSingleObject (process, 0) == WAIT_OBJECT_0) return true;
        const std::lock_guard<std::mutex> lock (mutex);
        return lost;
    }
    juce::uint32 exitCode() const
    {
        DWORD code = 0;
        if (process != nullptr)
        {
            WaitForSingleObject (process, 200); // pipe closure may precede the process signal
            GetExitCodeProcess (process, &code);
        }
        return code;
    }
    juce::uint32 pid = 0;

private:
    bool send (const juce::MemoryBlock& block)
    {
        const std::lock_guard<std::mutex> lock (sendMutex);
        return sendMessage (block);
    }
    void run() override
    {
        if (! send ({ "__ipc_st", 8 })) return connectionLost();
        while (! threadShouldExit())
        {
            if (! send ({ "__ipc_p_", 8 })) return connectionLost();
            wait (1000);
        }
    }
    void connectionMade() override {}
    void messageReceived (const juce::MemoryBlock& block) override
    {
        if (block.matches ("__ipc_p_", 8)) return;
        const std::lock_guard<std::mutex> lock (mutex);
        message = juce::parseXML (block.toString());
        if (message == nullptr) lost = true;
        condition.notify_one();
    }
    void connectionLost() override
    {
        const std::lock_guard<std::mutex> lock (mutex);
        lost = true;
        condition.notify_one();
    }
    HANDLE process = nullptr, job = nullptr;
    std::mutex mutex, sendMutex;
    std::condition_variable condition;
    std::unique_ptr<juce::XmlElement> message;
    bool lost = false;
};

PluginScanCoordinator::PluginScanCoordinator() : PluginScanCoordinator (Options {}) {}
PluginScanCoordinator::PluginScanCoordinator (Options o) : options (std::move (o)) {}
PluginScanCoordinator::~PluginScanCoordinator() = default;

bool PluginScanCoordinator::findPluginTypesFor (juce::AudioPluginFormat& format,
                                               juce::OwnedArray<juce::PluginDescription>& types,
                                               const juce::String& file)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    Result result;
    result.format = format.getName();
    result.file = file;
    const auto finish = [&] (Outcome outcome)
    {
        result.outcome = outcome;
        if (connection != nullptr) result.workerPid = connection->pid;
        if (outcome != Outcome::completed) connection.reset();
        result.elapsedMs = (juce::int64) (juce::Time::getMillisecondCounterHiRes() - started);
        record (result);
        // Cancellation must not permanently blacklist an innocent file.
        return outcome == Outcome::completed || outcome == Outcome::cancelled;
    };
    if (shouldExit() || cancelRequested) return finish (Outcome::cancelled);
    bool awaitingReady = connection == nullptr;
    if (awaitingReady)
    {
        connection = std::make_unique<Connection>();
        if (! connection->launch (options.executable)) return finish (Outcome::workerFailed);
    }
    else if (! connection->sendRequest (result.format, file))
    {
        result.exitCode = connection->exitCode();
        return finish (Outcome::crashed);
    }
    for (;;)
    {
        if (shouldExit() || cancelRequested) return finish (Outcome::cancelled);
        if (juce::Time::getMillisecondCounterHiRes() - started >= juce::jmax (1, options.timeoutMs))
            return finish (Outcome::timedOut);
        if (auto message = connection->waitForMessage())
        {
            if (awaitingReady)
            {
                if (! message->hasTagName ("READY") || (juce::uint32) message->getIntAttribute ("pid") != connection->pid)
                    return finish (Outcome::workerFailed);
                awaitingReady = false;
                if (! connection->sendRequest (result.format, file)) return finish (Outcome::workerFailed);
                continue;
            }
            if (! message->hasTagName ("RESULT") || ! message->getBoolAttribute ("ok")) return finish (Outcome::workerFailed);
            result.onMessageThread = message->getBoolAttribute ("messageThread");
            for (const auto* item : message->getChildIterator())
            {
                auto description = std::make_unique<juce::PluginDescription>();
                if (description->loadFromXml (*item))
                {
                    types.add (description.release());
                    ++result.numTypes;
                }
            }
            return finish (Outcome::completed);
        }
        if (connection->hasExited())
        {
            result.exitCode = connection->exitCode();
            return finish (Outcome::crashed);
        }
    }
}

void PluginScanCoordinator::scanFinished() { connection.reset(); }
void PluginScanCoordinator::prepareForScan (juce::KnownPluginList& list, const juce::File& crashMarker)
{
    cancelRequested = false;
    list.clearBlacklistedFiles();
    if (crashMarker != juce::File()) crashMarker.deleteFile();
    const std::lock_guard<std::mutex> lock (resultsMutex);
    results.clear();
}
std::vector<PluginScanCoordinator::Result> PluginScanCoordinator::getResults() const
{
    const std::lock_guard<std::mutex> lock (resultsMutex);
    return results;
}
juce::String PluginScanCoordinator::getSkippedMessage() const
{
    juce::StringArray files;
    for (const auto& result : getResults())
        if (result.outcome == Outcome::crashed || result.outcome == Outcome::timedOut)
            files.addIfNotAlreadyThere (result.file);
    if (files.isEmpty()) return {};
    for (auto& file : files) file = juce::File (file).getFileName();
    return juce::String::fromUTF8 ("스캔 중 튕기거나 멈춘 파일 ") + juce::String (files.size())
         + juce::String::fromUTF8 ("개는 건너뛰었습니다: ") + files.joinIntoString (", ");
}
void PluginScanCoordinator::record (const Result& result)
{
    const std::lock_guard<std::mutex> lock (resultsMutex);
    results.push_back (result);
    if (options.logFile == juce::File()) return;
    options.logFile.getParentDirectory().createDirectory();
    if (options.logFile.getSize() > 1024 * 1024)
        options.logFile.moveFileTo (options.logFile.getSiblingFile (options.logFile.getFileName() + ".1"));
    const auto line = juce::Time::getCurrentTime().toISO8601 (true) + "\t" + result.format + "\t"
                    + result.file.replaceCharacters ("\r\n\t", "   ") + "\t" + outcomeText (result)
                    + "\t" + juce::String (result.elapsedMs) + " ms\n";
    options.logFile.appendText (line, false, false, "\n");
}

bool PluginScanWorker::isWorkerCommandLine (const juce::String& line)
{
    return line.trimStart().startsWith (workerPrefix)
        || juce::ArgumentList ("LiveMix", line).containsOption ("--plugin-scan-worker");
}
void PluginScanWorker::suppressCrashDialogs()
{
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    WerSetFlags (WER_FAULT_REPORTING_NO_UI);
}
bool PluginScanWorker::start (const juce::String& commandLine)
{
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
   #if JUCE_PLUGINHOST_VST
    formats.addFormat (std::make_unique<juce::VSTPluginFormat>());
   #endif
    if (! initialiseFromCommandLine (commandLine, workerId)) return false;
    triggerAsyncUpdate();
    return true;
}
void PluginScanWorker::handleMessageFromCoordinator (const juce::MemoryBlock& block)
{
    const std::lock_guard<std::mutex> lock (mutex);
    pending.push (block);
    triggerAsyncUpdate();
}
void PluginScanWorker::handleConnectionLost()
{
    // Can run on the pipe thread while a plugin has hung the message thread.
    TerminateProcess (GetCurrentProcess(), 0);
}
void PluginScanWorker::send (const juce::XmlElement& xml)
{
    const auto text = xml.toString();
    if (! sendMessageToCoordinator ({ text.toRawUTF8(), text.getNumBytesAsUTF8() })) handleConnectionLost();
}
void PluginScanWorker::handleAsyncUpdate()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (! readySent)
    {
        juce::XmlElement ready ("READY");
        ready.setAttribute ("pid", (int) GetCurrentProcessId());
        send (ready);
        readySent = true;
    }
    for (;;)
    {
        juce::MemoryBlock block;
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (pending.empty()) return;
            block = std::move (pending.front());
            pending.pop();
        }
        juce::MemoryInputStream stream (block, false);
        const auto formatName = stream.readString();
        const auto file = stream.readString();
        juce::XmlElement reply ("RESULT");
        reply.setAttribute ("messageThread", juce::MessageManager::getInstance()->isThisTheMessageThread());
        reply.setAttribute ("ok", false);
        for (auto* format : formats.getFormats())
        {
            if (format->getName() != formatName) continue;
            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile (descriptions, file);
            for (const auto* description : descriptions) reply.addChildElement (description->createXml().release());
            reply.setAttribute ("ok", true);
            break;
        }
        send (reply);
    }
}
} // namespace gocue::livemix
