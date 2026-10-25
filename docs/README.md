# MiniMap (Kenshi RE_Kenshi plugin)

A small always-on minimap for the **top-left corner** of the screen. It shows a
**zoomed local view around the player character** (1/8 of the world map by
default) and mirrors the markers that matter:

- **your own markers** — dropped with the **middle mouse button on the big map**
  or directly on the minimap, always shown on the minimap (pinned to the edge
  when they are far away), removable by a second middle click (on either map) or
  by walking up to them, with a name;
- **player destination markers** you drop on the big in-game map
  (`MapScreen::setMovementMarker`);
- **town markers** and **character markers** shown on the big map
  (`MapScreen::MapMarkerTown` / `MapMarkerCharacter`);
- **the player's own arrow** at the centre of the minimap, turning with the
  direction the *character* faces.

The minimap can be **resized in-game**: hold the left mouse button on one of its
edges (or corners) and drag — the side under the cursor follows the mouse. A thin
frame in Kenshi's UI colours marks the edges; hovering one brightens that side
and shows a small **"Drag to resize"** hint, so the edge is discoverable before
the first drag. The size and position are remembered in `MiniMapLayout.txt` next
to `MiniMap.dll`.

Markers outside the visible window are skipped, except your own markers, which
are pinned to the minimap edge so they are always visible.

The minimap is a plain MyGUI overlay placed on the `Info` layer, so it stays
visible above the game's windows.

While the game is showing one of its own screens instead of the world — the
**title screen**, the **new-game character creation** and the **character
editor** — the overlay is hidden (and not created at all on the first one it
meets), so it never covers those screens nor reacts to clicks there. The game's
own state is what is checked, not the widget tree: the title screen object, the
open character editor window, and the player's character-editor flag.

The minimap also steps aside for the game's **interface windows**, because it is
a permanent overlay and would otherwise sit on top of a corner of them (and
swallow middle clicks there): any **inventory** window (I, loot, trade),
**character stats** (C), **dialogue**, a **modal message box**, the **management
screen** (map (M), squads, research, factions, AI), **prospecting** and the
**options window**. Those do not replace the world, so the minimap keeps being
updated while hidden and is correct again on the frame the window closes; the
player's own markers stay drawn on the big map even while it is open.

## Controls

| Action | Result |
| --- | --- |
| Middle-click on the big map (M) | drop one of your own markers there |
| Middle-click on the minimap | drop one of your own markers at that spot |
| Middle-click again near one of your markers | pick that marker up again (on either map) |
| Hold the left mouse button on a minimap edge and drag | resize the minimap (corners resize both sides at once); hovering the edge first shows a **"Drag to resize"** hint |
| Type a name, then Enter (right after dropping) | name the marker — the box starts empty and shows what you type; Escape or an empty box keeps the automatic name (`Marker 3`) |
| Walk up to one of your markers | the marker is picked up automatically |

Your markers are drawn on the **big map** as well (inside the game's own map
widget, so they pan and zoom with it) and on the minimap, where an off-screen one
is pinned to the edge. A marker counts as *reached* when the player's arrow
overlaps its icon on the minimap — see `kMarkerReachViewFraction`.

Your markers are stored in `MiniMapMarkers.txt` next to `MiniMap.dll` as
`name <TAB> x <TAB> y`, where `x`/`y` are fractions of the whole world map.

The minimap's own size and position are stored in `MiniMapLayout.txt` next to
`MiniMap.dll` as `left top width height`, and are written when an edge drag
finishes. Delete that file to get the default 300×300 map at (8, 8) back.

The *defaults* (size, corner offsets, zoom, marker lifetimes, radii, icons) are
tunable in `include/MiniMapConfig.h` — `kViewZoom` controls the zoom
(`1.0` = whole world, `8.0` = 1/8 of it), `kMinSize` the smallest size the map
can be dragged to.

## How it works

The plugin is a `KenshiLib` plugin DLL, built the same way as the reference
`kMod` project:

1. **Mirrored game classes.** Instead of including the heavy SDK headers (which
   need boost and an old toolchain), `include/KenshiMirror.h` hand-declares only
   the classes/methods we use. The build links against `KenshiLib.lib`; a wrong
   signature would be an unresolved external, so *a successful link is the
   correctness check*.
