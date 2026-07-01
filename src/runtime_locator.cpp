#include "runtime_locator.h"

#include "safe_scan.h"

#include <Windows.h>
#include <winnt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sora_console::runtime_locator {
namespace {

struct ImageSection {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    [[nodiscard]] bool Contains(std::uintptr_t address) const { return address >= begin && address < end; }
};

struct ImageLayout {
    std::uintptr_t base = 0;
    std::size_t image_size = 0;
    ImageSection text;
    ImageSection rdata;
    [[nodiscard]] bool Contains(std::uintptr_t address) const {
        return address >= base && address < (base + image_size);
    }
};

// MSVC RTTI x64 Complete Object Locator(字段为 image-relative RVA)。
struct CompleteObjectLocator64 {
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t cd_offset;
    std::int32_t type_descriptor_rva;
    std::int32_t class_descriptor_rva;
    std::int32_t self_rva;
};

struct VtableCandidate {
    std::uintptr_t address = 0;
    std::string type_name;
    bool exact_type = false;
};

std::mutex g_cache_mutex;
std::optional<ImageLayout> g_image;

[[nodiscard]] bool IsReadableProtection(DWORD protect) {
    const DWORD n = protect & 0xFFU;
    return n == PAGE_READONLY || n == PAGE_READWRITE || n == PAGE_WRITECOPY ||
           n == PAGE_EXECUTE_READ || n == PAGE_EXECUTE_READWRITE || n == PAGE_EXECUTE_WRITECOPY;
}

[[nodiscard]] std::optional<MEMORY_BASIC_INFORMATION> QueryRegion(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return std::nullopt;
    }
    return mbi;
}

[[nodiscard]] bool IsReadableRange(std::uintptr_t address, std::size_t byte_count) {
    if (address == 0 || byte_count == 0) {
        return false;
    }
    const auto region = QueryRegion(address);
    if (!region || region->State != MEM_COMMIT || !IsReadableProtection(region->Protect) ||
        (region->Protect & PAGE_GUARD) != 0) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(region->BaseAddress);
    const auto end = begin + region->RegionSize;
    return address >= begin && address <= end && byte_count <= (end - address);
}

[[nodiscard]] std::optional<ImageLayout> BuildMainImageLayout() {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (module == 0) {
        return std::nullopt;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return std::nullopt;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return std::nullopt;
    }
    ImageLayout layout{};
    layout.base = module;
    layout.image_size = nt->OptionalHeader.SizeOfImage;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        std::string name(reinterpret_cast<const char*>(section->Name),
                         reinterpret_cast<const char*>(section->Name) + 8);
        name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
        ImageSection entry{
            module + section->VirtualAddress,
            module + section->VirtualAddress +
                std::max<std::uint32_t>(section->Misc.VirtualSize, section->SizeOfRawData),
        };
        if (name == ".text") {
            layout.text = entry;
        } else if (name == ".rdata") {
            layout.rdata = entry;
        }
    }
    if (layout.text.begin == 0 || layout.rdata.begin == 0) {
        return std::nullopt;
    }
    return layout;
}

[[nodiscard]] ImageLayout GetImageLayout() {
    std::scoped_lock lock(g_cache_mutex);
    if (!g_image) {
        g_image = BuildMainImageLayout();
    }
    return g_image.value_or(ImageLayout{});
}

[[nodiscard]] std::string ReadAsciiFromImage(const ImageLayout& image, std::uintptr_t address, std::size_t max_length) {
    if (!image.Contains(address)) {
        return {};
    }
    std::string out;
    out.reserve(max_length);
    for (std::size_t i = 0; i < max_length && image.Contains(address + i); ++i) {
        const char ch = *reinterpret_cast<const char*>(address + i);
        if (ch == '\0') {
            break;
        }
        if (static_cast<unsigned char>(ch) < 32U || static_cast<unsigned char>(ch) > 126U) {
            return {};
        }
        out.push_back(ch);
    }
    return out;
}

