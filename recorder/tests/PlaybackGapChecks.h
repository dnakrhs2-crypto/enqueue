#pragma once
#include "StabilityTestAccess.h"
#include "CutSeamChecks.h"
#include "export/FinalVideoExporter.h"

namespace recorder_playback_gap
{
using namespace gocue::recorder;
using recorder_test::require;
enum class Missing { none, tail, head, interior };

inline void sessionGapCheck(Missing missing, Sample seamGap = 0, Sample sourceIn = 0)
{
    using namespace recorder_cut_seam;
    RecorderProject project; project.fps = {60, 1};
    auto media = std::make_shared<MediaRegistry>(); project.media = media;
    Track track;
    std::vector<std::shared_ptr<const VideoIndex>> sources;
    for (unsigned i = 0; i < 2; ++i)
    {
        const Sample clipLength = i == 0 ? 320640 : 4800;
        MediaAsset asset; asset.logicalLength = sourceIn + clipLength;
        asset.relativePath = "media/takes/gap-" + juce::String(i) + "/cam1.mp4";
        asset.contentIdentity = asset.assetId; asset.mediaGeneration = 1;
        asset.originalFormat.codec = "h264"; asset.originalFormat.width = 1920; asset.originalFormat.height = 1080;
        asset.originalFormat.fps = project.fps; asset.sourceUnitsNumerator = 1; asset.sourceUnitsDenominator = 800;
        asset.availableRanges = {{0, asset.logicalLength}};
        auto source = syntheticIndex();
        if (i == 0 && missing == Missing::tail)
        {
            asset.availableRanges = {{0, 320000}}; asset.gaps = {{320000, 640}};
            source->packets.resize(400); source->validateAndBuild();
            require(source->length == 320000, "Recovered index length");
        }
        if (i == 1 && missing == Missing::head)
        { asset.availableRanges = {{640, 4160}}; asset.gaps = {{0, 640}}; }
        if (i == 0 && missing == Missing::interior)
        { asset.availableRanges = {{0, 320000}, {320001, 639}}; asset.gaps = {{320000, 1}}; }
        Take take; take.number = int(i + 1); take.createdAt = "2026-09-10";
        take.logicalLength = asset.logicalLength; take.cam1AssetId = asset.assetId;
        take.state = asset.gaps.empty() ? TakeState::complete : TakeState::partial;
        take.placementSample = i == 0 ? 0 : 320640 + seamGap;
        Clip clip; clip.assetId = asset.assetId; clip.trackId = track.trackId;
        clip.sourceIn = sourceIn; clip.lengthSamples = clipLength; clip.timelineStartSample = take.placementSample;
        media->assets.push_back(asset); media->takes.push_back(take); track.clips.edit().push_back(clip); sources.push_back(source);
    }
    project.tracks.push_back(track);
    RecorderDocument document; const auto adopted = document.adopt(project, {}, {});
    require(adopted.wasOk(), adopted.getErrorMessage().toRawUTF8());
    RecorderSession session(document);
    const auto clips = StabilityTestAccess::prepareVideos(session, sources);
    require(clips.size() == (missing == Missing::interior ? 3u : 2u), "Session did not partition availability");
    for (const auto& clip : clips) require(clip.mapping.gaps.empty(), "Expected session-precut mappings");
    if (missing == Missing::tail)
        require(clips[0].mapping.lengthSamples == 320000 && clips[1].mapping.timelineStartSample == 320640, "Recovery-tail reproduction coordinates");
    if (missing == Missing::head)
        require(clips[1].mapping.sourceIn == 640 && clips[1].mapping.timelineStartSample == 321280, "Leading-gap reproduction coordinates");

    ExportJob job(project, juce::File::getCurrentWorkingDirectory());
    auto stats = std::make_shared<DecodeStats>();
    VideoPlaybackEngine video([stats](auto source) { return std::make_unique<DelayedDecoder>(source, stats); });
    video.prepare(clips);
    PlaybackDisplayState display;
    unsigned black = 0, empty = 0, mismatches = 0;
    std::vector<Sample> points{319999, 320000, 320001, 320639, 320640, 321279, 321280};
    if (seamGap) { points.push_back(320640 + seamGap - 1); points.push_back(320640 + seamGap); }
    for (Sample frame = 390; frame <= 406; ++frame) points.push_back(frame * 800);
    std::sort(points.begin(), points.end()); points.erase(std::unique(points.begin(), points.end()), points.end());
    for (const auto at : points)
    {
        const auto generation = video.seek(at);
        awaitFrame([&] { require(video.status().wasOk(), "Session video decode failed"); return video.ready(at, generation); });
        const auto selection = video.displaySelection(0);
        const auto decision = submitPicture(display, selection);
        bool exportGap = true;
        for (const auto& lane : job.plan().tracks) for (const auto& span : lane.spans)
            if (!span.isGap() && at >= span.timeline.start && at - span.timeline.start < span.timeline.length) exportGap = false;
        if (at % 800 == 0)
        {
            const auto mapping = FinalVideoExporter::mappingAt(job, TrackKind::cam1, at / 800);
            require(mapping.black() == exportGap, "Export frame disagrees with compiled gap");
            if (!mapping.black() && selection.frame)
                require(selection.frame->pts == mapping.sourceFrame, "Playback/export source PTS mismatch");
        }
        const bool expectedGap = (missing == Missing::tail && at >= 320000 && at < 320640)
            || (missing == Missing::head && at >= 320640 && at < 321280)
            || (missing == Missing::interior && at == 320000)
            || (seamGap && at >= 320640 && at < 320640 + seamGap);
        require(exportGap == expectedGap, "Export gap oracle does not match fixture");
        // Legacy empty timeline seams still hold the preceding final source
        // sample, including cold seeks beyond that sample's ordinary frame end.
        const bool playbackGap = exportGap && !seamGap;
        if (seamGap && exportGap)
            require(selection.frame && selection.frame->pts == sources[0]->packets[sources[0]->frameAt(sourceIn + 320639)].pts,
                "Session seam decode coordinate escaped the final source sample");
        black += decision.action == PlaybackDisplayAction::clear;
        empty += !playbackGap && !selection.frame;
        mismatches += selection.gap != playbackGap || (decision.action == PlaybackDisplayAction::clear) != playbackGap;
        if (selection.gap != playbackGap)
            std::cout << "SESSION_GAP case=" << int(missing) << " sample=" << at << " playbackGap=" << selection.gap
                      << " exportBlack=" << exportGap << " selectedPts=" << (selection.frame ? selection.frame->pts : -1) << '\n';
    }
    std::cout << "SESSION_GAP case=" << int(missing) << " seamGap=" << seamGap << " sourceIn=" << sourceIn
              << " preparedParts=" << clips.size() << " checked=" << points.size()
              << " mismatches=" << mismatches << " black=" << black << " missingAvailable=" << empty << '\n';
    require(mismatches == 0, "Actual RecorderSession preparation fills a source gap that export leaves black");
    require(empty == 0, "Available session clip lost a frame");
    if (missing == Missing::none) require(black == 0, "Joined session clips showed black");
}

inline void fragmentGapSplitCheck(bool tail)
{
    using namespace recorder_cut_seam;
    auto source = syntheticIndex(); auto stats = std::make_shared<DecodeStats>();
    PlaybackVideoClip a; a.source = source; a.mapping.clipId = newId(); a.mapping.trackId = newId();
    a.mapping.mediaGeneration = 1; a.mapping.lengthSamples = 4800;
    auto b = a; b.mapping.clipId = newId(); b.mapping.timelineStartSample = 4801;
    auto& fragment = tail ? a : b;
    fragment.beginsAtClipStart = tail; fragment.endsAtClipEnd = !tail;
    fragment.mapping.gaps = {{1600, 1}};
    VideoPlaybackEngine video([stats](auto s) { return std::make_unique<DelayedDecoder>(s, stats); });
    video.prepare({a, b}); video.seek(4800);
    require(video.displaySelection(0).gap, "Engine gap split restored a revoked original clip boundary");
}
}
