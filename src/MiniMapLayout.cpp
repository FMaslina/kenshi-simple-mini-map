#include "pch.h"
#include "../include/MiniMapLayout.h"
#include "../include/MiniMapConfig.h"
#include "../include/Logger.h"

#include <fstream>
#include <sstream>
#include <string>

namespace
{
    int g_left   = MiniMapConfig::kOffsetLeft;
    int g_top    = MiniMapConfig::kOffsetTop;
    int g_width  = MiniMapConfig::kWidth;
    int g_height = MiniMapConfig::kHeight;

    // Directory the MiniMap.dll itself lives in - that is where the mod is
    // installed, so the layout file sits next to the mod (same place the marker
    // file uses).
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

    std::wstring FileName()
    {
        std::wstring name;
        for (const char* p = MiniMapConfig::kLayoutFileName; p && *p; ++p)
            name.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        return ModuleDirectory() + name;
    }

    std::string Describe()
    {
        return std::to_string(g_width) + "x" + std::to_string(g_height) +
            " at (" + std::to_string(g_left) + "," + std::to_string(g_top) + ")";
    }
}

namespace MiniMapLayout
{
    void Load()
    {
        Reset();

        std::ifstream in(FileName().c_str());
        if (!in)
        {
            Logger::Info("MiniMapLayout: no layout file - default size " + Describe());
            return;
        }

        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            std::istringstream ss(line);
            int left = 0, top = 0, width = 0, height = 0;
            if (!(ss >> left >> top >> width >> height))
            {
                Logger::Warn("MiniMapLayout: unreadable layout line '" + line +
                    "' - keeping the default size");
                return;
            }

            Set(left, top, width, height);
            Logger::Info("MiniMapLayout: loaded " + Describe());
            return;
        }

        Logger::Info("MiniMapLayout: empty layout file - default size " + Describe());
    }

    void Save()
    {
        std::ofstream out(FileName().c_str(), std::ios::trunc);
        if (!out)
        {
            Logger::Warn("MiniMapLayout: could not write the layout file");
            return;
        }
        out << "# MiniMap layout: left top width height\n";
        out << g_left << ' ' << g_top << ' ' << g_width << ' ' << g_height << '\n';
    }

    int Width()  { return g_width; }
    int Height() { return g_height; }
    int Left()   { return g_left; }
    int Top()    { return g_top; }

    bool Set(int left, int top, int width, int height)
    {
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        if (width < MiniMapConfig::kMinSize)
            width = MiniMapConfig::kMinSize;
        if (height < MiniMapConfig::kMinSize)
            height = MiniMapConfig::kMinSize;

        if (left == g_left && top == g_top && width == g_width && height == g_height)
            return false;

        g_left = left;
        g_top = top;
        g_width = width;
        g_height = height;
        return true;
    }

    void Reset()
    {
        g_left = MiniMapConfig::kOffsetLeft;
        g_top = MiniMapConfig::kOffsetTop;
        g_width = MiniMapConfig::kWidth;
        g_height = MiniMapConfig::kHeight;
    }
}
