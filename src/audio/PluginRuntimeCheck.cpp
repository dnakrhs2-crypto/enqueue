#include "PluginRuntimeCheck.h"
#include <algorithm>
#include <array>
#include <set>
#include <windows.h>

namespace gocue
{
namespace
{
    using U64 = juce::uint64;
    using U32 = juce::uint32;

    U32 u32 (const void* data) { return juce::ByteOrder::littleEndianInt (data); }
    unsigned u16 (const void* data) { return juce::ByteOrder::littleEndianShort (data); }

    // https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
    // Read just headers, descriptors and names, even for very large Waves DLLs.
    class PeReader
    {
    public:
        PeReader (const juce::File& file, juce::int64* count) : stream (file), bytesRead (count)
        { length = (U64) juce::jmax ((juce::int64) 0, stream.getTotalLength()); }

        juce::StringArray read()
        {
            std::array<unsigned char, 64> dos {};
            if (! at (0, dos.data(), dos.size()) || u16 (dos.data()) != 0x5a4d) return {};
            const U64 pe = u32 (dos.data() + 0x3c);
            std::array<unsigned char, 24> coff {};
            if (pe < dos.size() || ! at (pe, coff.data(), coff.size())
                || u32 (coff.data()) != 0x4550 || u16 (coff.data() + 4) != 0x8664) return {};
            const auto sectionCount = u16 (coff.data() + 6);
            const auto optionalSize = u16 (coff.data() + 20);
            if (sectionCount == 0 || sectionCount > 96 || optionalSize < 112 || optionalSize > 4096) return {};
            std::vector<unsigned char> optional (optionalSize);
            if (! at (pe + 24, optional.data(), optional.size()) || u16 (optional.data()) != 0x20b) return {};
            imageBase = juce::ByteOrder::littleEndianInt64 (optional.data() + 24);
            headerSize = u32 (optional.data() + 60);
            const auto directoryCount = u32 (optional.data() + 108);
            if (directoryCount > (optionalSize - 112) / 8 || headerSize > length
                || headerSize < pe + 24 + optionalSize + sectionCount * 40) return {};
            for (unsigned i = 0; i < sectionCount; ++i)
            {
                std::array<unsigned char, 40> section {};
                if (! at (pe + 24 + optionalSize + i * 40, section.data(), section.size())) return {};
                Section s { u32 (section.data() + 12), u32 (section.data() + 16), u32 (section.data() + 20) };
                if (! range (s.offset, s.size) || (U64) s.rva + s.size > 0x100000000ULL) return {};
                sections.push_back (s);
            }

            juce::StringArray names;
            for (const unsigned index : { 1u, 13u })
            {
                if (directoryCount <= index) continue;
                const auto* dir = optional.data() + 112 + index * 8;
                const U64 rva = u32 (dir), size = u32 (dir + 4);
                if (rva == 0 && size == 0) continue;
                const bool delay = index == 13;
                const unsigned stride = delay ? 32 : 20;
                if (rva == 0 || size < stride || size / stride > 4097) return {};
                U64 offset = 0, available = 0;
                if (! locate (rva, offset, available) || size > available) return {};
                bool terminated = false;
                for (U64 pos = 0; pos + stride <= size; pos += stride)
                {
                    std::array<unsigned char, 32> descriptor {};
                    if (! at (offset + pos, descriptor.data(), stride)) return {};
                    if (std::all_of (descriptor.begin(), descriptor.begin() + stride, [] (auto c) { return c == 0; }))
                    { terminated = true; break; }
                    U64 nameRva = u32 (descriptor.data() + (delay ? 4 : 12));
                    if (delay)
                    {
                        const auto attributes = u32 (descriptor.data());
                        if (attributes > 1) return {};
                        if (attributes == 0)
                        {
                            if (nameRva < imageBase) return {};
                            nameRva -= imageBase;
                        }
                    }
                    const auto name = readName (nameRva);
                    if (name.isEmpty()) return {};
                    names.addIfNotAlreadyThere (name.toLowerCase());
                }
                if (! terminated) return {};
            }
            return names;
        }

