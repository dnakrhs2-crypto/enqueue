#include "model/LevelMatrix.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace gocue
{

//==============================================================================
juce::var dbToVar (double db)
{
    if (LevelMatrix::isSilent (db))
        return juce::var ("-inf");

    return juce::var (db);
}

double dbFromVar (const juce::var& v, double defaultDb)
{
    if (v.isVoid())
        return defaultDb;

    if (v.isString())
    {
        const auto text = v.toString().trim();

        if (text.equalsIgnoreCase ("-inf") || text.equalsIgnoreCase ("-infinity"))
            return LevelMatrix::silentDb;

        const double parsed = text.getDoubleValue();
        return std::isfinite (parsed) ? parsed : defaultDb;
    }

    const double d = (double) v;
    return std::isfinite (d) ? d : defaultDb;
}

//==============================================================================
float LevelMatrix::linear (double db) noexcept
{
    if (isSilent (db))
        return 0.0f;

    return juce::Decibels::decibelsToGain ((float) juce::jmin (db, maxDb));
}

double LevelMatrix::clampDb (double db) noexcept
{
    if (! std::isfinite (db) || db <= silentDb)
        return silentDb;

    return juce::jlimit (silentDb, maxDb, std::round (db * 10.0) / 10.0);
}

static double defaultCrosspoint (int numInputs, int input, int output) noexcept
{
    if (numInputs == 1)
        return output < 2 ? 0.0 : LevelMatrix::silentDb;

    return input == output ? 0.0 : LevelMatrix::silentDb;
}

void LevelMatrix::resize (int newInputs, int newOutputs)
{
    newInputs = juce::jlimit (0, maxInputs, newInputs);
    newOutputs = juce::jlimit (0, maxOutputs, newOutputs);
    const int oldInputs = numInputs();
    const int oldOutputs = numOutputs();

    inputDb.resize ((size_t) newInputs, 0.0);
    outputDb.resize ((size_t) newOutputs, 0.0);
    inputGang.resize ((size_t) newInputs, 0);
    outputGang.resize ((size_t) newOutputs, 0);
    crosspointDb.resize ((size_t) newInputs);
    crosspointGang.resize ((size_t) newInputs);

    // a mono file that becomes stereo (or the reverse) re-derives its default routing
    const bool routingChanged = (oldInputs == 1) != (newInputs == 1);

    for (int in = 0; in < newInputs; ++in)
    {
        auto& row = crosspointDb[(size_t) in];
        auto& gangRow = crosspointGang[(size_t) in];
        const int keep = (in < oldInputs && ! routingChanged) ? juce::jmin (oldOutputs, (int) row.size()) : 0;
        row.resize ((size_t) newOutputs);
        gangRow.resize ((size_t) newOutputs, 0);

        for (int out = keep; out < newOutputs; ++out)
            row[(size_t) out] = defaultCrosspoint (newInputs, in, out);
    }
}

void LevelMatrix::setDefaults()
{
    const int ins = numInputs(), outs = numOutputs();
    std::fill (inputDb.begin(), inputDb.end(), 0.0);
    std::fill (outputDb.begin(), outputDb.end(), 0.0);

    for (int in = 0; in < ins; ++in)
        for (int out = 0; out < outs; ++out)
            crosspointDb[(size_t) in][(size_t) out] = defaultCrosspoint (ins, in, out);
}

void LevelMatrix::silenceCrosspoints()
{
    for (auto& row : crosspointDb)
        std::fill (row.begin(), row.end(), silentDb);
}

float LevelMatrix::gainFor (int input, int output) const noexcept
{
    if (input < 0 || output < 0 || input >= numInputs() || output >= numOutputs())
        return 0.0f;

    const double in = inputDb[(size_t) input];
    const double out = outputDb[(size_t) output];
    const double cross = crosspointDb[(size_t) input][(size_t) output];

    if (isSilent (in) || isSilent (out) || isSilent (cross))
        return 0.0f;

    return linear (in + cross + out);
}

void LevelMatrix::sanitise() noexcept
{
    resize (numInputs(), numOutputs());   // makes every row the right length

    for (auto& v : inputDb)  v = clampDb (v);
    for (auto& v : outputDb) v = clampDb (v);

    for (auto& row : crosspointDb)
        for (auto& v : row)
            v = clampDb (v);

    mainGang = juce::jlimit (0, maxGang, mainGang);
    for (auto& g : inputGang)  g = juce::jlimit (0, maxGang, g);
    for (auto& g : outputGang) g = juce::jlimit (0, maxGang, g);

    for (auto& row : crosspointGang)
        for (auto& g : row)
            g = juce::jlimit (0, maxGang, g);
}

bool LevelMatrix::operator== (const LevelMatrix& o) const noexcept
{
    return inputDb == o.inputDb && outputDb == o.outputDb && crosspointDb == o.crosspointDb
        && mainGang == o.mainGang && inputGang == o.inputGang && outputGang == o.outputGang && crosspointGang == o.crosspointGang;
}

static juce::var dbArrayToVar (const std::vector<double>& values)
{
    juce::Array<juce::var> arr;

    for (double v : values)
        arr.add (dbToVar (v));

    return juce::var (arr);
}

static std::vector<double> dbArrayFromVar (const juce::var& v)
{
    std::vector<double> result;

    if (const auto* arr = v.getArray())
        for (const auto& item : *arr)
            result.push_back (dbFromVar (item, 0.0));

    return result;
}

static juce::var intArrayToVar (const std::vector<int>& values)
{
    juce::Array<juce::var> arr;

    for (int v : values)
        arr.add (v);

    return juce::var (arr);
}

static std::vector<int> intArrayFromVar (const juce::var& v)
{
    std::vector<int> result;

    if (const auto* arr = v.getArray())
        for (const auto& item : *arr)
            result.push_back ((int) item);

    return result;
}

juce::var LevelMatrix::toVar() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("inputs", dbArrayToVar (inputDb));
    obj->setProperty ("outputs", dbArrayToVar (outputDb));

    juce::Array<juce::var> rows;

    for (const auto& row : crosspointDb)
        rows.add (dbArrayToVar (row));

    obj->setProperty ("crosspoints", juce::var (rows));

    bool anyGang = mainGang != 0;

    for (int g : inputGang)  anyGang = anyGang || g != 0;
    for (int g : outputGang) anyGang = anyGang || g != 0;

    for (const auto& row : crosspointGang)
        for (int g : row)
            anyGang = anyGang || g != 0;

    if (anyGang)
    {
        obj->setProperty ("mainGang", mainGang);
        obj->setProperty ("inputGangs", intArrayToVar (inputGang));
        obj->setProperty ("outputGangs", intArrayToVar (outputGang));

        juce::Array<juce::var> gangRows;

        for (const auto& row : crosspointGang)
            gangRows.add (intArrayToVar (row));

        obj->setProperty ("crosspointGangs", juce::var (gangRows));
    }

    return juce::var (obj);
}

