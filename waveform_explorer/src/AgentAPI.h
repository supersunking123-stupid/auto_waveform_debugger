#pragma once

#include "WaveDatabase.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

class AgentAPI {
public:
    AgentAPI(WaveDatabase& db);

    // 1. Metadata
    json get_signal_info(const std::string& signal_path);
    json list_signals(
        const std::string& pattern = "",
        const std::vector<std::string>& types = std::vector<std::string>());
    json list_signals_page(const std::string& prefix, const std::string& cursor, uint64_t limit);

    // 2. State Snapshot
    json get_snapshot(const std::vector<std::string>& signal_paths, uint64_t time, const std::string& radix = "hex");
    json get_value_at_time(const std::string& signal_path, uint64_t time, const std::string& radix = "hex");
    json get_raw_value_at_time(const std::string& signal_path, uint64_t time);
    json get_last_transition_time(const std::string& signal_path);

    // 3. Temporal Search
    json find_edge(const std::string& signal_path, const std::string& edge_type, uint64_t start_time, const std::string& direction = "forward");
    json find_value_intervals(
        const std::string& signal_path,
        const std::string& value,
        uint64_t start_time,
        uint64_t end_time,
        const std::string& radix = "hex");
    
    // Simplistic expression evaluator for basic conditions (e.g. "TOP.valid == 1")
    json find_condition(const std::string& expression, uint64_t start_time, const std::string& direction = "forward");

    // 4. Compressed Transitions
    json get_transitions(const std::string& signal_path, uint64_t start_time, uint64_t end_time, int max_limit = 50);
    json count_transitions(
        const std::string& signal_path,
        uint64_t start_time,
        uint64_t end_time,
        const std::string& edge_type = "anyedge");
    json dump_waveform_data(
        const std::vector<std::string>& signal_paths,
        uint64_t start_time,
        uint64_t end_time,
        const std::string& output_path,
        const std::string& mode = "transitions",
        const json& sample_period = nullptr,
        const std::string& radix = "hex",
        bool overwrite = false);
    json get_signal_overview(
        const std::string& signal_path,
        uint64_t start_time,
        uint64_t end_time,
        const json& resolution,
        const std::string& radix = "hex");

    // 5. Pattern Analysis
    json analyze_pattern(const std::string& signal_path, uint64_t start_time, uint64_t end_time);

private:
    WaveDatabase& db;
};
