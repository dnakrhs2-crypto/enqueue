#include "ShortcutTestHarness.h"
#include "app/ShortcutDisplay.h"
#include "app/Commands.h"
#include "ui/KeyCapture.h"
#include "ui/ShortcutSettingsTab.h"
#include "ui/ShortcutRouter.h"
#include "model/Hotkeys.h"

namespace gocue::tests
{
namespace
{
using K = juce::KeyPress;
using M = juce::ModifierKeys;
using Harness = shortcut_test::Harness;
using Outcome = KeyCaptureSession::Outcome;
namespace Model = ShortcutSettingsModel;
juce::String xml (const juce::String& actions)
{
    return "<ENQUEUE_SHORTCUTS schemaVersion=\"1\" platform=\"windows\">" + actions + "</ENQUEUE_SHORTCUTS>";
}
}

class ShortcutUiTests : public juce::UnitTest
{
public:
    ShortcutUiTests() : juce::UnitTest ("Shortcut capture, settings and display", "Enqueue") {}
    void runTest() override
    {
        beginTest ("every supported vocabulary/modifier combination is a candidate, typed Esc only cancels");
        for (const auto& named : ShortcutKeyCodec::allowedKeys())
            for (const int mods : std::array<int, 8> { 0, M::ctrlModifier, M::altModifier, M::shiftModifier,
                                   M::ctrlModifier | M::altModifier, M::ctrlModifier | M::shiftModifier,
                                   M::altModifier | M::shiftModifier, M::ctrlModifier | M::altModifier | M::shiftModifier })
            {
                const K key (named.code, mods, 0);
                const auto result = KeyCaptureSession::input (key);
                if (named.code == K::escapeKey)
                {
                    expect (result.outcome == Outcome::cancelled);
                    expect (KeyCaptureSession::special (key).key == key);
                }
                else
                {
                    expect (result.outcome == Outcome::candidate, named.name);
                    expect (result.key == ShortcutKeyCodec::normalise (key), named.name);
                }
                expect (KeyCaptureSession::input (key, true).outcome == Outcome::repeated);
            }

        beginTest ("modifiers wait; lowercase normalises; unsupported and media events never become candidates");
        for (const int mod : { M::ctrlModifier, M::altModifier, M::shiftModifier })
            expect (KeyCaptureSession::input (K (0, mod, 0)).outcome == Outcome::waiting);
        expect (KeyCaptureSession::input (K ('a', M::ctrlModifier, 'a')).key == K ('A', M::ctrlModifier, 0));
        expect (KeyCaptureSession::input (K (K::F13Key, M::ctrlModifier | M::leftButtonModifier, 0)).key == K (K::F13Key, M::ctrlModifier, 0));
        for (const int code : { K::playKey, K::stopKey, K::fastForwardKey, K::rewindKey })
        {
            const auto result = KeyCaptureSession::input (K (code));
            expect (result.outcome == Outcome::rejected);
            expect (result.message.contains ("F13~F24"));
        }
        for (const int code : { 0x15, 0x19, K::F25Key })
            expect (KeyCaptureSession::input (K (code)).outcome == Outcome::rejected);
        expect (KeyCaptureSession::input (K ('A'), false, true).outcome == Outcome::rejected);
        expect (KeyCaptureSession::input (K (K::numberPad1)).key != KeyCaptureSession::input (K ('1')).key);

        beginTest ("shared widget keeps candidates uncommitted, rejects target policy, Esc cancels and stale owners cannot cancel another capture");
        {
            Harness h;
            KeyCapture widget (*h.service);
            int registered = 0;
            widget.onRegister = [&] (const K&) { ++registered; };
            widget.validate = [] (const K& key)
            {
                const bool rejected = key.getModifiers().isCtrlDown() || key.getModifiers().isAltDown() || Hotkeys::isReservedKey (key);
                return KeyCapture::Decision { ! rejected, rejected ? "cue policy" : "" };
            };
            widget.start ("cue");
            expect (h.service->isCapturing());
            for (const int code : { K::spaceKey, K::returnKey, K::tabKey, K::backspaceKey, K::deleteKey, K::F13Key })
            {
                h.service->deliverCaptureKey (K (code));
                expect (h.service->isCapturing());
                expectEquals (registered, 0);
            }
            h.service->deliverCaptureKey (K (K::escapeKey));
            expect (! widget.isCapturing());
            expect (! h.service->isCapturing());
            widget.start ("old");
            int replacement;
            h.service->beginCapture (&replacement);
            widget.cancel();
            expect (h.service->isCapturing());
            h.service->endCapture (&replacement);
        }

        beginTest ("router suppresses repeated candidates and all commands until held capture keys are released");
        {
            Harness h;
            std::set<int> down;
            ShortcutRouter::Callbacks callbacks;
            callbacks.keyDown = [&] (int code) { return down.count (code) != 0; };
            callbacks.nativeKeyDown = [] (int) { return false; };
            callbacks.applicationActive = [] { return true; };
            ShortcutRouter router (*h.service, h.manager, callbacks);
            std::vector<K> candidates;
            int token;
            h.service->beginCapture (&token, [&] (const K& key) { candidates.push_back (key); });
            for (const int code : { K::spaceKey, K::returnKey, K::tabKey, K::backspaceKey, K::deleteKey })
            {
                down.insert (code);
                expect (router.route (K (code), nullptr, {}, 1000));
                expect (router.route (K (code), nullptr, {}, 1001));
            }
            expectEquals (static_cast<int> (candidates.size()), 5);
            expect (h.target.invocations.empty());
            h.service->endCapture (&token);
            router.route (K (K::spaceKey), nullptr, {}, 1002);
            expect (h.target.invocations.empty());
            down.clear();
            router.pollKeyState();
            down.insert (K::spaceKey);
            router.route (K (K::spaceKey), nullptr, {}, 1003);
            expectEquals (static_cast<int> (std::count_if (h.target.invocations.begin(), h.target.invocations.end(), [] (const auto& call)
                { return call.commandID == CommandIDs::go && call.isKeyDown; })), 1);
        }

        beginTest ("registration waits for every held key; focus loss and show lock cancel the pending commit");
        {
            Harness h;
            bool held = true;
            KeyCapture widget (*h.service, [&] { return held; });
            int registered = 0;
            widget.onRegister = [&] (const K& key) { ++registered; h.service->addKey ("transport.go", key); };
            const auto clickRegister = [&]
            {
                for (auto* child : widget.getChildren())
                    if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getButtonText() == juce::String::fromUTF8 ("등록"))
                    { expect (button->isEnabled()); button->onClick(); }
            };
            widget.start ("GO");
            widget.receive (K (K::F13Key));
            clickRegister();
            widget.pollKeyRelease();
            expectEquals (registered, 0);
            expect (h.service->isCapturing());
            widget.receive (K (K::F14Key)); // cannot replace an already confirmed candidate while waiting
            held = false;
            widget.pollKeyRelease();
            expectEquals (registered, 1);
            expect (! h.service->isCapturing());
            expect (h.service->getKeys (CommandIDs::go).contains (K (K::F13Key)));
            expect (! h.service->getKeys (CommandIDs::go).contains (K (K::F14Key)));
            held = true;
            widget.start ("GO");
            widget.receive (K (K::F14Key));
            clickRegister();
            widget.focusLost (juce::Component::focusChangedDirectly);
            held = false;
            widget.pollKeyRelease();
            expectEquals (registered, 1);
            held = true;
            widget.start ("GO");
            widget.receive (K (K::F14Key));
            clickRegister();
            h.service->setEditingLocked (true);
            held = false;
            widget.pollKeyRelease();
            expectEquals (registered, 1);
            expect (! h.service->isCapturing());
        }

