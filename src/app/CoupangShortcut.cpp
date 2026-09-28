#include "CoupangShortcut.h"
#include "Links.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shlobj.h>
#endif

namespace gocue::CoupangShortcut
{
namespace
{
juce::File koreanShortcut (const juce::File& desktop)
{
    return desktop.getChildFile (juce::String::fromUTF8 ("쿠팡.url"));
}
}

bool existsOn (const juce::File& desktop)
{
    return koreanShortcut (desktop).exists() || desktop.getChildFile ("Coupang.url").exists();
}

juce::Result createOn (const juce::File& desktop, const juce::File& iconFile)
{
   #if JUCE_WINDOWS
    if (! desktop.isDirectory())
        return juce::Result::fail (juce::String::fromUTF8 ("바탕화면 폴더가 없습니다."));
    if (existsOn (desktop))
        return juce::Result::ok();

    // Written and verified as a temporary file first, then moved onto the desktop without replacing anything: a
    // failure never leaves a half-written 쿠팡.url there, and a shortcut another program makes meanwhile is never touched.
    const auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getNonexistentChildFile ("coupang-shortcut", ".tmp", false);
    const auto path = temp.getFullPathName();
    const auto handle = CreateFileW (path.toWideCharPointer(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return juce::Result::fail (juce::String::fromUTF8 ("바로가기 파일을 쓸 수 없습니다."));

    // A Unicode INI preserves Korean and other non-ANSI characters in IconFile.
    const unsigned char bom[] { 0xff, 0xfe };
    DWORD written = 0;
    const bool initialised = WriteFile (handle, bom, sizeof (bom), &written, nullptr) != 0 && written == sizeof (bom);
    CloseHandle (handle);
    const auto fail = [&temp] (const char* reason)
    {
        temp.deleteFile(); // only this call's temporary file (a leftover in the temp folder is harmless)
        return juce::Result::fail (juce::String::fromUTF8 (reason));
    };
    if (! initialised)
        return fail ("바로가기 파일을 쓸 수 없습니다.");

    const auto url = juce::String (Links::coupangShortcut);
    if (! WritePrivateProfileStringW (L"InternetShortcut", L"URL", url.toWideCharPointer(), path.toWideCharPointer()))
        return fail ("바로가기 주소를 쓸 수 없습니다.");
    if (iconFile.existsAsFile())
    {
        const auto iconPath = iconFile.getFullPathName();
        if (! WritePrivateProfileStringW (L"InternetShortcut", L"IconFile", iconPath.toWideCharPointer(), path.toWideCharPointer())
            || ! WritePrivateProfileStringW (L"InternetShortcut", L"IconIndex", L"0", path.toWideCharPointer()))
            return fail ("바로가기 아이콘 정보를 쓸 수 없습니다.");
    }
    wchar_t verified[512] {};
    GetPrivateProfileStringW (L"InternetShortcut", L"URL", L"", verified, 512, path.toWideCharPointer());
    if (juce::String (verified) != url)
        return fail ("저장한 바로가기 주소를 확인할 수 없습니다.");

    // Another program may have made either name meanwhile: theirs stays, ours goes.
    if (existsOn (desktop))
    {
        temp.deleteFile();
        return juce::Result::ok();
    }

    // No MOVEFILE_REPLACE_EXISTING: the move fails rather than overwrite a shortcut that appeared at the last moment.
    const auto file = koreanShortcut (desktop);
    if (! MoveFileExW (path.toWideCharPointer(), file.getFullPathName().toWideCharPointer(),
                       MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
    {
        const bool someoneElses = existsOn (desktop);
        temp.deleteFile();
        return someoneElses ? juce::Result::ok()
                            : juce::Result::fail (juce::String::fromUTF8 ("바로가기 파일을 쓸 수 없습니다."));
    }

    SHChangeNotify (SHCNE_CREATE, SHCNF_PATHW, file.getFullPathName().toWideCharPointer(), nullptr);
    return juce::Result::ok();
   #else
    juce::ignoreUnused (desktop, iconFile);
    return juce::Result::fail (juce::String::fromUTF8 ("Windows에서만 바로가기를 만들 수 있습니다."));
   #endif
}

juce::File userDesktop()
{
    return juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
}

juce::File installedIcon()
{
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("coupang.ico");
}

UpdateDecision decideUpdate (const juce::String& previous, const juce::String& current,
                             bool shortcutExists, bool existingSettingsWithoutVersion)
{
    const bool announce = previous != current && (previous.isNotEmpty() || existingSettingsWithoutVersion);
    return { announce, announce && ! shortcutExists };
}
}
