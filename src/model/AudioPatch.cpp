#include "model/AudioPatch.h"

#include <cmath>

namespace gocue
{

namespace
{
    juce::var pluginToVar (const PluginSlotState& p)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("format", p.format);
        obj->setProperty ("name", p.name);
        obj->setProperty ("fileOrIdentifier", p.fileOrIdentifier);
        obj->setProperty ("uniqueId", p.uniqueId);
        obj->setProperty ("state", p.stateBase64);
        obj->setProperty ("description", p.descriptionXml);
        obj->setProperty ("bypassed", p.bypassed);
        return juce::var (obj);
    }

    PluginSlotState pluginFromVar (const juce::var& v)
    {
        PluginSlotState p;
        p.format           = v.getProperty ("format", "VST3").toString();
        p.name             = v.getProperty ("name", "").toString();
        p.fileOrIdentifier = v.getProperty ("fileOrIdentifier", "").toString();
        p.uniqueId         = (int) v.getProperty ("uniqueId", 0);
        p.stateBase64      = v.getProperty ("state", "").toString();
        p.descriptionXml   = v.getProperty ("description", "").toString();
        p.bypassed         = (bool) v.getProperty ("bypassed", false);
        return p;
    }

    juce::var insertsToVar (const std::vector<std::vector<PluginSlotState>>& inserts)
    {
        juce::Array<juce::var> outer;

        for (const auto& chain : inserts)
        {
            juce::Array<juce::var> inner;

            for (const auto& p : chain)
                inner.add (pluginToVar (p));

            outer.add (juce::var (inner));
        }

        return juce::var (outer);
    }

    std::vector<std::vector<PluginSlotState>> insertsFromVar (const juce::var& v)
    {
        std::vector<std::vector<PluginSlotState>> result;

        if (const auto* outer = v.getArray())
        {
            for (const auto& chainVar : *outer)
            {
                std::vector<PluginSlotState> chain;

                if (const auto* inner = chainVar.getArray())
                    for (const auto& p : *inner)
                        if (p.getDynamicObject() != nullptr)
                            chain.push_back (pluginFromVar (p));

                result.push_back (std::move (chain));
            }
        }

        return result;
    }
}

AudioPatch AudioPatch::makeDefault (const juce::String& patchName)
{
    AudioPatch p;
    p.name = patchName.isNotEmpty() ? patchName : juce::String::fromUTF8 ("\xEA\xB8\xB0\xEB\xB3\xB8 \xED\x8C\xA8\xEC\xB9\x98");   // 기본 패치
    p.sanitise();
    return p;
}

double AudioPatch::defaultRoutingDb (int cueOutput, int deviceOutput) noexcept
{
    return cueOutput == deviceOutput ? 0.0 : LevelMatrix::silentDb;
}

double AudioPatch::routing (int cueOutput, int deviceOutput) const noexcept
{
    if (cueOutput < 0 || deviceOutput < 0 || cueOutput >= numCueOutputs)
        return LevelMatrix::silentDb;

    if (cueOutput < (int) routingDb.size() && deviceOutput < (int) routingDb[(size_t) cueOutput].size())
        return routingDb[(size_t) cueOutput][(size_t) deviceOutput];

    return defaultRoutingDb (cueOutput, deviceOutput);
}

void AudioPatch::setRouting (int cueOutput, int deviceOutput, double db)
{
    if (cueOutput < 0 || deviceOutput < 0 || cueOutput >= numCueOutputs || deviceOutput >= LevelMatrix::maxOutputs)
        return;

    ensureDeviceOutputs (deviceOutput + 1);
    routingDb[(size_t) cueOutput][(size_t) deviceOutput] = LevelMatrix::clampDb (db);
}

float AudioPatch::routingGain (int cueOutput, int deviceOutput) const noexcept
{
    const double db = routing (cueOutput, deviceOutput);

    if (LevelMatrix::isSilent (db) || LevelMatrix::isSilent (mainDb))
        return 0.0f;

    return LevelMatrix::linear (db + mainDb);
}

void AudioPatch::ensureDeviceOutputs (int numDeviceOutputs)
{
    numDeviceOutputs = juce::jlimit (0, LevelMatrix::maxOutputs, numDeviceOutputs);
    routingDb.resize ((size_t) numCueOutputs);

    for (int k = 0; k < numCueOutputs; ++k)
    {
        auto& row = routingDb[(size_t) k];
        const int had = (int) row.size();

        if (had < numDeviceOutputs)
        {
            row.resize ((size_t) numDeviceOutputs);

            for (int m = had; m < numDeviceOutputs; ++m)
                row[(size_t) m] = defaultRoutingDb (k, m);
        }
    }
}

int AudioPatch::numStoredDeviceOutputs() const noexcept
{
    int n = 0;

    for (const auto& row : routingDb)
        n = juce::jmax (n, (int) row.size());

    return n;
}

juce::String AudioPatch::cueOutputName (int cueOutput) const
{
    if (cueOutput >= 0 && cueOutput < cueOutputNames.size() && cueOutputNames[cueOutput].isNotEmpty())
        return cueOutputNames[cueOutput];

    return juce::String::fromUTF8 ("\xEC\xB6\x9C\xEB\xA0\xA5 ") + juce::String (cueOutput + 1);   // 출력 n
}

bool AudioPatch::isFirstOfPair (int cueOutput) const noexcept
{
    return cueOutput >= 0 && cueOutput + 1 < numCueOutputs
        && cueOutput < (int) cueOutputStereoWithNext.size() && cueOutputStereoWithNext[(size_t) cueOutput] != 0;
}

