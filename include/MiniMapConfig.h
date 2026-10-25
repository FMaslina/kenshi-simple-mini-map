#pragma once
// -----------------------------------------------------------------------------
// MiniMap - all user-tunable values live here.
// -----------------------------------------------------------------------------
namespace MiniMapConfig
{
    // --- placement (screen pixels, measured from the top-left corner) -----
    // Defaults only: the player can resize the minimap at runtime by dragging
    // one of its edges, and the layout is stored next to the DLL (see
    // MiniMapLayout.h). These values are used until such a file exists.
    const int   kWidth      = 300;   // default minimap width
    const int   kHeight     = 300;   // default minimap height
    const int   kOffsetLeft = 8;
    const int   kOffsetTop  = 8;

    // --- resizing ---------------------------------------------------------
    // Hold the left mouse button on one of the minimap's edges (or corners) and
    // drag: the side under the cursor follows the mouse. kResizeGrabPx is how
    // close to the edge the cursor has to be, kMinSize is the smallest side the
    // map may shrink to, and kScreenMargin keeps it off the screen border while
    // it grows.
    const int   kResizeGrabPx = 7;
    const int   kMinSize      = 120;
    const int   kScreenMargin = 4;

    // Where the last size/position is remembered (next to MiniMap.dll).
    const char* const kLayoutFileName = "MiniMapLayout.txt";

    // --- frame and resize hint -------------------------------------------
    // A thin frame in Kenshi's UI colours is drawn just inside the minimap
    // edge. It is pure decoration: every widget stays mouse-transparent, and
    // the drag itself is still polled (see PollResize()).
    //
    // Hovering one of the resize zones brightens that side of the frame and
    // shows a small "Drag to resize" hint next to the cursor, so the edge is
    // discoverable without having to click it first.
    const char* const kFrameSkin     = "WhiteSkin";   // solid white 2x2 skin
    const int         kFrameOuterPx  = 1;             // dark line at the edge
    const int         kFrameInnerPx  = 2;             // light line inside it
    const int         kFrameCornerPx = 5;             // corner accent size
    const char* const kHintSkin      = "Kenshi_GenericTextBoxFlatSmall";
    const char* const kHintText      = "Drag to resize";
    const int         kHintWidth     = 150;
    const int         kHintHeight    = 32;

    // --- zoom -------------------------------------------------------------
    // The minimap shows this fraction of the whole world map, centred on the
    // player. 1.0 == the entire world map; 8.0 == 1/8 of it (local view).
    const float kViewZoom   = 8.0f;

    // Map canvas size used until the game's map widget reports its own
    // (Kenshi's world map texture is 4096x4096).
    const int   kFallbackCanvas = 4096;

    // --- widgets ----------------------------------------------------------
    const char* const kRootName   = "MiniMapRoot";
    const char* const kLayer      = "Info";          // above windows / popups
    const char* const kRootSkin   = "PanelEmpty";
    const char* const kImageSkin  = "ImageBox";
    const char* const kMapTexture = "GUI_Map.dds";   // whole-world map texture

    const char* const kMarkerResource = "Kenshi_MapMarkers";

    // --- the player's own icon -------------------------------------------
    // Uses the map's "Camera" art (an arrow / view cone) on a rotating skin, so
    // it can point in the direction the *character* faces.
    const char* const kPlayerIconGroup  = "Camera";
    const char* const kPlayerIconSkin   = "RotatingSkin";
    const int         kPlayerIconSize   = 16;
    // The arrow art points up, and MyGUI rotates a RotatingSkin clockwise for
    // positive angles, so the angle for a map-space facing direction (dx, dy)
    // is atan2(dx, -dy). Kenshi's own `_NV_getOrientation()` forward axis turns
    // out to be the *opposite* of Ogre's usual -Z, so the arrow came out
    // pointing exactly 180 degrees away from the character; the offset below
    // cancels that. (offset is in radians: 3.14159 == 180 degrees.)
    const float       kPlayerIconAngleOffset = 3.14159265358979f;
    const float       kPlayerIconAngleSign   = 1.0f;

    // --- text widgets (skins verified to exist in Kenshi) ------------------
    const char* const kLabelSkin = "Kenshi_TextboxPaintedText";

    // The rename box is a plain TextBox: MyGUI's EditBox needs keyboard focus
    // (InputManager::setKeyFocusWidget) and that call is what crashed the game,
    // so the typed name is polled from the OS instead and simply drawn here.
    const char* const kRenameSkin      = "Kenshi_TextboxStandardText";
    const int         kRenameMaxLength = 24;   // bytes of UTF-8 (a Cyrillic letter is 2)

