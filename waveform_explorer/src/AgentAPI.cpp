#include "AgentAPI.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <map>

AgentAPI::AgentAPI(WaveDatabase& db) : db(db) {}

json AgentAPI::get_signal_info(const std::string& signal_path) {
    if (!db.has_signal(signal_path)) {
        return {{"status", "error"}, {"message", "Signal not found"}};
    }
    const auto& info = db.get_signal_info(signal_path);
    return {
        {"status", "success"},
        {"data", {
            {"name", info.name},
            {"path", info.path},
            {"width", info.width},
            {"type", info.type},
            {"timescale", db.get_timescale()}
        }}
    };
}

json AgentAPI::list_signals_page(const std::string& prefix, const std::string& cursor, uint64_t limit) {
    if (!db.is_fsdb_backend()) {
        return {
            {"status", "error"},
            {"message", "list_signals_page is only supported for FSDB waveforms"}
        };
    }

    bool has_more = false;
    std::string next_cursor;
    const auto page = db.list_signal_paths_page(prefix, cursor, static_cast<size_t>(limit), has_more, next_cursor);

    json data = json::array();
    for (const auto& path : page) {
        data.push_back(path);
    }

    return {
        {"status", "success"},
        {"data", data},
        {"prefix", prefix},
        {"cursor", cursor},
        {"limit", limit},
        {"has_more", has_more},
        {"next_cursor", has_more ? next_cursor : ""}
    };
}

json AgentAPI::get_snapshot(const std::vector<std::string>& signal_paths, uint64_t time) {
    json data = json::object();
    for (const auto& path : signal_paths) {
        data[path] = db.get_value_at_time(path, time);
    }
    return {{"status", "success"}, {"data", data}};
}

json AgentAPI::get_value_at_time(const std::string& signal_path, uint64_t time) {
    if (!db.has_signal(signal_path)) {
        return {{"status", "error"}, {"message", "Signal not found"}};
    }
    return {{"status", "success"}, {"data", db.get_value_at_time(signal_path, time)}};
}

json AgentAPI::find_edge(const std::string& signal_path, const std::string& edge_type, uint64_t start_time, const std::string& direction) {
    if (!db.has_signal(signal_path)) {
        return {{"status", "error"}, {"message", "Signal not found"}};
    }

    const auto& trans = db.get_transitions(signal_path);
    if (trans.empty()) return {{"status", "success"}, {"data", -1}};

    if (direction == "forward") {
        auto it = std::lower_bound(trans.begin(), trans.end(), start_time,
            [](const Transition& tr, uint64_t t) {
                return tr.timestamp < t;
            });
        
        while (it != trans.end()) {
            if (it->timestamp > start_time) {
                // Check edge type
                std::string prev_val = db.get_value_at_time(signal_path, it->timestamp - 1);
                std::string cur_val = it->value;
                
                bool match = false;
                if (edge_type == "posedge" && prev_val == "0" && cur_val == "1") match = true;
                else if (edge_type == "negedge" && prev_val == "1" && cur_val == "0") match = true;
                else if (edge_type == "anyedge" && prev_val != cur_val) match = true;
                
                if (match) return {{"status", "success"}, {"data", it->timestamp}};
            }
            ++it;
        }
    } else {
        // Backward search
        auto it = std::upper_bound(trans.begin(), trans.end(), start_time,
            [](uint64_t t, const Transition& tr) {
                return t < tr.timestamp;
            });

        if (it == trans.begin()) {
            return {{"status", "success"}, {"data", -1}};
        }
        --it;

        while (true) {
            std::string cur_val = it->value;
            std::string prev_val = (it == trans.begin()) ? "U" : std::prev(it)->value;
            
            bool match = false;
            if (edge_type == "posedge" && prev_val == "0" && cur_val == "1") match = true;
            else if (edge_type == "negedge" && prev_val == "1" && cur_val == "0") match = true;
            else if (edge_type == "anyedge" && prev_val != cur_val) match = true;
            
            if (match) return {{"status", "success"}, {"data", it->timestamp}};
            
            if (it == trans.begin()) break;
            --it;
        }
    }

    return {{"status", "success"}, {"data", -1}};
}

