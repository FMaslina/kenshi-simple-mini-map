#include "pch.h"
#include "../include/MarkerStore.h"
#include "../include/MiniMapConfig.h"
#include "../include/Logger.h"
#include <fstream>
#include <sstream>

namespace
{
    std::wstring Widen(const char* ascii)
    {
        std::wstring out;
        if (!ascii) return out;
        for (const char* p = ascii; *p; ++p)
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        return out;
    }

    // Directory the MiniMap.dll itself lives in - that is where the mod is
    // installed, so the marker file sits next to the mod.
    std::wstring ModuleDirectory()
    {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&ModuleDirectory), &mod))
        {
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(mod, path, MAX_PATH) > 0)
            {
                std::wstring dir(path);
                size_t sep = dir.find_last_of(L"\\/");
                if (sep != std::wstring::npos)
                    return dir.substr(0, sep + 1);
            }
        }
        wchar_t exe[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0)
        {
            std::wstring dir(exe);
            size_t sep = dir.find_last_of(L"\\/");
            if (sep != std::wstring::npos)
                return dir.substr(0, sep + 1);
        }
        return L".\\";
    }

    std::string Trim(const std::string& s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return std::string();
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
}

MarkerStore& MarkerStore::Get()
{
    static MarkerStore s_instance;
    return s_instance;
}

// File format, one marker per line:   name <TAB> nx <TAB> ny
void MarkerStore::Load()
{
    m_markers.clear();
    m_path = ModuleDirectory();
    std::wstring fileName = m_path + Widen(MiniMapConfig::kMarkersFileName);

    std::ifstream in(fileName.c_str());
    if (!in)
    {
        Logger::Info("MarkerStore: no existing marker file (starting empty)");
        return;
    }

    size_t skipped = 0;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;

        std::istringstream ss(line);
        std::string name;
        std::string sx, sy;
        if (!std::getline(ss, name, '\t') ||
            !std::getline(ss, sx, '\t') ||
            !std::getline(ss, sy, '\t'))
        {
            ++skipped;
            continue;
        }

        CustomMarker m{};
        m.name = Trim(name);
        try
        {
            m.nx = std::stof(Trim(sx));
            m.ny = std::stof(Trim(sy));
        }
        catch (...)
        {
            ++skipped;
            continue;
        }
        m.createdMs = 0;
        if (m.name.empty())
            m.name = "Marker " + std::to_string(m_markers.size() + 1);
        m_markers.push_back(m);
    }

    Logger::Info("MarkerStore: loaded " + std::to_string(m_markers.size()) + " marker(s)" +
        (skipped ? ", skipped " + std::to_string(skipped) + " unreadable line(s)" : ""));
}

void MarkerStore::Save()
{
    std::wstring fileName = m_path + Widen(MiniMapConfig::kMarkersFileName);

    std::ofstream out(fileName.c_str(), std::ios::trunc);
    if (!out)
    {
        Logger::Warn("MarkerStore: could not write marker file");
        return;
    }
    for (const auto& m : m_markers)
        out << m.name << '\t' << m.nx << '\t' << m.ny << '\n';
}

size_t MarkerStore::Add(float nx, float ny, const std::string& name)
{
    CustomMarker m{};
    m.name = name;
    m.nx = nx;
    m.ny = ny;
    m.createdMs = GetTickCount64();
    m_markers.push_back(m);
    Save();
    return m_markers.size() - 1;
}

int MarkerStore::RemoveNearest(float nx, float ny, float radiusNorm)
{
    float best = radiusNorm * radiusNorm;
    size_t bestIndex = m_markers.size();
    for (size_t i = 0; i < m_markers.size(); ++i)
    {
        const float dx = m_markers[i].nx - nx;
        const float dy = m_markers[i].ny - ny;
        const float d = dx * dx + dy * dy;
        if (d <= best)
        {
            best = d;
            bestIndex = i;
        }
    }
    if (bestIndex == m_markers.size())
        return -1;
    m_markers.erase(m_markers.begin() + bestIndex);
    Save();
    return static_cast<int>(bestIndex);
}

int MarkerStore::RemoveReached(float nx, float ny, float radiusNorm, unsigned long long graceMs)
{
    const float radiusSq = radiusNorm * radiusNorm;
    const unsigned long long now = GetTickCount64();

    int removed = 0;
    for (size_t i = 0; i < m_markers.size();)
    {
        const CustomMarker& m = m_markers[i];
        const bool young = (m.createdMs != 0) && (now - m.createdMs < graceMs);

        const float dx = m.nx - nx;
        const float dy = m.ny - ny;
        if (!young && (dx * dx + dy * dy) <= radiusSq)
        {
            m_markers.erase(m_markers.begin() + i);
            ++removed;
        }
        else
        {
            ++i;
        }
    }

    if (removed > 0)
        Save();
    return removed;
}

void MarkerStore::SetName(size_t index, const std::string& name)
{
    if (index >= m_markers.size() || name.empty())
        return;
    m_markers[index].name = name;
    Save();
}

void MarkerStore::Clear()
{
    m_markers.clear();
    Save();
}