        beginTest ("preview uses the same owner during capture, including cue and fixed component restrictions");
        {
            Harness h;
            expect (h.service->setKeys ("transport.go", { K ('N') }).wasOk());
            int token;
            h.service->beginCapture (&token);
            expect (h.service->resolveKeyOwner (K ('N'), {}).kind == ShortcutKeyOwner::Kind::capture);
            const auto issues = Model::inspect (*h.service, "transport.go", K ('N'), { { "1 Intro", K ('N') } });
            expectEquals (issues.conflicts.size(), 1);
            expect (issues.conflicts[0].contains (juce::String::fromUTF8 ("현재 프로젝트 큐와 충돌")));
            expect (issues.limitations.joinIntoString (" ").contains (juce::String::fromUTF8 ("번호 편집이 우선")));
            expect (issues.commandOwner.isEmpty());
            const auto other = Model::inspect (*h.service, "transport.preview", K ('N'), {});
            expectEquals (other.commandOwner, juce::String ("transport.go"));
            const auto panic = Model::inspect (*h.service, "transport.panicAll", K ('A'), {});
            expect (panic.limitations.joinIntoString (" ").contains (juce::String::fromUTF8 ("입력창에서도 작동")));
            h.service->endCapture (&token);
        }

        beginTest ("learned keypad GO previews the same waveform zoom restrictions as runtime characters");
        {
            struct ZoomKey { int pad, character; const char* action; };
            for (const auto& zoom : { ZoomKey { K::numberPadSubtract, '-', "waveform.zoomOut" },
                                      ZoomKey { K::numberPadAdd, '+', "waveform.zoomIn" },
                                      ZoomKey { K::numberPadEquals, '=', "waveform.zoomIn" } })
            {
                Harness h;
                const auto learned = KeyCaptureSession::input (K (zoom.pad, M::ctrlModifier, 0));
                expect (learned.outcome == Outcome::candidate);
                const K runtime (zoom.character, M::ctrlModifier, 0);
                const auto preview = Model::inspect (*h.service, "transport.go", learned.key, {});
                const auto expected = Model::inspect (*h.service, "transport.go", runtime, {});
                const auto* fixed = ShortcutCatalog::get().find (zoom.action);
                expect (preview.limitations == expected.limitations);
                expect (preview.limitations.joinIntoString (" ").contains (fixed->name));
                expect (! fixed->matchesKey (K (zoom.pad, M::ctrlModifier | M::shiftModifier, 0)));
                expect (h.service->setKeys ("transport.go", { learned.key }).wasOk());
                ShortcutKeyContext context;
                context.focus = ShortcutScope::waveform;
                for (const auto key : { learned.key, runtime })
                {
                    const auto owner = h.service->resolveKeyOwner (key, context);
                    expect (owner.kind == ShortcutKeyOwner::Kind::fixedComponent);
                    expectEquals (owner.id, juce::String (zoom.action));
                }
                const auto limited = Model::filter (Model::rows (*h.service, {}), "GO", Model::Category::all, Model::Status::limited);
                expect (std::any_of (limited.begin(), limited.end(), [] (const auto& row) { return row.id == "transport.go"; }));
            }
        }