    private:
        struct Section { U32 rva, size, offset; };
        bool range (U64 offset, U64 size) const { return offset <= length && size <= length - offset; }
        bool at (U64 offset, void* dest, size_t size)
        {
            if (stream.failedToOpen() || ! range (offset, size) || ! stream.setPosition ((juce::int64) offset)) return false;
            const auto got = stream.read (dest, (int) size);
            if (bytesRead != nullptr) *bytesRead += got;
            return got == (int) size;
        }
        bool locate (U64 rva, U64& offset, U64& available) const
        {
            if (rva == 0 || rva > 0xffffffffULL) return false;
            if (rva < headerSize) { offset = rva; available = headerSize - rva; return true; }
            for (const auto& s : sections)
                if (rva >= s.rva && rva - s.rva < s.size)
                {
                    offset = s.offset + (rva - s.rva);
                    available = s.size - (rva - s.rva);
                    return true;
                }
            return false;
        }
        juce::String readName (U64 rva)
        {
            U64 offset = 0, available = 0;
            if (! locate (rva, offset, available)) return {};
            std::array<char, 261> name {};
            const auto size = (size_t) juce::jmin (available, (U64) name.size());
            if (! at (offset, name.data(), size)) return {};
            for (size_t i = 0; i < size; ++i)
            {
                if (name[i] == 0) return i == 0 ? juce::String() : juce::String::fromUTF8 (name.data(), (int) i);
                if ((unsigned char) name[i] < 32 || (unsigned char) name[i] > 126) return {};
            }
            return {};
        }
        juce::FileInputStream stream;
        juce::int64* bytesRead;
        U64 length = 0, imageBase = 0, headerSize = 0;
        std::vector<Section> sections;
    };

    bool containsDll (const juce::File& folder, const juce::String& dll)
    { return folder != juce::File() && folder.getChildFile (dll).existsAsFile(); }

    juce::String wavesVersion (const juce::String& name)
    {
        for (int i = 0; i < name.length(); ++i)
        {
            if (! juce::CharacterFunctions::isDigit (name[i])) continue;
            const auto start = i;
            while (juce::CharacterFunctions::isDigit (name[i])) ++i;
            if (name[i] == '.' && juce::CharacterFunctions::isDigit (name[i + 1]))
                return name.substring (start, i);
        }
        return {};
    }
}

juce::StringArray readX64PeImports (const juce::File& file, juce::int64* bytesRead)
{
    if (bytesRead != nullptr) *bytesRead = 0;
    return PeReader (file, bytesRead).read();
}

PluginRuntimeEnvironment PluginRuntimeEnvironment::current()
{
    PluginRuntimeEnvironment e;
    wchar_t system[MAX_PATH + 1] {};
    const auto count = GetSystemDirectoryW (system, MAX_PATH + 1);
    if (count != 0 && count <= MAX_PATH) e.windowsSystemDirectory = juce::File (juce::String (system));
    e.applicationDirectory = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const auto envFolder = [] (const char* key)
    {
        const auto value = juce::SystemStats::getEnvironmentVariable (key, {});
        return value.isEmpty() ? juce::File() : juce::File (value);
    };
    e.programFilesX86 = envFolder ("ProgramFiles(x86)");
    e.programFiles = envFolder ("ProgramW6432");
    if (e.programFiles == juce::File()) e.programFiles = envFolder ("ProgramFiles");
    e.programData = envFolder ("ProgramData");
    return e;
}

const std::vector<PluginRuntimePackage>& pluginRuntimePackages()
{
    static const std::vector<PluginRuntimePackage> packages {
        { "Microsoft Visual C++ 2010 (x64)", "https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x64.exe",
          { "msvcr100.dll", "msvcp100.dll", "mfc100.dll", "mfc100u.dll", "vcomp100.dll", "atl100.dll" }, {} },
        { "Microsoft Visual C++ 2012 (x64)", "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe",
          { "msvcr110.dll", "msvcp110.dll", "mfc110.dll", "mfc110u.dll", "vcomp110.dll", "vccorlib110.dll" }, {} },
        { "Microsoft Visual C++ 2013 (x64)", "https://aka.ms/highdpimfc2013x64enu",
          { "msvcr120.dll", "msvcp120.dll", "mfc120.dll", "mfc120u.dll", "vcomp120.dll", "vccorlib120.dll" }, {} },
        { "Microsoft Visual C++ 2015-2022 (x64)", "https://aka.ms/vc14/vc_redist.x64.exe",
          { "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
            "msvcp140_codecvt_ids.dll", "concrt140.dll", "vccorlib140.dll", "vcomp140.dll", "mfc140.dll", "mfc140u.dll", "mfcm140.dll", "mfcm140u.dll" }, {} }
    };
    return packages;
}

PluginRuntimeCheck::PluginRuntimeCheck (Options o) : options (std::move (o)) {}

PluginRuntimeResult PluginRuntimeCheck::check (const juce::String& path, const juce::String& format,
                                               const std::function<bool()>& cancelled)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    PluginRuntimeResult result { path, format, {}, 0 };
    const auto& env = options.environment;
    auto packages = pluginRuntimePackages();
    std::map<juce::String, size_t> candidates;
    for (size_t p = 0; p < packages.size(); ++p)
        for (const auto& dll : packages[p].dlls)
            if (! containsDll (env.windowsSystemDirectory, dll)) candidates[dll] = p;
    // No PE opens or directory walks on a PC with all known runtime DLLs.
    if (candidates.empty())
    {
        result.elapsedMs = (juce::int64) (juce::Time::getMillisecondCounterHiRes() - started);
        return result;
    }

