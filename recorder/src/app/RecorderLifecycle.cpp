#include "RecorderLifecycle.h"

namespace gocue::recorder
{
juce::String recorderFaultText(RecorderFault fault)
{
    const char* text = "";
    switch (fault)
    {
        case RecorderFault::none: break;
        case RecorderFault::camera1Disconnected: text = "캠1 연결이 끊겼습니다. 원본 녹음과 연결된 캠2는 계속됩니다."; break;
        case RecorderFault::camera2Disconnected: text = "캠2 연결이 끊겼습니다. 캠1과 원본 녹음은 계속됩니다."; break;
        case RecorderFault::storageWrite: text = "저장 장치에 쓸 수 없어 녹화를 멈췄습니다."; break;
        case RecorderFault::audioOverflow: text = "처리 지연이 발생했습니다. 원본 오디오를 보존할 수 없어 녹화를 멈췄습니다."; break;
        case RecorderFault::audioReset: text = "오디오 장치가 재설정되어 녹화를 멈췄습니다. 장치를 다시 연결하세요."; break;
        case RecorderFault::audioRateChanged: text = "오디오 샘플레이트가 변경되어 녹화를 멈췄습니다. 장치를 다시 연결하세요."; break;
        case RecorderFault::audioInput: text = "오디오 수집을 중단했습니다. 저장된 원본을 확인하세요."; break;
        case RecorderFault::dubbingUnderrun: text = "오디오 재생이 끊겨 더빙을 중단했습니다."; break;
        case RecorderFault::finalize: text = "일반 MP4 마무리 실패 · 재시도"; break;
        case RecorderFault::processingDelay: text = "처리 지연이 발생했습니다"; break;
        case RecorderFault::resume: text = "절전 복귀로 녹화를 멈췄습니다. 장치를 다시 연결하세요."; break;
        case RecorderFault::gpuRemoved: text = "영상 표시 장치를 다시 연결하는 중입니다."; break;
        case RecorderFault::save: text = "저장하지 못했습니다. 저장 위치를 확인하고 다시 시도하세요."; break;
        case RecorderFault::recovery: text = "저장된 자료를 복구했습니다. 원본 파일은 보존했습니다."; break;
        case RecorderFault::updateBusy: text = "녹화·더빙·내보내기·복구와 저장이 끝난 뒤 업데이트를 다시 시도하세요."; break;
    }
    return juce::String::fromUTF8(text);
}
bool RecorderLifecycle::begin(Activity activity) noexcept
{
    if (captureBusy()) return false;
    auto previous = flags.load();
    // Unsaved edits may coexist with recording/export; they still prohibit updates.
    constexpr auto exclusive = recording | dubbing | exporting | recovering | finalizing | fileWork | configuring | closing;
    do { if (previous & exclusive) return false; }
    while (!flags.compare_exchange_weak(previous, previous | activity));
    return true;
}
void RecorderLifecycle::end(Activity activity) noexcept { flags.fetch_and(~std::uint32_t(activity)); }
void RecorderLifecycle::set(Activity activity, bool active) noexcept
{ if (active) flags.fetch_or(activity); else end(activity); }
}
