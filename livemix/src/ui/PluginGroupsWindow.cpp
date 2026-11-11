#include "Localisation.h"
#include "ui/PluginGroupsWindow.h"

#include "Widgets.h"

#include <vector>

namespace gocue::livemix
{

//==============================================================================
class PluginGroupsWindow::Content : public juce::Component
{
public:
    explicit Content (MixDocument& doc) : document (doc), groupsModel (*this), membersModel (*this)
    {
        title.setFont (titleFont (16.0f));
        title.setColour (juce::Label::textColourId, Palette::text);
        addAndMakeVisible (title);

        styleCaption (groupsCaption, tr ("그룹 (카드의 번호)"));
        addAndMakeVisible (groupsCaption);
        groupsList.setModel (&groupsModel);
        groupsList.setRowHeight (30);
        groupsList.setColour (juce::ListBox::backgroundColourId, Palette::card);
        groupsList.setColour (juce::ListBox::outlineColourId, Palette::line);
        groupsList.setOutlineThickness (1);
        addAndMakeVisible (groupsList);

        auto button = [this] (juce::TextButton& b, const juce::String& text, std::function<void()> fn)
        {
            b.setButtonText (text);
            b.setWantsKeyboardFocus (false);
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };

        button (addGroupButton, tr ("+ 그룹 추가"), [this] { addGroup(); });
        addGroupButton.setColour (juce::TextButton::buttonColourId, Palette::accent);
        addGroupButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        button (removeGroupButton, tr ("그룹 삭제"), [this] { removeGroup(); });
        button (offButton, tr ("이 그룹 끄기"), [this] { toggleOff(); });

        styleCaption (membersCaption, tr ("이 그룹에 넣을 플러그인 (지금 VST3 체인)"));
        addAndMakeVisible (membersCaption);
        membersList.setModel (&membersModel);
        membersList.setRowHeight (30);
        membersList.setColour (juce::ListBox::backgroundColourId, Palette::card);
        membersList.setColour (juce::ListBox::outlineColourId, Palette::line);
        membersList.setOutlineThickness (1);
        addAndMakeVisible (membersList);

        styleCaption (note, tr ("그룹을 만들면 마이크 카드의 체인 밑에 그 번호가 켜집니다. 번호를 누르면 그룹에 넣은 플러그인이 한꺼번에 꺼지고 (번호가 빨갛게), 다시 누르면 켜집니다. 그룹에 없는 플러그인은 그대로입니다."));
        note.setFont (bodyFont (12.5f));
        note.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (note);

        statusLabel.setFont (bodyFont (13.0f));
        statusLabel.setColour (juce::Label::textColourId, Palette::dimText);
        statusLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (statusLabel);

        setSize (680, 480);
    }

    void show (const juce::Uuid& id)
    {
        if (channelId != id)
        {
            channelId = id;
            selectedGroup = -1;
            statusLabel.setText ({}, juce::dontSendNotification);
        }

        doRefresh();

        if (selectedGroup < 0 && ! groups.empty())
            groupsList.selectRow (0);
    }

    /** Deferred: the document announces its changes from inside the edits (a list row's click, a button), and a
        list rebuilt inside its own callback is not safe. */
    void refresh()
    {
        if (refreshPending)
            return;

        refreshPending = true;
        juce::Component::SafePointer<Content> safe (this);
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
            {
                safe->refreshPending = false;
                safe->doRefresh();
            }
        });
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (20, 14);
        title.setBounds (area.removeFromTop (26));
        area.removeFromTop (8);
        statusLabel.setBounds (area.removeFromBottom (22));
        area.removeFromBottom (4);
        note.setBounds (area.removeFromBottom (36));
        area.removeFromBottom (10);

        auto left = area.removeFromLeft (juce::jlimit (200, 260, area.getWidth() * 2 / 5));
        area.removeFromLeft (16);

        groupsCaption.setBounds (left.removeFromTop (20));
        auto leftButtons = left.removeFromBottom (30);
        left.removeFromBottom (8);
        addGroupButton.setBounds (leftButtons.removeFromLeft (juce::jmin (120, leftButtons.getWidth() / 2)));
        leftButtons.removeFromLeft (8);
        removeGroupButton.setBounds (leftButtons);
        groupsList.setBounds (left);

        membersCaption.setBounds (area.removeFromTop (20));
        auto rightButtons = area.removeFromBottom (30);
        area.removeFromBottom (8);
        offButton.setBounds (rightButtons.removeFromLeft (juce::jmin (150, rightButtons.getWidth())));
        membersList.setBounds (area);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Palette::background); }

    juce::Uuid getChannelId() const noexcept { return channelId; }

