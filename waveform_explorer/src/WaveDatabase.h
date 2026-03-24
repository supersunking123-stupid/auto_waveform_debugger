#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cstddef>

#ifdef WAVE_HAS_FSDB
class ffrObject;
#endif

struct Transition {
    uint64_t timestamp;
    std::string value;
    bool is_glitch;
};

struct SignalInfo {
    std::string name;
    std::string path;
    uint32_t width;
    std::string type;
    std::string signal_id; // Added to point to shared transitions
};

class WaveDatabase {
public:
    WaveDatabase();
    ~WaveDatabase();
    bool load_vcd(const std::string& filepath);
    bool load_fst(const std::string& filepath);
    bool load_fsdb(const std::string& filepath);
    bool load(const std::string& filepath); // Auto-detect format

    std::string get_timescale() const { return timescale; }
    bool is_fsdb_backend() const { return backend_kind == BackendKind::Fsdb; }
    
    // Core accessors
    bool has_signal(const std::string& path) const;
    const SignalInfo& get_signal_info(const std::string& path) const;
    const std::vector<Transition>& get_transitions(const std::string& path) const;
    std::vector<std::string> list_signal_paths_page(
        const std::string& prefix,
        const std::string& cursor,
        size_t limit,
        bool& has_more,
        std::string& next_cursor) const;
    
    // Utility for snapshot
    std::string get_value_at_time(const std::string& path, uint64_t time) const;

    // Direct access to all signals
    const std::unordered_map<std::string, SignalInfo>& get_all_signals() const { return signal_info; }

private:
    enum class BackendKind {
        None,
        Vcd,
        Fst,
        Fsdb,
    };

    std::string normalize_loaded_path(const std::string& path) const;
    std::string resolve_query_path(const std::string& path) const;
    void rebuild_base_signal_path_cache();
    void clear();
    bool ensure_signal_transitions_loaded(const std::string& resolved_path) const;
#ifdef WAVE_HAS_FSDB
    bool ensure_fsdb_signal_loaded(const SignalInfo& info) const;
#endif

    std::string timescale;
    BackendKind backend_kind = BackendKind::None;
    
    // Map from full hierarchical path to signal details
    std::unordered_map<std::string, SignalInfo> signal_info;
    
    // Map from Signal ID (from VCD) to transitions
    mutable std::unordered_map<std::string, std::vector<Transition>> id_transitions;
    mutable std::vector<std::string> sorted_signal_paths_cache;
    mutable bool sorted_signal_paths_valid = false;
    std::unordered_map<std::string, std::string> base_signal_path_cache;
    std::unordered_set<std::string> ambiguous_base_signal_paths;

#ifdef WAVE_HAS_FSDB
    ffrObject* fsdb_obj = nullptr;
    mutable std::unordered_set<std::string> fsdb_loaded_signal_ids;
#endif
};
