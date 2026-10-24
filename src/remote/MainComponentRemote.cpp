#include "ui/MainComponent.h"
#include "ui/MidiModalScope.h"
#include "ui/UiUtils.h"
#include "app/Commands.h"
#include "app/ShortcutCatalog.h"

namespace gocue
{
void MainComponent::fireCartCue (const juce::Uuid& id)
{
    const auto* cue = document.findCueAnywhere (id);
    if (cue == nullptr) return;
    const auto copy = *cue; // a goto can replace the active list during fire
    if (controller.fire (copy.id) == CueController::GoResult::started)
        transport.showStatus (ko ("카트: ") + copy.name, false);
}
void MainComponent::stopCartCue (const juce::Uuid& id) { controller.stopCue (id, true); }
void MainComponent::pauseActiveCue (const juce::Uuid& id, bool resume)
{
    if (resume) controller.resumeCue (id);
    else controller.pauseCue (id);
}
void MainComponent::stopActiveCue (const juce::Uuid& id)
{
    // The card's x leaves the running sequence's follow intact, unlike cart stop.
    if (engine.isPlaying (id)) { engine.fadeOutAndStop (id); controller.cancelScheduledStart (id); }
    else controller.stopCue (id);
}
void MainComponent::cancelActiveWait (const juce::Uuid& id, WaitProgress::Kind kind, int startId)
{ controller.cancelWait (id, kind, startId); }

void MainComponent::moveRowsInList (CueList& list, const std::vector<int>& rows, int insertIndex, int intoGroup)
{
    if (showMode || rows.empty()) return;
    if (intoGroup >= 0)
    {
        if (! list.isValidIndex (intoGroup) || ! list.get (intoGroup).isGroup()) return;
        const auto groupId = list.get (intoGroup).id;
        document.perform (ko ("그룹으로 이동"), [&list, rows, groupId]
        {
            if (list.moveSubtreesInto (rows, list.indexOf (groupId))) list.setCollapsed (list.indexOf (groupId), false);
        });
    }
    else document.perform (ko ("큐 이동"), [&list, rows, insertIndex] { list.moveSubtrees (rows, insertIndex); });
}

void MainComponent::releaseRemote (uint64_t connection)
{
    const bool held = shortcuts->activations().anyHeld();
    shortcuts->activations().releaseSource (InputKind::remote, connection);
    if (held && ! shortcuts->activations().anyHeld())
        shortcuts->invokeInput ({ InputKind::remote, "transport.go", CommandIDs::go, false,
            { InputKind::remote, connection, 0, 0 }, juce::Time::getMillisecondCounterHiRes(), InputInvocation::nextEventID() });
}

juce::Result MainComponent::performRemote (uint64_t connection, const remote::Request& request, double time)
{
    if (request.error.failed()) return request.error;
    const juce::ScopedValueSetter<bool> remoteDispatch (remoteInput, true);
    const auto& type = request.type;
    const bool capture = shortcuts->isCapturing();
    const bool modal = MidiModalScope::active();
    const bool eligible = ! capture && ! modal;
    const auto blocked = juce::Result::fail (capture ? ko ("학습 중") : ko ("설정 창 열림"));
    const auto unavailable = juce::Result::fail (ko ("대상이 없습니다"));
    const auto invoke = [&] (int command, bool down = true, InputToken token = {})
    {
        const auto* definition = ShortcutCatalog::get().find (command);
        return shortcuts->invokeInput ({ InputKind::remote, definition != nullptr ? definition->id : juce::String(),
            command, down, token, time, InputInvocation::nextEventID() });
    };
    if (type == "go")
    {
        const InputToken token { InputKind::remote, connection, static_cast<uint64_t> (static_cast<juce::int64> (request["touch"])), 0 };
        if (! static_cast<bool> (request["down"]))
        {
            const bool held = shortcuts->activations().anyHeld();
            shortcuts->activations().release (token);
            if (held && ! shortcuts->activations().anyHeld()) invoke (CommandIDs::go, false, token);
            return juce::Result::ok();
        }
        if (shortcuts->activations().press (token, eligible, document.settings.requireKeyUp)) invoke (CommandIDs::go, true, token);
        return eligible ? juce::Result::ok() : blocked;
    }
    const bool panic = type == "cmd" && request["cmd"].toString() == "panic";
    if (capture || (modal && ! panic)) return blocked;
    if (panic)
    {
        if (const auto hard = shortcuts->panicGestures().activate (InputInvocation::nextEventID(), time)) panicFromAnywhere (time, *hard);
        return juce::Result::ok();
    }
    if (type == "cmd")
    {
        const auto command = request["cmd"].toString() == "pause" ? CommandIDs::pauseToggle : CommandIDs::fadeOutSelected;
        invoke (command);
        return juce::Result::ok();
    }
    if (type == "mode") { setShowMode (static_cast<bool> (request["show"])); return juce::Result::ok(); }
    if (type == "move" && showMode) return juce::Result::fail (ko ("쇼 모드"));
    if (type == "tab" || type == "playhead" || type == "move")
    {
        int tab = -1;
        for (int i = 0; i < document.getNumContainers(); ++i)
            if (document.getContainerInfo (i).id.toString() == request["tab"].toString()) { tab = i; break; }
        if (tab < 0) return unavailable;
        if (type == "tab") { document.setActiveContainer (tab); return juce::Result::ok(); }
        const juce::Uuid cueId (request["cue"].toString());
        int index = -1;
        auto* list = document.listContaining (cueId, &index);
        if (list == nullptr || document.containerOf (cueId) != tab) return unavailable;
        if (type == "playhead") { list->setPlayheadIndex (index); return juce::Result::ok(); }
        if (document.getContainerInfo (tab).isCart) return juce::Result::fail (ko ("큐 리스트가 아닙니다"));
        const auto parentText = request["parent"].toString();
        const auto beforeText = request["before"].toString();
        const juce::Uuid parent = parentText.isEmpty() ? juce::Uuid::null() : juce::Uuid (parentText);
        const int group = list->indexOf (parent);
        if (parentText.isNotEmpty() && (group < 0 || ! list->get (group).isGroup())) return unavailable;
        if (beforeText.isNotEmpty())
        {
            const int before = list->indexOf (juce::Uuid (beforeText));
            if (before < 0 || list->get (before).parentId != parent) return unavailable;
            moveRowsInList (*list, { index }, before);
        }
        else if (group >= 0) moveRowsInList (*list, { index }, -1, group);
        else moveRowsInList (*list, { index }, list->size());
        return juce::Result::ok();
    }
    const juce::Uuid id (request["cue"].toString());
    if (document.findCueAnywhere (id) == nullptr) return unavailable;
    if (type == "fire") fireCartCue (id);
    else if (type == "stopcue") stopCartCue (id);
    else if (type == "cue")
    {
        const auto op = request["op"].toString();
        if (op == "stop") stopActiveCue (id);
        else pauseActiveCue (id, op == "resume");
    }
    else if (type == "wait")
    {
        const auto kind = request["kind"].toString();
        cancelActiveWait (id, kind == "pre" ? WaitProgress::Kind::preWait : kind == "post" ? WaitProgress::Kind::postWait : WaitProgress::Kind::waitCue,
                          static_cast<int> (request["startId"]));
    }
    else return juce::Result::fail (ko ("잘못된 요청"));
    return juce::Result::ok();
}
void MainComponent::showRemoteWindow()
{
    if (remoteWindow == nullptr)
        remoteWindow = std::make_unique<remote::Window> (*remoteService, [this] { confirmRemoteNewKey(); }, [this] { commands.commandStatusChanged(); });
    remoteWindow->open();
}
void MainComponent::confirmRemoteNewKey()
{
    juce::Component::SafePointer<MainComponent> self (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, ko ("QR 새로 만들기"), ko ("연결된 기기가 모두 끊깁니다."),
        ko ("새로 만들기"), ko ("취소"), remoteWindow != nullptr && remoteWindow->isVisible() ? static_cast<juce::Component*> (remoteWindow.get()) : this,
        juce::ModalCallbackFunction::create ([self] (int result) { if (result != 0 && self != nullptr) self->remoteService->renewKey(); }));
}
}
