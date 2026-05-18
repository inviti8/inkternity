## Basic Usage
- Switch to a brush tool (vector or pixel — see below), and draw with the mouse by holding <kbd>LMB</kbd> over the canvas. If you have a tablet connected, just draw on the tablet
- To erase, switch to the eraser tool, and draw over the parts you want to erase. For tablets, you can switch to the eraser tool, or you can just use your pen's eraser while the brush tool is selected
- You can move around the canvas by holding the <kbd>MMB</kbd>/mouse wheel button. For tablets, pen button 1 is assigned to the middle mouse button. You can change this by going to Menu->Settings->Tablet->Middle click pen button
- To zoom in/out, use the scroll wheel, or hold the control key and <kbd>MMB</kbd> (pen button 1 on a tablet) and drag the mouse up/down
- Save your file by going to Menu->Save, or pressing <kbd>Ctrl</kbd> + <kbd>S</kbd>. The shortcut for Save As is <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>S</kbd>. Inkternity saves files with the `.inkternity` extension
- Open files by going to Menu->Open, or dragging a file into the window. Legacy `.infpnt` files from upstream InfiniPaint open read-only-on-disk; the next save migrates them to `.inkternity` (the original `.infpnt` is left in place). If the dropped file isn't a canvas file, the app assumes you meant to embed it on the canvas as an image
## Online Collaboration
- Before hosting/connecting, you should go to Menu->Settings->General->Display Name, and change your name that will appear to others online
- To host the file you're currently on, go to Menu->Host. You'll see two hosting modes:
    - **Collab** — ephemeral lobby code. Anyone you send the code to can join and edit the canvas with you. Default for canvases without portal metadata
    - **Subscription** — a stable share code derived from your app key + the canvas's portal-issued canvas id. Subscribers join **read-only** as live viewers — they see your edits in real time but can't write to the canvas. Available when the canvas has portal-issued subscription metadata, or when dev keys are configured (Menu->Settings->General)
- Copy the lobby address that pops up, and send it to anyone you want to join. Once you press "Host" in this popup, the lobby is open
- To connect to a lobby, go to Menu->Connect, paste the lobby address into the given textbox, and press "Connect"
- If either the host or the client needs to get the lobby address again, it will be available in Menu->Lobby Info after entering the lobby
- IMPORTANT NOTE: There is a chance that, even with both people online, you can't connect to each other. This could be due to router/firewall settings. One relay (TURN) server is hosted for this app, but it relies mostly on whether a direct connection can be established between the host and client (STUN)
- Once connected, a chat bubble pops up on the bottom left. Click that to open the chatbox, type your message, then press "Send". You can also press <kbd>F2</kbd> to open the chatbox, and <kbd>Enter</kbd> to send. <kbd>Esc</kbd> cancels
- A player list icon appears at the top of the screen when connected. Click that to view who's in the lobby. You can jump to any other player's location through this list
- **Viewers (subscribers)** see a "Viewing live: ..." label instead of the canvas name and have no editing toolbar — undo, redo, grid, layer, and brush-customization buttons are hidden because they have no effect on a read-only session
## Shortcuts
- To view all available shortcuts/keybinds, go to Menu->Settings->Keybinds
- Note that, for macOS users, the "META" key is the command key
- Shortcuts can be remapped by pressing the button containing the shortcut text, then pressing the new key combination for the shortcut while the button is highlighted
## Tools
- Vector Brush
    - Pressure sensitive if a tablet is being used
        - You can disable pen pressure sensitivity by unticking the checkbox at Menu->Tablet->Pen pressure affects brush size
        - Change the pen smoothing by going to Menu->Tablet->Smoothing sampling time
    - The size slider on the right changes the size of the brush. All size properties for all tools are relative to the current zoom level
    - Strokes are stored as editable vector objects (selectable, transformable, deletable per stroke)
