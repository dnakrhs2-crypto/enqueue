#pragma once
#include "app/RecorderDocument.h"
#include "model/TakeStackEdits.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace gocue::recorder
{
// Independent host component; refresh() after document publication. Does not
// replace RecorderDocument::onChanged, so it can coexist with the main timeline.
class TakeListPanel final : public juce::Component
{
public:
    explicit TakeListPanel(RecorderDocument&);
    void refresh();
    void selectStack(const Id&);
    void resized() override;
private:
    void updateImpact();
    RecorderDocument& document;
    juce::Label title, impactLabel;
    juce::ComboBox stacks, versions;
    juce::TextButton use{juce::String::fromUTF8("이 테이크 사용")};
    std::vector<Id> stackIds, versionIds;
    Id selectedStack;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TakeListPanel)
};
}
