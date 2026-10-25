#include "pch.h"
#include "../include/Logger.h"
#include <Debug.h>
#include <mutex>
#include <vector>

namespace
{
    HANDLE g_log = INVALID_HANDLE_VALUE;
    std::mutex g_logMutex;
    std::string g_logPathUtf8;

    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 1) return {};
        std::string out(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], n, nullptr, nullptr);
        return out;
    }

    HANDLE TryOpen(const std::wstring& path)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return h;
        SetFilePointer(h, 0, nullptr, FILE_END);
        return h;
    }

    void WriteLineRaw(const char* level, const std::string& message)
    {
        if (g_log == INVALID_HANDLE_VALUE) return;
        const std::string line = std::string("[") + level + "] " + message + "\r\n";
        DWORD written = 0;
        WriteFile(g_log, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        FlushFileBuffers(g_log);
    }
}

namespace Logger
{
    void Init()
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_log != INVALID_HANDLE_VALUE) return;

        std::vector<std::wstring> candidates;
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0)
        {
            std::wstring dir(exePath);
            size_t sep = dir.find_last_of(L"\\/");
            if (sep != std::wstring::npos)
                candidates.push_back(dir.substr(0, sep + 1) + L"MiniMap.log");
        }
        wchar_t temp[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, temp) > 0)
            candidates.push_back(std::wstring(temp) + L"MiniMap.log");

        for (const auto& c : candidates)
        {
            HANDLE h = TryOpen(c);
            if (h != INVALID_HANDLE_VALUE)
            {
                g_log = h;
                g_logPathUtf8 = ToUtf8(c);
                break;
            }
        }
        if (g_log == INVALID_HANDLE_VALUE)
        {
            OutputDebugStringA("MiniMap: could not open any MiniMap.log candidate\n");
            return;
        }
        WriteLineRaw("Info", std::string("Logger initialized: ") + g_logPathUtf8);
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_log != INVALID_HANDLE_VALUE)
        {
            WriteLineRaw("Info", "Logger shutdown");
            CloseHandle(g_log);
            g_log = INVALID_HANDLE_VALUE;
        }
    }

    std::string LogFilePath()
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        return g_logPathUtf8;
    }

    void Info(const std::string& msg)
    {
        DebugLog((std::string("MiniMap: ") + msg).c_str());
        std::lock_guard<std::mutex> lk(g_logMutex);
        WriteLineRaw("Info", msg);
    }

    void Warn(const std::string& msg)
    {
        DebugLog((std::string("MiniMap WARN: ") + msg).c_str());
        std::lock_guard<std::mutex> lk(g_logMutex);
        WriteLineRaw("Warn", msg);
    }

    void Error(const std::string& msg)
    {
        ErrorLog((std::string("MiniMap ERROR: ") + msg).c_str());
        std::lock_guard<std::mutex> lk(g_logMutex);
        WriteLineRaw("Error", msg);
    }
}
