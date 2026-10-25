#pragma once

#include <mygui/MyGUI_Widget.h>
#include <mygui/MyGUI_MouseButton.h>

// -----------------------------------------------------------------------------
// MiniMap overlay.
//
// Update() is driven once per frame from the KingOfRenderThread::mainloop() hook
// (UI thread). The On* functions are called from the game hooks and only record
// data; all widget work happens in Update().
// -----------------------------------------------------------------------------
namespace MiniMapGui
{
    // Creates / refreshes the overlay. Safe to call every frame.
    void Update();

    // Mirrors a destination marker the player dropped on the big map.
    void OnMovementMarker(float worldX, float worldY, float worldZ, float ttlSeconds);

    // Mirrors a town / character map marker (positions are map-image pixels).
    void OnTownMarker(const void* handle, int mapX, int mapY);
    void OnCharacterMarker(const void* handle, int mapX, int mapY);

    // Middle click on the big map: drop / pick up one of the player's own markers.
    void OnBigMapMousePressed(void* mapScreen, MyGUI::Widget* sender,
                              int left, int top, MyGUI::MouseButton id);

    // Drops every cached widget / marker reference (used on shutdown).
    void Shutdown();
}
