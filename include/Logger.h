#pragma once
#include <string>

// Small file logger. Writes MiniMap.log next to the game executable (or the
// DLL / temp dir as fallbacks) and mirrors every line into the RE_Kenshi log.
namespace Logger
{
    void Init();
    void Shutdown();
    void Info(const std::string& msg);
    void Warn(const std::string& msg);
    void Error(const std::string& msg);
    std::string LogFilePath();
}
