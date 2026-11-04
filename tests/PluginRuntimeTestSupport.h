#pragma once
#include "audio/PluginRuntimeCheck.h"
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace gocue::tests::runtime
{
inline juce::String ko (const char* text) { return juce::String::fromUTF8 (text); }

inline juce::MemoryBlock pe (const juce::StringArray& normal, const juce::StringArray& delayed = {},
                             unsigned machine = 0x8664, unsigned magic = 0x20b)
{
    juce::MemoryBlock block (4096, true);
    auto* bytes = static_cast<unsigned char*> (block.getData());
    const auto put = [&] (int offset, juce::uint32 value, int size = 4)
    { for (int i = 0; i < size; ++i) bytes[offset + i] = (unsigned char) (value >> (i * 8)); };
    put (0, 0x5a4d, 2);
    put (0x3c, 0x80);
    put (0x80, 0x4550);
    put (0x84, machine, 2);
    put (0x86, 1, 2);
    put (0x94, 240, 2);
    put (0x96, 0x2022, 2);
    put (0x98, magic, 2);
    put (0x98 + 24, 0x80000000);
    put (0x98 + 28, 1);
    put (0x98 + 32, 0x1000);
    put (0x98 + 36, 0x200);
    put (0x98 + 56, 0x2000);
    put (0x98 + 60, 0x200);
    put (0x98 + 108, 16);
    const int section = 0x98 + 240;
    put (section + 8, 0xe00);
    put (section + 12, 0x1000);
    put (section + 16, 0xe00);
    put (section + 20, 0x200);
    int nameOffset = 0x700;
    const auto descriptors = [&] (const juce::StringArray& names, bool delay)
    {
        if (names.isEmpty()) return;
        const int table = delay ? 0x400 : 0x200, stride = delay ? 32 : 20;
        const int directory = 0x98 + 112 + (delay ? 13 : 1) * 8;
        put (directory, table + 0xe00);
        put (directory + 4, (juce::uint32) ((names.size() + 1) * stride));
        for (int i = 0; i < names.size(); ++i)
        {
            if (delay) put (table + i * stride, 1);
            put (table + i * stride + (delay ? 4 : 12), (juce::uint32) (nameOffset + 0xe00));
            const auto size = names[i].getNumBytesAsUTF8() + 1;
            memcpy (bytes + nameOffset, names[i].toRawUTF8(), size);
            nameOffset += (int) size;
        }
    };
    descriptors (normal, false);
    descriptors (delayed, true);
    return block;
}

inline bool write (const juce::File& file, const juce::MemoryBlock& block)
{
    file.getParentDirectory().createDirectory();
    return file.replaceWithData (block.getData(), block.getSize());
}
inline bool touch (const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    return file.replaceWithText ("fixture");
}

struct Fixture
{
    juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("plugin-runtime-" + juce::Uuid().toString());
    PluginRuntimeCheck::Options options;
    Fixture()
    {
        root.createDirectory();
        options.environment = { root.getChildFile ("System32"), root.getChildFile ("App"),
                                root.getChildFile ("Program Files (x86)"), root.getChildFile ("Program Files"),
                                root.getChildFile ("ProgramData") };
    }
    ~Fixture() { root.deleteRecursively(); }
    juce::File plugin (const juce::String& name = "plugin.dll") const { return root.getChildFile ("Plugins").getChildFile (name); }
    juce::File additional() const { return options.environment.programData.getChildFile ("Waves Audio/Modules/AdditionalDLLs_x64"); }
    juce::File waves()
    {
        const auto shell = plugin ("WaveShell1-VST3 17.1_x64.vst3");
        write (shell, pe ({ "KERNEL32.dll" }));
        write (additional().getChildFile ("mkl_waves.dll"), pe ({ "MSVCR110.dll", "KERNEL32.dll" }));
        write (options.environment.programFilesX86.getChildFile (
                   "Waves/Plug-Ins V17/InTrigger.bundle/Contents/PlugIns/INT_CR8.bundle/Contents/Win64/INT_CR8.dll"),
               pe ({ "mkl_waves.dll" }));
        return shell;
    }
    PluginRuntimeResult check (const juce::File& file) const
    { return PluginRuntimeCheck (options).check (file.getFullPathName(), file.hasFileExtension ("dll") ? "VST" : "VST3"); }
};

inline bool pumpUntil (const std::function<bool()>& done, int timeout = 5000)
{
    // This repository sets JUCE_MODAL_LOOPS_PERMITTED=0, as do the existing window tests.
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeout;
    do
    {
        MSG message;
        for (int n = 0; n < 64 && PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE); ++n)
        { TranslateMessage (&message); DispatchMessageW (&message); }
        if (done()) return true;
        juce::Thread::sleep (5);
    } while (juce::Time::getMillisecondCounterHiRes() < deadline);
    return done();
}
} // namespace gocue::tests::runtime
