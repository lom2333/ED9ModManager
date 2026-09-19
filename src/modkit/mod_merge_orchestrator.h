#pragma once
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
namespace orchestrator {

struct Paths {
    std::wstring modsDir;
    std::wstring pacSteamDir;
    std::wstring cacheDir;
    std::wstring schemasDir;
};

struct RunResult {
    int merged = 0, skipped = 0, failed = 0, mods = 0, injected = 0, tbls = 0, conflicts = 0, assets = 0;
    std::string log;
};

struct ModInfo { std::string name; bool enabled = true; std::vector<std::string> disabled; };

Paths FromGameDir(const std::wstring& gameDir);

std::vector<ModInfo> ScanMods(const std::wstring& modsDir);
std::wstring ModRoot(const std::wstring& modsDir, const std::string& modName);
bool SaveMods(const std::wstring& modsDir, const std::vector<ModInfo>& mods);

RunResult Run(const Paths& paths, bool force = false);

}
}
}