        beginTest ("comma and keypad separator GO previews agree with actual level matrix input");
        {
            for (const int code : { static_cast<int> (','), K::numberPadSeparator })
            {
                Harness h;
                const K binding (code, 0, 0), runtime (',', 0, ',');
                expect (h.service->setKeys ("transport.go", { binding }).wasOk());
                const auto preview = Model::inspect (*h.service, "transport.go", binding, {});
                const auto actual = Model::inspect (*h.service, "transport.go", runtime, {});
                expect (preview.limitations == actual.limitations);
                expect (! preview.limitations.joinIntoString (" ").contains (ShortcutCatalog::get().find ("levelMatrix.typeValue")->name));

                ShortcutKeyContext context;
                context.focus = ShortcutScope::levelMatrix;
                const auto previewOwner = h.service->resolveKeyOwner (binding, context, true);
                const auto runtimeOwner = h.service->resolveKeyOwner (runtime, context);
                expect (previewOwner.kind == ShortcutKeyOwner::Kind::command);
                expect (runtimeOwner.kind == previewOwner.kind);
                expectEquals (previewOwner.id, juce::String ("transport.go"));
                expectEquals (runtimeOwner.id, previewOwner.id);
                expect (! ShortcutKeyInput::isLevelMatrixValueKey (runtime));

                ShortcutRouter::Callbacks callbacks;
                callbacks.keyDown = [] (int) { return true; };
                callbacks.nativeKeyDown = [] (int) { return false; };
                callbacks.applicationActive = [] { return true; };
                ShortcutRouter router (*h.service, h.manager, callbacks);
                expect (router.route (runtime, nullptr, context, 1000));
                expectEquals (h.target.invocationCount (CommandIDs::go), 1);
            }
        }

