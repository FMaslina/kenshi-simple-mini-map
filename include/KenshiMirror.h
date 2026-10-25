#pragma once
// -----------------------------------------------------------------------------
// MiniMap - hand-written mirrors of the Kenshi 1.0.65 game classes we touch.
//
// Why mirror instead of including the SDK headers:
//   The SDK headers pull in boost (not present in this checkout) and a lot of
//   other heavy dependencies. Only the *mangled names* of the members we use
//   have to match KenshiLib.lib exactly - a mismatch shows up as a link error,
//   which makes the build itself the correctness check.
//
// Rules (same as the reference kMod project):
//   * keep class / method / nesting names EXACTLY as declared in KenshiLib;
//   * only POD / pointers cross the DLL boundary;
//   * every field offset we actually read has a static_assert below.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>

#include <kenshi/Enums.h>            // lightweight: enums only (MapZoomLevel)

#include <ogre/OgreVector2.h>        // real types: ABI-safe by-value returns
#include <ogre/OgreVector3.h>
#include <ogre/OgreQuaternion.h>

#include <mygui/MyGUI_Types.h>       // MyGUI::types::TPoint<int>
#include <mygui/MyGUI_Widget.h>
#include <mygui/MyGUI_ImageBox.h>
#include <mygui/MyGUI_Window.h>
#include <mygui/MyGUI_ScrollView.h>
#include <mygui/MyGUI_RotatingSkin.h>
#include <mygui/MyGUI_Gui.h>

// -----------------------------------------------------------------------------
// MapScreen  (kenshi/gui/MapScreen.h)
// -----------------------------------------------------------------------------
class MapScreen
{
public:
    class MapMarkerTown
    {
    public:
        // RVA 0x48F590 - receives the final map-image pixel position.
        void update(const MyGUI::types::TPoint<int>& mapPosition, MapZoomLevel zoom);
    };

    class MapMarkerCharacter
    {
    public:
        // RVA 0x48F9D0 - receives the final map-image pixel position.
        void update(const MyGUI::types::TPoint<int>& mapPosition);
    };

    void update();   // RVA 0x49A510
    void refresh();  // RVA 0x49A7E0

    // RVA 0x48B6E0 - is the big map on screen?
    bool getVisible() const;

    // RVA 0x48C7F0 - the world position the game thinks the mouse is over.
    bool getMouseWorldPosition(Ogre::Vector3& position);

    // RVA 0x48B7A0 - the destination marker the player drops on the big map.
    void setMovementMarker(const Ogre::Vector3& p, float time);

    // RVA 0x48B700 - the big map's own mouse handler. Hooked so a middle click
    // can drop one of *our* markers (the game itself ignores the middle button).
    void mapMousePressed(MyGUI::Widget* _sender, int _left, int _top, MyGUI::MouseButton _id);

    // RVA 0x48BC60 / 0x48C6B0 - world <-> map-image pixel conversion.
    MyGUI::types::TPoint<int> worldToMapCoords(const Ogre::Vector3& position);
    Ogre::Vector3 mapCoordsToWorld(const MyGUI::types::TPoint<int>& point);

    // --- fields: only up to the ones we read ------------------------------
    char _pad0[0x10];
    MyGUI::Widget* mainWidget;         // 0x10 the map window itself
    MyGUI::ScrollView* mapScrollView;  // 0x18
    MyGUI::ImageBox* mapImage;         // 0x20 whole-world map texture widget
    MyGUI::ImageBox* cameraMarker;     // 0x28
    MyGUI::RotatingSkin* cameraMarkerSkin; // 0x30 the game's own view-cone arrow
    char _pad1[0x58 - 0x38];
    MapZoomLevel zoomLevelCurrent;     // 0x58
    char _pad2a[0x60 - 0x5C];
    // 0x60 - the containers the game's own town / character marker widgets live
    // in: [0..2] = towns per zoom layer ("MapOverlaysMin/Mid/Max"),
    // [3] = characters ("MapOverlaysCharacters").
    MyGUI::Widget* mapOverlaysContainer[4];
    char _pad2[0x184 - 0x80];
    Ogre::Vector2 mapMouseLastPosition; // 0x184 the game's own last map mouse pos
    char _pad3[0x1A4 - 0x18C];
    Ogre::Vector2 worldSize;           // 0x1A4 fallback canvas size
};

static_assert(offsetof(MapScreen, mainWidget) == 0x10, "MapScreen::mainWidget");
static_assert(offsetof(MapScreen, mapScrollView) == 0x18, "MapScreen::mapScrollView");
static_assert(offsetof(MapScreen, mapImage) == 0x20, "MapScreen::mapImage");
static_assert(offsetof(MapScreen, mapMouseLastPosition) == 0x184, "MapScreen::mapMouseLastPosition");
static_assert(offsetof(MapScreen, worldSize) == 0x1A4, "MapScreen::worldSize");
static_assert(offsetof(MapScreen, cameraMarker) == 0x28, "MapScreen::cameraMarker");
static_assert(offsetof(MapScreen, cameraMarkerSkin) == 0x30, "MapScreen::cameraMarkerSkin");
static_assert(offsetof(MapScreen, zoomLevelCurrent) == 0x58, "MapScreen::zoomLevelCurrent");
static_assert(offsetof(MapScreen, mapOverlaysContainer) == 0x60, "MapScreen::mapOverlaysContainer");

