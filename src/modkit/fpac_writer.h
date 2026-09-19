#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {
uint32_t FpacHash(const std::string& internalPath);

struct FpacItem {
    std::string  path;
    uint64_t     size = 0;
    uint64_t     srcOffset = 0;
    std::wstring srcFile;
};

using FpacProgress = std::function<void(uint64_t)>;

bool FpacWrite(const std::wstring& srcPac,
               std::vector<FpacItem> items,
               const std::wstring& target,
               const FpacProgress& onProgress,
               std::string& err);

class FpacExtractor {
public:
    bool Open(const std::wstring& pacPath, std::string& err);
    bool ExtractTo(const FpacItem& it, const std::wstring& outFile,
                   const FpacProgress& onProgress, std::string& err);

private:
    std::ifstream in_;
    bool          opened_ = false;
};

}
}
