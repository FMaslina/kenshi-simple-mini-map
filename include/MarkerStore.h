#pragma once
#include <string>
#include <vector>

// Markers the player drops on the big map with the middle mouse button.
//
// Stored as a fraction of the whole world map (0..1) rather than as map pixels:
// the game's map canvas is resized whenever the player zooms the big map, so
// only the fraction of the world survives a zoom change or a restart.
struct CustomMarker
{
    std::string name;
    float nx;
    float ny;
    unsigned long long createdMs;   // runtime only (grace period for the reach test)
};

class MarkerStore
{
public:
    static MarkerStore& Get();

    void Load();
    void Save();

    const std::vector<CustomMarker>& Markers() const { return m_markers; }

    // Returns the index of the marker that was added.
    size_t Add(float nx, float ny, const std::string& name);

    // Removes the closest marker within radiusNorm (world-map fractions) and
    // returns its index, or -1 when nothing was close enough.
    int RemoveNearest(float nx, float ny, float radiusNorm);

    // Removes every marker the player is standing on. Returns how many went.
    int RemoveReached(float nx, float ny, float radiusNorm, unsigned long long graceMs);

    void SetName(size_t index, const std::string& name);
    void Clear();

private:
    MarkerStore() {}

    std::vector<CustomMarker> m_markers;
    std::wstring m_path;
};
