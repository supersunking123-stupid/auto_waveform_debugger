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
    std::string signal_id;
};

// Abstract interface for waveform format backends.
// Each supported format (VCD, FST, FSDB, etc.) implements this interface.
class FormatAdapter {
public:
    virtual ~FormatAdapter() = default;

    // Load a waveform file. Returns true on success.
    virtual bool Load(const std::string& path) = 0;

    // Close/release any held resources. Called before destruction or re-load.
    virtual void Close() = 0;

    // Query the timescale string (e.g. "1ns", "1ps").
    virtual std::string GetTimescale() const = 0;

    // Populate the provided maps with signal metadata and (for formats that
    // eagerly read transitions) transition data.  For lazy-load formats like
    // FSDB, id_transitions may remain empty and transitions are fetched on
    // demand via LoadSignalTransitions.
    virtual void CollectSignals(
        std::unordered_map<std::string, SignalInfo>& signal_info,
        std::unordered_map<std::string, std::vector<Transition>>& id_transitions) = 0;

    // Lazy-load transitions for one signal (used by FSDB backend).
    // Returns true if transitions were loaded or were already present.
    // For eager-load formats this is a no-op that returns true.
    virtual bool LoadSignalTransitions(
        const SignalInfo& info,
        std::unordered_map<std::string, std::vector<Transition>>& id_transitions) = 0;
};
