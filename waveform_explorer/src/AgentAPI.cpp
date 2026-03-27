#include "AgentAPI.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>

AgentAPI::AgentAPI(WaveDatabase& db) : db(db) {}

namespace {
char normalize_logic_char(char ch) {
    switch (std::tolower(static_cast<unsigned char>(ch))) {
        case '0': return '0';
        case '1': return '1';
        case 'x':
        case 'u':
        case '?':
            return 'x';
        case 'z':
            return 'z';
        default:
            return 'x';
    }
}

std::string simplify_scalar_value(const std::string& value) {
    if (value == "0" || value == "1" || value == "x" || value == "z") {
        return value;
    }
    if (value == "U" || value == "u") {
        return "x";
    }
    if (value.empty()) {
        return "x";
    }

    const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(value.front())));
    if (ch == '0' || ch == '1' || ch == 'x' || ch == 'z') {
        return std::string(1, ch);
    }
    return "x";
}

std::string normalize_radix(const std::string& radix) {
    std::string normalized;
    normalized.reserve(radix.size());
    for (char ch : radix) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (normalized == "bin" || normalized == "binary" || normalized == "2") return "bin";
    if (normalized == "dec" || normalized == "decimal" || normalized == "10") return "dec";
    return "hex";
}

std::string normalize_multibit_bits(const std::string& value, uint32_t width) {
    if (value.empty()) {
        return "";
    }

    const size_t hex_digits = std::max<size_t>(1, (static_cast<size_t>(width) + 3) / 4);
    const size_t bit_digits = std::max<size_t>(1, static_cast<size_t>(width));

    if (value.front() == 'h' || value.front() == 'H') {
        std::string bits;
        bits.reserve((value.size() - 1) * 4);
        for (size_t i = 1; i < value.size(); ++i) {
            const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
            switch (ch) {
                case '0': bits += "0000"; break;
                case '1': bits += "0001"; break;
                case '2': bits += "0010"; break;
                case '3': bits += "0011"; break;
                case '4': bits += "0100"; break;
                case '5': bits += "0101"; break;
                case '6': bits += "0110"; break;
                case '7': bits += "0111"; break;
                case '8': bits += "1000"; break;
                case '9': bits += "1001"; break;
                case 'a': bits += "1010"; break;
                case 'b': bits += "1011"; break;
                case 'c': bits += "1100"; break;
                case 'd': bits += "1101"; break;
                case 'e': bits += "1110"; break;
                case 'f': bits += "1111"; break;
                case 'z': bits += "zzzz"; break;
                default: bits += "xxxx"; break;
            }
        }
        if (bits.size() < hex_digits * 4) {
            bits.insert(bits.begin(), hex_digits * 4 - bits.size(), '0');
        }
        if (bits.size() > bit_digits) {
            bits = bits.substr(bits.size() - bit_digits);
        }
        return bits;
    }

    if (value.front() == 'b' || value.front() == 'B') {
        std::string bits;
        bits.reserve(value.size() - 1);
        for (size_t i = 1; i < value.size(); ++i) {
            bits.push_back(normalize_logic_char(value[i]));
        }
        if (bits.empty()) {
            return "";
        }
        if (bits.size() < bit_digits) {
            bits.insert(bits.begin(), bit_digits - bits.size(), '0');
        }
        return bits;
    }

    std::string normalized = value;
    for (char& ch : normalized) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return normalized;
}

std::string binary_bits_to_decimal(const std::string& bits) {
    std::string decimal = "0";
    for (char bit : bits) {
        int carry = (bit == '1') ? 1 : 0;
        for (int i = static_cast<int>(decimal.size()) - 1; i >= 0; --i) {
            int digit = (decimal[static_cast<size_t>(i)] - '0') * 2 + carry;
            decimal[static_cast<size_t>(i)] = static_cast<char>('0' + (digit % 10));
            carry = digit / 10;
        }
        if (carry > 0) {
            decimal.insert(decimal.begin(), static_cast<char>('0' + carry));
        }
    }
    return decimal;
}