private:
    struct GroupInfo
    {
        int members = 0;
        bool off = false;
    };

    struct SlotInfo
    {
        juce::Uuid id;
        juce::String name;
        bool bypassed = false;
    };

    struct GroupsModel : public juce::ListBoxModel
    {
        explicit GroupsModel (Content& o) : owner (o) {}
        int getNumRows() override { return (int) owner.groups.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            if (row < 0 || row >= (int) owner.groups.size())
                return;

            const auto& info = owner.groups[(size_t) row];
            g.fillAll (selected ? Palette::accent.withAlpha (0.35f) : (row % 2 == 0 ? Palette::card : Palette::card2.withAlpha (0.5f)));

            // the number as the card shows it: red while the group is off
            auto badge = juce::Rectangle<float> (22.0f, 22.0f).withCentre ({ 20.0f, height * 0.5f });
            g.setColour (info.off ? Palette::danger : Palette::card2);
            g.fillRoundedRectangle (badge, 6.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (pt (12.0f), juce::Font::bold)));
            g.drawText (juce::String (row + 1), badge.toNearestInt(), juce::Justification::centred, false);

            g.setColour (Palette::text);
            g.setFont (bodyFont (13.5f));
            g.drawText (trf ("플러그인 그룹 {0}", juce::String (row + 1)), 40, 0, width - 48, height, juce::Justification::centredLeft, true);
            g.setColour (info.off ? Palette::danger : Palette::dimText);
            g.setFont (bodyFont (12.5f));
            g.drawText (trf ("{0}개{1}", juce::String (info.members), (info.off ? tr (" · 꺼짐") : juce::String())), 0, 0, width - 10, height, juce::Justification::centredRight, true);
        }

        void selectedRowsChanged (int row) override { owner.groupSelected (row); }

        Content& owner;
    };

    struct MembersModel : public juce::ListBoxModel
    {
        explicit MembersModel (Content& o) : owner (o) {}
        int getNumRows() override { return (int) owner.slots.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
        {
            if (row < 0 || row >= (int) owner.slots.size())
                return;

            const auto& slot = owner.slots[(size_t) row];
            const bool haveGroup = owner.selectedGroup >= 0;
            const bool member = haveGroup && owner.isMember (slot.id);
            g.fillAll (row % 2 == 0 ? Palette::card : Palette::card2.withAlpha (0.5f));

            const auto box = juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ 20.0f, height * 0.5f });
            g.setColour (member ? Palette::accent : (haveGroup ? Palette::dimText : Palette::line));
            g.drawRoundedRectangle (box, 4.0f, 1.5f);

            if (member)
            {
                g.setColour (Palette::accent);
                juce::Path tick;
                tick.startNewSubPath (box.getX() + 4.0f, box.getCentreY());
                tick.lineTo (box.getCentreX() - 1.0f, box.getBottom() - 5.0f);
                tick.lineTo (box.getRight() - 4.0f, box.getY() + 5.0f);
                g.strokePath (tick, juce::PathStrokeType (2.2f));
            }

            g.setColour (haveGroup ? Palette::text : Palette::dimText);
            g.setFont (bodyFont (13.5f));
            g.drawText (juce::String (row + 1) + ".  " + slot.name, 40, 0, width - 48, height, juce::Justification::centredLeft, true);

            if (slot.bypassed)
            {
                g.setColour (Palette::dimText);
                g.setFont (bodyFont (12.0f));
                g.drawText (tr ("지금 꺼짐"), 0, 0, width - 10, height, juce::Justification::centredRight, true);
            }
        }

        void listBoxItemClicked (int row, const juce::MouseEvent&) override { owner.toggleMember (row); }

        Content& owner;
    };

    const MixChannel* channel() const { return document.getSession().findChannel (channelId); }

    bool isMember (const juce::Uuid& slotId) const
    {
        const auto* c = channel();

        if (c == nullptr || selectedGroup < 0 || selectedGroup >= (int) c->pluginGroups.size())
            return false;

        const auto& members = c->pluginGroups[(size_t) selectedGroup].slots;
        return std::find (members.begin(), members.end(), slotId) != members.end();
    }

    /** What is shown, read again from the session and the live chain. */
    void doRefresh()
    {
        const auto* c = channel();

        if (c == nullptr)
        {
            if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
                window->setVisible (false);   // the channel was removed: nothing to show

            return;
        }

        title.setText (trf ("{0} - 플러그인 그룹", c->name), juce::dontSendNotification);

        groups.clear();

        for (const auto& g : c->pluginGroups)
            groups.push_back ({ (int) g.slots.size(), g.off });

        const int wanted = juce::jmin (selectedGroup, (int) groups.size() - 1);   // updateContent() below may report the old row gone (selectedRowsChanged (-1)): the row meant is put back after it

        slots.clear();

        if (auto* chain = document.getEngine().getChannelChain (channelId))
            for (int i = 0; i < chain->getNumSlots(); ++i)
            {
                const auto& slot = chain->getSlot (i);
                slots.push_back ({ slot.state.slotId, slot.plugin != nullptr ? slot.plugin->getName() : trf ("{0} (없음)", slot.state.name), slot.bypassed.load() });
            }

        groupsList.updateContent();
        selectedGroup = wanted;

        if (selectedGroup >= 0)
            groupsList.selectRow (selectedGroup, false, true);
        else
            groupsList.deselectAllRows();

        groupsList.repaint();
        membersList.updateContent();
        membersList.repaint();
        buttonsFollow();
    }

    void buttonsFollow()
    {
        const auto* c = channel();
        const bool have = c != nullptr && selectedGroup >= 0 && selectedGroup < (int) c->pluginGroups.size();
        addGroupButton.setEnabled (c != nullptr && (int) c->pluginGroups.size() < MixSession::maxPluginGroups);
        removeGroupButton.setEnabled (have);
        offButton.setEnabled (have);   // like the card's number: a group that exists can be switched, members or not
        offButton.setButtonText (have && c->pluginGroups[(size_t) selectedGroup].off ? tr ("이 그룹 켜기") : tr ("이 그룹 끄기"));
        offButton.setColour (juce::TextButton::buttonColourId, have && c->pluginGroups[(size_t) selectedGroup].off ? Palette::danger : Palette::card2);
        membersCaption.setText (have ? trf ("플러그인 그룹 {0}에 넣을 플러그인 (지금 VST3 체인, 눌러서 넣거나 뺌)", juce::String (selectedGroup + 1))
                                     : tr ("먼저 왼쪽에서 그룹을 고르거나 만드세요"), juce::dontSendNotification);
    }

    void groupSelected (int row)
    {
        selectedGroup = row;
        membersList.repaint();
        buttonsFollow();
    }

    void addGroup()
    {
        const int index = document.addPluginGroup (channelId);   // the document announces it: refresh() follows

        if (index < 0)
        {
            statusLabel.setText (trf ("그룹은 채널마다 {0}개까지입니다.", juce::String (MixSession::maxPluginGroups)), juce::dontSendNotification);
            return;
        }

        selectedGroup = index;
        statusLabel.setText (trf ("플러그인 그룹 {0}을 만들었습니다. 오른쪽에서 플러그인을 넣으세요.", juce::String (index + 1)), juce::dontSendNotification);
    }

    void removeGroup()
    {
        if (selectedGroup < 0)
            return;

        const int removed = selectedGroup;
        document.removePluginGroup (channelId, removed);
        selectedGroup = juce::jmin (removed, (int) groups.size() - 2);   // the one before it, or none
        statusLabel.setText (trf ("플러그인 그룹 {0}을 지웠습니다 (뒤의 그룹 번호가 하나씩 당겨집니다).", juce::String (removed + 1)), juce::dontSendNotification);
    }

    void toggleOff()
    {
        const auto* c = channel();

        if (c == nullptr || selectedGroup < 0 || selectedGroup >= (int) c->pluginGroups.size())
            return;

        document.setPluginGroupOff (channelId, selectedGroup, ! c->pluginGroups[(size_t) selectedGroup].off);
    }

    void toggleMember (int row)
    {
        if (row < 0 || row >= (int) slots.size())
            return;

        if (selectedGroup < 0)
        {
            statusLabel.setText (tr ("먼저 왼쪽에서 그룹을 고르거나 '+ 그룹 추가'로 만드세요."), juce::dontSendNotification);
            return;
        }

        const auto slotId = slots[(size_t) row].id;
        document.setPluginGroupMember (channelId, selectedGroup, slotId, ! isMember (slotId));
        statusLabel.setText ({}, juce::dontSendNotification);
    }

    MixDocument& document;
    juce::Uuid channelId = juce::Uuid::null();
    int selectedGroup = -1;
    bool refreshPending = false;
    std::vector<GroupInfo> groups;
    std::vector<SlotInfo> slots;
    GroupsModel groupsModel;
    MembersModel membersModel;

    juce::Label title, groupsCaption, membersCaption, note, statusLabel;
    juce::ListBox groupsList, membersList;
    juce::TextButton addGroupButton, removeGroupButton, offButton;
};

//==============================================================================
PluginGroupsWindow::PluginGroupsWindow (MixDocument& document)
    : DocumentWindow (tr ("플러그인 그룹"), Palette::background, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    auto* c = new Content (document);
    content = c;
    setContentOwned (c, true);
    setResizable (true, false);
    setResizeLimits (560, 400, 10000, 10000);
    centreWithSize (getWidth(), getHeight());   // the owner then centres it on its own display (centreAroundComponent)
}

PluginGroupsWindow::~PluginGroupsWindow()
{
    clearContentComponent();
}

void PluginGroupsWindow::open (const juce::Uuid& channelId)
{
    if (content != nullptr)
        content->show (channelId);

    setVisible (true);
    toFront (true);
}

void PluginGroupsWindow::refresh()
{
    if (content != nullptr && isVisible())
        content->refresh();
}

void PluginGroupsWindow::closeButtonPressed()
{
    setVisible (false);   // kept: reopening is instant
}

} // namespace gocue::livemix
