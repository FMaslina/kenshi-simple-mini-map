#include "pch.h"
#include "../include/Logger.h"
#include "../include/KenshiMirror.h"
#include "../include/MiniMapGui.h"
#include "../include/MarkerStore.h"

#include <core/Functions.h>
#include <Debug.h>

namespace
{
    bool g_initialized = false;

    // --- original trampolines ------------------------------------------------
    void (*g_origGuiUpdate)(ForgottenGUI*) = nullptr;
    void (*g_origMainloop)(KingOfRenderThread*, float) = nullptr;
    void (*g_origMapUpdate)(MapScreen*) = nullptr;
    void (*g_origSetMovementMarker)(MapScreen*, const Ogre::Vector3&, float) = nullptr;
    void (*g_origTownUpdate)(MapScreen::MapMarkerTown*, const MyGUI::types::TPoint<int>&, MapZoomLevel) = nullptr;
    void (*g_origCharUpdate)(MapScreen::MapMarkerCharacter*, const MyGUI::types::TPoint<int>&) = nullptr;
    void (*g_origMapMousePressed)(MapScreen*, MyGUI::Widget*, int, int, MyGUI::MouseButton) = nullptr;

    // --- detours -------------------------------------------------------------
    // Primary per-frame driver: KingOfRenderThread::mainloop() runs once per
    // rendered frame, so the minimap and its camera dot stay live in-world even
    // though the game only refreshes town/character markers while the big map
    // is open.
    void MainloopHook(KingOfRenderThread* self, float time)
    {
        if (g_origMainloop)
            g_origMainloop(self, time);
        MiniMapGui::Update();
    }

    // Secondary driver: ForgottenGUI::update() (fires less often than mainloop).
    void GuiUpdateHook(ForgottenGUI* self)
    {
        if (g_origGuiUpdate)
            g_origGuiUpdate(self);
        MiniMapGui::Update();
    }

    // Secondary driver (safety net): MapScreen::update() keeps the overlay
    // fresh while the big map is open even if the GUI hook fires late.
    void MapUpdateHook(MapScreen* self)
    {
        if (g_origMapUpdate)
            g_origMapUpdate(self);
        MiniMapGui::Update();
    }

    void SetMovementMarkerHook(MapScreen* self, const Ogre::Vector3& p, float time)
    {
        if (g_origSetMovementMarker)
            g_origSetMovementMarker(self, p, time);
        MiniMapGui::OnMovementMarker(p.x, p.y, p.z, time);
    }

    void TownUpdateHook(MapScreen::MapMarkerTown* self, const MyGUI::types::TPoint<int>& pos, MapZoomLevel zoom)
    {
        if (g_origTownUpdate)
            g_origTownUpdate(self, pos, zoom);
        MiniMapGui::OnTownMarker(self, pos.left, pos.top);
    }

    void CharUpdateHook(MapScreen::MapMarkerCharacter* self, const MyGUI::types::TPoint<int>& pos)
    {
        if (g_origCharUpdate)
            g_origCharUpdate(self, pos);
        MiniMapGui::OnCharacterMarker(self, pos.left, pos.top);
    }

    // Middle click on the big map: the game ignores the middle button, so it is
    // free for the player's own markers.
    void MapMousePressedHook(MapScreen* self, MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id)
    {
        if (g_origMapMousePressed)
            g_origMapMousePressed(self, sender, left, top, id);
        MiniMapGui::OnBigMapMousePressed(self, sender, left, top, id);
    }

    template <typename TFunc, typename TOrig>
    bool InstallHook(TFunc target, void* detour, TOrig** original)
    {
        const intptr_t addr = KenshiLib::GetRealAddress(target);
        if (!addr)
            return false;
        return KenshiLib::SUCCESS == KenshiLib::AddHook(addr, detour, original);
    }

    void PluginInitImpl()
    {
        Logger::Init();

        // Visible even without our own log file.
        DebugLog("MiniMap: startPlugin entry point reached");

        Logger::Info("MiniMap plugin starting (KenshiLib, target Kenshi 1.0.65)");

        MarkerStore::Get().Load();

        const bool move = InstallHook(&MapScreen::setMovementMarker, (void*)&SetMovementMarkerHook, &g_origSetMovementMarker);
        const bool town = InstallHook(&MapScreen::MapMarkerTown::update, (void*)&TownUpdateHook, &g_origTownUpdate);
        const bool chr = InstallHook(&MapScreen::MapMarkerCharacter::update, (void*)&CharUpdateHook, &g_origCharUpdate);
        const bool mapUpd = InstallHook(&MapScreen::update, (void*)&MapUpdateHook, &g_origMapUpdate);
        const bool guiHook = InstallHook(&ForgottenGUI::update, (void*)&GuiUpdateHook, &g_origGuiUpdate);
        const bool loop = InstallHook(&KingOfRenderThread::mainloop, (void*)&MainloopHook, &g_origMainloop);
        const bool mouse = InstallHook(&MapScreen::mapMousePressed, (void*)&MapMousePressedHook, &g_origMapMousePressed);

        Logger::Info(std::string("MiniMap: hooks - mainloop=") + (loop ? "ok" : "FAIL") +
            " gui=" + (guiHook ? "ok" : "FAIL") +
            " mapUpdate=" + (mapUpd ? "ok" : "FAIL") +
            " movement=" + (move ? "ok" : "FAIL") +
            " town=" + (town ? "ok" : "FAIL") +
            " character=" + (chr ? "ok" : "FAIL") +
            " mapMouse=" + (mouse ? "ok" : "FAIL"));

        Logger::Info("MiniMap: log file at " + Logger::LogFilePath());
    }
}

// Canonical RE_Kenshi plugin entry point.
__declspec(dllexport) void startPlugin()
{
    if (g_initialized)
        return;
    g_initialized = true;
    PluginInitImpl();
}