// -----------------------------------------------------------------------------
// InputHandler - the game's keyboard / mouse command state (kenshi/InputHandler.h).
//
// Only the master switch is mirrored. The game clears `controlEnabled` itself
// while one of its own text fields has focus, which is exactly what a marker name
// needs: with it cleared the keyboard goes to the rename box instead of the
// camera and the other hotkeys. (Verified against the shipped exe: the
// neighbouring flags the SDK header gives as tiltDown=0xF0 / tiltUp=0xF1 are
// really at those offsets, so 0xD0 is right here too.)
// -----------------------------------------------------------------------------
class InputHandler
{
public:
    char _pad0[0xD0];
    bool controlEnabled;                           // 0xD0
};
static_assert(offsetof(InputHandler, controlEnabled) == 0xD0, "InputHandler::controlEnabled");

// -----------------------------------------------------------------------------
// ManagementScreen - owns the (single) MapScreen.
// -----------------------------------------------------------------------------
class ManagementScreen
{
public:
    static ManagementScreen* getSingleton();   // RVA 0x2967F0

    char _pad0[0xA8];
    MapScreen* mapScreen;                      // 0xA8
};
static_assert(offsetof(ManagementScreen, mapScreen) == 0xA8, "ManagementScreen::mapScreen");

// -----------------------------------------------------------------------------
// ForgottenGUI - the game's GUI host (kenshi/gui/ForgottenGUI.h)
// -----------------------------------------------------------------------------
class ForgottenGUI
{
public:
    MyGUI::Gui* getGuiManager();                     // RVA 0x6EE0D0
    void update();                                   // RVA 0x6E91E0
    void destroyWidget(MyGUI::Widget* widget);       // RVA 0x6E15B0

    MyGUI::Window* createPanelAbs(const std::string& name, float top, float left,
        float width, float height, std::string layer, std::string skin); // 0x73F140

    MyGUI::ImageBox* createImageAbs(MyGUI::Widget* parent, int top, int left,
        int width, int height, const std::string& image);               // 0x73F260

    // --- fields: only up to the one we read -------------------------------
    char _pad0[0x1C0];
    // The open character editor window. Non-null while the game is editing a
    // character - both when a new game is being set up (EDIT_NEWGAME) and for
    // the in-game editor (EDIT_MIDGAME). While it is up the world is not being
    // played and the minimap would sit on top of it.
    void* characterEditor;                           // 0x1C0
};
static_assert(offsetof(ForgottenGUI, characterEditor) == 0x1C0, "ForgottenGUI::characterEditor");

// -----------------------------------------------------------------------------
// RootObject / Character / PlayerInterface - the character the player controls.
//
// getPosition()/getOrientation() are virtual, so the non-virtual entry points
// (_NV_...) are called instead - exactly the symbols KenshiLib exports.
// -----------------------------------------------------------------------------
class RootObject
{
public:
    Ogre::Quaternion _NV_getOrientation() const;   // RVA 0xD1EC0
};

class Character : public RootObject
{
public:
    Ogre::Vector3 _NV_getPosition();               // RVA 0x5CDF00
};

class PlayerInterface
{
public:
    Character* getAnyPlayerCharacter() const;      // RVA 0x7F19B0

    // --- fields: only up to the ones we read ------------------------------
    char _pad0[0x2F0];
    // Set while the character editor is open (new game character creation and
    // the in-game editor both go through it).
    bool characterEditorMode;                      // 0x2F0
};
static_assert(offsetof(PlayerInterface, characterEditorMode) == 0x2F0, "PlayerInterface::characterEditorMode");

// -----------------------------------------------------------------------------
// GameWorld
// -----------------------------------------------------------------------------
class GameWorld
{
public:
    const Ogre::Vector3 getCameraPos() const;      // RVA 0x784DB0 (orbits the player)
    const Ogre::Vector3 getCameraCenter() const;   // RVA 0x784D80 (what the camera looks at)

    char _pad0[0x580];
    PlayerInterface* player;                       // 0x580
};
static_assert(offsetof(GameWorld, player) == 0x580, "GameWorld::player");

// -----------------------------------------------------------------------------
// KingOfRenderThread - the game's main/render loop. mainloop() is invoked once
// per frame while a world is loaded, which makes it the reliable driver for the
// minimap overlay (ForgottenGUI::update() is not called on every frame).
// -----------------------------------------------------------------------------
class KingOfRenderThread
{
public:
    void mainloop(float time);   // RVA 0x789A90

    // --- fields: only up to the one we read -------------------------------
    char _pad0[0x20];
    // The title screen, while it is up (it is deleted once a game starts).
    void* titleScreen;           // 0x20
};
static_assert(offsetof(KingOfRenderThread, titleScreen) == 0x20, "KingOfRenderThread::titleScreen");

// Game globals exported by KenshiLib.
__declspec(dllimport) ForgottenGUI* gui;
__declspec(dllimport) GameWorld* ou;
__declspec(dllimport) InputHandler* key;
__declspec(dllimport) KingOfRenderThread* au;
