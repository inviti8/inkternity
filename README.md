<div align="center">
	<img alt="Inkternity Logo" src="logo.svg" width=150/>
	<h3>Comics on an infinite, infinitely zoomable canvas</h3>
	<p>
		<a href="docs/MANUAL.md">📕 Usage Manual</a> -
		<a href="docs/BUILDING.md">⚒️ Build Manual</a> -
		<a href="docs/design/PHASE1.md">🗺️ Phase 1 Design</a>
	</p>
	<p>
		<a href="LICENSE"><img alt="BUSL-1.1 License" src="https://img.shields.io/badge/license-BUSL--1.1-blue"/></a>
	</p>
</div>

## Inkternity

Inkternity is an infinite-canvas app for **producing and reading comics**. The canvas has no zoom-in or zoom-out limit, so a comic can range from a wide map of an entire story down to per-panel ink detail without crossing a tile boundary or losing context.

On top of that canvas Inkternity adds a directed *waypoint graph* that captures reading order: each waypoint is a named camera + framing snapshot, and edges between waypoints define the path a reader follows. Branching panels give multiple outgoing edges, which the reader navigates through skinnable per-waypoint nav buttons.

### A fork of InfiniPaint

Inkternity is a fork of [ErrorAtLine0/infinipaint](https://github.com/ErrorAtLine0/infinipaint) — the infinite-canvas drawing app it inherits everything else from. The canvas, the rendering pipeline, layers, collaboration, the file format, the existing tool set: all of it is InfiniPaint's work. Inkternity layers a comic-production workflow on top.

Inkternity and upstream InfiniPaint diverged in 2026: upstream is now GPL-3.0, Inkternity is BUSL-1.1 (with a 2029 transition to AGPL-3.0). See the License section below + `NOTICE` for the lineage and the MIT-era code Inkternity carries from before the divergence.

Files saved by Inkternity use the `.inkternity` extension; existing `.infpnt` files from InfiniPaint load read-only-on-disk, and the next save migrates them to `.inkternity` (the original `.infpnt` is left in place — no destructive auto-rename).

## What Inkternity adds on top of InfiniPaint

- **Waypoints** — droppable canvas markers that capture camera state, panel framing, and a position in a directed reading graph
- **Tree-view editor** — collapsible side panel for connecting waypoints into a reading order with optional branches; bidirectional sync with the canvas
- **Reader mode** — chrome-free presentation that follows the waypoint graph; arrow-key navigation; per-branch choice UI; per-waypoint speed multipliers and easing
- **Transitions** — waypoints can be flagged as transition points so the reader auto-advances through them, building cinematic camera moves between story beats
- **Frame animation** — author flipbook-style animations inline with the comic: a Next Frame button snap-pans the camera one viewport along a chosen axis (auto-wiring the chain edge on the next dropped waypoint), and a Copy Frame button rasterises the previous frame's active-layer pixels and pastes them at the live view as a regular image (transformable, erasable, hideable). Playback runs through the existing transition + stop-time machinery
- **Per-waypoint audio cues** — attach an mp3 to a waypoint (drag the file onto the canvas while the waypoint is selected) and reader mode plays it on arrival, looping if asked. Audio continues across waypoint transitions until the reader lands on a new audio-bearing waypoint (hard-cuts to the new clip) or a "stop audio" anchor waypoint (silence). Cumulative 30 MB cap per canvas, dedup-aware. Author mode is unconditionally silent
- **Waypoint skins** — capture a rectangle of the canvas (`ButtonSelectTool`) as a waypoint's skin, used as the artwork for nav buttons in reader mode and as node visuals in the tree view
- **Pixel (raster) brushes** — curated set built on [libmypaint](https://github.com/mypaint/libmypaint) (Sharp / Textured categories: technical pen, fine inker, brush pen, fine/broad markers, wet ink, pencil) with persistent tile data per layer
- **Brush customization + saved presets** — tune any libmypaint parameter (size, opacity, smudge, jitter, dabs, tracking, pressure curves…), capture a square icon from the canvas, and save the tuned brush to a per-user library
- **Stroke vectorize** — drag a rect over recorded libmypaint strokes on the layer you're editing to convert them into editable vector beziers
- **Parallax layers (multiplane camera)** — give any layer a depth and panning produces true multiplane parallax: near layers slide past, far layers crawl. Zoom stays uniform and infinite; depth edits are undoable and never move the layer at the moment you set them. Draw a cloud per layer and pan through a sky with real depth
- **Flatten Layer** — bake the entire active layer (ink, vector strokes, shapes, text, images) into one raster object. Collapses hundreds of strokes into a single component when a layer is done — the perf answer for dense crosshatching and for heavy parallax layers. Pixel detail stays crisp (zoom in first to keep vector detail); size cap adjustable in Settings → General. Large flattened projects benefit from the GPU resource cache budget setting (Settings → General)
- **Merge Down** — losslessly move a layer's contents into the layer below (vector stays vector, text stays editable, ink stays erasable) and drop the emptied layer. Refuses with an explanation rather than silently changing how the art composites
- **Particle effects (TimelineFX)** — import a `.eff` effect library (authored in the external [TimelineFX editor](https://www.rigzsoft.co.uk/) — File ▸ Import FX Library, or drag-drop), pick an effect from the FX Library panel, and paint instances onto the canvas with the particle brush (size + rate sliders, plus a "play on touch" toggle): rain, snow, embers, dust, magic, explosions. Each placement plays in author mode and animates in reader mode, moves/scales/rotates like any object, inherits its layer's parallax depth, and honors the effect's own Continuous/Finite setting — looping effects run while visible; one-shots play when they come into view (across parallax layers and reader-mode waypoint navigation) or on touch. Rendered faithfully through a Skia port of the legacy TimelineFX runtime (per-sprite tint + additive blending, animated sprite-sheets). The library — including its textures — is embedded once in the save and shared by every placement; host-authored in collaborative sessions
- **Artist avatar** — capture a 256×256 square from the canvas; a downscaled 64×64 broadcasts to peers and renders above their remote cursor during collaboration
- **Subscription hosting** — alongside ephemeral collab lobbies, a canvas with portal-issued metadata (or dev keys) can be published under a stable share code; subscribers join read-only as live viewers
- **Verifiable publishing (C2PA + Heavymeta on-chain trust)** — opt-in provenance for exported canvases. Enable via Settings → Verifiable publishing; the gateway generates a self-signed Ed25519 X.509 CA, walks the artist through a one-time on-chain registration via the Heavymeta portal + `hvym-cert-registry` Soroban contract, and from then on every PNG/JPG/WEBP export carries a signed C2PA manifest. Re-opening a signed file in Inkternity surfaces a "Provenance signature verifies" toast; tampered files toast red. Rotation + revocation are buttons in the same panel. Crypto-averse by default: the entire surface is hidden behind the toggle, the word "Stellar" stays inside a "Show advanced" disclosure

## Inherited from InfiniPaint

- Infinite canvas, infinite zoom (no zoom limit until memory)
- Online collaborative lobbies — text chat, see-each-other-draw, jump-to-player
- Graphics tablet support with pressure sensitivity
- Layers with blend modes
- Saveable color palettes; right-click quick menu (color swap, canvas rotate)
- Undo / redo
- PNG / JPG / WEBP / SVG export of canvas regions
- Transform (move, scale, rotate) selections (rectangle / lasso select)
- Embed images and animated GIFs on the canvas
- Hide UI with Tab; remappable keybinds; custom UI themes
- Square grids on the canvas as drawing guides
- Rich-text textboxes (bold, italics, underline, fonts, color, alignment, direction)
- Shape tools: rectangle (with **editable Polygon mode** — draggable corners, Add Point to grow N-gons, and **selective bezier curves** per node with smooth/cusp tangents), ellipse (**shearable/skewable** for circles in perspective), line
- **Shape masks** — flag any shape (rect / polygon / ellipse) as a mask to clip a layer's content to it (or invert to knock holes); ideal for comic panels. Composes (union / donut), shows red-dashed on the active layer only, and bakes into the image on Flatten
- Other tools: eye-dropper, edit cursor
- Copy/paste between canvases and tabs
- Drop arbitrary files onto the canvas

## Installation

Inkternity is under active development. Phase 1 (waypoint graph, reader mode, tree view), Phase 2 (transitions + per-waypoint timing), Phase 3 (brush customization, saved-preset library, artist avatars), Phase 4 (parallax layers + generalized flatten), Phase 5 (TimelineFX particle effects), Phase 5.5 (render-performance pass for large multi-section projects), Phase 6 (editable polygons + shearable ellipses), Phase 7 (shape masks), and Phase 8 (selective bezier curves on polygon nodes) have all landed; release-candidate installers are produced from each tagged build. Build from source via [BUILDING.md](docs/BUILDING.md).

## Contribution

Issue reports (bugs and feature requests) welcome. For pull requests of any meaningful scope, please open an issue first to align — Phase 1 is moving fast and large parts of the code are still being shaped.

## License

Inkternity is distributed under the [Business Source License 1.1](LICENSE) (BUSL-1.1). Non-production use — development, testing, education, academic research, personal non-commercial use — is permitted today; members of the HEAVYMETA cooperative get the production-use grant via their Membership Agreement. The license converts to AGPL-3.0 on the change date (2029-03-09) or on the fourth anniversary of any specific version's first publication, whichever comes first.

Inkternity carries code originally licensed under MIT by Yousef Khadadeh as InfiniPaint upstream; the MIT grant for that code is perpetual and the verbatim notice is preserved at `assets/data/third_party_licenses/InfiniPaint/LICENSE.txt`. Upstream relicensed its master branch to GPL-3.0 on 2026-05-11 — Inkternity's fork point predates that, so we carry no GPL-3.0 obligation, but we also don't pull post-relicense upstream changes (BUSL and GPL-3.0 aren't compatible in a single distribution). See `NOTICE` for the full lineage.

Third-party components retain their respective licenses; see the `About` menu in-app for the full list.