json AgentAPI::find_condition(const std::string& expression, uint64_t start_time, const std::string& direction) {
    // Very basic implementation: parse "PATH == VALUE"
    // For a real tool, we'd use a proper AST parser.
    std::istringstream iss(expression);
    std::string path, op, val;
    if (!(iss >> path >> op >> val)) {
        return {{"status", "error"}, {"message", "Invalid expression format. Use 'PATH == VALUE'"}};
    }

    if (!db.has_signal(path)) return {{"status", "error"}, {"message", "Signal not found: " + path}};

    const auto& trans = db.get_transitions(path);
    
    // Find first time >= start_time where condition is met
    // Optimization: only check timestamps where this signal changes
    if (direction == "forward") {
        // First, check if it's already true at start_time
        if (db.get_value_at_time(path, start_time) == val) return {{"status", "success"}, {"data", start_time}};

        auto it = std::lower_bound(trans.begin(), trans.end(), start_time,
            [](const Transition& tr, uint64_t t) {
                return tr.timestamp < t;
            });
        
        while (it != trans.end()) {
            if (it->value == val) return {{"status", "success"}, {"data", it->timestamp}};
            ++it;
        }
    }
    
    return {{"status", "success"}, {"data", -1}};
}

json AgentAPI::get_transitions(const std::string& signal_path, uint64_t start_time, uint64_t end_time, int max_limit) {
    if (!db.has_signal(signal_path)) return {{"status", "error"}, {"message", "Signal not found"}};

    const auto& trans = db.get_transitions(signal_path);
    json history = json::array();
    
    auto it = std::lower_bound(trans.begin(), trans.end(), start_time,
        [](const Transition& tr, uint64_t t) {
            return tr.timestamp < t;
        });

    int count = 0;
    while (it != trans.end() && it->timestamp <= end_time && count < max_limit) {
        history.push_back({
            {"t", it->timestamp},
            {"v", it->value},
            {"glitch", it->is_glitch}
        });
        ++it;
        ++count;
    }

    return {
        {"status", "success"},
        {"data", history},
        {"truncated", it != trans.end() && it->timestamp <= end_time}
    };
}

json AgentAPI::analyze_pattern(const std::string& signal_path, uint64_t start_time, uint64_t end_time) {
    if (!db.has_signal(signal_path)) return {{"status", "error"}, {"message", "Signal not found"}};

    const auto& trans = db.get_transitions(signal_path);
    auto it_start = std::lower_bound(trans.begin(), trans.end(), start_time, [](const Transition& tr, uint64_t t){ return tr.timestamp < t; });
    auto it_end = std::upper_bound(trans.begin(), trans.end(), end_time, [](uint64_t t, const Transition& tr){ return t < tr.timestamp; });

    long num_trans = std::distance(it_start, it_end);
    if (num_trans == 0) {
        return {{"status", "success"}, {"summary", "Static signal, no transitions in this window."}};
    }

    // Detect clock
    if (num_trans > 4) {
        std::vector<uint64_t> intervals;
        auto it = it_start;
        auto next = std::next(it);
        while (next != it_end) {
            intervals.push_back(next->timestamp - it->timestamp);
            it++;
            next++;
        }
        
        // Simple heuristic: if intervals are consistent
        uint64_t sum = 0;
        for (auto i : intervals) sum += i;
        double avg = (double)sum / intervals.size();
        
        bool consistent = true;
        for (auto i : intervals) {
            if (std::abs((double)i - avg) > avg * 0.05) { // 5% tolerance
                consistent = false;
                break;
            }
        }
        
        if (consistent) {
            std::stringstream ss;
            ss << "Clock-like signal with average period " << (avg * 2) << " units (half-period " << avg << ").";
            return {{"status", "success"}, {"summary", ss.str()}};
        }
    }

    std::stringstream ss;
    ss << "Dynamic signal with " << num_trans << " transitions.";
    return {{"status", "success"}, {"summary", ss.str()}};
}