        beginTest ("all 78 commands including unassigned appear; search combines name description and key tokens");
        {
            Harness h;
            expect (h.service->addKey ("transport.go", K (K::F13Key, M::ctrlModifier | M::altModifier, 0)).wasOk());
            const auto rows = Model::rows (*h.service, {});
            expectEquals (static_cast<int> (rows.size()), 78);
            for (const auto& search : { juce::String ("f13"), juce::String ("ctrl+alt"), juce::String ("GO F13"), ShortcutCatalog::get().find ("transport.go")->description })
            {
                const auto found = Model::filter (rows, search, Model::Category::all, Model::Status::all);
                expectEquals (static_cast<int> (found.size()), 1, search);
                if (! found.empty()) expectEquals (found[0].id, juce::String ("transport.go"));
            }
            expect (Model::filter (rows, "does-not-exist", Model::Category::all, Model::Status::all).empty());
        }

        beginTest ("categories partition commands and statuses distinguish changes, unassigned, conflicts, limits");
        {
            Harness h;
            h.service->setKeys ("transport.go", { K ('N') });
            h.service->setKeys ("transport.preview", {});
            const auto rows = Model::rows (*h.service, { { "cue", K ('N') } });
            int total = 0;
            for (const auto group : { Model::Category::playback, Model::Category::cue, Model::Category::edit,
                                        Model::Category::view, Model::Category::file, Model::Category::settings })
            {
                const auto filtered = Model::filter (rows, {}, group, Model::Status::all);
                expect (! filtered.empty());
                total += static_cast<int> (filtered.size());
                for (const auto& row : filtered) expect (row.category == group);
            }
            expectEquals (total, 78);
            expectEquals (static_cast<int> (Model::filter (rows, {}, Model::Category::all, Model::Status::changed).size()), 2);
            expectEquals (static_cast<int> (Model::filter (rows, {}, Model::Category::all, Model::Status::conflict).size()), 1);
            const auto empty = Model::filter (rows, {}, Model::Category::all, Model::Status::unassigned);
            expect (! empty.empty());
            for (const auto& row : empty) expect (row.keys.isEmpty());
            const auto limited = Model::filter (rows, {}, Model::Category::all, Model::Status::limited);
            expect (! limited.empty());
            for (const auto& row : limited) expect (! row.limitations.isEmpty());
            const auto sorted = Model::filter (rows, {}, Model::Category::all, Model::Status::all);
            for (size_t i = 1; i < sorted.size(); ++i)
                expect (sorted[i-1].category < sorted[i].category || (sorted[i-1].category == sorted[i].category
                         && sorted[i-1].name.compareNatural (sorted[i].name) <= 0));
        }

        beginTest ("multiple key labels fit with +N, selected key descriptions stay complete");
        {
            const ShortcutKeys keys { K (K::spaceKey), K (K::F13Key), K (K::F14Key) };
            const auto measure = [] (const juce::String& text) { return text.length(); };
            expectEquals (Model::compactKeys (keys, 30, measure), juce::String ("Space, F13, F14"));
            expectEquals (Model::compactKeys (keys, 9, measure), juce::String::fromUTF8 ("Space +2개"));
            expectEquals (Model::compactKeys (keys, 3, measure), juce::String::fromUTF8 ("+3개"));
            expectEquals (Model::compactKeys ({}, 30, measure), juce::String::fromUTF8 ("미지정"));
            expectEquals (ShortcutDisplay::key (K (K::F24Key, M::ctrlModifier | M::altModifier | M::shiftModifier, 0)), juce::String ("Ctrl+Alt+Shift+F24"));
            expectEquals (ShortcutDisplay::key (K (K::numberPad1)), juce::String ("NumPad1"));
        }

