#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ed9loader {
namespace modkit {

class FpacReader {
public:
    struct Entry { std::string name; uint64_t hash = 0, size = 0, location = 0; };

    bool Open(const std::wstring& pacPath);
    bool ReadEntry(const std::string& name, std::vector<uint8_t>& out) const;
    bool Has(const std::string& name) const;
    size_t Count() const { return entries_.size(); }
    const std::vector<Entry>& Entries() const { return entries_; }

private:
    std::wstring path_;
    std::vector<Entry> entries_;
};

}
}
