#include "WaveDatabase.h"
#include "FormatAdapter.h"
#include "FormatRegistry.h"
#include <algorithm>
#include <iostream>
#include <cstring>
#include <memory>

namespace {

std::string strip_top_prefix(const std::string& path) {
    static const std::string kTop = "TOP.";
    if (path.rfind(kTop, 0) == 0) return path.substr(kTop.size());
    return path;
}

std::string strip_bit_suffix(const std::string& path) {
    const size_t bracket = path.find('[');
    if (bracket == std::string::npos) return path;
    return path.substr(0, bracket);
}

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

WaveDatabase::WaveDatabase() {}

WaveDatabase::~WaveDatabase() {
    clear();
}

void WaveDatabase::clear() {
    if (adapter_) {
        adapter_->Close();
        adapter_.reset();
    }
    backend_kind = BackendKind::None;
    timescale.clear();
    signal_info.clear();
    id_transitions.clear();
    sorted_signal_paths_cache.clear();
    sorted_signal_paths_valid = false;
    base_signal_path_cache.clear();
    ambiguous_base_signal_paths.clear();
}

bool WaveDatabase::load(const std::string& filepath) {
    auto adapter = FormatRegistry::Create(filepath);
    if (!adapter) return false;
    if (!adapter->Load(filepath)) return false;

    clear();
    adapter_ = std::move(adapter);

    // Determine backend kind from file extension.
    if (ends_with_case_insensitive(filepath, ".fst")) {
        backend_kind = BackendKind::Fst;
    } else if (ends_with_case_insensitive(filepath, ".fsdb")) {
        backend_kind = BackendKind::Fsdb;
    } else {
        backend_kind = BackendKind::Vcd;
    }

    adapter_->CollectSignals(signal_info, id_transitions);
    timescale = adapter_->GetTimescale();
    rebuild_base_signal_path_cache();
    return true;
}

bool WaveDatabase::load_vcd(const std::string& filepath) {
    return load(filepath);
}

bool WaveDatabase::load_fst(const std::string& filepath) {
    return load(filepath);
}

bool WaveDatabase::load_fsdb(const std::string& filepath) {
    return load(filepath);
}

void WaveDatabase::rebuild_base_signal_path_cache() {
    base_signal_path_cache.clear();
    ambiguous_base_signal_paths.clear();

    for (const auto& entry : signal_info) {
        const std::string& path = entry.first;
        const std::string base_path = strip_bit_suffix(path);
        if (base_path == path) continue;
        if (ambiguous_base_signal_paths.find(base_path) != ambiguous_base_signal_paths.end()) continue;

        const auto it = base_signal_path_cache.find(base_path);
        if (it == base_signal_path_cache.end()) {
            base_signal_path_cache.emplace(base_path, path);
            continue;
        }
        if (it->second != path) {
            base_signal_path_cache.erase(it);
            ambiguous_base_signal_paths.insert(base_path);
        }
    }
}

bool WaveDatabase::has_signal(const std::string& path) const {
    return signal_info.find(resolve_query_path(path)) != signal_info.end();
}

const SignalInfo& WaveDatabase::get_signal_info(const std::string& path) const {
    return signal_info.at(resolve_query_path(path));
}

std::vector<std::string> WaveDatabase::list_signal_paths_page(
    const std::string& prefix,
    const std::string& cursor,
    size_t limit,
    bool& has_more,
    std::string& next_cursor) const {
    if (!sorted_signal_paths_valid) {
        sorted_signal_paths_cache.clear();
        sorted_signal_paths_cache.reserve(signal_info.size());
        for (const auto& pair : signal_info) {
            sorted_signal_paths_cache.push_back(pair.first);
        }
        std::sort(sorted_signal_paths_cache.begin(), sorted_signal_paths_cache.end());
        sorted_signal_paths_valid = true;
    }

    if (limit == 0) limit = 1;

    auto matches_prefix = [&](const std::string& path) {
        return prefix.empty() || path.rfind(prefix, 0) == 0;
    };

    auto begin_it = prefix.empty()
        ? sorted_signal_paths_cache.begin()
        : std::lower_bound(sorted_signal_paths_cache.begin(), sorted_signal_paths_cache.end(), prefix);

    if (!cursor.empty()) {
        auto cursor_it = std::upper_bound(sorted_signal_paths_cache.begin(), sorted_signal_paths_cache.end(), cursor);
        if (cursor_it > begin_it) begin_it = cursor_it;
    }

    std::vector<std::string> page;
    page.reserve(limit);
    auto it = begin_it;
    while (it != sorted_signal_paths_cache.end() && page.size() < limit) {
        if (!matches_prefix(*it)) {
            if (!prefix.empty() && *it > prefix) break;
            ++it;
            continue;
        }
        page.push_back(*it);
        ++it;
    }

    has_more = false;
    next_cursor.clear();
    while (it != sorted_signal_paths_cache.end()) {
        if (!matches_prefix(*it)) {
            if (!prefix.empty() && *it > prefix) break;
            ++it;
            continue;
        }
        has_more = true;
        break;
    }
    if (has_more && !page.empty()) next_cursor = page.back();
    return page;
}

const std::vector<Transition>& WaveDatabase::get_transitions(const std::string& path) const {
    const std::string resolved = resolve_query_path(path);
    if (signal_info.find(resolved) == signal_info.end()) {
        static const std::vector<Transition> empty;
        return empty;
    }
    if (!ensure_signal_transitions_loaded(resolved)) {
        static const std::vector<Transition> empty;
        return empty;
    }
    const std::string& id = signal_info.at(resolved).signal_id;
    return id_transitions.at(id);
}

std::string WaveDatabase::get_value_at_time(const std::string& path, uint64_t time) const {
    const std::string resolved = resolve_query_path(path);
    if (signal_info.find(resolved) == signal_info.end()) return "U";
    if (!ensure_signal_transitions_loaded(resolved)) return "U";

    const std::string& id = signal_info.at(resolved).signal_id;
    const auto& trans = id_transitions.at(id);
    if (trans.empty()) return "U";
    if (time < trans.front().timestamp) return "U";

    auto it = std::upper_bound(trans.begin(), trans.end(), time,
        [](uint64_t t, const Transition& tr) {
            return t < tr.timestamp;
        });

    if (it == trans.begin()) return "U";
    --it;
    return it->value;
}

std::string WaveDatabase::normalize_loaded_path(const std::string& path) const {
    return strip_top_prefix(path);
}

std::string WaveDatabase::resolve_query_path(const std::string& path) const {
    if (signal_info.find(path) != signal_info.end()) return path;

    const std::string no_top = strip_top_prefix(path);
    if (signal_info.find(no_top) != signal_info.end()) return no_top;

    const std::string with_top = "TOP." + path;
    if (signal_info.find(with_top) != signal_info.end()) return with_top;

    if (path.find('[') == std::string::npos) {
        const auto base_it = base_signal_path_cache.find(no_top);
        if (base_it != base_signal_path_cache.end()) return base_it->second;
    }

    return path;
}

bool WaveDatabase::ensure_signal_transitions_loaded(const std::string& resolved_path) const {
    const auto it = signal_info.find(resolved_path);
    if (it == signal_info.end()) return false;

    const std::string& signal_id = it->second.signal_id;
    if (id_transitions.find(signal_id) != id_transitions.end()) return true;

    // Delegate to the adapter for lazy-load formats.
    if (adapter_) {
        return adapter_->LoadSignalTransitions(it->second, id_transitions);
    }
    return false;
}
