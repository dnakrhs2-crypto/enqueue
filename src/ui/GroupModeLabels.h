#pragma once

#include "model/Cue.h"
#include "ui/UiUtils.h"

namespace gocue
{

/** How a group's mode is offered to the operator (0.9.8): a playlist with 반복 on is shown as a mode of its own -
    "순차 반복" (in order, round after round) and "랜덤 반복" (shuffled, reshuffled every round) - so a jukebox group
    is one pick in the mode box, not a mode plus two check boxes. The file keeps GroupMode + loop + shuffle as before,
    so projects from older versions open the same. The order here is the mode box's order. */
enum class GroupPreset { timeline, playlist, sequentialLoop, randomLoop, startFirstEnter, startFirst, random };

inline GroupPreset groupPresetOf (const GroupCueData& g) noexcept
{
    switch (g.mode)
    {
        case GroupMode::timeline:        return GroupPreset::timeline;
        case GroupMode::playlist:        return ! g.loop ? GroupPreset::playlist : g.shuffle ? GroupPreset::randomLoop : GroupPreset::sequentialLoop;
        case GroupMode::startFirstEnter: return GroupPreset::startFirstEnter;
        case GroupMode::startFirst:      return GroupPreset::startFirst;
        case GroupMode::random:          return GroupPreset::random;
    }

    return GroupPreset::timeline;
}

/** Sets the mode (and, for the playlist family, 반복 / 셔플) the preset stands for. The crossfade is left alone. */
inline void applyGroupPreset (GroupCueData& g, GroupPreset preset) noexcept
{
    switch (preset)
    {
        case GroupPreset::timeline:        g.mode = GroupMode::timeline; break;
        case GroupPreset::playlist:        g.mode = GroupMode::playlist; g.loop = false; g.shuffle = false; break;
        case GroupPreset::sequentialLoop:  g.mode = GroupMode::playlist; g.loop = true;  g.shuffle = false; break;
        case GroupPreset::randomLoop:      g.mode = GroupMode::playlist; g.loop = true;  g.shuffle = true; break;
        case GroupPreset::startFirstEnter: g.mode = GroupMode::startFirstEnter; break;
        case GroupPreset::startFirst:      g.mode = GroupMode::startFirst; break;
        case GroupPreset::random:          g.mode = GroupMode::random; break;
    }
}

/** The short name the cue list shows ("순차 반복 · 12개"). */
inline juce::String groupPresetName (const GroupCueData& g)
{
    switch (groupPresetOf (g))
    {
        case GroupPreset::timeline:        return ko ("타임라인");
        case GroupPreset::playlist:        return ko ("플레이리스트");
        case GroupPreset::sequentialLoop:  return ko ("순차 반복");
        case GroupPreset::randomLoop:      return ko ("랜덤 반복");
        case GroupPreset::startFirstEnter: return ko ("첫 큐 시작 후 진입");
        case GroupPreset::startFirst:      return ko ("첫 큐 시작");
        case GroupPreset::random:          return ko ("랜덤");
    }

    return {};
}

/** What the group does, in one line (the transport's description of the selected group). */
inline juce::String groupPresetDescription (const GroupCueData& g)
{
    switch (groupPresetOf (g))
    {
        case GroupPreset::timeline:        return ko ("자식 전부 동시에 시작 (각자 프리웨이트)");
        case GroupPreset::playlist:        return ko ("자식 차례로 한 바퀴 (두 번째 GO = 다음 곡)");
        case GroupPreset::sequentialLoop:  return ko ("자식 차례로, 끝나면 처음부터 계속 (두 번째 GO = 다음 곡)");
        case GroupPreset::randomLoop:      return ko ("자식 무작위로 계속, 한 바퀴마다 다시 섞음 (두 번째 GO = 다음 곡)");
        case GroupPreset::startFirstEnter: return ko ("첫 자식 시작, 플레이헤드는 그룹 안으로");
        case GroupPreset::startFirst:      return ko ("첫 자식 시작, 플레이헤드는 그룹 뒤로");
        case GroupPreset::random:          return ko ("GO마다 자식 하나를 랜덤으로 (한 바퀴에 한 번씩)");
    }

    return {};
}

} // namespace gocue