[[nodiscard]] std::vector<VtableCandidate> FindVtablesByTypeName(const ImageLayout& image, std::string_view type_fragment) {
    std::vector<VtableCandidate> matches;
    std::unordered_set<std::uintptr_t> seen;
    const std::string exact_type_name = ".?AV" + std::string(type_fragment) + "@@";

    for (std::uintptr_t entry = image.rdata.begin + sizeof(void*);
         entry + sizeof(void*) <= image.rdata.end; entry += sizeof(void*)) {
        const auto col_address = *reinterpret_cast<const std::uintptr_t*>(entry - sizeof(void*));
        if (!image.rdata.Contains(col_address)) {
            continue;
        }
        const auto* col = reinterpret_cast<const CompleteObjectLocator64*>(col_address);
        if (col->signature > 1U) {
            continue;
        }
        const auto type_descriptor = image.base + static_cast<std::intptr_t>(col->type_descriptor_rva);
        const auto name_address = type_descriptor + (sizeof(void*) * 2U);
        const std::string type_name = ReadAsciiFromImage(image, name_address, 160U);
        if (type_name.empty() || type_name.find(type_fragment) == std::string::npos) {
            continue;
        }
        const auto first_virtual = *reinterpret_cast<const std::uintptr_t*>(entry);
        if (!image.text.Contains(first_virtual)) {
            continue;
        }
        if (seen.insert(entry).second) {
            matches.push_back({entry, type_name, type_name == exact_type_name});
        }
    }

    std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
        if (left.exact_type != right.exact_type) {
            return left.exact_type;
        }
        return left.address < right.address;
    });
    return matches;
}

[[nodiscard]] std::optional<std::pair<std::uintptr_t, std::uintptr_t>> GetMainDataRange() {
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (module == 0) {
        return std::nullopt;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return std::nullopt;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return std::nullopt;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        std::string name(reinterpret_cast<const char*>(section->Name),
                         reinterpret_cast<const char*>(section->Name) + 8);
        name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
        if (name == ".data") {
            const std::uintptr_t begin = module + section->VirtualAddress;
            const std::uintptr_t end =
                begin + std::max<std::uint32_t>(section->Misc.VirtualSize, section->SizeOfRawData);
            return std::make_pair(begin, end);
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::uintptr_t SafeDeref(std::uintptr_t address) {
    if (!IsReadableRange(address, sizeof(std::uintptr_t))) {
        return 0;
    }
    std::uintptr_t value = 0;
    if (!safe_scan::SafeReadBytes(address, &value, sizeof(value))) {
        return 0;
    }
    return value;
}

[[nodiscard]] std::uintptr_t FindInstance(std::uintptr_t vtable_va) {
    if (vtable_va == 0) {
        return 0;
    }
    const auto range = GetMainDataRange();
    if (!range) {
        return 0;
    }
    for (std::uintptr_t address = range->first;
         address + sizeof(std::uintptr_t) <= range->second; address += sizeof(std::uintptr_t)) {
        std::uintptr_t candidate = 0;
        if (!safe_scan::SafeReadBytes(address, &candidate, sizeof(candidate))) {
            continue;
        }
        if (candidate < 0x10000 || (candidate & 7U) != 0) {
            continue;
        }
        if (SafeDeref(candidate) == vtable_va) {
            return candidate;
        }
    }
    return 0;
}

}  // namespace

std::uintptr_t FindVtableByType(std::string_view fragment) {
    const auto image = GetImageLayout();
    if (image.base == 0) {
        return 0;
    }
    const auto vtables = FindVtablesByTypeName(image, fragment);
    return vtables.empty() ? 0 : vtables.front().address;  // exact 优先(已排序)
}

std::uintptr_t FindInstanceByVtable(std::uintptr_t vtable_va) {
    return FindInstance(vtable_va);
}

}  // namespace sora_console::runtime_locator
