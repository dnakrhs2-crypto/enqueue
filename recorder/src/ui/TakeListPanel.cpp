#include "TakeListPanel.h"

namespace gocue::recorder
{
TakeListPanel::TakeListPanel(RecorderDocument& d) : document(d)
{
    title.setText(juce::String::fromUTF8("테이크 목록 · 구간 전체 버전"), juce::dontSendNotification);
    for (juce::Component* c : std::array<juce::Component*, 5>{&title, &stacks, &versions, &impactLabel, &use}) addAndMakeVisible(c);
    stacks.onChange = [this]
    {
        const auto i = stacks.getSelectedItemIndex(); if (i >= 0 && size_t(i) < stackIds.size()) selectStack(stackIds[size_t(i)]);
    };
    versions.onChange = [this] { updateImpact(); };
    use.onClick = [this]
    {
        const auto i = versions.getSelectedItemIndex(); if (i < 0 || size_t(i) >= versionIds.size()) return;
        // Recompute at commit time; the document/impact preview may have changed.
        const auto result = document.useTakeVersion(selectedStack, versionIds[size_t(i)]);
        if (result.failed()) impactLabel.setText(result.getErrorMessage(), juce::dontSendNotification); else refresh();
    };
    refresh();
}
void TakeListPanel::refresh()
{
    stacks.clear(juce::dontSendNotification); stackIds.clear();
    for (const auto& s : document.getProject().takeStacks)
    {
        stackIds.push_back(s.stackId);
        stacks.addItem(juce::String::fromUTF8("구간 ") + juce::String(stackIds.size()) + juce::String::fromUTF8(" · ") + juce::String(double(s.anchorSample) / document.getProject().Fs, 3) + juce::String::fromUTF8("초"), int(stackIds.size()));
    }
    if (!TakeStackEdits::find(document.getProject(), selectedStack)) selectedStack = stackIds.empty() ? Id{} : stackIds.back();
    selectStack(selectedStack);
}
void TakeListPanel::selectStack(const Id& id)
{
    selectedStack = id; versions.clear(juce::dontSendNotification); versionIds.clear();
    for (size_t i = 0; i < stackIds.size(); ++i) if (stackIds[i] == id) stacks.setSelectedId(int(i + 1), juce::dontSendNotification);
    if (const auto* s = TakeStackEdits::find(document.getProject(), id))
    {
        for (const auto& v : s->versions)
        {
            versionIds.push_back(v.versionId);
            versions.addItem(juce::String::fromUTF8("버전 ") + juce::String(s->versions.size() - versionIds.size() + 1)
                + (v.versionId == s->activeVersionId ? juce::String::fromUTF8(" · 사용 중") : ""), int(versionIds.size()));
            if (v.versionId == s->activeVersionId) versions.setSelectedId(int(versionIds.size()), juce::dontSendNotification);
        }
    }
    updateImpact();
}
void TakeListPanel::updateImpact()
{
    use.setEnabled(false); const auto i = versions.getSelectedItemIndex();
    if (i < 0 || size_t(i) >= versionIds.size()) { impactLabel.setText(juce::String::fromUTF8("더빙한 구간이 없습니다."), juce::dontSendNotification); return; }
    const auto impact = TakeStackEdits::impact(document.getProject(), selectedStack, versionIds[size_t(i)]);
    const auto Fs = document.getProject().Fs;
    auto text = juce::String::fromUTF8("영향 범위: ") + juce::String(double(impact.range.start) / Fs, 3) + juce::String::fromUTF8("–")
        + juce::String(double(impact.range.start + impact.range.length) / Fs, 3) + juce::String::fromUTF8("초 · 영상과 선택 마이크 전체");
    if (impact.status.failed()) text += juce::String::fromUTF8(" · ") + impact.status.getErrorMessage();
    impactLabel.setText(text, juce::dontSendNotification);
    use.setEnabled(impact.status.wasOk() && !document.isRecordingStructureLocked());
}
void TakeListPanel::resized()
{
    auto r = getLocalBounds().reduced(8); title.setBounds(r.removeFromTop(26));
    stacks.setBounds(r.removeFromTop(30)); r.removeFromTop(6); versions.setBounds(r.removeFromTop(30));
    r.removeFromTop(6); use.setBounds(r.removeFromTop(30)); impactLabel.setBounds(r.removeFromTop(50));
}
}
