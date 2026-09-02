#pragma once

#include "model/Cue.h"

#include <juce_core/juce_core.h>

#include <cmath>

namespace gocue
{

/** Per-project settings (QLab "Workspace Settings"). Saved in the .gocue file. */
struct WorkspaceSettings
{
    // 큐 템플릿: new audio cues copy their settings (not name / number / file) from this cue
    bool hasCueTemplate = false;
    Cue cueTemplate;
    int rowSize = 1;                       // cue list rows: 0 small, 1 medium, 2 large
    // 일반
    double doubleGoSeconds = 0.0;          // minimum time between two GOs; 0 = off
    bool requireKeyUp = false;             // the GO key must be released before it fires again
    double panicSeconds = 2.0;             // Esc: fade everything out over this, then stop
    bool autoNumber = true;
    double numberIncrement = 1.0;
    bool autoLoadNewCues = false;
    bool lockPlayheadToSelection = true;
    bool startOnOpen = false;
    juce::String startOnOpenCue;
    bool startOnClose = false;
    juce::String startOnCloseCue;

    // 오디오
    double maxLevelDb = 12.0;
    double minLevelDb = -60.0;

    // 파일
    bool copyFilesIntoProject = false;
    bool autoBackup = true;
    int backupIntervalSeconds = 60;
    bool backupBeforeSave = true;
    bool rotateBackups = true;

    static constexpr double maxPanicSeconds = 600.0;
    static constexpr double maxDoubleGoSeconds = 60.0;

    void sanitise() noexcept
    {
        auto fix = [] (double& v, double lo, double hi, double fallback)
        {
            if (! std::isfinite (v))
                v = fallback;

            v = juce::jlimit (lo, hi, v);
        };

        fix (doubleGoSeconds, 0.0, maxDoubleGoSeconds, 0.0);
        fix (panicSeconds, 0.0, maxPanicSeconds, 2.0);
        fix (numberIncrement, 0.001, 1000.0, 1.0);
        fix (maxLevelDb, -30.0, 60.0, 12.0);
        fix (minLevelDb, -180.0, -40.0, -60.0);
        backupIntervalSeconds = juce::jlimit (5, 600, backupIntervalSeconds);
        rowSize = juce::jlimit (0, 2, rowSize);
        cueTemplate.sanitise();
    }

    /** Copies the template's settings onto a fresh cue (identity, name, number, file and cached facts stay). */
    void applyTemplate (Cue& cue) const
    {
        if (! hasCueTemplate)
            return;

        const auto id = cue.id;
        const auto name = cue.name;
        const auto number = cue.number;
        const auto file = cue.file;
        const auto duration = cue.durationSeconds;
        const bool missing = cue.fileMissing;
        cue = cueTemplate;
        cue.id = id;
        cue.name = name;
        cue.number = number;
        cue.file = file;
        cue.durationSeconds = duration;
        cue.fileMissing = missing;
        cue.hotkey.clear();               // a hotkey must stay unique
        cue.wallClock.enabled = false;
    }
};

} // namespace gocue
