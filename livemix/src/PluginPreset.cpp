#include "Localisation.h"
#include "PluginPreset.h"

#include "model/ProjectSerializer.h"
#include "model/SafeFileWrite.h"

#include <algorithm>
#include <cmath>

namespace gocue::livemix
{

namespace
{
    juce::String k (const char* utf8) { return juce::String::fromUTF8 (utf8); }
}

juce::String PluginPreset::toJson() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("app", "LiveMix");
    root->setProperty ("kind", "pluginPreset");
    root->setProperty ("version", currentVersion);
    root->setProperty ("name", name);

    std::vector<PluginSlotState> capped = plugins;

    if ((int) capped.size() > maxPlugins)
        capped.resize ((size_t) maxPlugins);

    root->setProperty ("plugins", ProjectSerializer::pluginSlotsToVar (capped));
    return juce::JSON::toString (juce::var (root), false);
}

juce::Result PluginPreset::fromJson (const juce::String& json, PluginPreset& out)
{
    const auto root = juce::JSON::parse (json);

    if (root.getDynamicObject() == nullptr)
        return juce::Result::fail (tr ("플러그인 프리셋 파일이 아닙니다 (JSON이 아닙니다)"));

    if (root.getProperty ("app", "").toString() != "LiveMix" || root.getProperty ("kind", "").toString() != "pluginPreset")
        return juce::Result::fail (tr ("LiveMix 플러그인 프리셋 파일이 아닙니다"));

    const auto versionVar = root.getProperty ("version", juce::var());

    if (versionVar.isVoid())
        return juce::Result::fail (tr ("프리셋 파일에 버전 표시가 없습니다"));

    if (! (versionVar.isInt() || versionVar.isInt64() || versionVar.isDouble()))
        return juce::Result::fail (tr ("프리셋 파일의 버전 표시가 잘못됐습니다"));

    const double versionNumber = (double) versionVar;

    if (! std::isfinite (versionNumber) || versionNumber != std::floor (versionNumber) || versionNumber < 1.0 || versionNumber > 1.0e6)
        return juce::Result::fail (trf ("프리셋 파일의 버전 표시가 잘못됐습니다: {0}", versionVar.toString()));

    const int version = (int) versionNumber;

    if (version > currentVersion)
        return juce::Result::fail (trf ("더 새로운 LiveMix로 저장한 프리셋입니다 (파일 버전 {0}). LiveMix를 업데이트하세요.", juce::String (version)));

    if (ProjectSerializer::hasTrailingJsonData (json))
        return juce::Result::fail (tr ("프리셋 파일 뒤에 알 수 없는 내용이 붙어 있습니다"));

    // the plugin list: an array of objects, each with a decodable state - a wrong shape is refused, not quietly
    // shortened to a preset that loads as something else than what was saved
    const auto pluginsVar = root.getProperty ("plugins", juce::var());
    const auto* pluginsArray = pluginsVar.getArray();

    if (pluginsArray == nullptr)
        return juce::Result::fail (tr ("프리셋 파일에 플러그인 목록이 없습니다"));

    for (const auto& item : *pluginsArray)
    {
        if (item.getDynamicObject() == nullptr)
            return juce::Result::fail (tr ("프리셋 파일의 플러그인 항목이 잘못됐습니다"));

        const auto state = item.getProperty ("state", "").toString();

        if (state.isNotEmpty())
        {
            juce::MemoryOutputStream decoded;

            if (! juce::Base64::convertFromBase64 (decoded, state))
                return juce::Result::fail (trf ("프리셋 파일의 플러그인 설정 데이터를 읽을 수 없습니다: {0}", item.getProperty ("name", "").toString()));
        }
    }

    PluginPreset p;
    p.name = root.getProperty ("name", "").toString().trim();
    p.plugins = ProjectSerializer::pluginSlotsFromVar (pluginsVar);

    if ((int) p.plugins.size() > maxPlugins)
        p.plugins.resize ((size_t) maxPlugins);

    out = std::move (p);
    return juce::Result::ok();
}

juce::Result PluginPreset::save (const juce::File& target) const
{
    const auto json = toJson();
    const auto bytes = (juce::int64) json.getNumBytesAsUTF8();

    if (bytes > maxFileBytes)
        return juce::Result::fail (trf ("프리셋이 너무 커서 저장하지 않았습니다 ({0})", juce::File::descriptionOfSizeInBytes (bytes)));

    target.getParentDirectory().createDirectory();
    return SafeFileWrite::writeTextVerified (target, json, [] (const juce::String& readBack) -> juce::Result
    {
        PluginPreset check;
        return fromJson (readBack, check);
    });
}

juce::Result PluginPreset::load (const juce::File& file, PluginPreset& out)
{
    if (! file.existsAsFile())
        return juce::Result::fail (trf ("파일이 없습니다: {0}", file.getFullPathName()));

    if (file.getSize() > maxFileBytes)
        return juce::Result::fail (trf ("프리셋 파일이 너무 큽니다 ({0}): 프리셋 파일이 아닙니다", juce::File::descriptionOfSizeInBytes (file.getSize())));

    const auto result = fromJson (file.loadFileAsString(), out);

    if (result.wasOk())
    {
        out.file = file;

        if (out.name.isEmpty())
            out.name = file.getFileNameWithoutExtension();   // an unnamed preset goes by its file
    }

    return result;
}

juce::File PluginPreset::defaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("LiveMix").getChildFile (k ("플러그인 프리셋"));
}

juce::String PluginPreset::fileNameFor (const juce::String& name)
{
    auto legal = juce::File::createLegalFileName (name.trim());

    if (legal.isEmpty() || legal.startsWithChar ('.'))
        legal = k ("프리셋") + legal;

    return legal + fileExtension;
}

juce::File PluginPreset::fileFor (const juce::String& name, const juce::File& folder)
{
    return folder.getChildFile (fileNameFor (name));
}

bool PluginPreset::isPresetFileName (const juce::String& fileName)
{
    return fileName.endsWithIgnoreCase (fileExtension);
}

std::vector<PluginPreset> PluginPreset::listFolder (const juce::File& folder, juce::StringArray* problems)
{
    std::vector<PluginPreset> found;

    for (const auto& file : folder.findChildFiles (juce::File::findFiles, false, "*" + juce::String (fileExtension)))
    {
        PluginPreset p;

        if (const auto result = load (file, p); result.failed())
        {
            if (problems != nullptr)
                problems->add (file.getFileName() + ": " + result.getErrorMessage());

            continue;
        }

        found.push_back (std::move (p));
    }

    std::sort (found.begin(), found.end(), [] (const PluginPreset& a, const PluginPreset& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    return found;
}

juce::String PluginPreset::summary() const
{
    juce::StringArray parts;

    for (size_t i = 0; i < plugins.size(); ++i)
        parts.add (juce::String ((int) i + 1) + ". " + plugins[i].name);

    return parts.joinIntoString (juce::String::fromUTF8 (" \xE2\x86\x92 "));   // →
}

} // namespace gocue::livemix
