#include "FormatRegistry.h"
#include "vcd/VcdAdapter.h"
#include "fst/FstAdapter.h"

#ifdef WAVE_HAS_FSDB
#include "fsdb/FsdbAdapter.h"
#endif

#include <algorithm>
#include <cctype>
#include <iostream>

namespace {

bool ends_with_case_insensitive(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    const size_t start = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(s[start + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

} // namespace

std::unique_ptr<FormatAdapter> FormatRegistry::Create(const std::string& filepath) {
    if (ends_with_case_insensitive(filepath, ".fst")) {
        return std::make_unique<FstAdapter>();
    }
    if (ends_with_case_insensitive(filepath, ".fsdb")) {
#ifdef WAVE_HAS_FSDB
        return std::make_unique<FsdbAdapter>();
#else
        std::cerr << "FSDB support is not compiled in. Rebuild with Verdi FsdbReader SDK enabled." << std::endl;
        return nullptr;
#endif
    }
    // Default: treat as VCD
    return std::make_unique<VcdAdapter>();
}