std::string format_multibit_value(const std::string& value, uint32_t width, const std::string& radix) {
    const size_t hex_digits = std::max<size_t>(1, (static_cast<size_t>(width) + 3) / 4);
    const size_t bit_digits = std::max<size_t>(1, static_cast<size_t>(width));
    const std::string normalized_radix = normalize_radix(radix);
    const std::string normalized = normalize_multibit_bits(value, width);
    if (normalized.empty()) {
        return normalized_radix == "dec" ? "dx" : (normalized_radix == "bin" ? "bx" : "hx");
    }

    if (!normalized.empty() && normalized.front() != '0' && normalized.front() != '1' &&
        normalized.front() != 'x' && normalized.front() != 'z') {
        return normalized;
    }

    if (normalized_radix == "bin") {
        std::string bits = normalized;
        if (bits.size() < bit_digits) {
            bits.insert(bits.begin(), bit_digits - bits.size(), '0');
        }
        return "b" + bits;
    }

    if (normalized_radix == "dec") {
        for (char bit : normalized) {
            if (bit != '0' && bit != '1') {
                return "b" + normalized;
            }
        }
        return "d" + binary_bits_to_decimal(normalized);
    }

    std::string bits = normalized;
    if (bits.empty()) {
        return "hx";
    }

    if (bits.size() < hex_digits * 4) {
        bits.insert(bits.begin(), hex_digits * 4 - bits.size(), '0');
    }

    std::string out = "h";
    out.reserve(hex_digits + 1);
    for (size_t i = 0; i < bits.size(); i += 4) {
        const std::string nibble = bits.substr(i, 4);
        bool all_z = true;
        bool has_unknown = false;
        int numeric = 0;
        for (char bit : nibble) {
            if (bit != 'z') all_z = false;
            if (bit == 'x' || bit == 'z') {
                has_unknown = true;
                continue;
            }
            numeric = (numeric << 1) | (bit == '1' ? 1 : 0);
        }
        if (all_z) {
            out.push_back('z');
        } else if (has_unknown) {
            out.push_back('x');
        } else {
            out.push_back("0123456789abcdef"[numeric]);
        }
    }

    return out;
}

json format_signal_value_at_time(WaveDatabase& db, const std::string& signal_path, uint64_t time, const std::string& radix) {
    if (!db.has_signal(signal_path)) {
        return "x";
    }

    const auto& info = db.get_signal_info(signal_path);
    const auto& trans = db.get_transitions(signal_path);
    const std::string value = db.get_value_at_time(signal_path, time);
    const uint64_t before_time = time == 0 ? 0 : time - 1;
    const std::string value_before = time == 0 ? "U" : db.get_value_at_time(signal_path, before_time);

    auto it = std::lower_bound(
        trans.begin(),
        trans.end(),
        time,
        [](const Transition& tr, uint64_t t) {
            return tr.timestamp < t;
        });

    bool transition_at_time = it != trans.end() && it->timestamp == time;
    if (info.width <= 1) {
        if (transition_at_time) {
            if (value_before == "0" && value == "1") return "rising";
            if (value_before == "1" && value == "0") return "falling";
        }
        return simplify_scalar_value(value);
    }

    if (transition_at_time && value_before != value) {
        return "changing";
    }
    return format_multibit_value(value, info.width, radix);
}
}

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

json AgentAPI::get_snapshot(const std::vector<std::string>& signal_paths, uint64_t time, const std::string& radix) {
    json data = json::object();
    for (const auto& path : signal_paths) {
        data[path] = format_signal_value_at_time(db, path, time, radix);
    }
    return {{"status", "success"}, {"data", data}};
}

json AgentAPI::get_value_at_time(const std::string& signal_path, uint64_t time, const std::string& radix) {
    if (!db.has_signal(signal_path)) {
        return {{"status", "error"}, {"message", "Signal not found"}};
    }
    return {{"status", "success"}, {"data", format_signal_value_at_time(db, signal_path, time, radix)}};
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