    // --- marker image groups (from kenshi_images.xml) ---------------------
    const char* const kGroupCamera    = "Camera";
    const char* const kGroupTown      = "Town";
    const char* const kGroupCharacter = "Character";
    const char* const kGroupMovement  = "Movement";
    const char* const kGroupCustom    = "NewDiscovered";

    // --- lifetime of mirrored "recent" markers (seconds) ------------------
    // Towns never move, so once we know about one we keep it for good.
    const float kMovementMarkerMinTtl     = 2.0f;
    const float kMovementMarkerMaxTtl     = 120.0f;
    const float kMovementMarkerDefaultTtl = 15.0f;
    const float kTownMarkerTtl            = 0.0f;    // 0 == permanent
    const float kCharMarkerTtl            = 30.0f;   // refreshed while the map updates

    // --- harvesting the game's own map marker widgets ---------------------
    // The overlay reads the marker widgets straight out of the game's map
    // screen (MapOverlaysMin/Mid/Max for towns, MapOverlaysCharacters for
    // characters), so markers show up without ever opening the big map. A walk
    // that finds nothing keeps the previous result for a while, because the
    // game may only rebuild its markers when the map is open.
    const unsigned long long kHarvestKeepMs       = 60000;
    const unsigned long long kHarvestIntervalMs   = 500;   // min time between walks
    const int                kMaxWalkCandidates   = 1024;  // sanity limit per container
    const int                kMaxWalkDiagnostics  = 12;    // one-off tree dump size

    // While the big map is closed the game never updates its own markers, so the
    // overlay asks it to rebuild them every so often (seconds) - but only while
    // the harvest comes back empty.
    const int   kMarkerRefreshIntervalMs  = 10000;

    // --- marker widget pool ----------------------------------------------
    const int   kMaxMarkerWidgets = 512;
    const int   kMaxLabelWidgets  = 32;

    // --- the same markers on the *big* map --------------------------------
    // A marker is dropped by middle-clicking the big map, so it is drawn there
    // as well: one icon (plus its name) per marker, created inside the game's
    // own map image widget so they scroll and zoom with the map by themselves.
    const int   kMaxBigMapMarkers = 64;

    // --- the player's own map markers (persisted next to the DLL) ---------
    const char* const kMarkersFileName = "MiniMapMarkers.txt";

    // Middle-clicking the big map within this many screen pixels of an existing
    // marker picks that marker up again instead of dropping a new one (the game
    // shows the map 1:1, so this is converted to world fractions with the canvas
    // size the game reports at the current zoom).
    const int   kMarkerClickRadiusCanvasPx = 25;

    // A middle click on the minimap itself drops a marker where it was clicked,
    // and a click within this many *minimap* pixels of an existing marker picks
    // that one up again. The minimap is mouse-transparent, so this is polled
    // from the OS/MyGUI instead of through a widget event (see
    // PollMinimapClick()).
    const int   kMinimapClickRadiusPx = 12;

    // Walking this close to one of your markers picks it up automatically.
    // The distance is measured as a fraction of the *minimap's* own view: 1/32 of
    // the visible window is about 9 px on a 300 px minimap, i.e. exactly where the
    // player's arrow starts to overlap a marker icon. That way "the marker goes
    // away when I reach it" means the same thing whatever the big map's zoom is.
    const float kMarkerReachViewFraction = 1.0f / 32.0f;
    const float kMarkerReachRadiusNorm   = kMarkerReachViewFraction / kViewZoom;
    // Two triggers can report the same physical middle click (the game's map
    // mouse handler and the physical button poll); the second one within this
    // many milliseconds is ignored. Repeats of the *same* press (the button is
    // still held down) are dropped as well - otherwise one click dropped a
    // marker and picked it up again in the same instant.
    const unsigned long long kMarkerClickCooldownMs = 300;
    // A freshly dropped marker is ignored by the reach test for this long (ms),
    // so dropping one at your own feet does not remove it instantly.
    const unsigned long long kMarkerGraceMs = 5000;
    // Give up on naming a marker after this long *without typing* and keep the
    // automatic name, so the box never stays on screen forever.
    const unsigned long long kRenameTimeoutMs = 20000;
    // How many translated keystrokes of a single rename are written to the log.
    // They are the proof that the polled keyboard reaches the box at all.
    const int kRenameKeyLogLimit = 16;
}
