#include "app/UiTranslation.h"
#include "audio/PluginHost.h"

namespace gocue
{

namespace
{
    struct NameComparator
    {
        static int compareElements (const juce::PluginDescription& a, const juce::PluginDescription& b)
        {
            return a.name.compareIgnoreCase (b.name);
        }
    };
}

PluginHost::PluginHost()
{
    formatManager.addFormat (std::make_unique<juce::VST3PluginFormat>());   // VST2 joins in setVst2Enabled (true)
    knownPlugins.addChangeListener (this);
}

void PluginHost::setVst2Enabled (bool enabled)
{
    vst2Enabled = hasVst2Support() && enabled;

   #if JUCE_PLUGINHOST_VST
    // registered once, on the first switch-on: JUCE's list component scans every registered format, so an app
    // that never switches VST2 on (Enqueue) never sees it; the format stays registered after a switch-off (the
    // manager still filters it out of the menus and refuses instances)
    if (vst2Enabled && getFormat ("VST") == nullptr)
        formatManager.addFormat (std::make_unique<juce::VSTPluginFormat>());
   #endif
}

bool PluginHost::hasVst2Support() noexcept
{
   #if JUCE_PLUGINHOST_VST
    return true;
   #else
    return false;
   #endif
}

juce::AudioPluginFormat* PluginHost::getFormat (const juce::String& name) const
{
    for (auto* format : formatManager.getFormats())
        if (format->getName() == name)
            return format;

    return nullptr;
}

juce::String PluginHost::keyFor (const juce::PluginDescription& d)
{
    return d.pluginFormatName + "|" + juce::String (d.uniqueId) + "|" + d.name;   // the file may move; id + name stays
}

void PluginHost::setDisabledPlugins (const juce::StringArray& keys)
{
    disabledPlugins = keys;
    disabledPlugins.removeEmptyStrings();
    disabledPlugins.removeDuplicates (false);
}

void PluginHost::setPluginEnabled (const juce::PluginDescription& d, bool enabled)
{
    const auto key = keyFor (d);

    if (enabled)
        disabledPlugins.removeString (key);
    else if (! disabledPlugins.contains (key))
        disabledPlugins.add (key);
}

bool PluginHost::isPluginSwitchedOff (const juce::PluginDescription& d) const
{
    return disabledPlugins.contains (keyFor (d));
}

bool PluginHost::isPluginEnabled (const juce::PluginDescription& d) const
{
    if (d.pluginFormatName == "VST" && ! vst2Enabled)
        return false;

    return ! isPluginSwitchedOff (d);
}

PluginHost::~PluginHost()
{
    knownPlugins.removeChangeListener (this);
}

juce::AudioPluginFormat* PluginHost::getVST3Format() const
{
    for (auto* format : formatManager.getFormats())
        if (format->getName() == "VST3")
            return format;

    return nullptr;
}

bool PluginHost::isBundledPluginFile (const juce::String& file) const
{
    return bundledPluginsDirectory != juce::File() && juce::File::isAbsolutePath (file)
        && juce::File (file).isAChildOf (bundledPluginsDirectory);
}

juce::StringArray PluginHost::getBundledPluginKeys() const
{
    juce::StringArray keys;
    if (bundledPluginsDirectory != juce::File())
        for (const auto& type : knownPlugins.getTypes())
            if (isBundledPluginFile (type.fileOrIdentifier)) keys.add (keyFor (type));
    return keys;
}

bool PluginHost::isShadowedByBundledPlugin (const juce::PluginDescription& description, const juce::StringArray& keys) const
{
    return ! keys.isEmpty() && ! isBundledPluginFile (description.fileOrIdentifier) && keys.contains (keyFor (description));
}

juce::Array<juce::PluginDescription> PluginHost::getEffectTypes() const
{
    juce::Array<juce::PluginDescription> result;
    const auto bundledKeys = getBundledPluginKeys();

    for (const auto& type : knownPlugins.getTypes())
        if (! type.isInstrument && isPluginEnabled (type) && ! isShadowedByBundledPlugin (type, bundledKeys))
            result.add (type);

    NameComparator comparator;
    result.sort (comparator);
    return result;
}

juce::Array<juce::PluginDescription> PluginHost::getAllEffectTypes() const
{
    juce::Array<juce::PluginDescription> result;
    const auto bundledKeys = getBundledPluginKeys();

    for (const auto& type : knownPlugins.getTypes())
        if (! type.isInstrument && ! isShadowedByBundledPlugin (type, bundledKeys))
            result.add (type);

    NameComparator comparator;
    result.sort (comparator);
    return result;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description,
                                                                       double sampleRate, int blockSize, juce::String& error)
{
    if (safeMode)
    {
        error = sharedTrf ("안전 모드: 플러그인을 불러오지 않습니다 ({0})", description.name);
        return nullptr;
    }

    if (description.pluginFormatName == "VST" && ! vst2Enabled)
    {
        error = sharedTr ("VST2 사용이 꺼져 있습니다 (플러그인 관리에서 켜세요)");   // the callers name the plugin
        return nullptr;
    }

    std::unique_ptr<juce::AudioPluginInstance> instance;

    try
    {
        instance = formatManager.createPluginInstance (description, sampleRate, blockSize, error);
    }
    catch (...)
    {
        instance = nullptr;
        error = sharedTrf ("플러그인을 만드는 중 오류가 났습니다: {0}", description.name);
    }

    if (instance == nullptr && error.isEmpty())
        error = "Could not create plugin \"" + description.name + "\"";

    return instance;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const PluginSlotState& state,
                                                                       double sampleRate, int blockSize, juce::String& error)
{
    juce::PluginDescription description;
    bool haveDescription = false;

    if (state.descriptionXml.isNotEmpty())
        if (const auto xml = juce::parseXML (state.descriptionXml))
            haveDescription = description.loadFromXml (*xml);

    if (! haveDescription)
    {
        description.pluginFormatName = state.format;
        description.name = state.name;
        description.fileOrIdentifier = state.fileOrIdentifier;
        description.uniqueId = state.uniqueId;
    }

    bool choseBundled = false;
    if (bundledPluginsDirectory != juce::File())
        for (const auto& type : knownPlugins.getTypes())
            if (keyFor (type) == keyFor (description) && isBundledPluginFile (type.fileOrIdentifier))
            {
                description = type;
                choseBundled = true;
                break;
            }

    if (description.uniqueId != 0 && ! choseBundled)
    {
        // the known list wins (a plugin may have moved) - but by the saved file, or the saved name, before a bare id:
        // two plugins of one format may share an id (the list allows it when their files differ), and the first one
        // scanned must not quietly take a state saved for the other
        const auto known = knownPlugins.getTypes();
        const juce::PluginDescription* byFile = nullptr;
        const juce::PluginDescription* byName = nullptr;
        const juce::PluginDescription* byId = nullptr;
        int idMatches = 0;

        for (const auto& k : known)
        {
            if (k.uniqueId != description.uniqueId || k.pluginFormatName != description.pluginFormatName)
                continue;

            ++idMatches;
            byId = &k;

            if (byFile == nullptr && k.fileOrIdentifier.isNotEmpty() && k.fileOrIdentifier.equalsIgnoreCase (description.fileOrIdentifier))
                byFile = &k;
            else if (byName == nullptr && k.name.isNotEmpty() && k.name == description.name)
                byName = &k;
        }

        if (byFile != nullptr)
            description = *byFile;
        else if (byName != nullptr)
            description = *byName;
        else if (idMatches == 1)
            description = *byId;   // the one plugin with this id: it moved and was renamed
        else if (idMatches > 1)
        {
            error = sharedTrf ("같은 ID의 플러그인이 여러 개라 어느 것인지 알 수 없습니다 (저장된 파일과 이름이 목록에 없음): {0}", description.name);
            return nullptr;
        }
    }

    if (description.fileOrIdentifier.isEmpty())
    {
        error = sharedTr ("저장된 플러그인 파일 경로가 없습니다");
        return nullptr;
    }

    // a file-based plugin whose file is gone (another PC, uninstalled): say so instead of JUCE's "no compatible format"
    if (juce::File::isAbsolutePath (description.fileOrIdentifier) && ! juce::File (description.fileOrIdentifier).exists())
    {
        error = sharedTrf ("이 PC에 없는 플러그인 파일입니다: {0}", description.fileOrIdentifier);
        return nullptr;
    }

    return createInstance (description, sampleRate, blockSize, error);
}

PluginChain::Factory PluginHost::makeFactory (double sampleRate, int blockSize)
{
    return [this, sampleRate, blockSize] (const PluginSlotState& state, juce::String& error)
    {
        return createInstance (state, sampleRate, blockSize, error);
    };
}

void PluginHost::loadKnownPluginsFromXml (const juce::XmlElement* xml)
{
    if (xml != nullptr)
        knownPlugins.recreateFromXml (*xml);
}

std::unique_ptr<juce::XmlElement> PluginHost::createKnownPluginsXml() const
{
    return knownPlugins.createXml();
}

void PluginHost::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (onKnownPluginsChanged)
        onKnownPluginsChanged();
}

} // namespace gocue
