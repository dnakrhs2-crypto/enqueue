#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace gocue
{

/** QLab-style level matrix of one audio cue: rows = file channels (inputs), columns = cue outputs.
    Every level is in dB; anything at or below silentDb is silence (serialised as "-inf").
    The cue's *main* level lives in Cue::gainDb (the top-left "main" slider in QLab's grid), so the
    gain of one crosspoint = input + crosspoint + output (main is applied once, before the matrix).
    Gangs (0 = none, 1..8) only matter to the editor: sliders sharing a gang move together. */
struct LevelMatrix
{
    static constexpr double silentDb = -120.0;
    static constexpr double maxDb = 24.0;
    static constexpr int maxInputs = 24;
    static constexpr int maxOutputs = 128;
    static constexpr int maxGang = 8;

    std::vector<double> inputDb, outputDb;
    std::vector<std::vector<double>> crosspointDb;   // [input][output]
    int mainGang = 0;
    std::vector<int> inputGang, outputGang;
    std::vector<std::vector<int>> crosspointGang;

    int numInputs() const noexcept  { return (int) inputDb.size(); }
    int numOutputs() const noexcept { return (int) outputDb.size(); }

    static bool isSilent (double db) noexcept { return db <= silentDb; }
    static float linear (double db) noexcept;
    /** Rounds to the editor's 0.1 dB step and clamps into [silentDb, maxDb]; below silentDb becomes silentDb. */
    static double clampDb (double db) noexcept;

    /** Sets the size, keeping existing levels. New crosspoints follow the default routing:
        a mono input feeds outputs 1-2 at 0 dB, otherwise input n -> output n at 0 dB, the rest silent. */
    void resize (int newInputs, int newOutputs);
    /** Back to the default routing (all inputs / outputs 0 dB, diagonal crosspoints). */
    void setDefaults();
    /** Every crosspoint to silence (inputs / outputs stay). */
    void silenceCrosspoints();

    /** input + crosspoint + output as a linear gain; 0 when any of them is silent. Out-of-range = 0. */
    float gainFor (int input, int output) const noexcept;

    void sanitise() noexcept;
    bool operator== (const LevelMatrix& other) const noexcept;
    bool operator!= (const LevelMatrix& other) const noexcept { return ! (*this == other); }

    juce::var toVar() const;
    static LevelMatrix fromVar (const juce::var& v);
};

/** Per-cue trim (QLab "Trim" tab): a fixed offset applied after the matrix, never faded. */
struct TrimLevels
{
    double mainDb = 0.0;
    std::vector<double> outputDb;   // one per cue output

    void resize (int numOutputs);
    /** main + output trim as a linear gain (never silent: trims are offsets). */
    float gainForOutput (int output) const noexcept;
    void sanitise() noexcept;
    bool operator== (const TrimLevels& other) const noexcept { return mainDb == other.mainDb && outputDb == other.outputDb; }

    juce::var toVar() const;
    static TrimLevels fromVar (const juce::var& v);
};

/** dB <-> JSON value: numbers, or the string "-inf" for silence. */
juce::var dbToVar (double db);
double dbFromVar (const juce::var& v, double defaultDb = 0.0);

} // namespace gocue
