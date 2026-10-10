#pragma once
#include "model/SafeFileWrite.h"

namespace gocue::recorder
{
// Translate the shared writer's messages without changing its file transaction.
inline juce::Result writeRecorderTextVerified(const juce::File& file, const juce::String& text,
                                             const gocue::SafeFileWrite::Verifier& verify = nullptr)
{
    const auto result = gocue::SafeFileWrite::writeTextVerified(file, text, verify);
    if (result.wasOk()) return result;
    auto message = result.getErrorMessage();
    if (message.startsWith("Could not write "))
        message = juce::String::fromUTF8("파일을 저장할 수 없습니다: ") + message.substring(16);
    else if (message.startsWith("Could not replace "))
        message = juce::String::fromUTF8("파일을 교체할 수 없습니다: ") + message.substring(18);
    const juce::String mismatch = ": the file on disk does not match what was written (";
    const auto at = message.lastIndexOf(mismatch);
    if (at >= 0)
        message = message.substring(0, at) + juce::String::fromUTF8(": 저장된 파일이 쓴 내용과 다릅니다 (읽은 크기 ")
            + message.substring(at + mismatch.length()).replace(" of ", juce::String::fromUTF8(" / 쓴 크기 "))
                .replace(" bytes)", juce::String::fromUTF8(" 바이트)"));
    return juce::Result::fail(message);
}
}
