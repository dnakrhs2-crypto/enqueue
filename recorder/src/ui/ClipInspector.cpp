#include "ClipInspector.h"
#include <limits>

namespace gocue::recorder
{
ClipInspector::ClipInspector(TimelineEditController& c) : edits(c)
{
    addAndMakeVisible(viewport); viewport.setViewedComponent(&content, false); viewport.setScrollBarsShown(true, false);
    for (auto* label : {&title, &units, &link, &versions}) content.addAndMakeVisible(label);
    title.setText(ko("선택 속성"), juce::dontSendNotification); units.setText(ko("샘플 · 숫자 입력은 정확히 적용"), juce::dontSendNotification);
    title.setFont(recorderFont(13.5f, juce::Font::bold));
    for (auto* label : {&units, &link}) { label->setFont(recorderFont(12)); label->setColour(juce::Label::textColourId, Palette::dimText); }
    const char* captions[] = {"시작", "끝", "source In", "source Out", "길이"};
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        content.addAndMakeVisible(labels[i]); content.addAndMakeVisible(inputs[i]); labels[i].setText(ko(captions[i]), juce::dontSendNotification);
        inputs[i].setInputRestrictions(19, "0123456789"); inputs[i].onReturnKey = [this, i] { commit(i); };
        inputs[i].onEscapeKey = [this] { displayedClip.clear(); refresh(); };
        inputs[i].setTooltip(ko("0 이상의 정수 샘플 · Enter로 적용"));
        inputs[i].setFont(recorderMonoFont(12)); inputs[i].setJustification(juce::Justification::centredRight);
        labels[i].setFont(recorderFont(12)); labels[i].setColour(juce::Label::textColourId, Palette::dimText);
    }
    versions.setJustificationType(juce::Justification::topLeft); link.setJustificationType(juce::Justification::topLeft);
}
void ClipInspector::refresh()
{
    const auto ids = edits.targets(); const auto& p = edits.document.getProject(); const auto* c = ids.empty() ? nullptr : p.findClip(ids.front());
    const bool changed = displayedClip != (c ? c->clipId : Id()); displayedClip = c ? c->clipId : Id();
    const Sample values[] = {c ? c->timelineStartSample : 0, c ? c->timelineEnd() : 0, c ? c->sourceIn : 0, c ? c->sourceIn + c->lengthSamples : 0, c ? c->lengthSamples : 0};
    for (unsigned i = 0; i < inputs.size(); ++i)
    {
        inputs[i].setEnabled(c && !edits.isLocked());
        if (changed || !inputs[i].hasKeyboardFocus(true)) inputs[i].setText(c ? juce::String(values[i]) : juce::String(), false);
    }
    title.setText(c ? ko("선택 속성 · ") + juce::String(ids.size()) + ko("개") : ko("클립을 선택하세요"), juce::dontSendNotification);
    link.setText(c ? c->linkGroupId.isEmpty() ? ko("링크 없음") : ko("링크 · ") + juce::String(ids.size()) + ko("개 함께 편집") : juce::String(), juce::dontSendNotification);
    juce::String versionText = ko("테이크 버전 목록\n"); bool found = false;
    if (c) for (const auto& stack : p.takeStacks) if (stack.stackId == c->takeStackId)
    {
        unsigned n = 0; for (const auto& version : stack.versions)
        { ++n; versionText += ko("버전 ") + juce::String(n) + (version.versionId == stack.activeVersionId ? ko(" · 사용 중") : juce::String()) + "\n"; }
        found = true;
    }
    versionText += found ? ko("버전 전환 준비 전") : ko("버전 없음"); versions.setText(versionText, juce::dontSendNotification);
    versions.setTooltip(versionText);
}
void ClipInspector::commit(unsigned field)
{
    Sample value = 0; const auto* c = edits.document.getProject().findClip(displayedClip);
    if (!c || !TimelineEditController::parseSample(inputs[field].getText(), value))
    { if (onEdit) onEdit(juce::Result::fail(ko("0 이상의 정수 샘플을 입력하세요."))); return; }
    TimelineAction action = TimelineAction::move; Sample at = value;
    if (field == 1) action = TimelineAction::trimOut;
    if (field >= 2)
    {
        action = field == 2 ? TimelineAction::trimIn : TimelineAction::trimOut;
        const auto delta = field == 4 ? value : value - c->sourceIn;
        if (delta > (std::numeric_limits<Sample>::max)() - c->timelineStartSample)
        { if (onEdit) onEdit(juce::Result::fail(ko("샘플 범위를 초과했습니다."))); return; }
        at = c->timelineStartSample + delta;
    }
    const auto r = edits.execute(action, at, true, "numeric:" + displayedClip + ":" + juce::String(field));
    if (r.wasOk()) inputs[field].setText(juce::String(value), false);
    if (onEdit) onEdit(r);
}
void ClipInspector::resized()
{
    viewport.setBounds(getLocalBounds()); const auto w = juce::jmax(140, getWidth() - 18); content.setSize(w, 398);
    title.setBounds(12, 8, w - 24, 28); units.setBounds(12, 36, w - 24, 24);
    for (unsigned i = 0; i < inputs.size(); ++i) { labels[i].setBounds(12, 68 + int(i) * 36, 78, 28); inputs[i].setBounds(98, 68 + int(i) * 36, w - 110, 28); }
    link.setBounds(12, 252, w - 24, 42); versions.setBounds(12, 300, w - 24, 94);
}
}
