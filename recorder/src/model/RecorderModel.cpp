#include "RecorderModel.h"
#include <algorithm>
#include <intrin.h>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace gocue::recorder
{
namespace
{
constexpr Sample maxSample = (std::numeric_limits<Sample>::max)();
void check(bool condition, const char* message)
{ if (!condition) throw std::invalid_argument(message); }
bool rangeOk(Sample start, Sample length, Sample limit = maxSample)
{ return start >= 0 && length > 0 && start <= limit && length <= limit - start; }
template<class T> bool uniqueInsert(std::set<T>& ids, const T& id)
{ return ids.insert(id).second; }
void checkRanges(const std::vector<SampleRange>& ranges, Sample limit)
{
    Sample end = 0;
    for (const auto& r : ranges)
    {
        check(rangeOk(r.start, r.length, limit) && r.start >= end, "원본 범위가 비었거나 겹칩니다.");
        end = r.start + r.length;
    }
}
}
Id newId() { return juce::Uuid().toString(); }
bool isId(const Id& id)
{
    return id.length() == 32 && id.containsOnly("0123456789abcdef") && !juce::Uuid(id).isNull();
}
Sample rescaleRound(Sample position, std::uint64_t numerator, std::uint64_t denominator)
{
    if (numerator == 0 || denominator == 0) throw std::invalid_argument("시간 변환 비율이 0입니다.");
    const bool negative = position < 0;
    const auto magnitude = negative ? std::uint64_t(-(position + 1)) + 1 : std::uint64_t(position);
    unsigned __int64 high = 0, remainder = 0;
    const auto low = _umul128(magnitude, numerator, &high);
    if (high >= denominator) throw std::overflow_error("시간 변환 범위를 초과했습니다.");
    auto result = _udiv128(high, low, denominator, &remainder);
    const auto limit = std::uint64_t(maxSample) + (negative ? 1u : 0u);
    if (result > limit) throw std::overflow_error("시간 변환 범위를 초과했습니다.");
    if (remainder >= denominator / 2 + denominator % 2)
    {
        if (result == limit) throw std::overflow_error("시간 반올림 범위를 초과했습니다.");
        ++result;
    }
    if (!negative) return static_cast<Sample>(result);
    return result == std::uint64_t(maxSample) + 1 ? (std::numeric_limits<Sample>::min)() : -static_cast<Sample>(result);
}
Sample frameToSample(Sample frame, std::uint32_t Fs, FrameRate fps)
{
    if (Fs == 0 || fps.denominator == 0) throw std::invalid_argument("샘플레이트와 프레임레이트를 확인하세요.");
    return rescaleRound(frame, std::uint64_t(Fs) * fps.denominator, fps.numerator);
}
Sample sampleToFrame(Sample sample, std::uint32_t Fs, FrameRate fps)
{
    if (Fs == 0 || fps.denominator == 0) throw std::invalid_argument("샘플레이트와 프레임레이트를 확인하세요.");
    return rescaleRound(sample, fps.numerator, std::uint64_t(Fs) * fps.denominator);
}
Sample Clip::timelineEnd() const
{
    if (!rangeOk(timelineStartSample, lengthSamples)) throw std::overflow_error("클립의 시간 범위가 잘못되었습니다.");
    return timelineStartSample + lengthSamples;
}
bool isProjectRelativePath(const juce::String& path)
{
    if (path.isEmpty() || path.startsWithChar('/') || path.containsAnyOf("\\:<>\"|?*") || path.trim() != path)
        return false;
    const auto parts = juce::StringArray::fromTokens(path, "/", "");
    for (const auto& part : parts)
    {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWithChar('.') || part.endsWithChar(' ')) return false;
        const auto base = part.upToFirstOccurrenceOf(".", false, false).toUpperCase();
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL"
            || (base.length() == 4 && (base.startsWith("COM") || base.startsWith("LPT"))
                && base[3] >= '1' && base[3] <= '9')) return false;
        for (const auto c : part) if (c < 32) return false;
    }
    return true;
}
const MediaAsset* MediaRegistry::findAsset(const Id& id) const
{ for (const auto& a : assets) if (a.assetId == id) return &a; return nullptr; }
const Take* MediaRegistry::findTake(const Id& id) const
{ for (const auto& t : takes) if (t.takeId == id) return &t; return nullptr; }
const Clip* RecorderProject::findClip(const Id& id) const
{ for (const auto& t : tracks) for (const auto& c : t.clips.items()) if (c.clipId == id) return &c; return nullptr; }
bool RecorderProject::isActive(const Clip& clip) const
{
    if (clip.takeStackId.isEmpty()) return true;
    for (const auto& s : takeStacks) if (s.stackId == clip.takeStackId) return s.activeVersionId == clip.versionId;
    return false;
}
Sample RecorderProject::activeTimelineEnd() const
{
    Sample end = 0;
    for (const auto& t : tracks) for (const auto& c : t.clips.items()) if (isActive(c)) end = (std::max)(end, c.timelineEnd());
    return end;
}
juce::Result RecorderProject::validate() const
{
    try
    {
        check(schemaVersion == currentSchemaVersion && isId(projectId), "지원하지 않는 프로젝트 형식 또는 식별자입니다.");
        check(Fs > 0 && (fps == FrameRate{30, 1} || fps == FrameRate{60, 1}), "프로젝트 시간 기준이 잘못되었습니다.");
        check(editRevision >= 0 && media != nullptr, "프로젝트 저장 상태가 잘못되었습니다.");
        std::set<Id> entityIds;
        const auto id = [&](const Id& value) { check(isId(value) && uniqueInsert(entityIds, value), "식별자가 없거나 중복됩니다."); };
        id(projectId);
        std::set<juce::String> paths;
        std::map<Id, const MediaAsset*> assets;
        for (const auto& a : media->assets)
        {
            id(a.assetId); assets.emplace(a.assetId, &a);
            check(a.kind == AssetKind::camera || a.kind == AssetKind::mic || a.kind == AssetKind::importAudio, "잘못된 미디어 종류입니다.");
            check(a.logicalLength > 0 && a.mediaGeneration >= 0 && a.contentIdentity.isNotEmpty(), "미디어 길이 또는 원본 식별 정보가 없습니다.");
            check(a.sourceUnitsNumerator > 0 && a.sourceUnitsDenominator > 0
                  && a.sourceUnitsNumerator <= std::uint64_t(maxSample) && a.sourceUnitsDenominator <= std::uint64_t(maxSample), "원본 시간 매핑이 잘못되었습니다.");
            const auto addPath = [&](const juce::String& path)
            {
                const auto parts = juce::StringArray::fromTokens(path, "/", "");
                const bool recoveryPath = parts.size() > 3 && parts[0] == "recovery"
                    && juce::Uuid(parts[1]).toDashedString() == parts[1] && !juce::Uuid(parts[1]).isNull();
                check(isProjectRelativePath(path) && (path.startsWith("media/") || recoveryPath) && uniqueInsert(paths, path.toLowerCase()), "미디어 상대 경로가 잘못되었거나 중복됩니다.");
            };
            if (a.relativePath.isNotEmpty()) addPath(a.relativePath);
            check(a.relativePath.isNotEmpty() || !a.chunks.empty(), "미디어 경로가 없습니다.");
            std::vector<SampleRange> chunkRanges;
            for (const auto& chunk : a.chunks) { addPath(chunk.relativePath); chunkRanges.push_back(chunk.sourceRange); }
            checkRanges(chunkRanges, a.logicalLength);
            checkRanges(a.availableRanges, a.logicalLength); checkRanges(a.gaps, a.logicalLength);
            auto partition = a.availableRanges;
            partition.insert(partition.end(), a.gaps.begin(), a.gaps.end());
            std::sort(partition.begin(), partition.end(), [](auto x, auto y) { return x.start < y.start; });
            Sample end = 0;
            for (const auto r : partition) { check(r.start == end, "미디어의 가용 범위와 빈 구간이 일치하지 않습니다."); end += r.length; }
            check(end == a.logicalLength, "미디어 원본 범위가 빠졌습니다.");
            if (a.relativePath.isEmpty())
                for (const auto r : a.availableRanges)
                {
                    Sample covered = r.start;
                    for (const auto c : chunkRanges)
                        if (c.start <= covered && c.start + c.length > covered) covered = c.start + c.length;
                    check(covered >= r.start + r.length, "가용 미디어를 제공하는 청크가 없습니다.");
                }
            const auto& f = a.originalFormat;
            check(f.codec.isNotEmpty(), "원본 포맷이 없습니다.");
            if (a.kind == AssetKind::camera)
                check(f.width == 1920 && f.height == 1080 && f.fps.numerator > 0 && f.fps.denominator > 0, "카메라 원본 포맷이 잘못되었습니다.");
            else
            {
                check(f.sampleRate > 0 && f.channels > 0 && f.bitsPerSample >= 0, "오디오 원본 포맷이 잘못되었습니다.");
                if (a.kind == AssetKind::mic) check((f.channels == 1 || f.channels == 2) && f.bitsPerSample == 24 && f.sampleRate == Fs, "마이크 원본은 프로젝트 레이트의 모노 또는 스테레오 24비트여야 합니다.");
            }
            const std::uint64_t expectedNumerator = a.kind == AssetKind::camera ? f.fps.numerator : f.sampleRate;
            const std::uint64_t expectedDenominator = std::uint64_t(Fs) * (a.kind == AssetKind::camera ? f.fps.denominator : 1u);
            const auto expectedGcd = std::gcd(expectedNumerator, expectedDenominator);
            const auto actualGcd = std::gcd(a.sourceUnitsNumerator, a.sourceUnitsDenominator);
            check(a.sourceUnitsNumerator / actualGcd == expectedNumerator / expectedGcd
                && a.sourceUnitsDenominator / actualGcd == expectedDenominator / expectedGcd, "원본 포맷과 유리수 시간 매핑이 일치하지 않습니다.");
        }
        std::set<Id> takeAssets;
        std::map<Id, int> cameraSlots;
        std::set<int> takeNumbers;
        for (const auto& t : media->takes)
        {
            id(t.takeId);
            check(t.number > 0 && uniqueInsert(takeNumbers, t.number) && t.createdAt.isNotEmpty(), "테이크 번호 또는 시각이 잘못되었습니다.");
            check(t.mode == TakeMode::normal || t.mode == TakeMode::dub, "테이크 배치 모드가 잘못되었습니다.");
            check(t.state >= TakeState::recording && t.state <= TakeState::partial, "테이크 상태가 잘못되었습니다.");
            check(t.N0 >= 0 && t.O0 >= 0 && rangeOk(t.placementSample, t.logicalLength), "테이크 시간 범위가 잘못되었습니다.");
            check(t.microphoneAssetIds.size() <= 8 && t.capture.physicalInputs.size() == t.microphoneAssetIds.size(), "마이크 참조와 입력 매핑이 일치하지 않습니다.");
            std::set<int> inputs;
            check(t.capture.physicalInputsRight.empty() || t.capture.physicalInputsRight.size() == t.capture.physicalInputs.size(), "스테레오 입력 매핑 수가 일치하지 않습니다.");
            for (size_t i = 0; i < t.capture.physicalInputs.size(); ++i)
            {
                const int left = t.capture.physicalInputs[i], right = t.capture.physicalInputsRight.empty() ? -1 : t.capture.physicalInputsRight[i];
                check(left >= 0 && left <= 255 && uniqueInsert(inputs, left), "물리 입력이 잘못되었거나 중복됩니다.");
                check(right == -1 || (right == left + 1 && right <= 255 && uniqueInsert(inputs, right)), "스테레오 오른쪽 입력이 잘못되었거나 중복됩니다.");
                const auto found = assets.find(t.microphoneAssetIds[i]);
                check(found != assets.end() && found->second->originalFormat.channels == (right >= 0 ? 2 : 1), "마이크 채널 수와 물리 입력 매핑이 일치하지 않습니다.");
            }
            const auto ref = [&](const Id& value, AssetKind kind)
            {
                const auto found = assets.find(value);
                check(found != assets.end() && found->second->kind == kind && uniqueInsert(takeAssets, value), "테이크의 미디어 참조가 잘못되었습니다.");
                check(found->second->logicalLength <= t.logicalLength, "원본 길이가 테이크 논리 길이를 벗어납니다.");
            };
            // A round-06 audio-only crash can retain recovered microphones even
            // when no camera was registered. Normal take creation still needs cam1.
            if (t.cam1AssetId.isNotEmpty()) { ref(t.cam1AssetId, AssetKind::camera); cameraSlots[t.cam1AssetId] = 1; }
            else check(t.state == TakeState::partial && !t.microphoneAssetIds.empty(), "캠1 또는 복구된 마이크가 필요합니다.");
            if (t.cam2AssetId.isNotEmpty()) { ref(t.cam2AssetId, AssetKind::camera); cameraSlots[t.cam2AssetId] = 2; }
            for (const auto& mic : t.microphoneAssetIds) ref(mic, AssetKind::mic);
        }
        for (const auto& a : media->assets)
            check(a.kind == AssetKind::importAudio || takeAssets.count(a.assetId) != 0, "녹화 원본에 테이크 참조가 없습니다.");
        std::map<Id, const Clip*> clips;
        std::map<Id, TrackKind> clipKinds;
        std::set<int> micLanes;
        int cam1 = 0, cam2 = 0;
        for (const auto& t : tracks)
        {
            id(t.trackId);
            check(t.kind >= TrackKind::cam1 && t.kind <= TrackKind::importAudio, "트랙 종류가 잘못되었습니다.");
            if (t.kind == TrackKind::cam1) ++cam1;
            if (t.kind == TrackKind::cam2) ++cam2;
            if (t.kind == TrackKind::mic)
                check(t.microphoneIndex >= 0 && t.microphoneIndex < 8 && uniqueInsert(micLanes, t.microphoneIndex), "마이크 트랙 번호가 잘못되었습니다.");
            else check(t.microphoneIndex == -1, "마이크가 아닌 트랙에 입력 번호가 있습니다.");
            for (const auto& c : t.clips.items())
            {
                id(c.clipId); clips.emplace(c.clipId, &c); clipKinds.emplace(c.clipId, t.kind);
                check(c.trackId == t.trackId, "클립의 트랙 참조가 잘못되었습니다.");
                const auto a = assets.find(c.assetId);
                check(a != assets.end(), "클립의 원본을 찾을 수 없습니다.");
                const auto expected = t.kind == TrackKind::mic ? AssetKind::mic : t.kind == TrackKind::importAudio ? AssetKind::importAudio : AssetKind::camera;
                check(a->second->kind == expected, "클립 원본과 트랙 종류가 다릅니다.");
                if (expected == AssetKind::camera) check(cameraSlots[c.assetId] == (t.kind == TrackKind::cam1 ? 1 : 2), "클립의 카메라 번호가 다릅니다.");
                check(rangeOk(c.sourceIn, c.lengthSamples, a->second->logicalLength)
                      && rangeOk(c.timelineStartSample, c.lengthSamples), "클립이 비었거나 원본 또는 시간 범위를 벗어납니다.");
                check(c.takeStackId.isEmpty() == c.versionId.isEmpty(), "테이크 스택과 버전 참조가 일치하지 않습니다.");
            }
        }
        check(cam1 <= 1 && cam2 <= 1, "카메라 트랙은 각각 하나만 허용합니다.");
        std::set<Id> stacked, linked;
        for (const auto& s : takeStacks)
        {
            id(s.stackId);
            check(rangeOk(s.anchorSample, s.spanSamples) && !s.versions.empty(), "테이크 스택 범위 또는 버전이 잘못되었습니다.");
            bool hasActive = false;
            for (const auto& v : s.versions)
            {
                id(v.versionId); hasActive |= v.versionId == s.activeVersionId;
                std::map<Id, std::vector<SampleRange>> versionRanges;
                for (const auto& value : v.clipIds)
                {
                    const auto found = clips.find(value);
                    check(found != clips.end() && uniqueInsert(stacked, value), "테이크 버전의 클립이 없거나 중복됩니다.");
                    const auto& c = *found->second;
                    check(c.takeStackId == s.stackId && c.versionId == v.versionId && clipKinds[value] != TrackKind::importAudio, "테이크 버전 연결이 잘못되었습니다.");
                    if (clipKinds[value] == TrackKind::cam1 || clipKinds[value] == TrackKind::cam2)
                        check(c.timelineStartSample >= s.anchorSample && c.timelineEnd() <= s.anchorSample + s.spanSamples, "영상 클립이 테이크 스택 범위를 벗어납니다.");
                    versionRanges[c.trackId].push_back({c.timelineStartSample, c.lengthSamples});
                }
                for (auto& entry : versionRanges)
                {
                    auto& ranges = entry.second;
                    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.start < b.start; });
                    checkRanges(ranges, maxSample);
                }
            }
            check(hasActive, "활성 테이크 버전을 찾을 수 없습니다.");
        }
        for (const auto& g : linkGroups)
        {
            id(g.linkGroupId); check(g.clipIds.size() >= 2, "링크에는 클립이 두 개 이상 필요합니다.");
            const Clip* first = nullptr;
            for (const auto& value : g.clipIds)
            {
                const auto found = clips.find(value);
                check(found != clips.end() && uniqueInsert(linked, value) && found->second->linkGroupId == g.linkGroupId, "클립 링크가 일치하지 않습니다.");
                const auto* c = found->second;
                if (first != nullptr) check(isActive(*c) == isActive(*first)
                    && (c->takeStackId.isEmpty() || c->takeStackId != first->takeStackId || c->versionId == first->versionId), "서로 다른 활성 상태의 클립을 링크할 수 없습니다.");
                else first = c;
            }
        }
        for (const auto& entry : clips)
        {
            const auto& c = *entry.second;
            check((stacked.count(c.clipId) != 0) == c.takeStackId.isNotEmpty(), "클립의 테이크 스택 참조가 없습니다.");
            check((linked.count(c.clipId) != 0) == c.linkGroupId.isNotEmpty(), "클립의 링크 그룹 참조가 없습니다.");
        }
        for (const auto& t : tracks)
        {
            std::vector<SampleRange> ranges;
            for (const auto& c : t.clips.items()) if (isActive(c)) ranges.push_back({c.timelineStartSample, c.lengthSamples});
            std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.start < b.start; });
            checkRanges(ranges, maxSample);
        }
        for (const auto& m : markers) { id(m.markerId); check(m.sample >= 0, "마커 위치가 음수입니다."); }
        return juce::Result::ok();
    }
    catch (const std::exception& e) { return juce::Result::fail(juce::String::fromUTF8(e.what())); }
}
} // namespace gocue::recorder