bool AudioPatch::isSecondOfPair (int cueOutput) const noexcept
{
    return cueOutput > 0 && isFirstOfPair (cueOutput - 1);
}

void AudioPatch::sanitise()
{
    if (id.isNull())
        id = juce::Uuid();

    numCueOutputs = juce::jlimit (1, maxCueOutputs, numCueOutputs);

    while (cueOutputNames.size() < numCueOutputs)
        cueOutputNames.add ({});

    while (cueOutputNames.size() > numCueOutputs)
        cueOutputNames.remove (cueOutputNames.size() - 1);

    routingDb.resize ((size_t) numCueOutputs);

    for (auto& row : routingDb)
    {
        if ((int) row.size() > LevelMatrix::maxOutputs)
            row.resize ((size_t) LevelMatrix::maxOutputs);

        for (auto& v : row)
            v = LevelMatrix::clampDb (v);
    }

    if (! std::isfinite (mainDb))
        mainDb = 0.0;

    mainDb = LevelMatrix::clampDb (mainDb);
    cueOutputInserts.resize ((size_t) numCueOutputs);
    cueOutputStereoWithNext.resize ((size_t) numCueOutputs, 0);

    if ((int) deviceOutputInserts.size() > LevelMatrix::maxOutputs)
        deviceOutputInserts.resize ((size_t) LevelMatrix::maxOutputs);

    // pairs cannot overlap: a second-of-pair output cannot start another pair
    for (int k = 0; k < numCueOutputs; ++k)
        if (isSecondOfPair (k))
            cueOutputStereoWithNext[(size_t) k] = 0;

    if (numCueOutputs > 0)
        cueOutputStereoWithNext[(size_t) (numCueOutputs - 1)] = 0;   // nothing to pair with
}

static bool insertsEqual (const std::vector<std::vector<PluginSlotState>>& a, const std::vector<std::vector<PluginSlotState>>& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].size() != b[i].size())
            return false;

        for (size_t j = 0; j < a[i].size(); ++j)
        {
            const auto& x = a[i][j];
            const auto& y = b[i][j];

            if (x.format != y.format || x.name != y.name || x.fileOrIdentifier != y.fileOrIdentifier || x.uniqueId != y.uniqueId
                || x.stateBase64 != y.stateBase64 || x.descriptionXml != y.descriptionXml || x.bypassed != y.bypassed)
                return false;
        }
    }

    return true;
}

bool AudioPatch::operator== (const AudioPatch& o) const noexcept
{
    return id == o.id && name == o.name && numCueOutputs == o.numCueOutputs && cueOutputNames == o.cueOutputNames
        && routingDb == o.routingDb && mainDb == o.mainDb && cueOutputStereoWithNext == o.cueOutputStereoWithNext
        && insertsEqual (cueOutputInserts, o.cueOutputInserts) && insertsEqual (deviceOutputInserts, o.deviceOutputInserts);
}

juce::var AudioPatch::toVar() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("id", id.toString());
    obj->setProperty ("name", name);
    obj->setProperty ("cueOutputs", numCueOutputs);

    juce::Array<juce::var> names;

    for (const auto& n : cueOutputNames)
        names.add (n);

    obj->setProperty ("cueOutputNames", juce::var (names));

    juce::Array<juce::var> rows;

    for (const auto& row : routingDb)
    {
        juce::Array<juce::var> cols;

        for (double v : row)
            cols.add (dbToVar (v));

        rows.add (juce::var (cols));
    }

    obj->setProperty ("routing", juce::var (rows));
    obj->setProperty ("mainDb", dbToVar (mainDb));

    juce::Array<juce::var> pairs;

    for (char c : cueOutputStereoWithNext)
        pairs.add (c != 0);

    obj->setProperty ("stereoPairs", juce::var (pairs));
    obj->setProperty ("cueOutputInserts", insertsToVar (cueOutputInserts));
    obj->setProperty ("deviceOutputInserts", insertsToVar (deviceOutputInserts));
    return juce::var (obj);
}

AudioPatch AudioPatch::fromVar (const juce::var& v)
{
    AudioPatch p;

    if (v.getDynamicObject() == nullptr)
        return makeDefault();

    p.id = juce::Uuid (v.getProperty ("id", "").toString());
    p.name = v.getProperty ("name", "").toString();
    p.numCueOutputs = (int) v.getProperty ("cueOutputs", defaultCueOutputs);

    if (const auto* names = v.getProperty ("cueOutputNames", juce::var()).getArray())
        for (const auto& n : *names)
            p.cueOutputNames.add (n.toString());

    if (const auto* rows = v.getProperty ("routing", juce::var()).getArray())
    {
        for (const auto& rowVar : *rows)
        {
            std::vector<double> row;

            if (const auto* cols = rowVar.getArray())
                for (const auto& c : *cols)
                    row.push_back (dbFromVar (c, LevelMatrix::silentDb));

            p.routingDb.push_back (std::move (row));
        }
    }

    p.mainDb = dbFromVar (v.getProperty ("mainDb", 0.0), 0.0);

    if (const auto* pairs = v.getProperty ("stereoPairs", juce::var()).getArray())
        for (const auto& c : *pairs)
            p.cueOutputStereoWithNext.push_back ((bool) c ? 1 : 0);

    p.cueOutputInserts = insertsFromVar (v.getProperty ("cueOutputInserts", juce::var()));
    p.deviceOutputInserts = insertsFromVar (v.getProperty ("deviceOutputInserts", juce::var()));

    if (p.name.isEmpty())
        p.name = makeDefault().name;

    p.sanitise();
    return p;
}

} // namespace gocue