- Pixel Brush (Ink / Textured)
    - Built on the [libmypaint](https://github.com/mypaint/libmypaint) engine and split across two toolbar buttons: **Ink Brushes** (sharp, line-art-oriented presets) and **Textured Brushes** (pencil, marker, wet ink, etc.). The two buttons share one tool, just with the active preset pre-seeded for that category
    - Pressure sensitive on tablets; strokes are rasterized into per-layer tile data rather than stored as vector objects
    - The brush picker row at the top of the tool panel lists the curated presets in the active category; click to switch
    - See the **Brush Customization** and **Saved Presets** sections below for tuning + saving your own brushes
- Eraser tool
    - On vector layers: erases complete objects when they are touched by the eraser. Can't erase portions of an object
    - On pixel layers: erases tile pixels directly under the cursor
- Line tool
    - Hold <kbd>LMB</kbd> and drag the mouse to place a straight line
    - Hold <kbd>Shift</kbd> while creating the line to snap it to a set of common angles
- Textbox tool
    - Hold <kbd>LMB</kbd> and drag the mouse to place a textbox
    - Hold <kbd>Shift</kbd> while creating the textbox to make the textbox a square
    - Once a textbox is placed, the program will switch to the Edit tool automatically, and you will be able to type text into the box
- Ellipse tool
    - Hold <kbd>LMB</kbd> and drag the mouse to place an ellipse
    - Hold <kbd>Shift</kbd> while creating the ellipse to create a circle
    - You can change the properties of the ellipse between "Fill Only", "Outline Only", and "Fill and Outline". Outlines will use the main/top color on the left toolbar, which is the same as the one used for the brush tool. Fills will use the secondary/bottom color on the left toolbar
- Rectangle tool
    - Hold <kbd>LMB</kbd> and drag the mouse to place a rectangle
    - Hold <kbd>Shift</kbd> while creating the rectangle to create a square
    - You can change the properties of the rectangle between "Fill Only", "Outline Only", and "Fill and Outline". Outlines will use the main/top color on the left toolbar, which is the same as the one used for the brush tool. Fills will use the secondary/bottom color on the left toolbar
- Rectangle Select Tool
    - Hold <kbd>LMB</kbd> and drag the mouse to select objects within a rectangle
    - Once <kbd>LMB</kbd> is released, the objects will be selected
    - Move selected objects by holding <kbd>LMB</kbd> above the blue selection rectangle and moving the mouse to the desired position
    - Change the objects' size by dragging the cyan circle at the corner of the selection rectangle
    - Rotate the object by dragging the smaller of the two orange circles at the center of the rectangle
    - To change the center of rotation, drag the larger of the two orange circles at the center of the rectangle to the new center of rotation
    - You can copy (<kbd>Ctrl</kbd> + <kbd>C</kbd>), cut (<kbd>Ctrl</kbd> + <kbd>X</kbd>), and paste (<kbd>Ctrl</kbd> + <kbd>V</kbd>) objects that are selected with this tool. The paste will not change the objects' size, but it will move the objects to the position of the cursor when you paste
    - Press <kbd>Esc</kbd> or click anywhere out of the selection to unselect the objects
    - While selecting objects, hold <kbd>Alt</kbd> to unselect objects in the current selection
    - While selecting objects, hold <kbd>Shift</kbd> to add more objects to the current selection
- Lasso Select Tool
    - Hold <kbd>LMB</kbd> and drag the mouse to select objects within an area
    - Once <kbd>LMB</kbd> is released, the objects will be selected
    - The tool is otherwise identical to the Rectangle Select Tool
- Edit/Cursor Tool
    - You can click on any object except a brush stroke to select it when this tool is selected
    - Double click on any object to start editing it. The object's relevant properties will be displayed on the right
    - If an image/file on the canvas is clicked with this tool, you can download the file to your computer
- Color Select Tool (Eyedropper)
    - Click anywhere on the canvas with this tool to copy the color of the cursor's position
    - You can choose to select either the stroke color, or fill color
- Pan tool
    - Hold <kbd>LMB</kbd> to move around the canvas
    - There is a "Hold to Pan" keybind (set to <kbd>Space</kbd> by default) to temporarily switch to this tool
- Zoom tool
    - Hold <kbd>LMB</kbd> and move the mouse up to zoom in, or move it down to zoom out
    - There is a "Hold to Zoom" keybind (set to <kbd>Z</kbd> by default) to temporarily switch to this tool
- Waypoint tool
    - Drops a waypoint at the current camera position. Waypoints capture both the camera's center and the framing rectangle that defines what the reader sees
    - Each waypoint has a name, an optional skin (see the **Button Select** tool below), per-waypoint transition speed multiplier, and per-waypoint easing curve. Edit these in the Edit tool by double-clicking a waypoint
    - A waypoint can be flagged as a **transition point** — the reader auto-advances through transitions rather than stopping at them. Useful for chained camera moves between story beats
- Button Select Tool
    - Drag a rectangle over the canvas to capture that rectangle as the skin for the currently-selected waypoint. The skin is the artwork that renders for the waypoint's nav button in reader mode and as the node visual in the tree view
- Stroke Vectorize Tool
    - Drag a rectangle on a pixel (Ink) layer to convert the recorded libmypaint strokes inside it into editable vector beziers. The original pixel strokes are removed; the resulting vectors live on the active vector layer
## Brush Customization
- When a Pixel Brush is the active tool, a **brush customization** button (live-brush icon) appears in the top toolbar
- Open the drawer to expose every libmypaint parameter, grouped by category (Basic, Dabs, Speed, Smudge, Jitter, Tracking, Shape, Stroke, Rendering…). Each group is collapsible; only **Basic** is open by default to keep the slider count manageable
- Tweaks apply to the live brush immediately — strokes you draw with the drawer open use the current slider values
- Press **Save as preset...** to capture the live state into a named user preset. The save modal asks for a name + category (Sharp / Textured), and offers a **Capture icon...** button that switches the canvas to a square-aspect capture tool; drag a rectangle and the captured image becomes the preset's 64×64 thumbnail (saved as a sidecar PNG)
## Saved Presets
- A **saved presets** button (brush library icon) lives next to the customization button when a Pixel Brush is active. The drawer browses the curated set + every preset you've saved under your config directory
- Sections are collapsible. **Presets Sharp** and **Presets Textured** (the curated set) are closed by default; your own **Sharp** and **Textured** sections are open so saved brushes are immediately visible
- Typing in the search field filters every section by substring match. Collapsed sections with matching results auto-expand so hits aren't hidden
- Click a tile to activate that preset on the live brush. Click the **X** on a user preset to delete it
- User presets are stored under `<config>/brush_presets/<category>/<slug>.json`; the captured icon ships as a sibling PNG next to each JSON
## Waypoints
- Drop waypoints with the Waypoint tool. Each waypoint stores: name, camera center + framing rectangle, optional skin image, transition flag, speed multiplier (scales the camera-move duration for this waypoint), and easing preset (linear / ease-in / ease-out / ease-in-out etc.)
- Edit a waypoint by selecting it with the Edit tool and adjusting its properties on the right-hand panel
- Connect waypoints into a directed reading graph with **edges** (created via the Tree View — see below). Each edge has an optional label that the reader sees as the button label when there are multiple choices from one node
- A waypoint with 2+ outgoing edges is a **branch point** in reader mode — the reader gets a button for each outgoing choice
- A waypoint with no outgoing edges is a **dead end** in reader mode — the back-button is the only way out
## Reader Mode
- Toggle reader mode with the book icon in the top toolbar (or on the phone UI, the book icon at the top right)
- When active, the editor chrome hides: tools, color pickers, tool options, tree view, undo/redo/grid/layer buttons. The reader is left with the canvas + the book icon (so they can exit)
- The camera snaps to the currently-selected waypoint (or the first one in the graph if nothing is selected). The history stack records every waypoint you visit so **Back** can pop you out
- Navigation appears as floating buttons pinned to the bottom-center of the canvas: a **Back** arrow (when there's history to pop) plus one tile per outgoing edge. Each outgoing tile renders the target waypoint's skin (or a labeled rounded rect if no skin is set)
- Use the arrow keys for quick keyboard navigation: forward follows the first outgoing edge; back pops history
- Transition points (waypoints flagged as transition) auto-advance: after the camera arrives, an optional **stop time** counts down (per-waypoint), then the camera moves to the first outgoing waypoint silently. A chain protection limit guards against accidental cycles (A → P → A)
## Tree View
- Toggle the tree-view panel with the list icon in the editor toolbar (it's a placeholder icon until a graph icon ships)
- The panel renders the waypoint graph as a collapsible tree. Each node shows the waypoint's name and skin (when set); edges between nodes render as connecting lines
- Click a node to select that waypoint; double-click to jump the camera to it
- Drag from one node to another to create an edge; existing edges have a small delete affordance for unhooking
- The tree view stays in sync with on-canvas changes both ways — adding/deleting/renaming a waypoint from the canvas updates the tree, and vice versa
## Artist Avatar
- An **avatar tile** lives at the right end of the top toolbar. Click it to open a popover with **Capture from canvas...** and (when an avatar exists) **Clear**
- Capture switches the canvas to a square-aspect capture tool — drag a rectangle and the captured image becomes your avatar. It's saved locally at 256×256 (master copy) and 64×64 (wire form)
- During collaboration, your 64×64 avatar broadcasts to every peer alongside your cursor metadata. Other artists see your avatar rendered above your remote cursor — useful for distinguishing peers at a glance
- Viewers (subscription read-only mode) don't broadcast their cursor or avatar to the host
## Screenshots
- Start taking a screenshot by going to Menu->Take Screenshot
- Hold <kbd>LMB</kbd> and drag the mouse to select an area to take a screenshot of
- You can modify the screenshot area after placing it by dragging the cyan circles on the boundaries of the rectangle
- Click anywhere outside of the circles to cancel the screenshot
- You can change the dimensions of the screenshot, the file format of the screenshot, and later take the screenshot with the menu on the right. The dimensions can be anything, as long as they fit the aspect ratio of the screenshot area (the program will do this automatically), and as long as they aren't bigger than what the file format or your computer's memory allows
- You can export to SVG, JPG, PNG, or WEBP. In the case of SVG, grids can't be displayed, and resolution can't be changed.
- Note: Even though creating extremely large images is possible with this tool, the program you use to display the image later may crash if the image is too large
## Layers
- You can open the layer window by clicking the layer icon in the top toolbar
- To create a layer, give it a name in the given textbox, and press the + button next to the textbox
    - You can add a layer folder by pressing the folder button instead of the + button. Open/close the folder by pressing the arrow icon to the left of the folder, and drag layers into the folder while it's open to place them in the folder
    - There are two layer kinds: **vector** layers (host vector strokes, shapes, textboxes, embedded images) and **pixel** layers (host libmypaint raster strokes as tile data). New canvases ship with a default Sketch (vector) layer; an Ink layer is the conventional target for libmypaint strokes
- To set the layer to edit, double click it (or click the pencil icon to the right of the layer). The layer that is currently being edited has a pencil icon to the left of it, and anything you draw will be placed in that layer
- Select a layer by clicking it once. When it is selected, you can change its properties such as name, alpha, and blend mode (this is different from the "set to edit" mode)
- Hold shift and click to select multiple layers at once, or hold control and click to toggle the selection state of the layer. You can then hold <kbd>LMB</kbd> and move the mouse to move/sort the layers
- Blend modes are the default ones available with the rendering library this program is using ([Skia](https://skia.org/)). The following explanation of the blend modes is mostly just copy-pasted from the library's documentation at [SkBlendMode](https://api.skia.org/SkBlendMode_8h.html):
    - The documentation is expressed as if the component values are always 0..1 (floats).
    - For brevity, the documentation uses the following abbreviations s : source d : destination sa : source alpha da : destination alpha
    - Results are abbreviated r : if all 4 components are computed in the same manner ra : result alpha component rc : result "color": red, green, blue components
-------------------------------------------------------------------------------------------
| Blend mode name       | Equation                                                        |
|-----------------------|-----------------------------------------------------------------|
|Source Over            |r = s + (1-sa) * d                                               |
|Destination Over       |r = d + (1-da) * s                                               |
|Source In              |r = s * da                                                       |
|Destination In         |r = d * sa                                                       |
|Source Out             |r = s * (1-da)                                                   |
|Destination Out        |r = d * (1-sa)                                                   |
|Source Alpha Top       |r = s * da + d * (1-sa)                                          |
|Destination Alpha Top  |r = d * sa + s * (1-da)                                          |
|XOR                    |r = s * (1-da) + d * (1-sa)                                      |
|Plus                   |r = min(s + d, 1)                                                |
|Modulate               |r = s * d                                                        |
|Screen                 |r = s + d - s * d                                                |
|Overlay                |multiply or screen, depending on destination                     |
|Darken                 |rc = s + d - max(s * da, d * sa), ra = same as Source Over       |
|Lighten                |rc = s + d - min(s * da, d * sa), ra = same as Source Over       |
|Color Dodge            |brighten destination to reflect source                           |
|Color Burn             |darken destination to reflect source                             |
|Hard Light             |multiply or screen, depending on source                          |
|Soft Light             |lighten or darken, depending on source                           |
|Difference             |rc = s + d - 2 * (min(s * da, d * sa)), ra = same as Source Over |
|Exclusion              |rc = s + d - two(s * d), ra = same as Source Over                |
|Multiply               |r = s * (1-da) + d * (1-sa) + s * d                              |
|Hue                    |hue of source with saturation and luminosity of destination      |
|Saturation             |saturation of source with hue and luminosity of destination      |
|Color                  |hue and saturation of source with luminosity of destination      |
|Luminosity             |luminosity of source with hue and saturation of destination      |
-------------------------------------------------------------------------------------------
## Color Palettes
- Click the color button on the left toolbar to open the color picker. The top color button, initially white, is for the brush color and outlines. The bottom color button, initially black, is for fill color (if applicable)
- In the color picker window, you can use, create, and edit color palettes
- To create a palette, press the small + button next to the dropdown, give the palette a name, and press the "Create" button. To remove a color palette, press the small X button next to the dropdown. You cannot remove the default color palette
- To add a color to the palette, press the large + button with the color you want to add picked. To remove a color from the palette, select the color from the palette, and press the large X button
## Quick Menu
- Right click anywhere on the canvas to open the quick menu. Right click again to close the quick menu
- To change the colors shown in the inner circle of the quick menu, select a different palette in the color picker menu
- Drag the small circle in the outer circle of the quick menu to rotate the canvas. If you hover over the outer circle while dragging, the rotation will snap to a set of common angles (0 degrees, 45 degrees, 90 degrees...)
