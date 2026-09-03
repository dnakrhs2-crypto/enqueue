#include "app/ProjectDocument.h"

namespace gocue
{

ProjectDocument::ProjectDocument()
{
    clock = [] { return juce::Time::getMillisecondCounterHiRes(); };
    cues.addListener (this);

    auto main = std::make_unique<Container>();
    main->info.id = juce::Uuid();
    main->info.name = juce::String::fromUTF8 ("\xEB\xA9\x94\xEC\x9D\xB8 \xED\x81\x90 \xEB\xA6\xAC\xEC\x8A\xA4\xED\x8A\xB8");   // 메인 큐 리스트
    main->list.addListener (this);   // edits of an inactive list (control cues arm / retarget) dirty the document too
    containers.push_back (std::move (main));
}

std::vector<juce::Uuid> ProjectDocument::cueIdsOf (int container) const
{
    std::vector<juce::Uuid> ids;

    if (container < 0 || container >= (int) containers.size())
        return ids;

    const auto& list = container == active ? cues : containers[(size_t) container]->list;

    for (const auto& c : list.getAll())
        ids.push_back (c.id);

    return ids;
}

bool ProjectDocument::isNumberTaken (const juce::String& number, const juce::Uuid& exceptId) const
{
    if (number.isEmpty())
        return false;

    if (cues.isNumberTaken (number, exceptId))
        return true;

    for (const auto& c : containers)
        if (c->list.isNumberTaken (number, exceptId))
            return true;

    return false;
}

bool ProjectDocument::isHotkeyTaken (const juce::String& hotkey, const juce::Uuid& exceptId) const
{
    if (hotkey.isEmpty())
        return false;

    bool taken = false;
    const_cast<ProjectDocument*> (this)->forEachList ([&] (CueList& list)
    {
        for (const auto& c : list.getAll())
            if (c.hotkey == hotkey && c.id != exceptId)
                taken = true;
    });
    return taken;
}

//==============================================================================
ProjectDocument::ContainerInfo ProjectDocument::getContainerInfo (int index) const
{
    if (index < 0 || index >= (int) containers.size())
        return {};

    return containers[(size_t) index]->info;
}

bool ProjectDocument::isActiveCart() const noexcept
{
    return active >= 0 && active < (int) containers.size() && containers[(size_t) active]->info.isCart;
}

void ProjectDocument::swapActive (int index)
{
    if (onBeforeContainerSwitch)
        onBeforeContainerSwitch();   // pending cell / field edits belong to the list that is leaving

    const juce::ScopedValueSetter<bool> guard (switching, true);

    if (active >= 0 && active < (int) containers.size())
    {
        auto& old = containers[(size_t) active]->list;
        old.replaceAllWithCursors (cues.getAll(), cues.getSelectedIndices(), cues.getSelectedIndex(), cues.getPlayheadIndex());
    }

    active = juce::jlimit (0, (int) containers.size() - 1, index);
    auto& next = containers[(size_t) active]->list;
    // one round of notifications with the final cursors (no interim "playhead on row 0" that would auto-load)
    cues.replaceAllWithCursors (next.getAll(), next.getSelectedIndices(), next.getSelectedIndex(), next.getPlayheadIndex());
    next.clear();   // one copy of the cues: they live in 'cues' now
}

void ProjectDocument::setActiveContainer (int index)
{
    if (index < 0 || index >= (int) containers.size() || index == active)
        return;

    swapActive (index);
    notifyContainers();
}

int ProjectDocument::addContainer (const juce::String& name, bool isCart)
{
    auto c = std::make_unique<Container>();
    c->info.id = juce::Uuid();
    c->info.name = name.isNotEmpty() ? name : juce::String::fromUTF8 (isCart ? "\xEC\xB9\xB4\xED\x8A\xB8" : "\xED\x81\x90 \xEB\xA6\xAC\xEC\x8A\xA4\xED\x8A\xB8");
    c->info.isCart = isCart;
    c->list.addListener (this);
    containers.push_back (std::move (c));
    markDirty();
    notifyContainers();
    return (int) containers.size() - 1;
}

bool ProjectDocument::removeContainer (int index)
{
    if (index < 0 || index >= (int) containers.size() || containers.size() <= 1)
        return false;

    if (index == active)
        swapActive (index > 0 ? index - 1 : 1);   // the neighbour takes over first

    containers.erase (containers.begin() + index);

    if (active > index)
        --active;

    active = juce::jlimit (0, (int) containers.size() - 1, active);
    markDirty();
    notifyContainers();
    return true;
}

void ProjectDocument::renameContainer (int index, const juce::String& name)
{
    if (index < 0 || index >= (int) containers.size() || name.isEmpty())
        return;

    containers[(size_t) index]->info.name = name;
    markDirty();
    notifyContainers();
}

void ProjectDocument::setContainerCart (int index, bool isCart, int rows, int cols)
{
    if (index < 0 || index >= (int) containers.size())
        return;

    auto& info = containers[(size_t) index]->info;
    info.isCart = isCart;
    info.cartRows = juce::jlimit (1, CueContainer::maxGrid, rows);
    info.cartCols = juce::jlimit (1, CueContainer::maxGrid, cols);
    markDirty();
    notifyContainers();
}

CueList* ProjectDocument::listContaining (const juce::Uuid& id, int* indexOut) noexcept
{
    if (const int i = cues.indexOf (id); i >= 0)
    {
        if (indexOut != nullptr)
            *indexOut = i;

        return &cues;
    }

    for (auto& c : containers)
        if (const int i = c->list.indexOf (id); i >= 0)
        {
            if (indexOut != nullptr)
                *indexOut = i;

            return &c->list;
        }

    return nullptr;
}

const CueList* ProjectDocument::listContaining (const juce::Uuid& id, int* indexOut) const noexcept
{
    return const_cast<ProjectDocument*> (this)->listContaining (id, indexOut);
}

int ProjectDocument::containerOf (const juce::Uuid& id) const noexcept
{
    if (cues.indexOf (id) >= 0)
        return active;

    for (int i = 0; i < (int) containers.size(); ++i)
        if (containers[(size_t) i]->list.indexOf (id) >= 0)
            return i;

    return -1;
}

const Cue* ProjectDocument::findCueAnywhere (const juce::Uuid& id) const noexcept
{
    int index = -1;

    if (const auto* list = listContaining (id, &index))
        return &list->get (index);

    return nullptr;
}

void ProjectDocument::forEachList (const std::function<void (CueList&)>& fn)
{
    fn (cues);

    for (auto& c : containers)
        if (! c->list.isEmpty())
            fn (c->list);
}

void ProjectDocument::notifyContainers()
{
    listeners.call ([] (Listener& l) { l.containersChanged(); });
}

ProjectDocument::~ProjectDocument()
{
    cues.removeListener (this);
}

juce::String ProjectDocument::getDisplayName() const
{
    if (hasFile())
        return file.getFileNameWithoutExtension();

    return juce::String::fromUTF8 ("제목 없음");
}

juce::String ProjectDocument::getWindowTitle() const
{
    juce::String title ("Enqueue - ");
    title << getDisplayName();

    if (dirty)
        title << "*";

    return title;
}

void ProjectDocument::newProject()
{
    history.clear();
    containers.clear();
    auto main = std::make_unique<Container>();
    main->info.id = juce::Uuid();
    main->info.name = juce::String::fromUTF8 ("\xEB\xA9\x94\xEC\x9D\xB8 \xED\x81\x90 \xEB\xA6\xAC\xEC\x8A\xA4\xED\x8A\xB8");
    main->list.addListener (this);
    containers.push_back (std::move (main));
    active = 0;
    cues.clear();
    masterPlugins.clear();
    patches.clear();
    patches.push_back (AudioPatch::makeDefault());
    settings = WorkspaceSettings();
    file = juce::File();
    markClean();
    notifyContainers();
}

void ProjectDocument::setCueNumber (const juce::Uuid& id, const juce::String& number)
{
    perform (juce::String (juce::CharPointer_UTF8 ("\xEB\xB2\x88\xED\x98\xB8")), [this, id, number]   // "번호"
    {
        int index = -1;
        auto* list = listContaining (id, &index);

        if (list == nullptr)
            return;

        list->update (index, [number] (Cue& c) { c.number = number; });
        list->placeByNumber (index);   // the row follows its number
    });
}

void ProjectDocument::setSettings (const WorkspaceSettings& newSettings)
{
    settings = newSettings;
    settings.sanitise();
    dirty = true;
    notify();
}

void ProjectDocument::setPatches (std::vector<AudioPatch> newPatches)
{
    patches = std::move (newPatches);

    for (auto& p : patches)
        p.sanitise();

    if (patches.empty())
        patches.push_back (AudioPatch::makeDefault());

    dirty = true;
    notify();

    if (onPatchesChanged)
        onPatchesChanged();
}

const AudioPatch* ProjectDocument::findPatch (const juce::Uuid& id) const noexcept
{
    for (const auto& p : patches)
        if (p.id == id)
            return &p;

    return nullptr;
}

const AudioPatch& ProjectDocument::patchForCue (const Cue& cue) const noexcept
{
    if (const auto* p = findPatch (cue.patchId))
        return *p;

    static const AudioPatch fallback = AudioPatch::makeDefault();
    return patches.empty() ? fallback : patches.front();
}

juce::Result ProjectDocument::parse (const juce::File& projectFile, Project& out, juce::StringArray* warnings)
{
    return ProjectSerializer::load (projectFile, out, warnings);
}

void ProjectDocument::adopt (Project project, const juce::File& projectFile)
{
    history.clear();
    project.ensureMainList();
    containers.clear();

    for (auto& list : project.lists)
    {
        auto c = std::make_unique<Container>();
        c->info.id = list.id;
        c->info.name = list.name;
        c->info.isCart = list.isCart;
        c->info.cartRows = list.cartRows;
        c->info.cartCols = list.cartCols;
        c->list.replaceAll (std::move (list.cues));
        c->list.addListener (this);
        containers.push_back (std::move (c));
    }

    active = juce::jlimit (0, (int) containers.size() - 1, project.activeList);

    {
        const juce::ScopedValueSetter<bool> guard (switching, true);
        cues.replaceAll (containers[(size_t) active]->list.getAll());
        containers[(size_t) active]->list.clear();
    }

    masterPlugins = std::move (project.masterPlugins);
    project.ensureDefaultPatch();
    patches = std::move (project.patches);
    settings = project.settings;
    settings.sanitise();
    file = projectFile;
    markClean();
    notifyContainers();
}

juce::Result ProjectDocument::load (const juce::File& projectFile, juce::StringArray* warnings)
{
    Project project;
    const auto result = parse (projectFile, project, warnings);

    if (result.failed())
        return result;

    adopt (std::move (project), projectFile);
    return juce::Result::ok();
}

juce::Result ProjectDocument::save (const juce::File& projectFile, const std::function<void (Project&)>& decorate)
{
    auto project = toProject();

    if (decorate)
        decorate (project);

    const auto result = ProjectSerializer::save (project, projectFile);

    if (result.failed())
        return result;

    // Keep the in-memory snapshot in sync with what was written (plugin states).
    masterPlugins = project.masterPlugins;

    for (const auto& list : project.lists)
        for (const auto& c : list.cues)
        {
            int index = -1;

            if (auto* live = listContaining (c.id, &index))
                live->setPluginStatesQuietly (index, c.plugins);
        }

    file = projectFile;
    markClean();
    return juce::Result::ok();
}

void ProjectDocument::markDirty()
{
    if (dirty)
        return;

    dirty = true;
    notify();
}

void ProjectDocument::markClean()
{
    dirty = false;
    notify();
}

Project ProjectDocument::toProject() const
{
    Project project;
    project.name = getDisplayName();

    for (int i = 0; i < (int) containers.size(); ++i)
    {
        const auto& c = *containers[(size_t) i];
        CueContainer list;
        list.id = c.info.id;
        list.name = c.info.name;
        list.isCart = c.info.isCart;
        list.cartRows = c.info.cartRows;
        list.cartCols = c.info.cartCols;
        list.cues = i == active ? cues.getAll() : c.list.getAll();
        project.lists.push_back (std::move (list));
    }

    project.ensureMainList();
    project.activeList = active;
    project.masterPlugins = masterPlugins;
    project.patches = patches;
    project.settings = settings;
    return project;
}

//==============================================================================
ProjectSnapshot ProjectDocument::makeSnapshot (bool capturePluginStates) const
{
    ProjectSnapshot snapshot;
    snapshot.project = toProject();

    if (const auto* selected = cues.getSelected())
        snapshot.selectedId = selected->id;

    for (int i : cues.getSelectedIndices())
        if (cues.isValidIndex (i))
            snapshot.selectedIds.push_back (cues.get (i).id);

    if (const auto* playhead = cues.getPlayhead())
        snapshot.playheadId = playhead->id;

    if (capturePluginStates && snapshotDecorator)
    {
        snapshotDecorator (snapshot.project);
        snapshot.pluginStatesCaptured = true;
    }

    return snapshot;
}

void ProjectDocument::restoreSnapshot (const ProjectSnapshot& snapshot)
{
    // settings are deliberately left alone: they are not part of the undo history
    const bool containersDiffer = (int) snapshot.project.lists.size() != (int) containers.size() || snapshot.project.activeList != active
                                  || [&]
                                     {
                                         for (size_t i = 0; i < containers.size(); ++i)
                                         {
                                             const auto& a = containers[i]->info;
                                             const auto& b = snapshot.project.lists[i];

                                             if (a.id != b.id || a.name != b.name || a.isCart != b.isCart || a.cartRows != b.cartRows || a.cartCols != b.cartCols)
                                                 return true;
                                         }

                                         return false;
                                     }();

    {
        const juce::ScopedValueSetter<bool> guard (switching, true);
        containers.clear();

        for (const auto& list : snapshot.project.lists)
        {
            auto c = std::make_unique<Container>();
            c->info.id = list.id;
            c->info.name = list.name;
            c->info.isCart = list.isCart;
            c->info.cartRows = list.cartRows;
            c->info.cartCols = list.cartCols;
            c->list.replaceAll (list.cues);
            c->list.addListener (this);
            containers.push_back (std::move (c));
        }

        if (containers.empty())
        {
            auto main = std::make_unique<Container>();
            main->info.id = juce::Uuid();
            main->info.name = juce::String::fromUTF8 ("\xEB\xA9\x94\xEC\x9D\xB8 \xED\x81\x90 \xEB\xA6\xAC\xEC\x8A\xA4\xED\x8A\xB8");
            main->list.addListener (this);
            containers.push_back (std::move (main));
        }

        active = juce::jlimit (0, (int) containers.size() - 1, snapshot.project.activeList);
        cues.replaceAll (containers[(size_t) active]->list.getAll());
        containers[(size_t) active]->list.clear();
    }

    masterPlugins = snapshot.project.masterPlugins;

    // selection (all of it), then the playhead: with the lock off they may differ
    std::vector<int> indices;

    for (const auto& id : snapshot.selectedIds)
        if (const int index = cues.indexOf (id); index >= 0)
            indices.push_back (index);

    const int primary = snapshot.selectedId.isNull() ? -1 : cues.indexOf (snapshot.selectedId);
    const int playhead = snapshot.playheadId.isNull() ? -1 : cues.indexOf (snapshot.playheadId);

    if (indices.empty() && primary >= 0)
        indices.push_back (primary);

    // both cursors exactly as they were: a multi-selection survives the playhead lock
    cues.restoreCursors (indices, primary, playhead >= 0 ? playhead : cues.getPlayheadIndex());

    dirty = true;
    notify();

    if (containersDiffer)
        notifyContainers();

    if (onSnapshotRestored)
        onSnapshotRestored (snapshot);
}

void ProjectDocument::perform (const juce::String& name, const std::function<void()>& edit, const EditOptions& options)
{
    history.push (makeSnapshot (options.capturePluginStates), name, options.coalesceKey, clock ? clock() : 0.0);

    if (edit)
        edit();

    notify();
}

bool ProjectDocument::undo()
{
    auto restored = history.undo ([this] (bool capture) { return makeSnapshot (capture); });

    if (! restored.has_value())
        return false;

    restoreSnapshot (*restored);
    return true;
}

bool ProjectDocument::redo()
{
    auto restored = history.redo ([this] (bool capture) { return makeSnapshot (capture); });

    if (! restored.has_value())
        return false;

    restoreSnapshot (*restored);
    return true;
}

void ProjectDocument::notify()
{
    listeners.call ([] (Listener& l) { l.documentStateChanged(); });
}

} // namespace gocue
