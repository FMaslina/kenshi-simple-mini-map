#include "pch.h"
#include "../include/MiniMapGui.h"
#include "../include/KenshiMirror.h"
#include "../include/MiniMapConfig.h"
#include "../include/MarkerStore.h"
#include "../include/Logger.h"
#include "../include/MyGuiCompat.h"

#include <mygui/MyGUI_Delegate.h>
#include <mygui/MyGUI_Exception.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_InputManager.h>
#include <mygui/MyGUI_RotatingSkin.h>
#include <mygui/MyGUI_TextBox.h>

#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace
{
    // -------------------------------------------------------------------------
    // Overlay state (created lazily on the UI thread from Update()).
    // -------------------------------------------------------------------------
    bool             g_created = false;

    // The GUI manager the current overlay was created in. The game can drop and
    // rebuild its whole MyGUI tree (world load / GUI clear), which invalidates
    // every widget pointer we hold - so the identity is checked every frame.
    MyGUI::Gui*      g_mgr = nullptr;

    // Camera position. The game reports it in *map-canvas* pixels, and the canvas
    // is resized whenever the player zooms the big map, so the zoom-independent
    // fraction of the world is what gets stored.
    float            g_lastCamNx = -1.0f;
    float            g_lastCamNy = -1.0f;
    int              g_lastCamPx = 0;       // raw canvas pixels (diagnostics only)
    int              g_lastCamPy = 0;
    int              g_lastCanvasW = 0;
    int              g_lastCanvasH = 0;

    bool             g_bgTextureSet = false;   // setImageTexture() already applied to g_bg

    // currently applied world-map crop, in map-texture pixels
    bool             g_viewSet = false;
    int              g_viewSrcX = 0;
    int              g_viewSrcY = 0;
    int              g_viewSrcW = 0;
    int              g_viewSrcH = 0;

    MyGUI::Widget*   g_root = nullptr;
    MyGUI::ImageBox* g_bg = nullptr;
    std::vector<MyGUI::ImageBox*> g_pool;
    std::vector<std::string>      g_poolGroup;

    // The player's own arrow. It uses the "RotatingSkin" skin, whose main
    // sub-widget can be rotated, so the arrow points where the *character*
    // faces instead of where the camera happens to be looking.
    MyGUI::ImageBox*     g_playerIcon = nullptr;
    MyGUI::RotatingSkin* g_playerIconSkin = nullptr;

    // Marker names are drawn next to the player's own markers.
    std::vector<MyGUI::TextBox*> g_labels;

    // Rename box: a plain TextBox whose text is polled from the OS (see
    // MiniMapConfig::kRenameSkin).
    MyGUI::TextBox*  g_renameBox = nullptr;
    std::string      g_typing;             // what has been typed so far (UTF-8)
    bool             g_renaming = false;
    size_t           g_renameIndex = 0;
    long long        g_pendingRename = -1;   // index requested by the big-map hook
    unsigned long long g_renameStartMs = 0;
    bool             g_renameKeyPrev[0x100] = {};
    bool             g_renameKeysPrimed = false;
    int              g_renameKeyLogLeft = 0;

    // True while *this plugin* has switched the game's keyboard control off for
    // the rename box (see SuppressGameControl). Only the plugin that switched it
    // off may switch it back on, and a frame-time safety net restores it if a
    // session ever ends without going through Commit/CancelRename.
    bool             g_gameControlTaken = false;

    // True while the whole overlay is hidden because the game is showing one of
    // its menu screens or one of its interface windows (see ProbeScreens).
    bool             g_overlayHidden = false;

    // The same markers drawn on the big map (inside the game's map image widget).
    std::vector<MyGUI::ImageBox*> g_bigIcons;
    std::vector<MyGUI::TextBox*>  g_bigLabels;
    std::vector<std::string>      g_bigGroup;
    MyGUI::Widget*                g_bigParent = nullptr;
    size_t                        g_bigUsed = 0;   // widgets drawn on the big map
    void DropBigMapPool();   // defined with the rest of the big-map marker code
    void RestoreGameControl();   // defined with the rename box (releases the keyboard)

    // Vtable identities, learnt from widgets we created ourselves: used to tell
    // the game's marker widgets apart without the type-name API.
    const void*      g_textBoxVTable = nullptr;
    const void*      g_imageBoxVTable = nullptr;

    // Player marker diagnostics (also used to centre the minimap).
    bool  g_havePlayer = false;
    float g_lastPlayerNx = -1.0f;
    float g_lastPlayerNy = -1.0f;
    float g_lastPlayerAngle = 0.0f;

    // Last time we asked the game to rebuild its map markers.
    unsigned long long g_lastMarkerRefreshMs = 0;

    // True once the game's own map mouse handler has delivered a middle click.
    // Until then the physical button is polled as well (some setups never pass
    // the middle button through to MyGUI).
    bool g_hookSawMiddleClick = false;
    bool g_middlePollSawClick = false;

    // True while the physical middle press that has already been handled is still
    // held down. Cleared by the per-frame update as soon as the button comes up
    // (see ResetMiddlePress), never from inside the click handler - that handler
    // only runs while the button *is* down, so a reset there would never happen
    // and every click after the first one would be swallowed.
    bool g_middlePressConsumed = false;


    // -------------------------------------------------------------------------
    // Data recorded by the game hooks (guarded by g_mutex).
    // -------------------------------------------------------------------------
    struct MapEntry      // already in normalised [0,1] map coordinates
    {
        float nx;
        float ny;
        std::string group;
        unsigned long long expireMs;   // 0 == permanent
        // The player's own markers stay visible no matter how far away they are:
        // an off-view marker is pinned to the edge of the minimap instead of
        // being dropped.
        bool clampEdge = false;
        std::string label;             // drawn under the marker (own markers only)
    };
    struct WorldEntry    // raw world position, converted on the UI thread
    {
        float x;
        float y;
        float z;
        unsigned long long expireMs;
    };

    std::mutex g_mutex;
    // Keyed by the game's own handle *inside* the marker (hand::index/serial),
    // not by the marker pointer: the game may recreate its marker objects when it
    // refreshes them, and the handle stays the same.
    std::map<unsigned long long, MapEntry> g_townMarkers;
    std::map<unsigned long long, MapEntry> g_charMarkers;
    std::vector<WorldEntry>         g_movementMarkers;

    // -------------------------------------------------------------------------
    // Markers read straight out of the game's map widgets. This is what makes
    // towns and characters appear without opening the big map first.
    // -------------------------------------------------------------------------
    std::vector<MapEntry> g_walkMarkers;        // replaced wholesale by each walk
    unsigned long long    g_walkStampMs = 0;
    const MapScreen*      g_walkSource = nullptr;   // map screen the walk came from
    unsigned long long    g_lastWalkMs = 0;
    bool                  g_walkTreeLogged = false;
    int                   g_walkDiagnosticsLeft = MiniMapConfig::kMaxWalkDiagnostics;

    // Map screen the hook-recorded markers belong to. A different one means the
    // game has moved to another world, so the old markers are dropped rather
    // than drawn forever (the hooks are keyed by handle, which is per-world).
    const MapScreen*      g_hookSource = nullptr;

    // -------------------------------------------------------------------------
    // Failure reporting. Every risky MyGUI call runs through TryStep() so the
    // log always names the exact call that failed and why, instead of a single
    // opaque "exception while updating the overlay".
    // -------------------------------------------------------------------------
    std::set<std::string>& ReportedSteps()
    {
        static std::set<std::string> s;
        return s;
    }

    void ReportStepFailure(const char* kind, const std::string& step, const std::string& detail)
    {
        if (!ReportedSteps().insert(step + '|' + kind).second)
            return;   // already reported this exact failure once
        Logger::Error("MiniMap: step '" + step + "' failed (" + kind + "): " + detail);
    }

    template <typename F>
    bool TryStep(const std::string& step, F fn)
    {
        try
        {
            fn();
            return true;
        }
        catch (const MyGUI::Exception& e)
        {
            ReportStepFailure("MyGUI", step, e.getFullDescription());
        }
        catch (const std::exception& e)
        {
            ReportStepFailure("std", step, e.what());
        }
        catch (...)
        {
            ReportStepFailure("unknown", step, "unknown exception (access violation under /EHa?)");
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    MapScreen* GetMapScreen()
    {
        ManagementScreen* ms = ManagementScreen::getSingleton();
        return ms ? ms->mapScreen : nullptr;
    }

    bool GetCanvasSize(MapScreen* ms, int& w, int& h)
    {
        w = h = 0;
        if (!ms)
            return false;
        if (ms->mapImage)
        {
            w = ms->mapImage->getWidth();
            h = ms->mapImage->getHeight();
        }
        if (w <= 0 || h <= 0)
        {
            float sw = ms->worldSize.x;
            float sh = ms->worldSize.y;
            if (sw > 1.0f && sh > 1.0f && sw < 1000000.0f && sh < 1000000.0f)
            {
                w = static_cast<int>(sw);
                h = static_cast<int>(sh);
            }
        }
        return w > 0 && h > 0;
    }

    float ClampF(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    int ClampI(int v, int lo, int hi)
    {
        if (hi < lo)
            return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // How far the closest of the player's own markers is, as a fraction of the
    // world map. Diagnostics only - it is what the heartbeat reports in metres.
    float NearestUserMarkerNorm(float nx, float ny)
    {
        float best = -1.0f;
        for (const auto& m : MarkerStore::Get().Markers())
        {
            const float dx = m.nx - nx;
            const float dy = m.ny - ny;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (best < 0.0f || d < best)
                best = d;
        }
        return best;
    }

    struct GroupSize { const char* group; int w; int h; };
    const GroupSize kSizes[] =
    {
        { "Camera",        14, 14 },
        { "Town",          20, 15 },
        { "Outpost",       11, 22 },
        { "Character",     10, 10 },
        { "SmallPlace",     9,  9 },
        { "Nest",           9,  9 },
        { "Village",        9,  9 },
        { "Camp",           9,  9 },
        { "Ruin",           7, 11 },
        { "Workcamp",       9,  9 },
        { "Movement",      10, 12 },
        { "NewDiscovered", 14, 14 },
    };

    void GetGroupSize(const std::string& group, int& w, int& h)
    {
        for (const auto& s : kSizes)
        {
            if (group == s.group)
            {
                w = s.w;
                h = s.h;
                return;
            }
        }
        w = 10;
        h = 10;
    }

    // -------------------------------------------------------------------------
    // Is the big map really on screen?
    //
    // MapScreen::getVisible() is only `mainWidget->getVisible()`, i.e. the
    // widget's *own* flag - it stays true while a parent hides the map. Two other
    // signals are used instead: the inherited visibility of mainWidget, and the
    // widget MyGUI currently believes the mouse is over (walking its parent chain
    // up to mainWidget).
    // -------------------------------------------------------------------------
    bool BigMapMouseOver(MapScreen* ms)
    {
        if (!ms || !ms->mainWidget)
            return false;

        MyGUI::Widget* focus = nullptr;
        if (MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr())
            focus = im->getMouseFocusWidget();

        for (int depth = 0; focus && depth < 64; ++depth)
        {
            if (focus == ms->mainWidget)
                return true;
            MyGUI::Widget* parent = focus->getParent();
            if (parent == focus)
                break;
            focus = parent;
        }
        return false;
    }

    bool BigMapShown(MapScreen* ms)
    {
        if (!ms || !ms->mainWidget)
            return false;
        return ms->mainWidget->getVisible() && ms->mainWidget->getInheritedVisible();
    }

    // Is the root widget we created still part of the live GUI? Checks the
    // registry only - it never dereferences g_root itself, so it is safe to call
    // after the game has destroyed the whole widget tree.
    bool RootIsAlive(MyGUI::Gui* mgr)
    {
        if (!g_created || !g_root || !mgr)
            return false;

        MyGUI::Widget* found = nullptr;
        if (!TryStep("findWidget(root, liveness)",
                     [&] { found = mgr->findWidgetT(MiniMapConfig::kRootName, false); }))
            return false;
        return found == g_root;
    }

    // -------------------------------------------------------------------------
    // Harvesting the game's own marker widgets.
    //
    // Kenshi keeps one widget per town marker in MapOverlaysMin/Mid/Max and one
    // per character in MapOverlaysCharacters, positioned on the map image at the
    // marker's place in the world. Reading that widget tree is what makes towns
    // and characters show up without the big map ever being opened.
    // -------------------------------------------------------------------------
    MyGUI::Widget* FirstImageBoxChild(MyGUI::Widget* parent)
    {
        if (!parent || !g_imageBoxVTable)
            return nullptr;

        const size_t count = parent->getChildCount();
        for (size_t i = 0; i < count && i < 16; ++i)
        {
            MyGUI::Widget* child = parent->getChildAt(i);
            if (child && MyGuiCompat::VTable(child) == g_imageBoxVTable)
                return child;
        }
        return nullptr;
    }

    int WalkContainer(MapScreen* ms, int index, bool characterLayer, std::vector<MapEntry>& out,
                      int imageLeft, int imageTop, int imageW, int imageH)
    {
        MyGUI::Widget* container = ms->mapOverlaysContainer[index];
        if (!container)
            return 0;

        const size_t count = container->getChildCount();
        if (count == 0 || count > static_cast<size_t>(MiniMapConfig::kMaxWalkCandidates))
            return 0;

        int added = 0;
        for (size_t i = 0; i < count; ++i)
        {
            MyGUI::Widget* child = container->getChildAt(i);
            if (!child)
                continue;

            // The widget that carries the marker art: the town marker is a panel
            // ("PanelEmpty") with the image inside it, the character marker *is*
            // the image. The image is also what the game positions on the map.
            MyGUI::Widget* anchor = child;
            if (MyGuiCompat::VTable(child) != g_imageBoxVTable)
            {
                if (MyGUI::Widget* image = FirstImageBoxChild(child))
                    anchor = image;
            }

            // The game centres the marker art on the position it computes, so
            // the widget's *top-left* corner sits half an icon away from it.
            // Using the corner would shift every town/character by a constant
            // number of *canvas* pixels - i.e. by a fraction that shrinks as the
            // big map is zoomed in, which is exactly the "town icons move with
            // the big map's zoom" bug. The middle of the art is the game's own
            // position.
            const int ax = MyGuiCompat::AbsoluteLeft(anchor) + MyGuiCompat::Width(anchor) / 2;
            const int ay = MyGuiCompat::AbsoluteTop(anchor) + MyGuiCompat::Height(anchor) / 2;
            const float nx = imageW > 0 ? static_cast<float>(ax - imageLeft) / static_cast<float>(imageW) : 0.0f;
            const float ny = imageH > 0 ? static_cast<float>(ay - imageTop) / static_cast<float>(imageH) : 0.0f;

            // Markers the game has not positioned yet sit at the origin and are
            // not markers at all.
            if (nx < -0.02f || nx > 1.02f || ny < -0.02f || ny > 1.02f)
                continue;

            if (g_walkDiagnosticsLeft > 0)
            {
                --g_walkDiagnosticsLeft;
                Logger::Info("MiniMap: map widget " + std::string(characterLayer ? "character" : "town") +
                    " #" + std::to_string(i) + " container=" + std::to_string(index) +
                    " name='" + child->getName() + "' children=" + std::to_string(child->getChildCount()) +
                    " abs=(" + std::to_string(ax) + "," + std::to_string(ay) + ")" +
                    " norm=(" + std::to_string(nx) + "," + std::to_string(ny) + ")");
            }

            MapEntry e{ nx, ny,
                        characterLayer ? MiniMapConfig::kGroupCharacter : MiniMapConfig::kGroupTown,
                        0 };
            out.push_back(e);
            ++added;
        }
        return added;
    }

    // Collects every marker the game currently has on its map. Returns the number
    // of markers found (0 when the map widgets are not built yet).
    int HarvestMapMarkers(MapScreen* ms, std::vector<MapEntry>& out)
    {
        out.clear();
        // mainWidget non-null also means the map layout has been built, so the
        // marker containers are real widgets rather than uninitialised fields.
        if (!ms || !ms->mainWidget || !ms->mapImage)
            return 0;

        const int imageW = MyGuiCompat::Width(ms->mapImage);
        const int imageH = MyGuiCompat::Height(ms->mapImage);
        const int imageLeft = MyGuiCompat::AbsoluteLeft(ms->mapImage);
        const int imageTop = MyGuiCompat::AbsoluteTop(ms->mapImage);
        if (imageW <= 0 || imageH <= 0)
            return 0;

        int towns = 0;
        int characters = 0;
        for (int i = 0; i < 4; ++i)
        {
            const bool characterLayer = (i == 3);
            const int added = WalkContainer(ms, i, characterLayer, out, imageLeft, imageTop, imageW, imageH);
            if (characterLayer)
                characters += added;
            else
                towns += added;
        }

        // Towns are listed once per zoom layer, so the same town can come back
        // several times; duplicates are dropped on a 1/4096-of-the-world grid.
        std::set<std::string> seen;
        std::vector<MapEntry> unique;
        unique.reserve(out.size());
        for (const auto& e : out)
        {
            const std::string key = std::to_string(static_cast<int>(e.nx * 4096.0f + 0.5f)) + ":" +
                                    std::to_string(static_cast<int>(e.ny * 4096.0f + 0.5f)) + ":" + e.group;
            if (seen.insert(key).second)
                unique.push_back(e);
        }
        out.swap(unique);

        if (!g_walkTreeLogged)
        {
            g_walkTreeLogged = true;
            std::string line = "MiniMap: map widgets - image=" + std::to_string(imageW) + "x" +
                std::to_string(imageH) + " at (" + std::to_string(imageLeft) + "," +
                std::to_string(imageTop) + ")";
            for (int i = 0; i < 4; ++i)
            {
                MyGUI::Widget* c = ms->mapOverlaysContainer[i];
                line += " container" + std::to_string(i) + "=" +
                    (c ? "'" + c->getName() + "' x" + std::to_string(c->getChildCount())
                       : std::string("null"));
            }
            line += " -> towns=" + std::to_string(towns) + " characters=" + std::to_string(characters);
            Logger::Info(line);
        }
        return towns + characters;
    }

    void ResetState()
    {
        // Only ever *drops* our knowledge: this runs when the game has already
        // destroyed the widgets (GUI rebuild), so no old pointer may be touched.
        g_created = false;
        g_mgr = nullptr;
        g_lastCamNx = -1.0f;
        g_lastCamNy = -1.0f;
        g_lastCamPx = 0;
        g_lastCamPy = 0;
        g_lastCanvasW = 0;
        g_lastCanvasH = 0;
        g_bgTextureSet = false;
        g_viewSet = false;
        g_viewSrcX = 0;
        g_viewSrcY = 0;
        g_viewSrcW = 0;
        g_viewSrcH = 0;
        g_root = nullptr;
        g_bg = nullptr;
        g_pool.clear();
        g_poolGroup.clear();
        g_playerIcon = nullptr;
        g_playerIconSkin = nullptr;
        g_labels.clear();
        g_renameBox = nullptr;
        g_renaming = false;
        RestoreGameControl();   // never leave the game without keyboard control
        g_overlayHidden = false;   // a fresh root widget is created visible
        g_typing.clear();
        g_pendingRename = -1;
        DropBigMapPool();
        g_havePlayer = false;
        g_lastPlayerNx = -1.0f;
        g_lastPlayerNy = -1.0f;
        g_lastPlayerAngle = 0.0f;
        g_walkMarkers.clear();
        g_walkStampMs = 0;
        g_walkSource = nullptr;
        g_hookSource = nullptr;

        std::lock_guard<std::mutex> lk(g_mutex);
        g_townMarkers.clear();
        g_charMarkers.clear();
        g_movementMarkers.clear();
    }

    // -------------------------------------------------------------------------
    // The player's own markers.
    //
    // A marker is dropped with the *middle mouse button on the big map* (the game
    // itself ignores that button), so it can mark any place in the world and is
    // then drawn on the minimap - pinned to the edge of the minimap when it lies
    // outside the current view. It disappears when it is middle-clicked again or
    // when the player walks up to it.
    //
    // NOTE: MyGUI's event delegates are deliberately NOT used here. Attaching a
    // delegate to Widget::eventMouseButtonPressed access-violates inside MyGUI's
    // delegate manager in this build (verified in-game via step logging), so
    // input state is polled once per frame instead.
    // -------------------------------------------------------------------------
    // Keyboard control while a marker name is being typed.
    //
    // The rename box reads its keys straight from the OS (see PollRenameKeys), so
    // without this the game would see them too: WASD pans the camera, Space
    // pauses, R rotates the view, and so on. Kenshi's InputHandler has a master
    // switch for exactly that - the game clears `controlEnabled` itself while one
    // of its own text fields has focus - so the game's own control state is
    // toggled instead of swallowing keys with a global system-wide hook.
    //
    // Enter and Escape keep working: PollRenameKeys reads them from
    // GetAsyncKeyState, which does not depend on the game's switch.
    // -------------------------------------------------------------------------
    void SuppressGameControl()
    {
        if (g_gameControlTaken || !::key)
            return;

        // `controlEnabled` is a bool: anything but 0/1 means the mirrored offset
        // does not match this build of the game, and writing there could corrupt
        // an unrelated field. Leave the game alone in that case.
        const unsigned char raw = *reinterpret_cast<const unsigned char*>(
            reinterpret_cast<const char*>(::key) + offsetof(InputHandler, controlEnabled));
        if (raw > 1)
        {
            Logger::Warn("MiniMap: InputHandler::controlEnabled reads " +
                std::to_string(static_cast<int>(raw)) +
                " - offset mismatch, the keyboard is NOT blocked while naming");
            return;
        }
        if (raw == 0)
            return;   // switched off by the game already - not ours to switch on

        ::key->controlEnabled = false;
        g_gameControlTaken = true;
        Logger::Info("MiniMap: game keyboard control disabled while naming the marker");
    }

    void RestoreGameControl()
    {
        if (!g_gameControlTaken)
            return;
        g_gameControlTaken = false;
        if (::key)
            ::key->controlEnabled = true;
        Logger::Info("MiniMap: game keyboard control restored");
    }

    void CommitRename()
    {
        const auto& markers = MarkerStore::Get().Markers();
        if (!g_typing.empty() && g_renameIndex < markers.size())
        {
            if (g_typing != markers[g_renameIndex].name)
            {
                MarkerStore::Get().SetName(g_renameIndex, g_typing);
                Logger::Info("MiniMap: marker renamed to '" + g_typing + "'");
            }
        }
        if (g_renameBox)
            TryStep("renameBox->setVisible(false)", [&] { g_renameBox->setVisible(false); });
        g_renaming = false;
        RestoreGameControl();
    }

    void CancelRename()
    {
        if (g_renameBox)
            TryStep("renameBox->setVisible(false)", [&] { g_renameBox->setVisible(false); });
        g_renaming = false;
        RestoreGameControl();
    }

    // --- typed text -------------------------------------------------------
    // Kept as UTF-8 in an std::string so no MyGUI::UString ever has to live on
    // our stack (its size is decided by the engine's headers, not ours).
    void AppendUtf8(std::string& out, wchar_t c)
    {
        const unsigned long cp = static_cast<unsigned long>(c);
        if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Backspace deletes the last *character*, not the last byte: a name typed on a
    // Cyrillic (or any non-ASCII) layout is multi-byte, and cutting it in the
    // middle would leave an invalid sequence behind.
    void BackspaceUtf8(std::string& text)
    {
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
            text.pop_back();
        if (!text.empty())
            text.pop_back();
    }

    void RefreshRenameCaption()
    {
        if (!g_renameBox)
            return;
        // The hint is part of the caption so there is no second widget to place,
        // and the text that has been typed so far is always visible.
        const std::string shown = g_typing.empty()
            ? std::string("Name: _   [Enter = OK, Esc = skip]")
            : "Name: " + g_typing + "_";
        MyGuiCompat::UStringHolder text(shown);
        TryStep("renameBox->setCaption", [&] { g_renameBox->setCaption(text.Get()); });
    }

    // Keys of the rename box are polled from the OS. The editor cannot use
    // MyGUI's EditBox: that needs InputManager::setKeyFocusWidget(), which is the
    // call that crashed the game right after a marker was dropped.
    //
    // The key state that ToUnicode() needs is built by hand from GetAsyncKeyState
    // instead of geting it from GetKeyboardState(): the latter only reports what
    // the *calling thread's* message queue saw, and this thread never pumps
    // messages, so it returned FALSE and not a single typed character ever made
    // it into the box.
    void PollRenameKeys()
    {
        BYTE keys[256] = {};
        for (int i = 0; i < 256; ++i)
        {
            if (GetAsyncKeyState(i) & 0x8000)
                keys[i] = 0x80;
        }
        keys[VK_CAPITAL] = (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
        const struct { int generic; int left; int right; } mods[] =
        {
            { VK_SHIFT,   VK_LSHIFT,   VK_RSHIFT   },
            { VK_CONTROL, VK_LCONTROL, VK_RCONTROL },
            { VK_MENU,    VK_LMENU,    VK_RMENU    },
        };
        for (const auto& m : mods)
        {
            if (GetAsyncKeyState(m.generic) & 0x8000)
                keys[m.generic] = keys[m.left] = keys[m.right] = 0x80;
        }

        // The character a key produces depends on the keyboard layout of the
        // *foreground* window, i.e. the game's, not on whatever this thread has.
        HKL layout = nullptr;
        if (HWND fg = GetForegroundWindow())
        {
            const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
            if (tid)
                layout = GetKeyboardLayout(tid);
        }

        for (int vk = 0x08; vk < 0x100; ++vk)
        {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool fresh = down && !g_renameKeyPrev[vk];
            g_renameKeyPrev[vk] = down;

            // The first poll after the box appeared only records the state, so a
            // key that was already held does not count as typed text.
            if (!fresh || !g_renameKeysPrimed)
                continue;

            if (vk == VK_BACK)
            {
                BackspaceUtf8(g_typing);
            }
            else if (vk == VK_RETURN)
            {
                CommitRename();
                return;
            }
            else if (vk == VK_ESCAPE)
            {
                CancelRename();
                return;
            }
            else
            {
                const UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
                wchar_t buf[8] = {};
                const int n = ToUnicodeEx(static_cast<UINT>(vk), sc, keys, buf, 8, 0, layout);
                for (int i = 0; i < n; ++i)
                {
                    if (buf[i] >= 32 && static_cast<int>(g_typing.size()) <
                                        MiniMapConfig::kRenameMaxLength)
                    {
                        AppendUtf8(g_typing, buf[i]);
                        if (g_renameKeyLogLeft > 0)
                        {
                            --g_renameKeyLogLeft;
                            static const char* const kHex = "0123456789ABCDEF";
                            unsigned cp = static_cast<unsigned>(buf[i]);
                            std::string hex = "U+";
                            hex += kHex[(cp >> 12) & 0xF];
                            hex += kHex[(cp >> 8) & 0xF];
                            hex += kHex[(cp >> 4) & 0xF];
                            hex += kHex[cp & 0xF];
                            Logger::Info("MiniMap: rename key vk=0x" +
                                std::to_string(vk) + " -> " + hex);
                        }
                    }
                }
            }

            // Typing keeps the box alive: the timeout is an *idle* timeout.
            g_renameStartMs = GetTickCount64();
            RefreshRenameCaption();
        }

        g_renameKeysPrimed = true;
    }

    // Naming a freshly dropped marker: Enter keeps the typed name, Escape keeps
    // the automatic one. The keys are polled - see the delegate note above.
    void UpdateRenameBox()
    {
        if (g_pendingRename >= 0)
        {
            const long long index = g_pendingRename;
            g_pendingRename = -1;

            const auto& markers = MarkerStore::Get().Markers();
            if (g_renameBox && index >= 0 && static_cast<size_t>(index) < markers.size())
            {
                // The same marker can be reported twice for one physical middle
                // click (the game's map handler and the polled button both see
                // it). Restarting the session then threw away everything the
                // player had typed, so an already running session for this very
                // marker is simply kept - only its timeout is pushed back.
                const bool sameMarker = g_renaming && g_renameIndex == static_cast<size_t>(index);

                g_renameIndex = static_cast<size_t>(index);
                g_renaming = true;
                g_renameStartMs = GetTickCount64();

                if (!sameMarker)
                {
                    // The box starts empty: the automatic name ("Marker 3") is
                    // *kept* unless something is typed, so the player never has to
                    // delete it first - and a partly typed name can never end up
                    // glued to the automatic one.
                    g_typing.clear();
                    g_renameKeyLogLeft = MiniMapConfig::kRenameKeyLogLimit;
                    g_renameKeysPrimed = false;
                    Logger::Info("MiniMap: type a name for '" + markers[g_renameIndex].name +
                        "' and press Enter (Escape keeps the automatic name)");
                }

                for (int vk = 0; vk < 0x100; ++vk)
                {
                    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    g_renameKeyPrev[vk] = down;
                }
                RefreshRenameCaption();
                TryStep("renameBox->setVisible(true)", [&] { g_renameBox->setVisible(true); });

                // The keyboard belongs to the box now: stop the game from acting
                // on it (camera, hotkeys). Released again in Commit/CancelRename.
                SuppressGameControl();
            }
        }

        if (!g_renaming)
            return;

        PollRenameKeys();

        if (g_renaming && GetTickCount64() - g_renameStartMs > MiniMapConfig::kRenameTimeoutMs)
            CommitRename();   // never leave the rename box up forever

        // Safety net: whatever ended the session (a stray reset, a widget that
        // never came back), the game must never be left without keyboard control.
        if (!g_renaming)
            RestoreGameControl();
    }

    // -------------------------------------------------------------------------
    // Marker name labels (a small pool of TextBoxes).
    // -------------------------------------------------------------------------
    MyGUI::TextBox* AcquireLabel(size_t index)
    {
        if (index < g_labels.size())
            return g_labels[index];
        if (static_cast<int>(g_labels.size()) >= MiniMapConfig::kMaxLabelWidgets)
            return nullptr;

        MyGUI::TextBox* box = nullptr;
        if (!TryStep("createWidget(label)",
                     [&] {
                         box = g_root->createWidget<MyGUI::TextBox>(
                             MiniMapConfig::kLabelSkin, 0, 0, 60, 14,
                             MyGUI::Align::Default, "");
                     }))
            return nullptr;
        if (box)
        {
            MyGuiCompat::SetNeedMouseFocus(box, false);
            // The minimap is a dark map texture, so the marker names have to be
            // bright to be readable on top of it.
            TryStep("label->setTextColour", [&] { box->setTextColour(MyGUI::Colour::White); });
            box->setVisible(false);
        }
        g_labels.push_back(box);
        return box;
    }

    // -------------------------------------------------------------------------
    // Dropping / picking up the player's own markers on the big map.
    //
    // The position always comes from the game itself
    // (MapScreen::getMouseWorldPosition()), so it cannot disagree with the
    // coordinate space the town / destination hooks report; the widget event
    // coordinates are only a fallback.
    // -------------------------------------------------------------------------
    void ApplyBigMapMarkerClick(const char* source, bool fromHook, bool haveFallback, int fallbackX, int fallbackY)
    {
        const std::string prefix = std::string("MiniMap: ") + source + " middle click: ";

        MapScreen* ms = GetMapScreen();
        if (!ms)
        {
            Logger::Info(prefix + "no map screen");
            return;
        }

        // The big map has to be the window under the mouse - that is the only way
        // a middle click can hit it. This is a much better test than
        // MapScreen::getVisible(), which only reports mainWidget's own flag and
        // stays true while a parent hides the map. A click that arrives through
        // the game's own map mouse handler is trusted without the test.
        const bool mouseOverMap = BigMapMouseOver(ms);
        if (!fromHook && !mouseOverMap)
        {
            Logger::Info(prefix + "ignored, the mouse is not over the big map");
            return;
        }

        // Two triggers may report the same physical click (the game's map mouse
        // handler and the physical button poll), so the second one is ignored.
        static unsigned long long s_lastTriggerMs = 0;
        const unsigned long long nowMs = GetTickCount64();
        if (nowMs - s_lastTriggerMs < MiniMapConfig::kMarkerClickCooldownMs)
            return;
        s_lastTriggerMs = nowMs;

        // One physical press can also reach us several times in a row: MyGUI hands
        // the press to every widget in the chain (the map image *and* the scroll
        // view), and the game's handler may run once for each of them. While the
        // button is still down it is the same press, so it is consumed only once -
        // otherwise a single click dropped a marker and immediately picked it up
        // again, which in turn restarted the rename box and threw away the name
        // that had just been typed.
        //
        // The flag is cleared by the per-frame update once the button is up
        // again (see g_middlePressConsumed). Resetting it here would never run:
        // this function is only called while the button is down, so the flag
        // stayed set forever and swallowed every click after the first one.
        const bool downNow = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        if (downNow && g_middlePressConsumed)
            return;

        int cw = 0, ch = 0;
        if (!GetCanvasSize(ms, cw, ch))
        {
            cw = MiniMapConfig::kFallbackCanvas;
            ch = MiniMapConfig::kFallbackCanvas;
        }

        Ogre::Vector3 mouseWorld(0.0f, 0.0f, 0.0f);
        bool haveMouseWorld = false;
        try
        {
            haveMouseWorld = ms->getMouseWorldPosition(mouseWorld);
        }
        catch (...)
        {
            haveMouseWorld = false;
        }

        int mapX = fallbackX;
        int mapY = fallbackY;
        bool haveMap = haveFallback;
        const char* used = "eventCoords";

        if (haveMouseWorld)
        {
            try
            {
                const MyGUI::types::TPoint<int> mp = ms->worldToMapCoords(mouseWorld);
                const bool sane = mp.left >= -cw / 8 && mp.left <= cw + cw / 8 &&
                                  mp.top >= -ch / 8 && mp.top <= ch + ch / 8;
                if (sane)
                {
                    mapX = mp.left;
                    mapY = mp.top;
                    haveMap = true;
                    used = "mouseWorld";
                }
            }
            catch (...)
            {
            }
        }

        if (!haveMap)
        {
            Logger::Warn(prefix + "ignored, no usable map position");
            return;
        }

        const float nx = cw > 0 ? static_cast<float>(mapX) / static_cast<float>(cw) : 0.0f;
        const float ny = ch > 0 ? static_cast<float>(mapY) / static_cast<float>(ch) : 0.0f;

        if (downNow)
            g_middlePressConsumed = true;   // this press is handled, ignore its repeats

        Logger::Info(prefix + "used(" + used + ") map=(" + std::to_string(mapX) + "," +
            std::to_string(mapY) + ") canvas=" + std::to_string(cw) + "x" + std::to_string(ch) +
            " normalised=(" + std::to_string(nx) + "," + std::to_string(ny) + ")" +
            " mouseOverMap=" + (mouseOverMap ? "yes" : "no") +
            " mapShown=" + (BigMapShown(ms) ? "yes" : "no"));

        // Diagnostic for the player arrow's rotation: the game's own camera
        // marker points wherever the camera looks, i.e. in exactly the same
        // map-space direction space our arrow uses. If the two angles agree,
        // the arrow's rotation convention is confirmed and only the character's
        // forward axis can still be off.
        if (ms->cameraMarkerSkin && ou)
        {
            try
            {
                const float gameAngle = ms->cameraMarkerSkin->getAngle();
                const Ogre::Vector3 camWorld = ou->getCameraPos();
                Ogre::Vector3 focusWorld(0.0f, 0.0f, 0.0f);
                bool haveFocus = false;
                if (ou->player && ou->player->getAnyPlayerCharacter())
                {
                    focusWorld = ou->player->getAnyPlayerCharacter()->_NV_getPosition();
                    haveFocus = true;
                }
                else
                {
                    focusWorld = ou->getCameraCenter();
                    haveFocus = true;
                }

                if (haveFocus)
                {
                    const MyGUI::types::TPoint<int> a = ms->worldToMapCoords(camWorld);
                    const MyGUI::types::TPoint<int> b = ms->worldToMapCoords(focusWorld);
                    const float cdx = static_cast<float>(b.left - a.left);
                    const float cdy = static_cast<float>(b.top - a.top);
                    Logger::Info("MiniMap: arrow cross-check: gameCameraMarkerAngle=" +
                        std::to_string(gameAngle) + " myAngleForSameDirection=" +
                        std::to_string(std::atan2(cdx, -cdy)));
                }
            }
            catch (...)
            {
            }
        }

        // A second middle click next to an existing marker picks it up again.
        // The tolerance is a screen distance, so it is converted with the canvas
        // size of the current zoom.
        const float clickRadiusNorm = cw > 0
            ? static_cast<float>(MiniMapConfig::kMarkerClickRadiusCanvasPx) / static_cast<float>(cw)
            : MiniMapConfig::kMarkerReachRadiusNorm;
        const int removed = MarkerStore::Get().RemoveNearest(nx, ny, clickRadiusNorm);
        if (removed >= 0)
        {
            // A rename session points at a marker by *index*, so it has to follow
            // the removal: it either names the marker that just went (then it is
            // over) or it names one that shifted down by one.
            if (g_renaming)
            {
                if (static_cast<size_t>(removed) == g_renameIndex)
                    CancelRename();
                else if (static_cast<size_t>(removed) < g_renameIndex)
                    --g_renameIndex;
            }
            Logger::Info("MiniMap: picked up a marker");
            return;
        }

        const std::string name = "Marker " + std::to_string(MarkerStore::Get().Markers().size() + 1);
        const size_t index = MarkerStore::Get().Add(nx, ny, name);
        Logger::Info("MiniMap: dropped marker '" + name + "' (press Enter to name it)");
        g_pendingRename = static_cast<long long>(index);
    }

    // Is the mouse cursor inside the minimap rectangle (MyGUI screen pixels)?
    // The minimap is drawn on top of everything, so a middle click there belongs
    // to the minimap and not to the big map underneath it.
    bool CursorOverMinimap()
    {
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        if (!im)
            return false;
        const MyGUI::IntPoint mouse = im->getMousePosition();
        const int mx = mouse.left - MiniMapConfig::kOffsetLeft;
        const int my = mouse.top - MiniMapConfig::kOffsetTop;
        return mx >= 0 && my >= 0 && mx < MiniMapConfig::kWidth && my < MiniMapConfig::kHeight;
    }

    // Physical middle button on the big map. The game's own map mouse handler
    // and this poll may both see the same physical press; the de-duplication
    // inside ApplyBigMapMarkerClick() handles that, so the poll stays alive even
    // after the game has forwarded a middle click once. (Making it sticky used
    // to switch the poll off for good, which also broke clicks that arrive while
    // the big map is closed.)
    void PollBigMapMiddleClick()
    {
        static bool s_prevDown = false;
        const bool down = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool pressed = down && !s_prevDown;
        s_prevDown = down;

        if (!pressed)
            return;

        // A click inside the minimap rectangle is the minimap's business (see
        // PollMinimapClick) - the minimap covers that part of the big map.
        if (CursorOverMinimap())
            return;

        // Do not touch the log for middle clicks that are none of our business
        // (the button also rotates the camera in the world view).
        MapScreen* ms = GetMapScreen();
        if (!ms)
            return;
        if (!BigMapMouseOver(ms))
            return;

        if (!g_middlePollSawClick)
        {
            g_middlePollSawClick = true;
            Logger::Info("MiniMap: middle click is polled directly (the game does not forward it)");
        }
        ApplyBigMapMarkerClick("poll", false, false, 0, 0);
    }

    // Physical middle button on the minimap. The minimap sits in the top-left
    // corner and its widgets are mouse-transparent, so a click there can only be
    // picked up by polling. The cursor is turned into a world-map fraction
    // through the crop the background is showing, which makes the marker land
    // exactly under the cursor whatever the big map's zoom is. A click next to
    // one of the player's own markers picks that one up again.
    //
    // `enabled` is false while the overlay is hidden: the button edge is still
    // tracked (so a press that happens meanwhile is not replayed as a fresh click
    // when the minimap comes back), but nothing is dropped then.
    void PollMinimapClick(bool enabled)
    {
        static bool s_prevDown = false;
        const bool down = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool pressed = down && !s_prevDown;
        s_prevDown = down;
        if (!pressed || !enabled)
            return;

        // The minimap is drawn on top of everything, so a click inside its
        // rectangle is handled here and nowhere else (PollBigMapMiddleClick skips
        // this area). This must NOT test BigMapMouseOver(): that walks the
        // mouse-focus widget up to the map's mainWidget and stays true for a while
        // after the big map was hidden, which silently disabled every minimap
        // click.
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        if (!im)
            return;
        const MyGUI::IntPoint mouse = im->getMousePosition();
        const int mx = mouse.left - MiniMapConfig::kOffsetLeft;
        const int my = mouse.top - MiniMapConfig::kOffsetTop;
        if (mx < 0 || my < 0 || mx >= MiniMapConfig::kWidth || my >= MiniMapConfig::kHeight)
            return;

        // Only meaningful once the background shows a real crop; before the
        // player's position is known there is no mapping from minimap pixels to
        // world fractions.
        if (!g_viewSet || g_viewSrcW <= 0 || g_viewSrcH <= 0)
            return;

        float texW = static_cast<float>(MiniMapConfig::kFallbackCanvas);
        float texH = static_cast<float>(MiniMapConfig::kFallbackCanvas);
        if (g_bg && g_bgTextureSet)
        {
            const MyGUI::IntSize ts = g_bg->getImageSize();
            if (ts.width > 0 && ts.height > 0)
            {
                texW = static_cast<float>(ts.width);
                texH = static_cast<float>(ts.height);
            }
        }

        const float fsx = static_cast<float>(g_viewSrcX) +
            static_cast<float>(mx) / static_cast<float>(MiniMapConfig::kWidth) * static_cast<float>(g_viewSrcW);
        const float fsy = static_cast<float>(g_viewSrcY) +
            static_cast<float>(my) / static_cast<float>(MiniMapConfig::kHeight) * static_cast<float>(g_viewSrcH);
        const float nx = fsx / texW;
        const float ny = fsy / texH;

        // The pick-up tolerance is a screen distance, converted with the crop so
        // it means the same thing on the minimap whatever the big map's zoom is.
        const float radiusNorm = static_cast<float>(MiniMapConfig::kMinimapClickRadiusPx) /
            static_cast<float>(MiniMapConfig::kWidth) * (static_cast<float>(g_viewSrcW) / texW);

        const int removed = MarkerStore::Get().RemoveNearest(nx, ny, radiusNorm);
        if (removed >= 0)
        {
            if (g_renaming)
            {
                if (static_cast<size_t>(removed) == g_renameIndex)
                    CancelRename();
                else if (static_cast<size_t>(removed) < g_renameIndex)
                    --g_renameIndex;
            }
            Logger::Info("MiniMap: picked up a marker on the minimap");
            return;
        }

        const std::string name = "Marker " + std::to_string(MarkerStore::Get().Markers().size() + 1);
        const size_t index = MarkerStore::Get().Add(nx, ny, name);
        Logger::Info("MiniMap: dropped marker '" + name + "' on the minimap at (" +
            std::to_string(nx) + "," + std::to_string(ny) + ") (press Enter to name it)");
        g_pendingRename = static_cast<long long>(index);
    }

    // -------------------------------------------------------------------------
    // Widget creation
    // -------------------------------------------------------------------------
    bool EnsureCreated(MyGUI::Gui* mgr)
    {
        MyGUI::Widget* existing = nullptr;
        if (!TryStep("findWidget(root)", [&] { existing = mgr->findWidgetT(MiniMapConfig::kRootName, false); }))
            return false;

        if (existing)
        {
            if (g_created && existing == g_root)
                return true;

            // Leftover from a GUI clear or a partially failed creation:
            // drop it and rebuild from scratch so we never duplicate the name.
            ResetState();
            if (!TryStep("destroyWidget(leftover root)", [&] { mgr->destroyWidget(existing); }))
                return false;
        }

        MyGUI::Widget* root = nullptr;
        if (!TryStep("createWidget(root, skin=" + std::string(MiniMapConfig::kRootSkin) +
                     ", layer=" + MiniMapConfig::kLayer + ")",
                     [&] {
                         root = mgr->createWidget<MyGUI::Widget>(
                             MiniMapConfig::kRootSkin,
                             MiniMapConfig::kOffsetLeft, MiniMapConfig::kOffsetTop,
                             MiniMapConfig::kWidth, MiniMapConfig::kHeight,
                             MyGUI::Align::Left | MyGUI::Align::Top,
                             MiniMapConfig::kLayer,
                             MiniMapConfig::kRootName);
                     }))
            return false;
        if (!root)
        {
            ReportStepFailure("null", "createWidget(root)", "createWidget returned nullptr");
            return false;
        }

        g_root = root;
        // NOTE: every widget we create is made "mouse transparent"
        // (needMouseFocus == false) so the game's own input handling in the
        // top-left corner is untouched; all of our input is polled from the OS.
        // The call must go through MyGuiCompat - the SDK headers put WidgetInput
        // at the wrong offset and the plain call corrupted engine memory.
        TryStep("root->setNeedMouseFocus", [&] { MyGuiCompat::SetNeedMouseFocus(g_root, false); });
        // No MyGUI delegate is attached here - clicks are polled from the
        // per-frame update instead. See PollMinimapClick().

        MyGUI::ImageBox* bg = nullptr;
        if (TryStep("createWidget(bg, skin=" + std::string(MiniMapConfig::kImageSkin) + ")",
                    [&] {
                        bg = root->createWidget<MyGUI::ImageBox>(
                            MiniMapConfig::kImageSkin, 0, 0,
                            MiniMapConfig::kWidth, MiniMapConfig::kHeight,
                            MyGUI::Align::Default, "MiniMapBackground");
                    }) && bg)
        {
            // The map texture is loaded separately (and retried) from UpdateImpl:
            // it can only be resolved once the world's resources are up.
            TryStep("bg->setNeedMouseFocus", [&] { MyGuiCompat::SetNeedMouseFocus(bg, false); });
        }
        else
        {
            ReportStepFailure("null", "createWidget(bg)", "background ImageBox unavailable");
        }
        g_bg = bg;
        g_imageBoxVTable = MyGuiCompat::VTable(bg);

        // --- the player's own arrow (rotating skin) --------------------------
        // Rotation is applied to the widget's main sub-widget. RotatingSkin's
        // setAngle()/setCenter() are not virtual, so a plain cast is enough here
        // (and it avoids MyGUI's cross-module RTTI table).
        MyGUI::ImageBox* icon = nullptr;
        if (TryStep("createWidget(playerIcon)",
                    [&] {
                        icon = root->createWidget<MyGUI::ImageBox>(
                            MiniMapConfig::kPlayerIconSkin, 0, 0,
                            MiniMapConfig::kPlayerIconSize, MiniMapConfig::kPlayerIconSize,
                            MyGUI::Align::Default, "MiniMapPlayerIcon");
                    }) && icon)
        {
            TryStep("playerIcon->setNeedMouseFocus", [&] { MyGuiCompat::SetNeedMouseFocus(icon, false); });
            TryStep("playerIcon->setVisible(false)", [&] { icon->setVisible(false); });

            bool artOk = false;
            TryStep("playerIcon->setItemResource",
                    [&] {
                        artOk = icon->setItemResource(MiniMapConfig::kMarkerResource);
                        icon->setItemGroup(MiniMapConfig::kPlayerIconGroup);
                        icon->setItemName(MiniMapConfig::kPlayerIconGroup);
                    });
            if (!artOk)
                Logger::Warn("MiniMap: player arrow art '" +
                    std::string(MiniMapConfig::kPlayerIconGroup) + "' not found in " +
                    std::string(MiniMapConfig::kMarkerResource));

            // getSubWidgetMain() is inherited from SkinItem, whose offset inside
            // the engine's Widget differs from the SDK header - hence the helper.
            MyGUI::ISubWidgetRect* sub = nullptr;
            TryStep("playerIcon->getSubWidgetMain", [&] { sub = MyGuiCompat::SubWidgetMain(icon); });
            g_playerIconSkin = reinterpret_cast<MyGUI::RotatingSkin*>(sub);
            if (g_playerIconSkin)
            {
                // Rotate around the middle of the icon (the skin defaults to its
                // top-left corner).
                TryStep("playerIcon skin->setCenter",
                        [&] {
                            g_playerIconSkin->setCenter(MyGUI::IntPoint(
                                MiniMapConfig::kPlayerIconSize / 2,
                                MiniMapConfig::kPlayerIconSize / 2));
                        });
                Logger::Info("MiniMap: player arrow rotates (rotating skin at 0x" +
                    std::to_string(reinterpret_cast<unsigned long long>(sub)) + ")");
            }
            else
            {
                Logger::Warn("MiniMap: player arrow has no rotating skin - the arrow will not turn");
            }

            // A lower depth draws on top: the marker pool is created later (and
            // would otherwise cover the arrow when the player stands in a town).
            // (BUGFIX: this used to call setDepth on the *global*, which is still
            // null while the widget is being built.)
            if (g_bg)
                TryStep("playerIcon->setDepth", [&] { icon->setDepth(g_bg->getDepth() - 1); });

            g_playerIcon = icon;
        }

        // --- rename box ------------------------------------------------------
        // A plain TextBox: no keyboard focus is taken from the game, the typed
        // name is polled from the OS (see PollRenameKeys).
        MyGUI::TextBox* box = nullptr;
        if (TryStep("createWidget(renameBox)",
                    [&] {
                        box = root->createWidget<MyGUI::TextBox>(
                            MiniMapConfig::kRenameSkin, 6, MiniMapConfig::kHeight - 22,
                            MiniMapConfig::kWidth - 12, 18,
                            MyGUI::Align::Default, "MiniMapRenameBox");
                    }) && box)
        {
            TryStep("renameBox->setNeedMouseFocus", [&] { MyGuiCompat::SetNeedMouseFocus(box, false); });
            TryStep("renameBox->setTextColour", [&] { box->setTextColour(MyGUI::Colour::White); });
            // The skin is a layout, and applying a layout re-applies the layout's
            // own position - so the widget is placed explicitly *afterwards*.
            TryStep("renameBox->setCoord",
                    [&] {
                        box->setCoord(6, MiniMapConfig::kHeight - 22,
                                      MiniMapConfig::kWidth - 12, 18);
                    });
            if (g_bg)
                TryStep("renameBox->setDepth", [&] { box->setDepth(g_bg->getDepth() - 2); });
            TryStep("renameBox->setVisible(false)", [&] { box->setVisible(false); });
        }
        g_renameBox = box;
        g_textBoxVTable = MyGuiCompat::VTable(box);

        g_mgr = mgr;
        g_created = true;
        Logger::Info("MiniMap: overlay created (textbox vtable=0x" +
            std::to_string(reinterpret_cast<unsigned long long>(g_textBoxVTable)) +
            ", imagebox vtable=0x" +
            std::to_string(reinterpret_cast<unsigned long long>(g_imageBoxVTable)) + ")");
        return true;
    }

    MyGUI::ImageBox* AcquireMarker(size_t index)
    {
        if (index < g_pool.size())
            return g_pool[index];
        if (static_cast<int>(g_pool.size()) >= MiniMapConfig::kMaxMarkerWidgets)
            return nullptr;

        MyGUI::ImageBox* box = g_root->createWidget<MyGUI::ImageBox>(
            MiniMapConfig::kImageSkin, 0, 0, 10, 10, MyGUI::Align::Default, "");
        if (box)
        {
            MyGuiCompat::SetNeedMouseFocus(box, false);
            box->setVisible(false);
        }
        g_pool.push_back(box);
        g_poolGroup.push_back(std::string());
        return box;
    }

    void SetMarkerGroup(MyGUI::ImageBox* box, size_t index, const std::string& group)
    {
        if (index < g_poolGroup.size() && g_poolGroup[index] == group)
            return;
        if (!box->setItemResource(MiniMapConfig::kMarkerResource))
        {
            ReportStepFailure("missing", "image set " + std::string(MiniMapConfig::kMarkerResource),
                              "ImageBox::setItemResource returned false");
            return;
        }
        box->setItemGroup(group);
        box->setItemName(group);
        if (index < g_poolGroup.size())
            g_poolGroup[index] = group;
    }

    // -------------------------------------------------------------------------
    // The player's own markers on the *big* map.
    //
    // Markers are dropped by middle-clicking the big map, so they have to be
    // visible there as well. Their widgets are created *inside the game's own map
    // image widget* (MapScreen::mapImage == the whole-world canvas): a child of it
    // is positioned in map-canvas pixels, which is exactly the space
    // MapScreen::worldToMapCoords() returns, and MyGUI scrolls and clips it
    // together with the map. Nothing has to be repositioned while the player pans
    // or zooms - the map widget itself moves everything.
    // -------------------------------------------------------------------------
    void DropBigMapPool()
    {
        // The widgets live inside the game's map widget and are destroyed with it
        // (or with the whole GUI), so they are *never* destroyed from here - only
        // our pointers to them are dropped.
        g_bigIcons.clear();
        g_bigLabels.clear();
        g_bigGroup.clear();
        g_bigParent = nullptr;
        g_bigUsed = 0;
    }

    bool ChildListContains(const std::vector<MyGUI::Widget*>& children, MyGUI::Widget* widget)
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            if (children[i] == widget)
                return true;
        }
        return false;
    }

    // Are the widgets we created still children of the map image? The parent is
    // alive (it came from the game this very frame), so its child list can be read
    // and simply compared against our pointers - this is the one liveness test
    // that never dereferences a possibly destroyed widget.
    bool BigMapPoolAlive(MyGUI::Widget* parent)
    {
        if (g_bigIcons.empty() && g_bigLabels.empty())
            return true;
        if (!parent)
            return false;

        // 512 is a sanity limit - the map image only ever has our markers plus a
        // couple of the game's own pieces under it.
        size_t count = parent->getChildCount();
        if (count > 512)
            count = 512;
        std::vector<MyGUI::Widget*> children;
        children.reserve(count);
        for (size_t i = 0; i < count; ++i)
            children.push_back(parent->getChildAt(i));

        for (size_t i = 0; i < g_bigIcons.size(); ++i)
        {
            if (g_bigIcons[i] && !ChildListContains(children, g_bigIcons[i]))
                return false;
        }
        for (size_t i = 0; i < g_bigLabels.size(); ++i)
        {
            if (g_bigLabels[i] && !ChildListContains(children, g_bigLabels[i]))
                return false;
        }
        return true;
    }

    MyGUI::ImageBox* AcquireBigIcon(size_t index, MyGUI::Widget* parent)
    {
        if (index < g_bigIcons.size())
            return g_bigIcons[index];
        if (static_cast<int>(g_bigIcons.size()) >= MiniMapConfig::kMaxBigMapMarkers)
            return nullptr;

        MyGUI::ImageBox* box = nullptr;
        if (!TryStep("createWidget(big-map marker)",
                     [&] {
                         box = parent->createWidget<MyGUI::ImageBox>(
                             MiniMapConfig::kImageSkin, 0, 0, 16, 16, MyGUI::Align::Default, "");
                     }))
            return nullptr;
        if (box)
        {
            MyGuiCompat::SetNeedMouseFocus(box, false);
            box->setVisible(false);
        }
        g_bigIcons.push_back(box);
        g_bigGroup.push_back(std::string());
        return box;
    }

    MyGUI::TextBox* AcquireBigLabel(size_t index, MyGUI::Widget* parent)
    {
        if (index < g_bigLabels.size())
            return g_bigLabels[index];
        if (static_cast<int>(g_bigLabels.size()) >= MiniMapConfig::kMaxBigMapMarkers)
            return nullptr;

        MyGUI::TextBox* box = nullptr;
        if (!TryStep("createWidget(big-map label)",
                     [&] {
                         box = parent->createWidget<MyGUI::TextBox>(
                             MiniMapConfig::kLabelSkin, 0, 0, 120, 16, MyGUI::Align::Default, "");
                     }))
            return nullptr;
        if (box)
        {
            MyGuiCompat::SetNeedMouseFocus(box, false);
            TryStep("big label->setTextColour", [&] { box->setTextColour(MyGUI::Colour::White); });
            box->setVisible(false);
        }
        g_bigLabels.push_back(box);
        return box;
    }

    void SetBigMarkerGroup(MyGUI::ImageBox* box, size_t index, const std::string& group)
    {
        if (index < g_bigGroup.size() && g_bigGroup[index] == group)
            return;
        if (!box->setItemResource(MiniMapConfig::kMarkerResource))
        {
            ReportStepFailure("missing", "big image set " + std::string(MiniMapConfig::kMarkerResource),
                              "ImageBox::setItemResource returned false");
            return;
        }
        box->setItemGroup(group);
        box->setItemName(group);
        if (index < g_bigGroup.size())
            g_bigGroup[index] = group;
    }

    // Draws every marker of the player on the big map, at map-canvas pixels.
    void UpdateBigMapMarkers(MapScreen* ms)
    {
        if (!ms || !ms->mapImage)
            return;

        MyGUI::Widget* parent = ms->mapImage;
        if (g_bigParent != parent || !BigMapPoolAlive(parent))
        {
            DropBigMapPool();
            g_bigParent = parent;
        }

        const int cw = MyGuiCompat::Width(parent);
        const int ch = MyGuiCompat::Height(parent);
        if (cw <= 0 || ch <= 0)
            return;

        const auto& markers = MarkerStore::Get().Markers();
        int mw = 16, mh = 16;
        GetGroupSize(MiniMapConfig::kGroupCustom, mw, mh);

        size_t used = 0;
        for (size_t i = 0; i < markers.size() &&
                           used < static_cast<size_t>(MiniMapConfig::kMaxBigMapMarkers); ++i)
        {
            const int px = static_cast<int>(markers[i].nx * static_cast<float>(cw)) - mw / 2;
            const int py = static_cast<int>(markers[i].ny * static_cast<float>(ch)) - mh / 2;

            MyGUI::ImageBox* icon = AcquireBigIcon(used, parent);
            if (!icon)
                break;
            if (!TryStep("place big-map marker",
                         [&] {
                             SetBigMarkerGroup(icon, used, MiniMapConfig::kGroupCustom);
                             icon->setCoord(px, py, mw, mh);
                             icon->setVisible(true);
                         }))
                break;

            if (MyGUI::TextBox* label = AcquireBigLabel(used, parent))
            {
                TryStep("place big-map marker label",
                        [&] {
                            MyGuiCompat::UStringHolder text(markers[i].name);
                            label->setCaption(text.Get());
                            label->setCoord(px + mw + 3, py, 120, 16);
                            label->setVisible(true);
                        });
            }
            ++used;
        }

        for (size_t i = used; i < g_bigIcons.size(); ++i)
        {
            if (g_bigIcons[i])
                TryStep("hide unused big-map marker", [&] { g_bigIcons[i]->setVisible(false); });
        }
        for (size_t i = used; i < g_bigLabels.size(); ++i)
        {
            if (g_bigLabels[i])
                TryStep("hide unused big-map marker label", [&] { g_bigLabels[i]->setVisible(false); });
        }
        g_bigUsed = used;
    }
}

namespace MiniMapGui
{
    static void UpdateImpl(MyGUI::Gui* mgr);

    void Update()
    {
        if (!gui)
            return;
        MyGUI::Gui* mgr = gui->getGuiManager();
        if (!mgr)
            return;

        static bool firstUpdateLogged = false;
        if (!firstUpdateLogged)
        {
            firstUpdateLogged = true;
            Logger::Info("MiniMap: first overlay update (gui manager is live)");
        }

        // Individual steps report their own precise failure via TryStep(); this
        // outer catch is only a last-resort net so the game never sees a throw.
        try
        {
            UpdateImpl(mgr);
        }
        catch (const MyGUI::Exception& e)
        {
            static bool logged = false;
            if (!logged) { logged = true; Logger::Error("MiniMap: unhandled MyGUI exception: " + e.getFullDescription()); }
        }
        catch (const std::exception& e)
        {
            static bool logged = false;
            if (!logged) { logged = true; Logger::Error(std::string("MiniMap: unhandled exception: ") + e.what()); }
        }
        catch (...)
        {
            static bool logged = false;
            if (!logged) { logged = true; Logger::Error("MiniMap: unhandled unknown exception"); }
        }
    }

    // Is the game showing one of its own screens instead of the world? The
    // minimap is a permanent overlay, so without this it is drawn on top of the
    // title screen and of the character creation / character editor screens - and
    // it would eat middle clicks there (the character creation needs the whole
    // screen for its own controls).
    //
    // All three are recognised through the game's own state, so nothing has to be
    // guessed from the widget tree:
    //   * the title screen object exists (deleted once a game starts),
    //   * the character editor window is open (a new game uses EDIT_NEWGAME, the
    //     in-game editor EDIT_MIDGAME - both take over the screen),
    //   * the player is flagged as being in the character editor.
    // `why` (optional) receives which of them matched, for the log.
    bool MenuScreenActive(const char** why = nullptr)
    {
        if (::au && ::au->titleScreen)
        {
            if (why) *why = "title screen";
            return true;
        }
        if (::gui && ::gui->characterEditor)
        {
            if (why) *why = "character editor window";
            return true;
        }
        if (::ou && ::ou->player && ::ou->player->characterEditorMode)
        {
            if (why) *why = "player characterEditorMode";
            return true;
        }
        return false;
    }

    // What the game currently shows over the world, sampled once per frame.
    // It feeds two things: the overlay's visibility (see UpdateImpl) and the
    // "screen check" line in the log.
    struct ScreenProbe
    {
        const char* menuWhy = nullptr;     // title screen / character editor
        const char* windowWhy = nullptr;   // one of the game's interface windows

        // Raw signals behind windowWhy (diagnostics).
        int  openInventories = 0;
        bool statsWindow = false;
        bool dialogue = false;
        bool modalMessage = false;
        bool management = false;
        bool prospecting = false;
        bool options = false;

        bool InMenu() const { return menuWhy != nullptr; }
        bool WindowOpen() const { return windowWhy != nullptr; }
        bool Hidden() const { return InMenu() || WindowOpen(); }
    };

    // The minimap is a permanent overlay, so it has to step aside for the game's
    // own *interface windows* as well: inventory (I, loot, trade), character
    // stats (C), dialogues, message boxes, the management window (map (M),
    // squads, research, factions, AI), prospecting and the options window. While
    // one of them is up the player is working in that window - the minimap would
    // cover a corner of it and swallow middle clicks there.
    //
    // Every check is one of the game's own queries, never a guess from the widget
    // tree, so a window this build does not know about simply does not hide the
    // overlay instead of hiding it forever.
    ScreenProbe ProbeScreens()
    {
        ScreenProbe p;
        MenuScreenActive(&p.menuWhy);

        // A menu screen replaces the world, so the windows below cannot be up -
        // and their singletons may not even exist yet.
        if (p.InMenu())
            return p;

        if (::gui)
        {
            p.openInventories = ::gui->getNumOpenInventoryWindows();
            p.statsWindow = ::gui->isStatsWindowOpen();
            p.dialogue = ::gui->inDialogue();
            p.modalMessage = ::gui->hasModalMessage();
        }

        if (ManagementScreen* mgmt = ManagementScreen::getSingleton())
            p.management = mgmt->getVisible();

        // Only asked for once a world (and its player) exists: these two
        // singletons are created with the world.
        if (::ou && ::ou->player)
        {
            if (ProspectingWindow* prospecting = ProspectingWindow::getSingleton())
                p.prospecting = prospecting->getVisible();
            if (OptionsWindow* options = OptionsWindow::getSingleton())
                p.options = options->isVisible();
        }

        if (p.openInventories > 0)
            p.windowWhy = "inventory window";
        else if (p.statsWindow)
            p.windowWhy = "character stats window";
        else if (p.dialogue)
            p.windowWhy = "dialogue";
        else if (p.modalMessage)
            p.windowWhy = "modal message box";
        else if (p.management)
            p.windowWhy = "management screen (map / squads / research / factions)";
        else if (p.prospecting)
            p.windowWhy = "prospecting window";
        else if (p.options)
            p.windowWhy = "options window";
        return p;
    }

    static void UpdateImpl(MyGUI::Gui* mgr)
    {
        // The GUI manager is checked every frame: the game rebuilds (and
        // destroys) its whole MyGUI tree on world loads, which turns every widget
        // pointer we hold into a dangling one. On a mismatch the overlay state is
        // dropped *without touching the old pointers* and rebuilt from scratch.
        if (g_created && (g_mgr != mgr || !RootIsAlive(mgr)))
        {
            static int s_rebuilds = 0;
            if (s_rebuilds < 20)
            {
                ++s_rebuilds;
                Logger::Info(g_mgr != mgr
                    ? "MiniMap: GUI manager changed - rebuilding the overlay"
                    : "MiniMap: our root widget is gone - rebuilding the overlay");
            }
            ResetState();
            return;
        }

        // --- what the game shows over the world -------------------------------
        // While the title screen, the character creation or the character editor
        // is up, the overlay must not exist at all: it would cover those screens
        // and swallow clicks. While one of the game's *interface windows* is up
        // (inventory, stats, dialogue, map / squads, ...) the world keeps running
        // and the overlay is only hidden - it is updated as usual, so it is
        // correct again the moment the window closes.
        //
        // The state is sampled every frame, so closing the window brings the
        // minimap straight back.
        const ScreenProbe probe = ProbeScreens();
        const bool inMenu = probe.InMenu();
        const bool hidden = probe.Hidden();

        // Diagnostics: the heartbeat below is only written while the overlay is
        // actually drawn, so the raw state is reported on its own (throttled) -
        // this is what tells a hidden overlay apart from a missed one.
        {
            static unsigned long long s_lastScreenLog = 0;
            const unsigned long long logNow = GetTickCount64();
            if (logNow - s_lastScreenLog >= 15000)
            {
                s_lastScreenLog = logNow;
                Logger::Info(std::string("MiniMap: screen check - inMenu=") +
                    (inMenu ? "yes" : "no") +
                    " titleScreen=" + ((::au && ::au->titleScreen) ? "yes" : "no") +
                    " characterEditor=" + ((::gui && ::gui->characterEditor) ? "yes" : "no") +
                    " editorMode=" + ((::ou && ::ou->player && ::ou->player->characterEditorMode) ? "yes" : "no") +
                    " player=" + ((::ou && ::ou->player) ? "yes" : "no") +
                    " window=" + (probe.windowWhy ? probe.windowWhy : "none") +
                    " inventories=" + std::to_string(probe.openInventories) +
                    " stats=" + (probe.statsWindow ? "yes" : "no") +
                    " dialogue=" + (probe.dialogue ? "yes" : "no") +
                    " modal=" + (probe.modalMessage ? "yes" : "no") +
                    " management=" + (probe.management ? "yes" : "no") +
                    " prospecting=" + (probe.prospecting ? "yes" : "no") +
                    " options=" + (probe.options ? "yes" : "no"));
            }
        }

        // The first ForgottenGUI::update() after a world load can fire while the
        // game is still rebuilding its GUI, so widget creation is retried with a
        // short backoff instead of being attempted once at an unsafe moment.
        if (!g_created)
        {
            // Nothing is created before a world (and its player) exists: the
            // widgets would be built into a GUI that is still being assembled.
            // Nothing is created on a menu screen either (see above). An open
            // interface window does not matter here - the overlay is created
            // hidden and shows up when the window closes.
            if (inMenu || !ou || !ou->player)
                return;

            static unsigned long long s_nextAttempt = 0;
            static int s_attempts = 0;
            const unsigned long long nowMs = GetTickCount64();
            if (nowMs < s_nextAttempt)
                return;
            if (!EnsureCreated(mgr))
            {
                ++s_attempts;
                s_nextAttempt = nowMs + 1000;
                if (s_attempts == 1 || (s_attempts % 15) == 0)
                    Logger::Warn("MiniMap: overlay creation attempt " + std::to_string(s_attempts) +
                        " failed; will retry");
                return;
            }
        }

        if (g_root && hidden != g_overlayHidden)
        {
            g_overlayHidden = hidden;
            Logger::Info(hidden
                ? std::string("MiniMap: overlay hidden (") +
                      (probe.menuWhy ? probe.menuWhy : (probe.windowWhy ? probe.windowWhy : "menu screen")) + ")"
                : std::string("MiniMap: overlay shown again"));
            TryStep("root->setVisible", [&] { g_root->setVisible(!hidden); });
        }
        if (inMenu)
        {
            // A name box must never be left up over the character creation.
            if (g_renaming)
                CancelRename();
            return;
        }
        if (hidden)
        {
            // The overlay is not drawn while a window covers the screen, so a
            // rename box that is still up would take the keyboard without being
            // visible, and a half-typed name belongs to a marker the player can
            // no longer see. Drop both; the automatic name stays.
            if (g_renaming)
                CancelRename();
            g_pendingRename = -1;
        }

        const unsigned long long now = GetTickCount64();
        MapScreen* ms = GetMapScreen();
        // The map canvas equals the world map texture size (4096x4096 per the
        // layout). If the map widget has not been built yet, fall back to the
        // known size so the overlay works from the very first frame.
        int canvasW = 0, canvasH = 0;
        if (!GetCanvasSize(ms, canvasW, canvasH))
        {
            canvasW = MiniMapConfig::kFallbackCanvas;
            canvasH = MiniMapConfig::kFallbackCanvas;
        }

        std::vector<MapEntry> visible;
        visible.reserve(64);
        std::vector<WorldEntry> movement;

        // --- the character the player controls (not the camera) --------------
        // The minimap is centred on the character, and the arrow turns with the
        // character's facing. Orbiting the camera therefore no longer drags the
        // whole map around while the arrow stays put.
        Character* playerCharacter = nullptr;
        Ogre::Vector3 playerPos(0.0f, 0.0f, 0.0f);
        bool havePlayer = false;
        if (ou && ou->player)
        {
            if (TryStep("player->getAnyPlayerCharacter",
                        [&] { playerCharacter = ou->player->getAnyPlayerCharacter(); }) && playerCharacter)
            {
                if (TryStep("character->_NV_getPosition",
                            [&] { playerPos = playerCharacter->_NV_getPosition(); }))
                    havePlayer = true;
            }
        }
        g_havePlayer = havePlayer;

        // --- harvest the game's own town / character markers -----------------
        // Reading the map screen's marker widgets is what makes towns and
        // characters appear without the big map ever having been opened.
        const bool mapShown = BigMapShown(ms);
        if (ms && (g_walkSource != ms ||
                   now - g_lastWalkMs >= static_cast<unsigned long long>(MiniMapConfig::kHarvestIntervalMs)))
        {
            g_lastWalkMs = now;

            std::vector<MapEntry> walk;
            int found = 0;
            try
            {
                found = HarvestMapMarkers(ms, walk);
            }
            catch (...)
            {
                // A walk that fails must never take the frame down with it: the
                // previous harvest stays in place.
                found = 0;
            }
            if (found > 0)
            {
                g_walkMarkers.swap(walk);
                g_walkStampMs = now;
                g_walkSource = ms;
            }
            else if (g_walkSource != ms)
            {
                // a different map screen and nothing to read: forget the old one
                g_walkMarkers.clear();
                g_walkStampMs = 0;
                g_walkSource = nullptr;
            }
        }

        const bool haveWalk = !g_walkMarkers.empty() &&
            (now - g_walkStampMs) < MiniMapConfig::kHarvestKeepMs;

        // Only ask the game to rebuild its markers while our own harvest comes up
        // empty: with the map closed the game does not keep them up to date, but
        // it does recreate them on refresh().
        if (ms && !haveWalk &&
            now - g_lastMarkerRefreshMs >= static_cast<unsigned long long>(MiniMapConfig::kMarkerRefreshIntervalMs))
        {
            g_lastMarkerRefreshMs = now;
            if (!mapShown)
            {
                if (TryStep("mapScreen->refresh", [&] { ms->refresh(); }))
                    Logger::Info("MiniMap: asked the game to rebuild its map markers (nothing to read yet)");
            }
        }

        // --- a marker the player walked up to is picked up again -------------
        // (done further down, once the player's position on the map is known)

        // Which source fed this frame's town/character markers (diagnostics).
        bool usedHooks = false;
        size_t hookedCount = 0;
        {
            std::lock_guard<std::mutex> lk(g_mutex);

            // The widget harvest is the primary source: it is a complete
            // snapshot of every marker the game has on its map, and the game
            // keeps it fresh (the harvest runs while the big map is closed as
            // well). The hook-recorded markers are only a fallback - the game
            // calls update() for a marker only when it actually refreshes that
            // one, so the hooks routinely see a small subset (measured in-game:
            // 2 hook markers against 16 harvested ones) and using them as the
            // primary source hides all the other towns.
            hookedCount = g_townMarkers.size() + g_charMarkers.size();

            if (haveWalk)
            {
                visible.insert(visible.end(), g_walkMarkers.begin(), g_walkMarkers.end());
            }
            else
            {
                usedHooks = hookedCount > 0;
                for (auto it = g_townMarkers.begin(); it != g_townMarkers.end();)
                {
                    if (it->second.expireMs != 0 && now > it->second.expireMs)
                        it = g_townMarkers.erase(it);
                    else
                    {
                        visible.push_back(it->second);
                        ++it;
                    }
                }
                for (auto it = g_charMarkers.begin(); it != g_charMarkers.end();)
                {
                    if (it->second.expireMs != 0 && now > it->second.expireMs)
                        it = g_charMarkers.erase(it);
                    else
                    {
                        visible.push_back(it->second);
                        ++it;
                    }
                }
            }

            // destination markers the player dropped on the big map
            for (auto it = g_movementMarkers.begin(); it != g_movementMarkers.end();)
            {
                if (it->expireMs != 0 && now > it->expireMs)
                    it = g_movementMarkers.erase(it);
                else
                {
                    movement.push_back(*it);
                    ++it;
                }
            }
        }

        // world -> map-image conversion happens outside the lock
        if (ms)
        {
            for (const auto& w : movement)
            {
                MyGUI::types::TPoint<int> mp(0, 0);
                if (!TryStep("movement marker -> map coords",
                             [&] { mp = ms->worldToMapCoords(Ogre::Vector3(w.x, w.y, w.z)); }))
                    break;
                MapEntry e{ static_cast<float>(mp.left) / static_cast<float>(canvasW),
                            static_cast<float>(mp.top) / static_cast<float>(canvasH),
                            MiniMapConfig::kGroupMovement, w.expireMs };
                visible.push_back(e);
            }
        }

        // The player's own markers, straight in world-map fractions. They are
        // always shown, so they are appended last: when the widget pool runs out
        // they win.
        if (ms)
        {
            const auto& userMarkers = MarkerStore::Get().Markers();
            for (size_t i = 0; i < userMarkers.size(); ++i)
            {
                MapEntry e{ userMarkers[i].nx, userMarkers[i].ny,
                            MiniMapConfig::kGroupCustom, 0,
                            true, userMarkers[i].name };
                visible.push_back(e);
            }
        }

        // player position - this also chooses the visible region
        Ogre::Vector3 centre(0.0f, 0.0f, 0.0f);
        bool haveCentre = false;
        if (havePlayer)
        {
            centre = playerPos;
            haveCentre = true;
        }
        else if (ou)
        {
            if (TryStep("ou->getCameraCenter", [&] { centre = ou->getCameraCenter(); }))
                haveCentre = true;
        }

        if (ms && haveCentre)
        {
            MyGUI::types::TPoint<int> mp(0, 0);
            if (TryStep("centre -> map coords",
                        [&] { mp = ms->worldToMapCoords(centre); }))
            {
                g_lastCamPx = mp.left;
                g_lastCamPy = mp.top;
                g_lastCanvasW = canvasW;
                g_lastCanvasH = canvasH;
                g_lastCamNx = canvasW > 0 ? static_cast<float>(mp.left) / static_cast<float>(canvasW) : -1.0f;
                g_lastCamNy = canvasH > 0 ? static_cast<float>(mp.top) / static_cast<float>(canvasH) : -1.0f;
                g_lastPlayerNx = g_lastCamNx;
                g_lastPlayerNy = g_lastCamNy;

                static bool loggedLayout = false;
                if (!loggedLayout)
                {
                    loggedLayout = true;
                    Logger::Info("MiniMap: map canvas=" + std::to_string(canvasW) + "x" + std::to_string(canvasH) +
                        " centre(" + std::string(havePlayer ? "character" : "camera") + ") px=(" +
                        std::to_string(mp.left) + "," + std::to_string(mp.top) + ")" +
                        " normalised=(" + std::to_string(g_lastCamNx) + "," + std::to_string(g_lastCamNy) + ")" +
                        " viewZoom=1/" + std::to_string(static_cast<int>(MiniMapConfig::kViewZoom)));
                }
            }
        }

        // A marker the player has walked up to is picked up again. The distance is
        // a fraction of the world map (see kMarkerReachViewFraction), so it means
        // the same thing on the minimap whatever the big map's zoom is.
        if (havePlayer && g_lastCamNx >= 0.0f && !MarkerStore::Get().Markers().empty())
        {
            const int reached = MarkerStore::Get().RemoveReached(
                g_lastCamNx, g_lastCamNy, MiniMapConfig::kMarkerReachRadiusNorm,
                MiniMapConfig::kMarkerGraceMs);
            if (reached > 0)
            {
                // The rename session refers to a marker by index, which may just
                // have shifted - so it is finished before anything else happens.
                if (g_renaming)
                    CommitRename();
                Logger::Info("MiniMap: reached " + std::to_string(reached) + " marker(s) - removed");
            }
        }

        // ---------------------------------------------------------------------
        // Visible region of the world map texture: a zoomed window centred on
        // the player.
        //
        // Everything here is measured in *texture* pixels. The game's map canvas
        // is resized when the player zooms the big map, so canvas pixels change
        // meaning from frame to frame - only the fraction of the world (nx, ny)
        // is stable, and setImageCoord() needs real texture pixels.
        // ---------------------------------------------------------------------
        int texW = MiniMapConfig::kFallbackCanvas;
        int texH = MiniMapConfig::kFallbackCanvas;
        if (g_bg && g_bgTextureSet)
        {
            const MyGUI::IntSize ts = g_bg->getImageSize();
            if (ts.width > 0 && ts.height > 0)
            {
                texW = ts.width;
                texH = ts.height;
            }
        }

        float viewX = 0.0f, viewY = 0.0f;
        float viewW = 0.0f, viewH = 0.0f;
        bool cropped = false;

        if (g_lastCamNx >= 0.0f && g_lastCamNy >= 0.0f)
        {
            viewW = static_cast<float>(texW) / MiniMapConfig::kViewZoom;
            viewH = static_cast<float>(texH) / MiniMapConfig::kViewZoom;
            if (viewW > static_cast<float>(texW)) viewW = static_cast<float>(texW);
            if (viewH > static_cast<float>(texH)) viewH = static_cast<float>(texH);

            if (viewW > 0.0f && viewH > 0.0f)
            {
                const float camTexX = g_lastCamNx * static_cast<float>(texW);
                const float camTexY = g_lastCamNy * static_cast<float>(texH);
                viewX = ClampF(camTexX - viewW * 0.5f, 0.0f, static_cast<float>(texW) - viewW);
                viewY = ClampF(camTexY - viewH * 0.5f, 0.0f, static_cast<float>(texH) - viewH);
                cropped = true;
            }
        }

        // Background = the current crop of the world map texture.
        //
        // MyGUI's ImageBox builds a grid of *tiles*: it splits ImageCoord into
        // pieces of ImageTile size. Passing ImageTile = the whole texture made
        // that grid empty (512/4096 = 0 pieces), so nothing was drawn at all.
        // Setting ImageCoord alone creates exactly one piece, and the main skin
        // stretches that UV region over the whole widget - which is the zoom.
        if (g_bg)
        {
            if (!g_bgTextureSet)
            {
                if (TryStep("bg->setImageTexture(" + std::string(MiniMapConfig::kMapTexture) + ")",
                            [&] { g_bg->setImageTexture(MiniMapConfig::kMapTexture); }))
                    g_bgTextureSet = true;
            }

            if (g_bgTextureSet && cropped)
            {
                const int sx = static_cast<int>(viewX);
                const int sy = static_cast<int>(viewY);
                const int sw = static_cast<int>(viewW);
                const int sh = static_cast<int>(viewH);

                if (!g_viewSet || sx != g_viewSrcX || sy != g_viewSrcY ||
                    sw != g_viewSrcW || sh != g_viewSrcH)
                {
                    if (TryStep("bg->setImageCoord",
                                [&] {
                                    g_bg->setImageTile(MyGUI::IntSize(sw, sh));
                                    g_bg->setImageCoord(MyGUI::IntCoord(sx, sy, sw, sh));
                                }))
                    {
                        g_viewSet = true;
                        g_viewSrcX = sx;
                        g_viewSrcY = sy;
                        g_viewSrcW = sw;
                        g_viewSrcH = sh;
                    }
                }
            }
        }

        // ---------------------------------------------------------------------
        // The player's own arrow: position on the minimap + facing direction.
        //
        // The facing is turned into a *map-space* direction (the world position
        // is projected twice), so the arrow orientation does not depend on the
        // world axis conventions at all. The arrow art points up, and MyGUI
        // rotates a RotatingSkin clockwise for positive angles, which makes
        // atan2(dx, -dy) the angle for the map-space direction (dx, dy).
        // ---------------------------------------------------------------------
        if (g_playerIcon && cropped && viewW > 0.0f && viewH > 0.0f)
        {
            const int iw = MiniMapConfig::kPlayerIconSize;
            const int ih = MiniMapConfig::kPlayerIconSize;

            const int px = static_cast<int>((g_lastPlayerNx * static_cast<float>(texW) - viewX) /
                                            viewW * static_cast<float>(MiniMapConfig::kWidth)) - iw / 2;
            const int py = static_cast<int>((g_lastPlayerNy * static_cast<float>(texH) - viewY) /
                                            viewH * static_cast<float>(MiniMapConfig::kHeight)) - ih / 2;

            float angle = g_lastPlayerAngle;
            if (havePlayer && ms && playerCharacter)
            {
                Ogre::Vector3 facing(0.0f, 0.0f, -1.0f);   // Ogre objects face -Z
                if (TryStep("character->_NV_getOrientation",
                            [&] {
                                facing = playerCharacter->_NV_getOrientation() *
                                         Ogre::Vector3(0.0f, 0.0f, -1.0f);
                            }))
                {
                    MyGUI::types::TPoint<int> a(0, 0), b(0, 0);
                    if (TryStep("facing -> map coords",
                                [&] {
                                    a = ms->worldToMapCoords(playerPos);
                                    b = ms->worldToMapCoords(playerPos + facing * 256.0f);
                                }))
                    {
                        const float dx = static_cast<float>(b.left - a.left);
                        const float dy = static_cast<float>(b.top - a.top);
                        if (dx * dx + dy * dy > 0.5f)
                        {
                            angle = std::atan2(dx, -dy) + MiniMapConfig::kPlayerIconAngleOffset;
                            angle *= MiniMapConfig::kPlayerIconAngleSign;
                            g_lastPlayerAngle = angle;
                        }
                    }
                }
            }

            TryStep("place player arrow",
                    [&] {
                        g_playerIcon->setCoord(px, py, iw, ih);
                        g_playerIcon->setVisible(true);
                    });
            if (g_playerIconSkin)
                TryStep("playerIcon setAngle", [&] { g_playerIconSkin->setAngle(angle); });
        }
        else if (g_playerIcon)
        {
            TryStep("hide player arrow", [&] { g_playerIcon->setVisible(false); });
        }

        // refresh the widget pool
        int used = 0;
        int labelUsed = 0;
        const int cap = MiniMapConfig::kMaxMarkerWidgets;
        for (size_t i = 0; i < visible.size() && used < cap; ++i)
        {
            int mw = 10, mh = 10;
            GetGroupSize(visible[i].group, mw, mh);

            // world-fraction position of this marker -> minimap widget position
            int px = 0, py = 0;
            if (cropped)
            {
                const float mpx = visible[i].nx * static_cast<float>(texW);
                const float mpy = visible[i].ny * static_cast<float>(texH);

                // markers outside the visible window are not drawn - except the
                // player's own markers, which are pinned to the edge instead
                if (!visible[i].clampEdge &&
                    (mpx < viewX - mw || mpx > viewX + viewW + mw ||
                     mpy < viewY - mh || mpy > viewY + viewH + mh))
                    continue;

                px = static_cast<int>((mpx - viewX) / viewW * static_cast<float>(MiniMapConfig::kWidth)) - mw / 2;
                py = static_cast<int>((mpy - viewY) / viewH * static_cast<float>(MiniMapConfig::kHeight)) - mh / 2;

                if (visible[i].clampEdge)
                {
                    px = ClampI(px, 2, MiniMapConfig::kWidth - mw - 2);
                    py = ClampI(py, 2, MiniMapConfig::kHeight - mh - 2);
                }
            }
            else
            {
                px = static_cast<int>(visible[i].nx * MiniMapConfig::kWidth) - mw / 2;
                py = static_cast<int>(visible[i].ny * MiniMapConfig::kHeight) - mh / 2;
            }

            MyGUI::ImageBox* box = nullptr;
            if (!TryStep("acquire marker widget",
                         [&] { box = AcquireMarker(static_cast<size_t>(used)); }))
                break;
            if (!box)
                break;

            const size_t idx = static_cast<size_t>(used);
            const std::string group = visible[i].group;

            if (!TryStep("place marker group " + group,
                         [&] {
                             SetMarkerGroup(box, idx, group);
                             box->setCoord(px, py, mw, mh);
                             box->setVisible(true);
                         }))
                break;

            // name of the player's own markers, right next to the icon
            if (!visible[i].label.empty() && labelUsed < MiniMapConfig::kMaxLabelWidgets)
            {
                MyGUI::TextBox* label = AcquireLabel(static_cast<size_t>(labelUsed));
                if (label)
                {
                    const int lx = ClampI(px + mw + 2, 0, MiniMapConfig::kWidth - 60);
                    const int ly = ClampI(py + 2, 0, MiniMapConfig::kHeight - 14);
                    if (TryStep("place marker label",
                                [&] {
                                    // The caption needs a MyGUI::UString, which is
                                    // built inside the engine's own class size (see
                                    // MyGuiCompat::UStringHolder).
                                    MyGuiCompat::UStringHolder text(visible[i].label);
                                    label->setCaption(text.Get());
                                    label->setCoord(lx, ly, 58, 14);
                                    label->setVisible(true);
                                }))
                        ++labelUsed;
                }
            }

            ++used;
        }
        // Hide whatever the pool did not use this frame. These loops only ever run
        // right after the liveness check at the top of UpdateImpl, but they are
        // wrapped anyway: a widget the game removed must never be dereferenced
        // unguarded.
        for (size_t i = static_cast<size_t>(used); i < g_pool.size(); ++i)
        {
            if (g_pool[i])
                TryStep("hide unused marker", [&] { g_pool[i]->setVisible(false); });
        }
        for (size_t i = static_cast<size_t>(labelUsed); i < g_labels.size(); ++i)
        {
            if (g_labels[i])
                TryStep("hide unused label", [&] { g_labels[i]->setVisible(false); });
        }

        // The rename box belongs to a visible overlay: while it is hidden (an
        // interface window is up) the box stays off, so its polled keys cannot
        // take the game's keyboard either. The minimap's own middle click is
        // polled every frame but only acts while the overlay is visible.
        if (!hidden)
            UpdateRenameBox();
        PollMinimapClick(!hidden);

        // Same markers, now on the game's own big map. They stay live even while
        // the overlay is hidden - opening the big map is one of the windows that
        // hides the minimap, and the player's markers must still be there.
        UpdateBigMapMarkers(ms);

        // A middle press is handled once; as soon as the button is up again the
        // flag is cleared so the next press is handled too. This has to happen in
        // the per-frame update - the click handler itself only runs while the
        // button is down and could never clear it.
        if ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) == 0)
            g_middlePressConsumed = false;

        // Middle-click triggers, polled because the game does not forward them to
        // the overlay (we are mouse-transparent): on the big map
        // (PollBigMapMiddleClick; the minimap's own poll runs above and only while
        // the overlay is visible). They are mutually exclusive - a click inside
        // the minimap rectangle is the minimap's business and the big-map poll
        // skips that area.
        PollBigMapMiddleClick();

        // Periodic proof that the per-frame driver is alive and that the camera
        // really tracks the player.
        static unsigned long long s_lastHeartbeat = 0;
        static unsigned long long s_updates = 0;
        ++s_updates;
        const unsigned long long tick = GetTickCount64();
        if (tick - s_lastHeartbeat >= 5000)
        {
            s_lastHeartbeat = tick;
            Logger::Info("MiniMap: heartbeat updates=" + std::to_string(s_updates) +
                " drawn=" + std::to_string(used) + "/" + std::to_string(visible.size()) +
                " src=" + std::string(usedHooks ? "hooks" : (haveWalk ? "widgets" : "none")) +
                " hooks=" + std::to_string(hookedCount) +
                " walked=" + std::to_string(g_walkMarkers.size()) +
                " age=" + std::to_string(g_walkStampMs ? (tick - g_walkStampMs) / 1000 : 0) + "s" +
                " user=" + std::to_string(MarkerStore::Get().Markers().size()) +
                " player=" + std::string(g_havePlayer ? "yes" : "no") +
                " playerNorm=(" + std::to_string(g_lastPlayerNx) + "," + std::to_string(g_lastPlayerNy) + ")" +
                " ang=" + std::to_string(g_lastPlayerAngle) +
                " canvas=" + std::to_string(canvasW) + "x" + std::to_string(canvasH) +
                " tex=" + std::to_string(texW) + "x" + std::to_string(texH) +
                " crop=(" + std::to_string(g_viewSrcX) + "," + std::to_string(g_viewSrcY) + "," +
                std::to_string(g_viewSrcW) + "x" + std::to_string(g_viewSrcH) + ")" +
                (g_viewSet ? "" : " NO-CROP") +
                " overlay=" + (hidden ? "hidden" : "shown") +
                " mapShown=" + (mapShown ? "yes" : "no") +
                " mouseOverMap=" + (BigMapMouseOver(ms) ? "yes" : "no") +
                " mouseHook=" + (g_hookSawMiddleClick ? "yes" : "no") +
                " arrow=" + (g_playerIconSkin ? "rotating" : "fixed"));
            // One extra line, but only while the player has markers of their own:
            // it is the number that decides when a marker is picked up again.
            if (!MarkerStore::Get().Markers().empty())
            {
                float worldUnits = 0.0f;
                if (ms && ms->worldSize.x > 1.0f && ms->worldSize.x < 1000000.0f)
                    worldUnits = ms->worldSize.x;
                const float nearest = NearestUserMarkerNorm(g_lastPlayerNx, g_lastPlayerNy);
                Logger::Info("MiniMap: markers on the big map=" + std::to_string(g_bigUsed) +
                    " reach=" + std::to_string(MiniMapConfig::kMarkerReachRadiusNorm * worldUnits) +
                    "m nearest=" + std::to_string(nearest > 0.0f ? nearest * worldUnits : 0.0f) +
                    "m world=" + std::to_string(worldUnits));
            }
        }
    }

    void OnMovementMarker(float worldX, float worldY, float worldZ, float ttlSeconds)
    {
        float ttl = ttlSeconds;
        if (!(ttl > 0.0f))
            ttl = MiniMapConfig::kMovementMarkerDefaultTtl;
        if (ttl < MiniMapConfig::kMovementMarkerMinTtl)
            ttl = MiniMapConfig::kMovementMarkerMinTtl;
        if (ttl > MiniMapConfig::kMovementMarkerMaxTtl)
            ttl = MiniMapConfig::kMovementMarkerMaxTtl;

        WorldEntry e{ worldX, worldY, worldZ,
                      GetTickCount64() + static_cast<unsigned long long>(ttl * 1000.0f) };

        std::lock_guard<std::mutex> lk(g_mutex);
        g_movementMarkers.push_back(e);
        if (g_movementMarkers.size() > 64)
            g_movementMarkers.erase(g_movementMarkers.begin());
    }

    // Identity of a map marker: the game's own handle, which is the first member
    // of both MapMarkerTown and MapMarkerCharacter. hand's layout
    // (kenshi/util/hand.h): 0x14 = index, 0x18 = serial. Reading the two numbers
    // is enough for identity and avoids mirroring a class that has a vtable.
    unsigned long long MarkerHandleKey(const void* marker)
    {
        if (!marker)
            return 0ULL;
        const char* p = static_cast<const char*>(marker);
        unsigned int index = 0;
        unsigned int serial = 0;
        std::memcpy(&index, p + 0x14, sizeof(index));
        std::memcpy(&serial, p + 0x18, sizeof(serial));
        return (static_cast<unsigned long long>(serial) << 32) | index;
    }

    void OnTownMarker(const void* handle, int mapX, int mapY)
    {
        MapScreen* ms = GetMapScreen();
        int cw = 0, ch = 0;
        if (!ms || !GetCanvasSize(ms, cw, ch))
            return;

        // Towns never move, so kTownMarkerTtl == 0 keeps them once we know them.
        const float ttl = MiniMapConfig::kTownMarkerTtl;
        const unsigned long long expire = ttl > 0.0f
            ? GetTickCount64() + static_cast<unsigned long long>(ttl * 1000.0f)
            : 0ULL;

        MapEntry e{ static_cast<float>(mapX) / static_cast<float>(cw),
                    static_cast<float>(mapY) / static_cast<float>(ch),
                    MiniMapConfig::kGroupTown,
                    expire };

        const unsigned long long key = MarkerHandleKey(handle);
        std::lock_guard<std::mutex> lk(g_mutex);
        if (ms != g_hookSource)
        {
            g_townMarkers.clear();
            g_charMarkers.clear();
            g_hookSource = ms;
        }
        g_townMarkers[key] = e;
    }

    void OnCharacterMarker(const void* handle, int mapX, int mapY)
    {
        MapScreen* ms = GetMapScreen();
        int cw = 0, ch = 0;
        if (!ms || !GetCanvasSize(ms, cw, ch))
            return;

        const float ttl = MiniMapConfig::kCharMarkerTtl;
        const unsigned long long expire = ttl > 0.0f
            ? GetTickCount64() + static_cast<unsigned long long>(ttl * 1000.0f)
            : 0ULL;

        MapEntry e{ static_cast<float>(mapX) / static_cast<float>(cw),
                    static_cast<float>(mapY) / static_cast<float>(ch),
                    MiniMapConfig::kGroupCharacter,
                    expire };

        const unsigned long long key = MarkerHandleKey(handle);
        std::lock_guard<std::mutex> lk(g_mutex);
        if (ms != g_hookSource)
        {
            g_townMarkers.clear();
            g_charMarkers.clear();
            g_hookSource = ms;
        }
        g_charMarkers[key] = e;
    }

    // The game's own map mouse handler. It gives the exact widget event
    // coordinates, and getting called at all proves that middle clicks reach
    // MyGUI (otherwise PollBigMapMiddleClick() handles them).
    void OnBigMapMousePressed(void* mapScreen, MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id)
    {
        MapScreen* ms = static_cast<MapScreen*>(mapScreen);
        if (!ms || id != MyGUI::MouseButton::Middle)
            return;

        g_hookSawMiddleClick = true;

        // Event coordinates are relative to the widget that was pressed, so when
        // that is the scroll view instead of the map image the scroll offset has
        // to be added to get map-image pixels. They are only a fallback - the
        // position normally comes from the game itself.
        int mx = left;
        int my = top;
        const char* where = "other";
        if (sender == ms->mapImage)
        {
            where = "mapImage";
        }
        else if (ms->mapScrollView && sender == ms->mapScrollView)
        {
            where = "mapScrollView";
            const MyGUI::IntPoint off = ms->mapScrollView->getViewOffset();
            mx += off.left;
            my += off.top;
        }

        ApplyBigMapMarkerClick(where, true, true, mx, my);
    }

    void Shutdown()
    {
        ResetState();
    }
}
