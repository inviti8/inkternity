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
- **Waypoint skins** — capture a rectangle of the canvas (`ButtonSelectTool`) as a waypoint's skin, used as the artwork for nav buttons in reader mode and as node visuals in the tree view
- **Pixel (raster) brushes** — curated set built on [libmypaint](https://github.com/mypaint/libmypaint) (Sharp / Textured categories: technical pen, fine inker, brush pen, fine/broad markers, wet ink, pencil) with persistent tile data per layer
- **Brush customization + saved presets** — tune any libmypaint parameter (size, opacity, smudge, jitter, dabs, tracking, pressure curves…), capture a square icon from the canvas, and save the tuned brush to a per-user library
- **Stroke vectorize** — drag a rect over recorded libmypaint strokes to convert them into editable vector beziers
- **Artist avatar** — capture a 256×256 square from the canvas; a downscaled 64×64 broadcasts to peers and renders above their remote cursor during collaboration
- **Subscription hosting** — alongside ephemeral collab lobbies, a canvas with portal-issued metadata (or dev keys) can be published under a stable share code; subscribers join read-only as live viewers

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
- Other tools: rectangle, ellipse, line, eye-dropper, edit cursor
- Copy/paste between canvases and tabs
- Drop arbitrary files onto the canvas

## Installation

Inkternity is under active development. Phase 1 (waypoint graph, reader mode, tree view), Phase 2 (transitions + per-waypoint timing), and Phase 3 (brush customization, saved-preset library, artist avatars) have all landed; release-candidate installers are produced from each tagged build. Build from source via [BUILDING.md](docs/BUILDING.md).

## Contribution

Issue reports (bugs and feature requests) welcome. For pull requests of any meaningful scope, please open an issue first to align — Phase 1 is moving fast and large parts of the code are still being shaped.

## License

Inkternity is distributed under the [Business Source License 1.1](LICENSE) (BUSL-1.1). Non-production use — development, testing, education, academic research, personal non-commercial use — is permitted today; members of the HEAVYMETA cooperative get the production-use grant via their Membership Agreement. The license converts to AGPL-3.0 on the change date (2029-03-09) or on the fourth anniversary of any specific version's first publication, whichever comes first.

Inkternity carries code originally licensed under MIT by Yousef Khadadeh as InfiniPaint upstream; the MIT grant for that code is perpetual and the verbatim notice is preserved at `assets/data/third_party_licenses/InfiniPaint/LICENSE.txt`. Upstream relicensed its master branch to GPL-3.0 on 2026-05-11 — Inkternity's fork point predates that, so we carry no GPL-3.0 obligation, but we also don't pull post-relicense upstream changes (BUSL and GPL-3.0 aren't compatible in a single distribution). See `NOTICE` for the full lineage.

Third-party components retain their respective licenses; see the `About` menu in-app for the full list.
