# LiveMix screen languages (Korean, English, Japanese)

LiveMix 0.20.0 shows its screens in Korean, English or Japanese. This note is for whoever adds or changes screen text.

## How a language is chosen

`chooseLanguage` (`livemix/src/Localisation.cpp`), in this order:

1. the environment variable `LIVEMIX_LANGUAGE` (`ko`, `en`, `ja`) - for tests and checks only, never saved;
2. the saved setting `language` (Settings > Language);
3. a settings file that already has `lastRunVersion` (someone who used LiveMix before 0.20.0): Korean;
4. a new user: the Windows display language (`GetUserDefaultUILanguage`) - Korean, Japanese, anything else English.

What 3 and 4 decide is saved at once, so it does not change later by itself.

Picking a language in Settings saves it immediately and, when it differs from the language that runs, asks in the
picked language whether to restart now. Text is read once at start; nothing is retranslated while LiveMix runs.
"Restart now" goes through the ordinary quit path (the save question included) and starts the new process with
`--restart-after <pid>` (`livemix/src/AppRestart.cpp`), which waits for the old one to end.

## Adding or changing screen text

- The Korean text is the key. Write `tr ("한국어")`, `trf ("{0}개", n)` for values, `trIn (...)` for another language
  than the running one. Code shared with Enqueue (`src/`) uses `sharedTr` / `formatUiText` from
  `src/app/UiTranslation.h`; only LiveMix installs the hook, Enqueue shows the Korean as before.
- Add a row to `livemix/i18n/strings.json` (`ko`, `en`, `ja`, `where`). The file is built into the exe. Placeholders
  (`{0}`, `{1}`) must be the same set in the three languages. 합방 is "Collab" / 「コラボ」.
- Korean that must stay Korean in the source (folder and file names that existing users' data depends on, log
  lines, names read for migration) goes to `livemix/i18n/korean-allowlist.json` with the reason.
- The test `LiveMix localisation` (`tests/LiveMixLocalisationTests.cpp`) reads the sources and fails for Korean text
  that is neither in the table nor in the allowlist, for a key that is missing or doubled, and for placeholders that
  differ.

## Names that are stored

A translated name must not become something that is compared later.

- The default FX of a new session is named in the running language (리버브 / Reverb / リバーブ). A plugin set stores
  the FX name of each send; `matchPluginSetSends` (`livemix/src/PluginSet.cpp`) treats the three default names as the
  same FX, after the id and after an exact name.
- The default session file: an existing `기본 세션.livemix` is opened first, a new one gets the name in the running
  language (`createDefaultSession`, `livemix/src/Main.cpp`).
- `플러그인 프리셋`, `플러그인 세트`, `/LiveMix 백업`, `프리셋_` stay Korean in every language: they are paths of
  existing data.
- The default recording folder and the recording track names follow the running language; a folder the user picked
  is kept as it is.

## Layout

Korean layouts are the reference and must not move. English and Japanese text is usually longer:

- `livemix/src/ui/Widgets.h`: `captionWidth`, `textButtonWidth`, `tickBoxWidth` return the fixed Korean width for
  Korean and grow only for the other languages when the text needs it. Use them for a label or a button with a fixed
  width instead of a new number.
- Fonts (`uiTypefaceFor`): Malgun Gothic for Korean, Segoe UI for English (Malgun Gothic draws the backslash of a
  path as the won sign), Yu Gothic UI for Japanese.
- Japanese has no spaces: a message box that wraps by words needs wrapping by characters
  (`messageLayout` in `livemix/src/ui/CollabWindow.cpp`).

## Looking at every screen in the three languages

`LiveMix i18n screenshots` (`tests/LiveMixI18nShotsTests.cpp`) writes about a hundred pictures per language when
`LIVEMIX_I18N_SHOTS_DIR` is set (without it the test only runs its checks):

    set LIVEMIX_I18N_SHOTS_DIR=<folder>
    python C:/Users/claude/tools/claude_harness/run_tests_hidden_desktop.py <EnqueueTests.exe> <log> "LiveMix i18n screenshots"

Each language folder gets the PNG files, `overflow.txt` (text that does not fit its box - some lines are cut in
Korean as well, so read it against the Korean file), `menus.txt` and `manifest.tsv`. Compare the Korean pictures
before and after a layout change: they must be the same.

## Outside the app

- Installer: `installer/LiveMix.iss` has Korean, English and Japanese; our own messages are in
  `installer/LiveMix.messages.iss` and `installer/Enqueue.messages.iss`. An update keeps the language of the first
  install.
- The update window (WinSparkle) is shown in the language chosen in LiveMix (`Updater::initialise`, last argument).
- Not translated: the OBS plugin's Japanese locale file (adding it shows existing users a new "restart OBS" prompt),
  the Stream Deck plugin, the web guest page of Collab, the site, the release notes.