2. **Hooks.**
   - `KingOfRenderThread::mainloop()` is the primary per-frame driver (it runs on
     every rendered frame, which `ForgottenGUI::update()` does not).
     `ForgottenGUI::update()` and `MapScreen::update()` also refresh the overlay.
   - `MapScreen::setMovementMarker()` records the destination the player dropped.
   - `MapScreen::MapMarkerTown::update()` / `MapMarkerCharacter::update()` receive
     the final map-image pixel position, so no extra coordinate math is needed.
     They are only a **fallback** though: the game calls them for a marker only
     when it actually refreshes that one, so they regularly see a small subset of
     the markers (measured in-game: 2 against 16 harvested).
   - `MapScreen::mapMousePressed()` catches the middle mouse button on the big
     map, and `GetAsyncKeyState(VK_MBUTTON)` polls the physical button: on the big
     map *and* on the minimap (which is mouse-transparent, so it cannot report a
     click itself). The poll always runs - a forwarded click and the poll can
     report the same press, and `ApplyBigMapMarkerClick()` de-duplicates them, so
     the poll must not switch itself off after the first forwarded click. One
     physical press is consumed **once**: while the button is still held every
     further report of it is ignored (a short cooldown covers a press that is
     already released again). Without that, a single click dropped a marker and
     immediately picked it up again.
     That "already handled" flag is cleared by the **per-frame update** as soon as
     the button comes up again, never from inside the click handler: the handler
     only ever runs while the button *is* down, so a reset in there never
     happened, the flag stayed set for the rest of the session and every click
     after the first one was swallowed.
   - The minimap's own click is routed by the **cursor position**: a middle click
     inside the minimap rectangle belongs to the minimap, and the big-map poll
     skips that area. This deliberately does not test `BigMapMouseOver()` - that
     walks the mouse-focus widget up to the map's `mainWidget` and stays true for
     a while after the big map was hidden, so it silently disabled every minimap
     click.
3. **The view is centred on the player character** (`GameWorld::player` ->
   `PlayerInterface::getAnyPlayerCharacter()`), *not* on `getCameraPos()`: the
   camera orbits the character, which used to drag the whole map around while
   the dot stayed put. The arrow's angle comes from the character's
   `getOrientation()` quaternion, converted into a **map-space** direction (the
   world position is projected twice through `worldToMapCoords`), so no world
   axis convention has to be guessed. The arrow art points up and MyGUI rotates a
   `RotatingSkin` clockwise for positive angles, which makes `atan2(dx, -dy)`
   the right angle; Kenshi's character forward axis is the opposite of Ogre's
   usual `-Z`, so `kPlayerIconAngleOffset` adds 180 degrees.
4. **Markers are read out of the game's own map widgets.** Kenshi keeps one
   widget per town marker in `MapOverlaysMin/Mid/Max` and one per character in
   `MapOverlaysCharacters` (`MapScreen::mapOverlaysContainer[4]`, offset `0x60`,
   children of `mapImage`) and positions each marker widget *centred* on its map
   position. Walking that widget tree gives a complete snapshot of every marker -
   including while the big map has never been opened. The result is cached and
   only re-walked every `kHarvestIntervalMs`; a walk that comes up empty keeps the
   previous one for `kHarvestKeepMs` and asks the game to rebuild its markers with
   `MapScreen::refresh()` (only while the map is hidden).
   The `MapMarkerTown::update()` / `MapMarkerCharacter::update()` hooks are kept
   as a *fallback* only: the game calls them for a marker only when it actually
   refreshes that one, so they regularly report a small subset (measured in-game:
   2 hook markers against 16 harvested ones) and using them first hides all the
   other towns.
   The marker's *middle* is read, not its top-left corner: the game centres the
   art (`root->setCoord(pos - size / 2)`), so the corner sits half an icon away
   from the real position. Since the icon size is a fixed number of canvas pixels
   the error shrank as the big map was zoomed in - town icons crept around with
   the big map's zoom, and a town icon and the character icon next to it (a
   different art size) drifted apart.
