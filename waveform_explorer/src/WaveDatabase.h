#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

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
    bool load_vcd(const std::string& filepath);

    std::string get_timescale() const { return timescale; }
    
    // Core accessors
    bool has_signal(const std::string& path) const;
    const SignalInfo& get_signal_info(const std::string& path) const;
    const std::vector<Transition>& get_transitions(const std::string& path) const;
    
    // Utility for snapshot
    std::string get_value_at_time(const std::string& path, uint64_t time) const;

    // Direct access to all signals
    const std::unordered_map<std::string, SignalInfo>& get_all_signals() const { return signal_info; }

private:
    std::string timescale;
    
    // Map from full hierarchical path to signal details
    std::unordered_map<std::string, SignalInfo> signal_info;
    
    // Map from Signal ID (from VCD) to transitions
    std::unordered_map<std::string, std::vector<Transition>> id_transitions;
};
