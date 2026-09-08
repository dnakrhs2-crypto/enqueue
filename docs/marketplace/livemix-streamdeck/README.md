# LiveMix 1.0.0 submission package

Use [listing-en.md](listing-en.md) for the primary listing,
[listing-ko.md](listing-ko.md) for Korean copy,
[release-notes-en.md](release-notes-en.md) for version notes,
[review-notes-en.md](review-notes-en.md) for review setup/tests, and
[submission-checklist-ko.md](submission-checklist-ko.md) for the client's Console steps.

## Media handoff to Claude

Each composition is standalone HTML: inline CSS/SVG, system fonts, English
captions, no external assets, script, network request or animation. The canvas
is declared by `<meta name="canvas" content="WxH">`. Load each file in Chromium,
use its exact viewport dimensions at device scale factor **1**, wait for
`document.fonts.ready`, and capture the viewport with the background included.
No page margins, browser chrome, resizing or crop should be applied.

| HTML source | Export filename | Canvas |
|---|---|---|
| [app-icon.html](app-icon.html) | app-icon.png | 288×288 |
| [thumbnail.html](thumbnail.html) | thumbnail.png | 1920×960 |
| [gallery-01-microphones.html](gallery-01-microphones.html) | gallery-01-microphones.png | 1920×960 |
| [gallery-02-groups.html](gallery-02-groups.html) | gallery-02-groups.png | 1920×960 |
| [gallery-03-send.html](gallery-03-send.html) | gallery-03-send.png | 1920×960 |

The thumbnail uses **Microphones and FX at your fingertips**. The galleries
cover microphone ON/OFF/mixed states, distinct mute/plugin groups, and key/dial
FX sends. Device frames show a 15-key Stream Deck and an eight-key, four-dial
Stream Deck +. They are original illustrations, not device photos.
LiveMix is a window silhouette with unlabeled controls, explicitly identified
as an illustration; it does not invent an English version of the Korean app.
Example names and amounts demonstrate supported behavior.

Regenerate with `npm.cmd run media` from `streamdeck/`. Edit
`streamdeck/tools/generate-marketplace.mjs` for layout/copy and
`streamdeck/src/ui/artwork.ts` for the shared key/brand geometry.
Regeneration overwrites these five HTML files. Inspect their PNG exports for
legibility and clipping before upload.

The plugin preferences PNGs (256/512) are already produced by the asset
generator and packaged. They differ in size from the 288×288 Marketplace icon.
Alternative Chromium compositions are
`streamdeck/design/plugin-256.html` and `plugin-512.html`.
If replacing packaged PNGs with Chromium exports, rerun validation and packing;
record a new package size/hash in VALIDATION.md. A normal build deterministically
regenerates the geometric PNGs.

## Actual demonstration video

Record real LiveMix 0.6.0+ and physical controls; no simulated footage is supplied.
Use **demo.mp4, 1920×1080**, below **250 MB** (internal target ≤50 MB), with
**demo-thumbnail.png, 1920×960**. Include English captions explaining the Korean
LiveMix UI. Suggested sequence: enable External Control; toggle Host; show a
mixed all-mics state; apply mic/FX group mutes; switch plugin group 1; change the
send with a Mobile key; rotate/short-press/rotate-while-held on Stream Deck +;
restart LiveMix and show reconnection. Use ASIO and an effect only for the audible
portion. Supply the video with the review material because the dial feature
depends on hardware. [Review requirements](https://docs.elgato.com/maker-console/review-process/)

## Checked guideline sources

Checked 2026-09-08. Plugin and category/action/key sizes are separate from
Marketplace media requirements. [Plugin icon guidelines](https://docs.elgato.com/guidelines/stream-deck/plugins/),
[manifest reference](https://docs.elgato.com/streamdeck/sdk/references/manifest/).

Marketplace requires a 288×288 PNG app icon, 1920×960 PNG thumbnail/gallery
images, and 3–10 gallery items. Its video specification is 1920×1080 MP4 under
250 MB; the separate email-submission section's 50 MB limit is not the Console
plugin limit. [Product/media guidelines](https://docs.elgato.com/guidelines/products/),
[submission instructions](https://docs.elgato.com/maker-console/submitting-products/).

The client's remaining decisions are the public Maker organization spelling
(manifest Author currently Gomtwigim), available relevant Console tags, and
who handles English support. The requested tagline is already used verbatim.
Claude owns final media exports and site updates. The client owns the real dial
recording, Maker Agreement, submission and manual publication.