5. **Coordinates.** Positions are kept as a fraction of the whole world map, then
   scaled by the *texture* size (4096 x 4096). This matters because the game
   resizes its map canvas whenever the player zooms the big map, so
   `MapScreen::worldToMapCoords()` and the marker hooks return pixels in the
   current zoom, not in texture pixels. The visible window is a `kViewZoom`-sized
   texture crop centred on the player, clamped to the map edges.
6. **Rendering.** The background is an `ImageBox` showing a *crop* of the world
   map texture `GUI_Map.dds` via `setImageTexture()` + `setImageTile()` +
   `setImageCoord()` (one tile = the crop, stretched over the widget). Markers are
   pooled `ImageBox` widgets using the `Kenshi_MapMarkers` image set, the player
   arrow uses the same art on a `RotatingSkin`.
   *(Careful: `setImageInfo(tex, coord, tile)` would be wrong — MyGUI builds a
   tile grid of `coord/tile`, so a tile equal to the whole texture yields zero
   tiles and the widget is silently blank.)*
   The player's own markers get a second pool whose widgets are created **inside
   the game's `MapScreen::mapImage`** — the whole-world map widget. A child of it
   is positioned in map-canvas pixels, which is exactly the space
   `worldToMapCoords()` returns (`(world - worldMin) / worldSize * mapImage size`),
   and MyGUI scrolls and clips it together with the map, so panning and zooming
   need no code at all. Those widgets are destroyed with their parent, never by
   us: before use the frame checks that they are still in the map widget's child
   list (`BigMapPoolAlive`), which is also the only liveness test that cannot
   dereference a stale widget.
7. **No MyGUI event delegates.** Attaching a delegate to
   `Widget::eventMouseButtonPressed` access-violates inside MyGUI's delegate
   manager in this build, so all input is polled: the big-map middle click comes
   from the `mapMousePressed` hook plus a physical-button poll, the minimap's own
   middle click is a physical-button poll too (the overlay is mouse-transparent,
   so it can never receive a click itself), **resizing** polls the left button
   the same way (press inside an edge zone, then follow the mouse delta until
   the button comes up), and the rename box
   (a plain `TextBox`) reads its keys with `GetAsyncKeyState()` /
   `ToUnicode()`. MyGUI's `EditBox` is deliberately not used: it needs
   `InputManager::setKeyFocusWidget()`, which crashed the game. The key state that
   `ToUnicode()` needs is assembled by hand from `GetAsyncKeyState()`:
   `GetKeyboardState()` only reports what the *calling thread's* message queue has
   seen, and this thread never pumps messages, so it failed and not one typed
   character reached the box. The character also depends on the keyboard layout,
   so the foreground window's layout is passed in (`GetKeyboardLayout`).
   Because the keys are read straight from the OS, the game would otherwise act on
   them as well (WASD pans the camera, Space pauses, R rotates the view, ...).
   While the box is up the game's *own* control switch is therefore cleared:
   `key->controlEnabled = false` on Kenshi's `InputHandler` global (`key`, mirrored
   in `KenshiMirror.h`), restored in `CommitRename()` / `CancelRename()`. That flag
   is the same one the game clears while one of its own text fields has focus, so
   no system-wide keyboard hook is needed. Enter and Escape keep working - they are
   polled like every other key, independently of the game's switch. The flag is
   only restored if this plugin is the one that cleared it, and a per-frame safety
   net (`if (!g_renaming) RestoreGameControl()`) plus `ResetState()` make sure the
   game can never be left without keyboard control. If the mirrored offset ever
   failed to match the build the value read back would not be 0/1; the code logs a
   warning and leaves the game alone instead of writing a wrong address.
