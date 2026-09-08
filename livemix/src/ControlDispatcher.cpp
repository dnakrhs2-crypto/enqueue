#include "ControlDispatcher.h"
#include "MixDocument.h"
#include "MuteGroups.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <type_traits>

namespace gocue::livemix
{

ControlDispatcher::ControlDispatcher (MixDocument& doc, MuteGroups& muteGroups, ControlState& controlState,
                                      juce::Uuid instance, std::function<bool()> running)
    : document (doc), groups (muteGroups), state (controlState), instanceId (instance), audioRunning (std::move (running))
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    jassert (! instanceId.isNull() && audioRunning);
}

ControlDispatcher::Result ControlDispatcher::dispatch (const ControlProtocol::Command& command)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    using P = ControlProtocol;
    using Code = P::ErrorCode;
    const auto context = [&] { return P::Context { instanceId, state.getCurrent().sessionId, state.getCurrent().revision }; };
    const auto fail = [&] (Code code) -> Result
    {
        return P::ErrorResponse { { code, P::isRequestId (command.id) ? command.id : juce::String() }, context(), {}, {} };
    };
    // A UI edit or callback-free dirty/audio change may have happened before the publish timer ran.
    if (! state.update (ControlState::capture (document, groups, audioRunning()))) return fail (Code::internalError);
    if (const auto error = P::validateCommand (command)) return fail (error->code);
    if (command.instanceId != instanceId) return fail (Code::instanceChanged);
    if (command.sessionId && *command.sessionId != state.getCurrent().sessionId) return fail (Code::sessionChanged);
    if (command.ifRevision && *command.ifRevision != state.getCurrent().revision) return fail (Code::revisionConflict);

    bool changed = false;
    const auto canAdvance = [&] { return ! changed || state.getCurrent().revision < P::maxSafeInteger; };
    const auto applied = std::visit ([&] (const auto& a) -> std::variant<P::CommandResult, Code>
    {
        using T = std::decay_t<decltype (a)>;
        const auto& session = document.getSession();
        if constexpr (std::is_same_v<T, P::RequestState>)
            return P::CommandResult { P::RequestStateResult { state.getCurrent().revision } };
        else
        {
            // Reserve a revision before edits; equal-value sets still succeed at the safe-integer bound.
            if constexpr (std::is_same_v<T, P::SetAllChannelsOn>)
            {
                const auto count = (int) session.channels.size();
                changed = std::any_of (session.channels.begin(), session.channels.end(), [&a] (const auto& c) { return c.on != a.on; });
                if (! canAdvance()) return Code::internalError;
                if (changed) document.setAllChannelsOn (a.on);
                return P::CommandResult { P::AllChannelsResult { a.on, count } };
            }
            else if constexpr (std::is_same_v<T, P::SetMuteGroup> || std::is_same_v<T, P::ToggleMuteGroup>)
            {
                const auto group = a.group == P::MuteGroup::mic ? MuteGroups::Group::mic : MuteGroups::Group::fx;
                if constexpr (std::is_same_v<T, P::ToggleMuteGroup>)
                {
                    changed = true;
                    if (! canAdvance()) return Code::internalError;
                    groups.toggle (group);
                }
                else
                {
                    changed = groups.isMuted (group) != a.muted;
                    if (! canAdvance()) return Code::internalError;
                    if (changed) groups.set (group, a.muted);
                }
                return P::CommandResult { P::MuteGroupResult { a.group, groups.isMuted (group) } };
            }
            else
            {
                const auto* channel = session.findChannel (a.channelId);
                if (channel == nullptr) return Code::channelNotFound;
                if constexpr (std::is_same_v<T, P::SetChannelOn> || std::is_same_v<T, P::ToggleChannel>)
                {
                    bool on;
                    if constexpr (std::is_same_v<T, P::SetChannelOn>) on = a.on;
                    else on = ! channel->on;
                    changed = channel->on != on;
                    if (! canAdvance()) return Code::internalError;
                    if (changed) document.setChannelOn (a.channelId, on);
                    return P::CommandResult { P::OnResult { on } };
                }
                else if constexpr (std::is_same_v<T, P::SetPluginGroupOff>)
                {
                    const auto index = (size_t) (a.index - 1);
                    if (index >= channel->pluginGroups.size()) return Code::pluginGroupNotFound;
                    changed = channel->pluginGroups[index].off != a.off;
                    if (! canAdvance()) return Code::internalError;
                    if (changed) document.setPluginGroupOff (a.channelId, (int) index, a.off);
                    return P::CommandResult { P::PluginGroupResult { a.index, a.off } };
                }
                else if constexpr (std::is_same_v<T, P::SetSend>)
                {
                    if (session.findFx (a.fxId) == nullptr) return Code::fxNotFound;
                    const auto send = std::find_if (channel->sends.begin(), channel->sends.end(), [&a] (const auto& s) { return s.fx == a.fxId; });
                    const auto oldAmount = send != channel->sends.end() ? send->amount : 0.0;
                    const auto oldPre = send != channel->sends.end() && send->pre;
                    const auto amount = a.amount.value_or (oldAmount);
                    const auto pre = a.pre.value_or (oldPre);
                    changed = amount != oldAmount || pre != oldPre;
                    if (! canAdvance()) return Code::internalError;
                    if (changed) document.setSend (a.channelId, a.fxId, amount, pre);
                    return P::CommandResult { P::SendResult { amount, pre } };
                }
            }
        }
    }, command.args);
    if (const auto* error = std::get_if<Code> (&applied)) return fail (*error);
    // Multiple existing callbacks from one setter produce one complete observation and one revision.
    if (! state.update (ControlState::capture (document, groups, audioRunning()))) return fail (Code::internalError);
    return P::Ack { command.id, context(), changed, std::get<P::CommandResult> (applied) };
}

} // namespace gocue::livemix