    const juce::File failed (path);
    const bool waves = failed.getFileName().startsWithIgnoreCase ("WaveShell");
    const auto additional = waves && env.programData != juce::File()
                          ? env.programData.getChildFile ("Waves Audio/Modules/AdditionalDLLs_x64") : juce::File();
    std::set<juce::String> visited, found;
    int fileCount = 0;
    const auto stopped = [&]
    {
        return fileCount >= options.maxFiles || found.size() == candidates.size()
            || juce::Time::getMillisecondCounterHiRes() - started >= options.maxMilliseconds
            || (cancelled && cancelled());
    };
    // Iterative dependency traversal avoids stack growth on malformed dependency chains.
    const auto inspect = [&] (const juce::File& root)
    {
        std::vector<juce::File> pending { root };
        while (! pending.empty() && ! stopped())
        {
            auto file = pending.back();
            pending.pop_back();
            const auto key = file.getFullPathName().toLowerCase();
            if (! visited.insert (key).second || ! file.existsAsFile()) continue;
            ++fileCount;
            auto cached = imports.find (key);
            if (cached == imports.end())
            {
                if (options.onFileRead) options.onFileRead (file);
                cached = imports.emplace (key, readX64PeImports (file)).first;
            }
            for (const auto& dll : cached->second)
            {
                // Import strings cannot escape the importing file's directory.
                if (dll.containsAnyOf ("/\\:") || dll == "." || dll == "..") continue;
                const auto beside = file.getSiblingFile (dll);
                const bool presentBeside = beside.existsAsFile();
                if (presentBeside && dll.endsWithIgnoreCase (".dll")) pending.push_back (beside);
                const auto candidate = candidates.find (dll);
                if (candidate != candidates.end() && ! presentBeside
                    && ! containsDll (env.applicationDirectory, dll) && ! containsDll (additional, dll))
                {
                    packages[candidate->second].evidence.push_back ({ dll, file });
                    found.insert (dll);
                }
            }
        }
    };
    const auto walk = [&] (const juce::File& folder)
    {
        if (folder == juce::File() || ! folder.isDirectory() || stopped()) return;
        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles,
                                                               juce::File::FollowSymlinks::noCycles))
        {
            if (stopped()) break;
            const auto file = entry.getFile();
            if (file.hasFileExtension ("dll;vst3")) inspect (file);
        }
    };
    if (failed.isDirectory()) walk (failed); else inspect (failed);
    if (waves)
    {
        const auto version = wavesVersion (failed.getFileName());
        // AdditionalDLLs is shared by Waves versions; inspect it even if the shell lacks a version.
        walk (additional);
        if (version.isNotEmpty())
            for (const auto& folder : { env.programFilesX86, env.programFiles })
                if (folder != juce::File()) walk (folder.getChildFile ("Waves/Plug-Ins V" + version));
    }
    for (auto& package : packages)
        if (! package.evidence.empty()) result.packages.push_back (std::move (package));
    result.elapsedMs = (juce::int64) (juce::Time::getMillisecondCounterHiRes() - started);
    return result;
}
} // namespace gocue