8. **The SDK's MyGUI headers do not match the engine's class layout.**
   `include/MyGuiCompat.h` documents the measured offsets and routes the affected
   calls through base pointers rebuilt with the *engine's* offsets:

   | base class         | engine `Widget` | SDK header |
   |--------------------|-----------------|------------|
   | `ICroppedRectangle`| `0x008`         | `0x008`    |
   | `WidgetInput`      | `0x080`         | `0x070`    |
   | `SkinItem`         | `0x288`         | `0x1E0`    |

   In an inherited *non-virtual* member call the compiler passes
   `(char*)widget + <SDK offset>`, so `setNeedMouseFocus()` wrote a byte into the
   widget's user data (silent corruption, the crash after a marker was dropped)
   and `getSubWidgetMain()` read the wrong field (returned null, so the player
   arrow never turned). MyGUI's `UString` is built in an oversized buffer for the
   same reason (its size is the engine's, not ours).


## Requirements

- Kenshi **1.0.65** (Steam or GOG) — the only version with matching RVAs.
- **RE_Kenshi** and **KenshiLib** installed in the game directory.

## Building

Open `MiniMap.slnx` (or `MiniMap.vcxproj`) and build **Release | x64**, or:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' `
  .\MiniMap.slnx /m /p:Configuration=Release /p:Platform=x64
```

Output: `x64\Release\MiniMap.dll`.

Build notes:

- **Whole Program Optimization must stay enabled** (`<WholeProgramOptimization>true</WholeProgramOptimization>`,
  i.e. `/GL` + `/LTCG`). `KenshiLib.lib` is an *import* library: without `/GL` the
  compiler materialises a local import thunk inside `MiniMap.dll` for every game
  function, so `&MapScreen::mapMousePressed` points at `MiniMap.dll` instead of
  the real stub in `KenshiLib.dll`. `KenshiLib::GetRealAddress()` then fails its
  `[FUNC_BEGIN, FUNC_END]` range check and asserts with
  *"Incorrect address in KenshiLib::GetRealAddress()"*. With `/GL` the address is
  loaded from the import address table, which the loader fills with the stub's
  real address in `KenshiLib.dll`. This is also why every KenshiLib example mod
  and the reference `kMod` enable it.
- `/std:c++14` is required — the (old) MyGUI headers do not compile with C++17+
  in this configuration.
- `/EHa` (async exceptions) matches the reference project.
- Only `Release|x64` is supported (`_DEBUG` changes the MyGUI ABI and would not
  match the shipped release `MyGUIEngine_x64.dll`).
- Paths to the SDK and to `KenshiLib.lib` are set in `Directory.Build.props` and
  can be overridden with `/p:KenshiLibSdkRoot=...` / `/p:KenshiLibReleaseRoot=...`.

See `INSTALL.md` for deployment.

## Troubleshooting

The plugin mirrors all of its messages into `RE_Kenshi_log.txt` (lines prefixed
`MiniMap:`) and also writes `MiniMap.log` next to the game executable. On start it
logs which hooks were installed:

```
[MiniMap: hooks - mainloop=ok gui=ok mapUpdate=ok movement=ok town=ok character=ok mapMouse=ok]
```

- No `MiniMap:` lines at all → the DLL is not loaded (check the mod folder and
  that the mod is enabled in the RE_Kenshi launcher).
- A hook reports `FAIL` → that RVA did not resolve (usually a game version other
  than 1.0.65).
- Minimap does not appear → check the log for `MiniMap: overlay created`. If only
  `MiniMap: step '...' failed` lines appear, the step name says exactly which
  MyGUI call failed and why. Creation is retried once per second, so a failure
  during the world transition heals by itself.
- `MiniMap: heartbeat updates=...` is written every 5 seconds and proves the
  per-frame driver is alive. It also prints `size=<w>x<h> at (<left>,<top>)`
  (the current layout), `src=hooks|widgets|none` (where the town / character
  markers came from), `hooks=<count>` / `walked=<count>
  age=<seconds>` (the two caches), `player=yes/no`, `playerNorm=(x,y)` (the
  fraction of the world the minimap is centred on), `ang` (the player arrow's
  angle in radians), `overlay=hidden|shown` (whether a menu screen or an
  interface window is keeping the minimap off), `mapShown=` / `mouseOverMap=`
  (whether the big map is up),
  `mouseHook=` (whether the game ever forwarded a middle click) and
  `arrow=rotating|fixed`.
- `MiniMap: map widgets - image=... at (...) container0='MapOverlaysMin' x<n> ...`
  is the one-off dump of the game's marker containers (`towns=<n>
  characters=<n>`). `containerN=null` means the map screen has not built its
  layout yet.
- `MiniMap: map widget town #0 container=0 name='...' children=.. abs=(..) norm=(..)`
  lines are the first few harvested markers: they show the raw widget position and
  the world fraction derived from it.
- `MiniMap: asked the game to rebuild its map markers (nothing to read yet)`
  means neither source has produced anything yet, so `MapScreen::refresh()` was
  called as a fallback.
- `MiniMap: GUI manager changed - rebuilding the overlay` /
  `our root widget is gone - rebuilding the overlay` are normal after loading a
  save: the game destroys its whole MyGUI tree and the overlay is rebuilt.
- `MiniMap: overlay hidden (<reason>)` is written when the minimap is switched
  off because the game showed a menu screen (the reason is `title screen`,
  `character editor window` or `player characterEditorMode`) or one of its
  interface windows (`inventory window`, `character stats window`, `dialogue`,
  `modal message box`, `management screen (map / squads / research / factions)`,
  `prospecting window`, `options window`), and `MiniMap: overlay shown again`
  when it is back. The minimap is therefore expected to disappear during
  new-game character creation and while an inventory / stats / map / research
  window is open.
- `MiniMap: screen check - inMenu=.. titleScreen=.. characterEditor=..
  editorMode=.. player=.. window=.. inventories=.. stats=.. dialogue=..
  modal=.. management=.. prospecting=.. options=..` is written every 15 seconds
  and reports the raw flags behind that decision (`window=` names the window
  that currently hides the overlay, `none` when it is visible). If the minimap
  wrongly covers a window, these flags say which signal was missed; if the
  minimap never comes back in normal play, the same line shows which signal is
  stuck (then drop that signal from `ProbeScreens()` in `src/MiniMapGui.cpp`).
- `MiniMap: player arrow rotates (rotating skin at 0x...)` confirms that the
  arrow can actually be turned; without it the log says
  `player arrow has no rotating skin`.
- `MiniMap: mapImage middle click: used(mouseWorld) map=(...) canvas=...` (or
  `poll middle click: ...`) is written for every middle click on the open big map
  and shows which coordinate source was used (`mouseWorld` = the game's own
  `getMouseWorldPosition()`, `eventCoords` = the widget event coordinates).
- `MiniMap: type a name for 'Marker N' and press Enter` appears together with the
  rename box; `MiniMap: rename key vk=0x.. -> U+..` is written for the first
  `kRenameKeyLogLimit` keys of that name (proof that the polled keyboard works and
  which character each key produced), and `marker renamed to '...'` when the name
  is stored. Nothing typed → the log stops after the first line.
- `MiniMap: game keyboard control disabled while naming the marker` /
  `... restored` bracket every rename session. If the first one appears but the
  game still reacts to WASD while typing, the switch is not the one this build
  obeys; `InputHandler::controlEnabled reads <n> - offset mismatch, the keyboard is
  NOT blocked while naming` means the mirrored offset does not match the exe at all.
- `MiniMap: markers on the big map=<n> reach=..m nearest=..m world=..` is the
  second heartbeat line, written only while you have markers of your own:
  how many are drawn on the big map, the distance at which a marker counts as
  reached and how far away the nearest one is (both in world units/metres, using
  `MapScreen::worldSize`).
- `MiniMap: resize drag started (edge x=.. y=..)` / `MiniMap: resized to WxH at
  (left,top)` bracket an edge drag; the second line is also when
  `MiniMapLayout.txt` is written. A press on the edge that never moved does not
  log the second line and changes nothing.
- `MiniMap: resize frame and hover hint created` confirms the decorative frame
  and the "Drag to resize" panel were built; `hover hint unavailable` means the
  game's `Kenshi_GenericTextBoxFlatSmall` layout could not be created (the frame
  itself still works and the drag is unaffected).
- The arrow does not point the way the character faces → tune
  `kPlayerIconAngleOffset` / `kPlayerIconAngleSign` in `MiniMapConfig.h`
  (the offset is in radians; `3.14159` = 180 degrees).