LevelMatrix LevelMatrix::fromVar (const juce::var& v)
{
    LevelMatrix m;

    if (v.getDynamicObject() == nullptr)
        return m;

    m.inputDb = dbArrayFromVar (v.getProperty ("inputs", juce::var()));
    m.outputDb = dbArrayFromVar (v.getProperty ("outputs", juce::var()));

    if (const auto* rows = v.getProperty ("crosspoints", juce::var()).getArray())
        for (const auto& row : *rows)
            m.crosspointDb.push_back (dbArrayFromVar (row));

    m.mainGang = (int) v.getProperty ("mainGang", 0);
    m.inputGang = intArrayFromVar (v.getProperty ("inputGangs", juce::var()));
    m.outputGang = intArrayFromVar (v.getProperty ("outputGangs", juce::var()));

    if (const auto* rows = v.getProperty ("crosspointGangs", juce::var()).getArray())
        for (const auto& row : *rows)
            m.crosspointGang.push_back (intArrayFromVar (row));

    // rows must match the input count; crosspoint rows shorter than the outputs get defaults
    const int ins = m.numInputs();
    const int outs = m.numOutputs();
    m.crosspointDb.resize ((size_t) ins);
    m.crosspointGang.resize ((size_t) ins);
    m.inputGang.resize ((size_t) ins, 0);
    m.outputGang.resize ((size_t) outs, 0);

    for (int in = 0; in < ins; ++in)
    {
        auto& row = m.crosspointDb[(size_t) in];
        const int had = (int) row.size();
        row.resize ((size_t) outs);

        for (int out = had; out < outs; ++out)
            row[(size_t) out] = defaultCrosspoint (ins, in, out);

        m.crosspointGang[(size_t) in].resize ((size_t) outs, 0);
    }

    m.sanitise();
    return m;
}

//==============================================================================
void TrimLevels::resize (int numOutputs)
{
    outputDb.resize ((size_t) juce::jlimit (0, LevelMatrix::maxOutputs, numOutputs), 0.0);
}

float TrimLevels::gainForOutput (int output) const noexcept
{
    const double out = output >= 0 && output < (int) outputDb.size() ? outputDb[(size_t) output] : 0.0;
    return juce::Decibels::decibelsToGain ((float) (mainDb + out));
}

void TrimLevels::sanitise() noexcept
{
    auto clamp = [] (double db) { return std::isfinite (db) ? juce::jlimit (-60.0, LevelMatrix::maxDb, std::round (db * 10.0) / 10.0) : 0.0; };
    mainDb = clamp (mainDb);

    for (auto& v : outputDb)
        v = clamp (v);
}

juce::var TrimLevels::toVar() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("main", mainDb);
    obj->setProperty ("outputs", dbArrayToVar (outputDb));
    return juce::var (obj);
}

TrimLevels TrimLevels::fromVar (const juce::var& v)
{
    TrimLevels t;

    if (v.getDynamicObject() == nullptr)
        return t;

    t.mainDb = dbFromVar (v.getProperty ("main", 0.0), 0.0);
    t.outputDb = dbArrayFromVar (v.getProperty ("outputs", juce::var()));
    t.sanitise();
    return t;
}

} // namespace gocue
