#pragma once
// -----------------------------------------------------------------------------
// MiniMap - the overlay's live size and position.
//
// The defaults live in MiniMapConfig (kWidth / kHeight / kOffset*), but the
// player can change them at runtime by dragging one of the minimap's edges, so
// they cannot be compile-time constants any more. The last layout is written
// next to MiniMap.dll so a resize survives a restart.
//
// Only the GUI thread touches this (Update() runs on the game's UI thread).
// -----------------------------------------------------------------------------
namespace MiniMapLayout
{
    // Reads MiniMapLayout.txt. A missing or unreadable file keeps the defaults
    // from MiniMapConfig. Called once on plugin start.
    void Load();

    // Writes the current layout next to the DLL. Called when the player has
    // finished resizing, not every frame.
    void Save();

    int Width();
    int Height();
    int Left();
    int Top();

    // Stores a new layout. The size is raised to MiniMapConfig::kMinSize and the
    // position is kept on screen by the caller (which is the only place that
    // knows the current view size). Returns true when something changed.
    bool Set(int left, int top, int width, int height);

    // Back to the MiniMapConfig defaults (not persisted until Save()).
    void Reset();
}