        beginTest ("current display changes immediately; duplicates do not write; save failure preserves labels and mappings");
        {
            Harness h;
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String ("Space"));
            expect (h.service->addKey ("transport.go", K (K::spaceKey)).wasOk());
            expectEquals (h.saves, 0);
            expect (h.service->addKey ("transport.go", K (K::F13Key)).wasOk());
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String ("Space, F13"));
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go, 1), juce::String::fromUTF8 ("Space +1개"));
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go, 1, false), juce::String ("Space"));
            h.failSave = true;
            expect (h.service->setKeys ("transport.go", { K (K::F14Key) }).failed());
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String ("Space, F13"));
            h.failSave = false;
            expect (h.service->setKeys ("transport.go", {}).wasOk());
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String::fromUTF8 ("미지정"));
        }

        beginTest ("import preview compares replacement mapping without applying or writing, warns on last panic removal");
        {
            Harness h;
            const auto source = xml ("<ACTION id=\"transport.go\"><KEY key=\"F13\"/></ACTION><ACTION id=\"transport.panicAll\"/>");
            const auto preview = Model::previewImport (*h.service, source, { { "cue F13", K (K::F13Key) } });
            expect (preview.applicable && preview.removesLastPanic);
            expectEquals (preview.changes.size(), 2);
            expect (preview.changes.joinIntoString (" ").contains (juce::String::fromUTF8 ("Space → F13")));
            expect (preview.conflicts.joinIntoString (" ").contains ("cue F13"));
            expect (preview.unassigned.contains (ShortcutCatalog::get().find ("transport.panicAll")->name));
            expectEquals (h.saves, 0);
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String ("Space"));
            expect (h.service->importProfile (source).wasOk());
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String ("F13"));
            expect (Model::previewImport (*h.service, h.service->exportProfile(), {}).changes.isEmpty());
        }

        beginTest ("preview reports all explicit conflicts and malformed inputs never partially apply");
        {
            Harness h;
            const auto before = h.service->exportProfile();
            const auto collision = xml ("<ACTION id=\"transport.go\"><KEY key=\"F13\"/></ACTION>"
                                       "<ACTION id=\"transport.preview\"><KEY key=\"F13\"/></ACTION>"
                                       "<ACTION id=\"transport.panicAll\"><KEY key=\"F13\"/></ACTION>");
            const auto preview = Model::previewImport (*h.service, collision, {});
            expect (! preview.applicable);
            expectEquals (preview.conflicts.size(), 3);
            expect (! preview.error.isEmpty());
            for (const auto& bad : { collision, juce::String ("broken"), xml ("<ACTION id=\"transport.go\"><KEY key=\"F99\"/></ACTION>") })
            {
                expect (! Model::previewImport (*h.service, bad, {}).applicable);
                expect (h.service->importProfile (bad).failed());
                expectEquals (h.service->exportProfile(), before);
            }
            expectEquals (h.saves, 0);
        }

        beginTest ("PC tab does not dirty project; show-mode lock cancels capture and rejects every mutation");
        {
            Harness h;
            ProjectDocument document;
            document.markClean();
            ShortcutSettingsTab tab (*h.service, document);
            tab.setSize (624, 514);
            int token;
            h.service->beginCapture (&token);
            h.service->setEditingLocked (true);
            expect (! h.service->isCapturing());
            expect (h.service->addKey ("transport.go", K (K::F13Key)).failed());
            expect (h.service->replaceKey ("transport.go", 0, K (K::F13Key)).failed());
            expect (h.service->removeKey ("transport.go", 0).failed());
            expect (h.service->setKeys ("transport.go", {}).failed());
            expect (h.service->restoreCommandDefaults ("transport.go").failed());
            expect (h.service->restoreAllDefaults().failed());
            expect (h.service->importProfile (h.service->exportProfile()).failed());
            expectEquals (h.saves, 0);
            h.service->setEditingLocked (false);
            expect (h.service->addKey ("transport.go", K (K::F13Key)).wasOk());
            expect (! document.isDirty());
        }

        beginTest ("confirmed default restoration moves conflicting keys atomically and restores inheritance");
        {
            Harness h;
            expect (h.service->setKeys ("transport.go", { K (K::F13Key) }).wasOk());
            expect (h.service->setKeys ("transport.preview", { K (K::spaceKey) }, ShortcutService::ConflictPolicy::move).wasOk());
            const auto saves = h.saves;
            h.failSave = true;
            expect (h.service->restoreCommandDefaults ("transport.go", ShortcutService::ConflictPolicy::move).failed());
            expect (h.service->getKeys ("transport.preview").contains (K (K::spaceKey)));
            h.failSave = false;
            expect (h.service->restoreCommandDefaults ("transport.go", ShortcutService::ConflictPolicy::move).wasOk());
            expectEquals (h.saves, saves + 2);
            expect (h.service->getKeys ("transport.preview").isEmpty());
            expect (h.service->getProfile().overrides.count ("transport.go") == 0);
            expectEquals (ShortcutDisplay::currentKeys (h.service.get(), CommandIDs::go), juce::String ("Space"));
        }

        beginTest ("moving all aliases of the last physical panic key requires the same removal warning");
        {
            Harness h;
            expect (h.service->setKeys ("transport.panicAll", { K (K::numberPadSubtract), K ('-') }).wasOk());
            expect (Model::moveRemovesLastPanic (*h.service, "transport.go", { K ('-') }));
            expect (! Model::moveRemovesLastPanic (*h.service, "transport.panicAll", { K ('-') }));
            expect (h.service->addKey ("transport.panicAll", K (K::F12Key)).wasOk());
            expect (! Model::moveRemovesLastPanic (*h.service, "transport.go", { K ('-') }));
            expect (Model::moveRemovesLastPanic (*h.service, "transport.go", { K ('-'), K (K::F12Key) }));
            expect (h.service->setKeys ("transport.panicAll", {}).wasOk());
            expect (! Model::moveRemovesLastPanic (*h.service, "transport.go", { K ('-') }));
        }

        beginTest ("logical layout reserves footer, selection and four keyboard/MIDI rows at each supported scale");
        {
            Harness h;
            ProjectDocument document;
            ShortcutSettingsTab tab (*h.service, document);
            tab.setSize (624, 514);
            for (const float scale : { 1.0f, 1.1f, 1.25f, 1.5f })
            {
                tab.setTransform (juce::AffineTransform::scale (scale));
                int footers = 0;
                for (auto* child : tab.getChildren())
                {
                    expect (tab.getLocalBounds().contains (child->getBounds()));
                    if (auto* button = dynamic_cast<juce::TextButton*> (child); button != nullptr && button->getY() > 470)
                        ++footers;
                    if (auto* table = dynamic_cast<juce::ListBox*> (child))
                    {
                        expectEquals (table->getRowHeight(), 48);
                        expectEquals ((table->getHeight() - 2) / table->getRowHeight(), 4);
                    }
                }
                expectEquals (footers, 3);
            }
        }

        beginTest ("learning survives row-button reuse and a filtered-out stale row cannot start a capture");
        {
            Harness h;
            ProjectDocument document;
            ShortcutSettingsTab tab (*h.service, document);
            tab.setSize (624, 514);
            juce::ListBox* list = nullptr;
            juce::TextEditor* search = nullptr;
            for (auto* child : tab.getChildren())
            {
                if (auto* found = dynamic_cast<juce::ListBox*> (child)) list = found;
                if (auto* found = dynamic_cast<juce::TextEditor*> (child)) search = found;
            }
            expect (list != nullptr && search != nullptr);
            if (list != nullptr && search != nullptr)
            {
                auto* row = list->getComponentForRowNumber (1);
                expect (row != nullptr);
                if (row != nullptr)
                    for (auto* child : row->getChildren())
                        if (auto* button = dynamic_cast<juce::TextButton*> (child))
                        {
                            auto stale = button->onClick;
                            button->onClick();
                            expect (h.service->isCapturing());
                            tab.cancelCapture();
                            expect (! h.service->isCapturing());
                            for (auto* control : tab.getChildren())
                                if (auto* viewport = dynamic_cast<juce::Viewport*> (control)) expect (viewport->isVisible());
                            search->setText ("no-such-action", false);
                            search->onTextChange();
                            stale();
                            expect (! h.service->isCapturing());
                            break; // filtering may have deleted the row and its button
                        }
            }
        }
    }
};
static ShortcutUiTests shortcutUiTests;
}
