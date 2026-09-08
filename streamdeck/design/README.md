# LiveMix release artwork

Edit [artwork.ts](../src/ui/artwork.ts) for geometry/colors and
[generate-assets.mjs](../tools/generate-assets.mjs) for the static variant catalogue.
Run `node tools/generate-assets.mjs` from `streamdeck/`.
The generator writes SVG/PNG sources here, then copies the distributable images
to `com.gomtwigim.livemix.sdPlugin/imgs/`. No font, icon library, native image
dependency or download is needed. Node 24 reads the standalone TypeScript source.

| Asset | Files | Dimensions |
|---|---|---|
| Preferences brand | plugin.svg / plugin@2x.svg and matching PNG | 256×256 / 512×512 |
| Brand HTML exports | plugin-256.html / plugin-512.html | 256×256 / 512×512 |
| Category | category.svg / category@2x.svg | 28×28 / 56×56; 24-unit viewBox |
| Action list | actions/*/icon.svg / icon@2x.svg | 20×20 / 40×40; 24-unit viewBox |
| Key state/variant | actions/*/*.svg, excluding icon/encoder | 72×72 / 144×144; 144-unit viewBox |
| Encoder | actions/fx-send/encoder.svg / encoder@2x.svg | 72×72 / 144×144 |
| Connection problems | status/*.svg and each action's disconnected.svg | 72×72 / 144×144 |

All menu icons are white monochrome with transparent backgrounds. The category
uses the ON lettering; the actions use a mic, three mics, bracketed mic, bracketed
FX, numbered chip, dial, ± and a level bar, and a connection/power ring.
The red brand tile is geometric lettering, so its PNG and SVG exports do not
depend on installed fonts. `tools/brand-png.mjs` samples those same primitives
at 4× resolution for antialiased PNGs. The two PNG-ready HTMLs are also supplied
for Claude's Chromium export workflow.

| Meaning | Color / additional cue |
|---|---|
| LiveMix brand | #E5302D red tile, white ON |
| Microphone ON / OFF | #35D07F / #3A3F47, ON/OFF text |
| Group mute | #FF5A5F, slash and Muted; mic keys retain Originally ON/OFF |
| Plugin group ON / OFF | #4C8DFF / #FF5A5F, numbered chip and ON/OFF |
| Some microphones ON | Green/gray individual mics, #FFB454 marker and count |
| Missing target / audio stopped | #FFB454 dashed outline / pause badge |
| Background / labels | #15171B / #F1F2F4, white glyphs |

Brand, background, text, lamp ON/OFF, accent and danger are copied from
`livemix/src/ui/LiveMixPalette.h`; amber #FFB454 is the plugin's existing
warning color. Static samples use English labels and example counts (three
microphones / two mute members). Runtime uses live counts and ko/en labels;
channel names are host titles, never part of the shared image cache.
Leave y=106–144 for those titles. At runtime, disconnected/checking/missing
images replace action previews as soon as a key appears; commands wait for
a fresh snapshot. A green control indication does not establish audible output.

State filenames retain the existing manifest contract: mute groups use
`off` = unmuted and `on` = muted; plugin groups use ON = active.
Single-state send actions use an unknown-value preview in `off.svg`.
Additional files include microphone muted ON/OFF, all-mics mixed, groups 1–5,
send increase/decrease/set/zero/muted and stopped-audio status.

Run `npm.cmd run validate` for dimensions, white menu colors, every manifest
reference in both sizes, design/distribution equality, translations and Elgato
validation. Actual key/font rendering should also be checked in Stream Deck.
Marketplace compositions are generated separately by `npm.cmd run media`;
see [the submission folder](../../docs/marketplace/livemix-streamdeck/README.md).
