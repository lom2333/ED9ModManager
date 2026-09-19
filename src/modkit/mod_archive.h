#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
namespace archive {
bool IsArchiveFile(const std::filesystem::path& p);

std::string ArchiveModName(const std::filesystem::path& p);

std::filesystem::path StagingRoot(const std::filesystem::path& modsDir);

std::filesystem::path FindArchive(const std::filesystem::path& modsDir, const std::wstring& name);

std::vector<std::string> ListArchiveMods(const std::filesystem::path& modsDir);

std::filesystem::path EnsureStaged(const std::filesystem::path& archive,
                                   const std::filesystem::path& stagingRoot,
                                   std::string& err, bool* changed = nullptr);

int EnsureAllStaged(const std::filesystem::path& modsDir, std::string& log);

std::string BackendSummary();

}
}
}
