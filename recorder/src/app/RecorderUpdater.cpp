#include "RecorderUpdater.h"
#include "ProductIdentity.h"
#include <atomic>
#include <memory>
#include <mutex>
extern "C" {
#include <libavutil/avutil.h>
}
#if RECORDER_HAS_WINSPARKLE
#include <winsparkle.h>
#endif
#ifndef RECORDER_UPDATE_PUBLIC_KEY
#define RECORDER_UPDATE_PUBLIC_KEY ""
#endif

namespace gocue::recorder
{
namespace
{
struct State
{
    RecorderUpdater::Callbacks callbacks;
    std::atomic<bool> active { true };
};
std::shared_ptr<State> state;
std::mutex stateMutex;
std::shared_ptr<State> currentState()
{
    const std::lock_guard<std::mutex> lock(stateMutex);
    return state;
}
bool mayClose(const std::shared_ptr<State>& value)
{
    try { return value && value->active.load() && (!value->callbacks.canShutdown || value->callbacks.canShutdown()); }
    catch (...) { return false; } // never let an exception cross the C ABI
}
#if RECORDER_HAS_WINSPARKLE
int __cdecl canShutdownThunk() { return mayClose(currentState()) ? 1 : 0; }
void __cdecl requestShutdownThunk()
{
    auto value = currentState();
    if (!mayClose(value)) return;
    juce::MessageManager::callAsync([value]
    {
        if (!mayClose(value)) return;
        if (value->callbacks.requestShutdown) value->callbacks.requestShutdown();
        else if (auto* app = juce::JUCEApplication::getInstance()) app->systemRequestedQuit();
    });
}
#endif
}
bool RecorderUpdater::isAvailable()
{
    return RECORDER_HAS_WINSPARKLE != 0 && ProductIdentity::publicationConfirmed()
        && ProductIdentity::appcastUrl().startsWith("https://")
        && !ProductIdentity::appcastUrl().contains(".invalid")
        && juce::String(RECORDER_UPDATE_PUBLIC_KEY).isNotEmpty();
}
void RecorderUpdater::initialise(Callbacks callbacks)
{
#if RECORDER_HAS_WINSPARKLE
    if (!isAvailable() || currentState()) return;
    if (win_sparkle_set_eddsa_public_key(RECORDER_UPDATE_PUBLIC_KEY) != 1) return;
    auto value = std::make_shared<State>();
    value->callbacks = std::move(callbacks);
    { const std::lock_guard<std::mutex> lock(stateMutex); state = value; }
    win_sparkle_set_appcast_url(ProductIdentity::appcastUrl().toRawUTF8());
    win_sparkle_set_registry_path(ProductIdentity::updateRegistryKey().toRawUTF8());
    win_sparkle_set_app_details(juce::String(RECORDER_COMPANY).toWideCharPointer(),
        ProductIdentity::displayName().toWideCharPointer(), ProductIdentity::version().toWideCharPointer());
    win_sparkle_set_can_shutdown_callback(canShutdownThunk);
    win_sparkle_set_shutdown_request_callback(requestShutdownThunk);
    win_sparkle_set_automatic_check_for_updates(0);
    win_sparkle_init();
#else
    juce::ignoreUnused(callbacks);
#endif
}
void RecorderUpdater::shutdown()
{
    auto value = currentState();
    if (!value) return;
    value->active.store(false);
#if RECORDER_HAS_WINSPARKLE
    win_sparkle_cleanup(); // joins callbacks before their captures can be destroyed
#endif
    const std::lock_guard<std::mutex> lock(stateMutex);
    state.reset();
}
void RecorderUpdater::checkForUpdatesWithUI()
{
#if RECORDER_HAS_WINSPARKLE
    if (mayClose(currentState())) win_sparkle_check_update_with_ui();
#endif
}
void RecorderUpdater::checkQuietly()
{
#if RECORDER_HAS_WINSPARKLE
    if (mayClose(currentState())) win_sparkle_check_update_without_ui();
#endif
}
juce::String RecorderUpdater::aboutText()
{
    return ProductIdentity::displayName() + " " + ProductIdentity::version()
        + "\nFFmpeg " + juce::String(av_version_info())
        + juce::String::fromUTF8("\n이 프로그램은 LGPL v3-or-later로 배포되는 FFmpeg 공유 라이브러리를 사용합니다.\n"
            "라이브러리 수정·디버깅과 호환 DLL 교체를 허용합니다. 고지: 설치 폴더의 licenses.\n")
        + (ProductIdentity::publicationConfirmed()
            ? juce::String::fromUTF8("같은 릴리스의 대응 소스: ") + ProductIdentity::correspondingSourceUrl()
            : juce::String::fromUTF8("같은 릴리스의 대응 소스: 공개 전 준비 중"));
}
void RecorderUpdater::showAboutDialog()
{
    auto options = juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::InfoIcon)
        .withTitle(juce::String::fromUTF8("앱 정보")).withMessage(aboutText());
    if (ProductIdentity::publicationConfirmed()) options = options.withButton(juce::String::fromUTF8("대응 소스 열기"));
    options = options.withButton(juce::String::fromUTF8("닫기"));
    juce::AlertWindow::showAsync(options, [](int result)
    {
        if (result == 1 && ProductIdentity::publicationConfirmed())
            juce::URL(ProductIdentity::correspondingSourceUrl()).launchInDefaultBrowser();
    });
}
}
