#include "AgentAPI.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
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

std::string lowercase_copy(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

std::string glob_to_regex(const std::string& pattern) {
    std::string regex = "^";
    regex.reserve(pattern.size() * 2 + 2);
    for (char ch : pattern) {
        switch (ch) {
            case '*':
                regex += ".*";
                break;
            case '?':
                regex += '.';
                break;
            case '.':
            case '^':
            case '$':
            case '|':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '+':
            case '\\':
                regex.push_back('\\');
                regex.push_back(ch);
                break;
            default:
                regex.push_back(ch);
                break;
        }
    }
    regex += "$";
    return regex;
}

bool is_top_module_signal_path(const std::string& path) {
    const size_t first_dot = path.find('.');
    if (first_dot == std::string::npos) {
        return true;
    }
    return path.find('.', first_dot + 1) == std::string::npos;
}

std::string canonical_signal_type(const std::string& raw_type) {
    const std::string lowered = lowercase_copy(raw_type);
    if (lowered == "input") return "input";
    if (lowered == "output") return "output";
    if (lowered == "inout") return "inout";
    if (lowered == "reg" || lowered == "register") return "register";
    if (lowered == "wire" || lowered == "net" || lowered == "tri" || lowered == "tri0" ||
        lowered == "tri1" || lowered == "wand" || lowered == "wor" ||
        lowered == "supply0" || lowered == "supply1") {
        return "net";
    }
    return lowered;
}

bool signal_matches_types(const SignalInfo& info, const std::set<std::string>& allowed_types) {
    if (allowed_types.empty()) {
        return true;
    }
    return allowed_types.find(canonical_signal_type(info.type)) != allowed_types.end();
}

json build_signal_type_filter(const std::vector<std::string>& requested_types, std::set<std::string>& allowed_types) {
    static const std::set<std::string> kSupported = {
        "input",
        "output",
        "inout",
        "net",
        "register",
    };

    for (const auto& raw_type : requested_types) {
        const std::string canonical = canonical_signal_type(raw_type);
        if (kSupported.find(canonical) == kSupported.end()) {
            return {
                {"status", "error"},
                {"message", "Unsupported signal type filter: " + raw_type},
            };
        }
        allowed_types.insert(canonical);
    }
    return {{"status", "success"}};
}

json build_path_matcher(const std::string& pattern, std::regex& matcher) {
    if (pattern.empty() || pattern == "*") {
        return {{"status", "success"}};
    }

    try {
        if (pattern.rfind("regex:", 0) == 0) {
            matcher = std::regex(pattern.substr(6));
        } else {
            matcher = std::regex(glob_to_regex(pattern));
        }
    } catch (const std::regex_error& e) {
        return {
            {"status", "error"},
            {"message", std::string("Invalid signal pattern: ") + e.what()},
        };
    }
    return {{"status", "success"}};
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

std::string decimal_string_to_bits(std::string decimal) {
    if (decimal.empty()) {
        return "";
    }

    for (char ch : decimal) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return "";
        }
    }

    size_t first_non_zero = decimal.find_first_not_of('0');
    if (first_non_zero == std::string::npos) {
        return "0";
    }
    decimal = decimal.substr(first_non_zero);

    std::string bits;
    while (!decimal.empty() && decimal != "0") {
        int carry = 0;
        std::string quotient;
        quotient.reserve(decimal.size());
        for (char ch : decimal) {
            const int cur = carry * 10 + (ch - '0');
            const int q = cur / 2;
            carry = cur % 2;
            if (!quotient.empty() || q != 0) {
                quotient.push_back(static_cast<char>('0' + q));
            }
        }
        bits.push_back(static_cast<char>('0' + carry));
        decimal = quotient.empty() ? "0" : quotient;
    }
    std::reverse(bits.begin(), bits.end());
    return bits;
}

std::string normalize_query_value_to_bits(const std::string& value, uint32_t width, const std::string& radix) {
    if (value.empty()) {
        return "";
    }

    const size_t bit_digits = std::max<size_t>(1, static_cast<size_t>(width));
    const char prefix = static_cast<char>(std::tolower(static_cast<unsigned char>(value.front())));
    const std::string normalized_radix = normalize_radix(radix);

    if (width <= 1) {
        if (value == "0" || value == "1" || value == "x" || value == "z") {
            return value;
        }
        if (prefix == 'b' || prefix == 'd' || prefix == 'h') {
            return normalize_query_value_to_bits(value.substr(1), width, prefix == 'b' ? "bin" : (prefix == 'd' ? "dec" : "hex"));
        }
        return simplify_scalar_value(value);
    }

    if (prefix == 'b') {
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
        } else if (bits.size() > bit_digits) {
            bits = bits.substr(bits.size() - bit_digits);
        }
        return bits;
    }

    if (prefix == 'h') {
        return normalize_multibit_bits(value, width);
    }

    if (prefix == 'd' || normalized_radix == "dec") {
        const std::string decimal_digits = prefix == 'd' ? value.substr(1) : value;
        std::string bits = decimal_string_to_bits(decimal_digits);
        if (bits.empty()) {
            return "";
        }
        if (bits.size() < bit_digits) {
            bits.insert(bits.begin(), bit_digits - bits.size(), '0');
        } else if (bits.size() > bit_digits) {
            bits = bits.substr(bits.size() - bit_digits);
        }
        return bits;
    }

    if (normalized_radix == "bin") {
        std::string bits;
        bits.reserve(value.size());
        for (char ch : value) {
            bits.push_back(normalize_logic_char(ch));
        }
        if (bits.empty()) {
            return "";
        }
        if (bits.size() < bit_digits) {
            bits.insert(bits.begin(), bit_digits - bits.size(), '0');
        } else if (bits.size() > bit_digits) {
            bits = bits.substr(bits.size() - bit_digits);
        }
        return bits;
    }

    return normalize_multibit_bits("h" + value, width);
}

bool value_matches_query(const SignalInfo& info, const std::string& raw_value, const std::string& query_bits) {
    if (query_bits.empty()) {
        return false;
    }
    if (info.width <= 1) {
        return simplify_scalar_value(raw_value) == query_bits;
    }
    const std::string raw_bits = normalize_multibit_bits(raw_value, info.width);
    return raw_bits == query_bits;
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

struct OverviewSegment {
    uint64_t start;
    uint64_t end;
    std::string state;
    std::string value;
    int transitions = 0;
    int unique_values = 0;
};

bool same_overview_segment_shape(const OverviewSegment& lhs, const OverviewSegment& rhs) {
    return lhs.state == rhs.state &&
           lhs.value == rhs.value &&
           lhs.transitions == rhs.transitions &&
           lhs.unique_values == rhs.unique_values;
}

void append_overview_segment(
    std::vector<OverviewSegment>& segments,
    uint64_t start,
    uint64_t end,
    const std::string& state,
    const std::string& value,
    int transitions,
    int unique_values) {
    if (start > end) {
        return;
    }

    OverviewSegment next{start, end, state, value, transitions, unique_values};
    if (!segments.empty()) {
        OverviewSegment& prev = segments.back();
        if (prev.end + 1 == next.start && same_overview_segment_shape(prev, next)) {
            prev.end = next.end;
            return;
        }
    }
    segments.push_back(next);
}

void append_stable_overview_segment(
    std::vector<OverviewSegment>& segments,
    const SignalInfo& info,
    uint64_t start,
    uint64_t end,
    const std::string& raw_value,
    const std::string& radix) {
    if (info.width <= 1) {
        append_overview_segment(segments, start, end, simplify_scalar_value(raw_value), "", 0, 0);
        return;
    }
    append_overview_segment(segments, start, end, "stable", format_multibit_value(raw_value, info.width, radix), 0, 0);
}

std::vector<Transition> get_window_transitions(
    const std::vector<Transition>& all_transitions,
    uint64_t start_time,
    uint64_t end_time) {
    std::vector<Transition> window;
    auto it = std::upper_bound(
        all_transitions.begin(),
        all_transitions.end(),
        start_time,
        [](uint64_t t, const Transition& tr) {
            return t < tr.timestamp;
        });
    while (it != all_transitions.end() && it->timestamp <= end_time) {
        window.push_back(*it);
        ++it;
    }
    return window;
}

std::vector<OverviewSegment> build_signal_overview_segments(
    const SignalInfo& info,
    const std::vector<Transition>& window_transitions,
    uint64_t start_time,
    uint64_t end_time,
    uint64_t resolution,
    const std::string& start_value,
    const std::string& radix) {
    std::vector<OverviewSegment> segments;
    uint64_t cursor = start_time;
    std::string current_value = start_value;

    size_t i = 0;
    while (i < window_transitions.size()) {
        const Transition& current = window_transitions[i];

        if (i + 1 < window_transitions.size() &&
            window_transitions[i + 1].timestamp >= current.timestamp &&
            window_transitions[i + 1].timestamp - current.timestamp < resolution) {
            size_t j = i + 1;
            while (j + 1 < window_transitions.size() &&
                   window_transitions[j + 1].timestamp >= window_transitions[j].timestamp &&
                   window_transitions[j + 1].timestamp - window_transitions[j].timestamp < resolution) {
                ++j;
            }

            if (cursor < current.timestamp) {
                append_stable_overview_segment(segments, info, cursor, current.timestamp - 1, current_value, radix);
            }

            std::set<std::string> unique_values_seen;
            unique_values_seen.insert(current_value);
            for (size_t k = i; k <= j; ++k) {
                unique_values_seen.insert(window_transitions[k].value);
            }

            uint64_t flipping_end = window_transitions[j].timestamp;
            if (window_transitions[j].timestamp > current.timestamp) {
                flipping_end = window_transitions[j].timestamp - 1;
            }
            if (flipping_end < current.timestamp) {
                flipping_end = current.timestamp;
            }

            if (info.width <= 1) {
                append_overview_segment(
                    segments,
                    current.timestamp,
                    flipping_end,
                    "flipping",
                    "",
                    static_cast<int>(j - i + 1),
                    0);
            } else {
                append_overview_segment(
                    segments,
                    current.timestamp,
                    flipping_end,
                    "flipping",
                    "",
                    static_cast<int>(j - i + 1),
                    static_cast<int>(unique_values_seen.size()));
            }

            current_value = window_transitions[j].value;
            cursor = window_transitions[j].timestamp;
            if (flipping_end == cursor) {
                if (cursor == std::numeric_limits<uint64_t>::max()) {
                    break;
                }
                ++cursor;
            }
            i = j + 1;
            continue;
        }

        if (cursor < current.timestamp) {
            append_stable_overview_segment(segments, info, cursor, current.timestamp - 1, current_value, radix);
        }
        current_value = current.value;
        cursor = current.timestamp;
        ++i;
    }

    if (cursor <= end_time) {
        append_stable_overview_segment(segments, info, cursor, end_time, current_value, radix);
    }

    return segments;
}

json overview_segments_to_json(const std::vector<OverviewSegment>& segments) {
    json out = json::array();
    for (const auto& segment : segments) {
        json item = {
            {"start", segment.start},
            {"end", segment.end},
            {"state", segment.state},
        };
        if (!segment.value.empty()) {
            item["value"] = segment.value;
        }
        if (segment.transitions > 0) {
            item["transitions"] = segment.transitions;
        }
        if (segment.unique_values > 0) {
            item["unique_values"] = segment.unique_values;
        }
        out.push_back(item);
    }
    return out;
}

json parse_overview_resolution(const json& resolution_arg, std::string& requested_kind) {
    if (resolution_arg.is_number_integer() || resolution_arg.is_number_unsigned()) {
        const auto value = resolution_arg.get<int64_t>();
        if (value <= 0) {
            return {{"status", "error"}, {"message", "resolution must be > 0"}};
        }
        requested_kind = std::to_string(value);
        return {{"status", "success"}, {"resolution", static_cast<uint64_t>(value)}};
    }

    if (resolution_arg.is_string()) {
        const std::string raw = resolution_arg.get<std::string>();
        std::string lowered;
        lowered.reserve(raw.size());
        for (char ch : raw) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        if (lowered == "auto") {
            requested_kind = "auto";
            return {{"status", "success"}, {"resolution", "auto"}};
        }
        try {
            const auto value = std::stoll(raw);
            if (value <= 0) {
                return {{"status", "error"}, {"message", "resolution must be > 0"}};
            }
            requested_kind = raw;
            return {{"status", "success"}, {"resolution", static_cast<uint64_t>(value)}};
        } catch (...) {
            return {{"status", "error"}, {"message", "resolution must be an integer or 'auto'"}};
        }
    }

    return {{"status", "error"}, {"message", "resolution must be an integer or 'auto'"}};
}

uint64_t choose_auto_resolution(
    const std::vector<Transition>& window_transitions,
    uint64_t start_time,
    uint64_t end_time) {
    constexpr size_t kTargetSegments = 20;

    if (window_transitions.empty()) {
        return 1;
    }

    auto estimated_segment_count_for = [&](uint64_t resolution) -> size_t {
        size_t count = 0;
        uint64_t cursor = start_time;

        for (size_t i = 0; i < window_transitions.size();) {
            const Transition& current = window_transitions[i];
            if (i + 1 < window_transitions.size() &&
                window_transitions[i + 1].timestamp >= current.timestamp &&
                window_transitions[i + 1].timestamp - current.timestamp < resolution) {
                size_t j = i + 1;
                while (j + 1 < window_transitions.size() &&
                       window_transitions[j + 1].timestamp >= window_transitions[j].timestamp &&
                       window_transitions[j + 1].timestamp - window_transitions[j].timestamp < resolution) {
                    ++j;
                }

                if (cursor < current.timestamp) {
                    ++count;
                }
                ++count;

                uint64_t flipping_end = window_transitions[j].timestamp;
                if (window_transitions[j].timestamp > current.timestamp) {
                    flipping_end = window_transitions[j].timestamp - 1;
                }
                if (flipping_end < current.timestamp) {
                    flipping_end = current.timestamp;
                }

                cursor = window_transitions[j].timestamp;
                if (flipping_end == cursor && cursor != std::numeric_limits<uint64_t>::max()) {
                    ++cursor;
                }
                i = j + 1;
                continue;
            }

            if (cursor < current.timestamp) {
                ++count;
            }
            cursor = current.timestamp;
            ++i;
        }

        if (cursor <= end_time) {
            ++count;
        }
        return count;
    };

    const uint64_t span = end_time >= start_time ? (end_time - start_time + 1) : 1;
    uint64_t low = 1;
    uint64_t high = std::max<uint64_t>(1, span);
    if (estimated_segment_count_for(low) <= kTargetSegments) {
        return low;
    }

    while (low < high) {
        const uint64_t mid = low + (high - low) / 2;
        if (estimated_segment_count_for(mid) <= kTargetSegments) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low;
}

struct ClassifiedTransition {
    bool changed = false;
    bool matches_requested_edge = false;
    std::string event;
    std::string value_before;
    std::string value_after;
};

std::optional<std::string> parse_sample_period_arg(const json& sample_period) {
    if (sample_period.is_null()) {
        return std::nullopt;
    }

    if (sample_period.is_number_integer() || sample_period.is_number_unsigned()) {
        const auto value = sample_period.get<int64_t>();
        if (value <= 0) {
            return "sample_period must be > 0";
        }
        return std::nullopt;
    }

    if (sample_period.is_string()) {
        try {
            const auto value = std::stoll(sample_period.get<std::string>());
            if (value <= 0) {
                return "sample_period must be > 0";
            }
            return std::nullopt;
        } catch (...) {
            return "sample_period must be an integer";
        }
    }

    return "sample_period must be an integer";
}

uint64_t extract_sample_period_arg(const json& sample_period) {
    if (sample_period.is_number_integer() || sample_period.is_number_unsigned()) {
        return sample_period.get<uint64_t>();
    }
    return static_cast<uint64_t>(std::stoull(sample_period.get<std::string>()));
}

ClassifiedTransition classify_transition_event(
    const SignalInfo& info,
    const std::string& raw_before,
    const std::string& raw_after,
    const std::string& requested_edge_type) {
    ClassifiedTransition result;
    result.value_before = info.width <= 1 ? simplify_scalar_value(raw_before) : raw_before;
    result.value_after = info.width <= 1 ? simplify_scalar_value(raw_after) : raw_after;

    if (info.width <= 1) {
        if (result.value_before == result.value_after) {
            return result;
        }
        result.changed = true;
        if (result.value_before == "0" && result.value_after == "1") {
            result.event = "posedge";
        } else if (result.value_before == "1" && result.value_after == "0") {
            result.event = "negedge";
        } else {
            result.event = "anyedge";
        }
        result.matches_requested_edge =
            requested_edge_type == "anyedge" ||
            requested_edge_type == result.event ||
            (requested_edge_type == "anyedge" && result.changed);
        return result;
    }

    if (raw_before == raw_after) {
        return result;
    }
    result.changed = true;
    result.event = "toggle";
    result.matches_requested_edge = true;
    return result;
}

std::string effective_count_mode(const SignalInfo& info, const std::string& requested_edge_type) {
    if (info.width <= 1) {
        return requested_edge_type;
    }
    return "toggle";
}

struct DumpTransitionRecord {
    uint64_t timestamp = 0;
    std::string signal;
    std::string event;
    std::string value_before;
    std::string value_after;
    bool glitch = false;
};

bool dump_transition_record_less(const DumpTransitionRecord& lhs, const DumpTransitionRecord& rhs) {
    if (lhs.timestamp != rhs.timestamp) {
        return lhs.timestamp < rhs.timestamp;
    }
    return lhs.signal < rhs.signal;
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

json AgentAPI::list_signals(const std::string& pattern, const std::vector<std::string>& types) {
    std::set<std::string> allowed_types;
    json type_filter = build_signal_type_filter(types, allowed_types);
    if (type_filter.value("status", "error") != "success") {
        return type_filter;
    }

    std::regex matcher;
    const bool has_pattern = !pattern.empty() && pattern != "*";
    json path_filter = build_path_matcher(pattern, matcher);
    if (path_filter.value("status", "error") != "success") {
        return path_filter;
    }

    const bool top_only = pattern.empty();
    std::vector<std::string> signals;
    signals.reserve(db.get_all_signals().size());
    for (const auto& entry : db.get_all_signals()) {
        const SignalInfo& info = entry.second;
        if (top_only && !is_top_module_signal_path(info.path)) {
            continue;
        }
        if (has_pattern && !std::regex_match(info.path, matcher)) {
            continue;
        }
        if (!signal_matches_types(info, allowed_types)) {
            continue;
        }
        signals.push_back(info.path);
    }

    std::sort(signals.begin(), signals.end());
    return {
        {"status", "success"},
        {"data", signals},
        {"pattern", pattern.empty() ? "" : pattern},
        {"types", types},
        {"top_module_only", top_only},
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

json AgentAPI::find_value_intervals(
    const std::string& signal_path,
    const std::string& value,
    uint64_t start_time,
    uint64_t end_time,
    const std::string& radix) {
    if (!db.has_signal(signal_path)) {
        return {{"status", "error"}, {"message", "Signal not found"}};
    }
    if (start_time > end_time) {
        return {{"status", "error"}, {"message", "start_time must be <= end_time"}};
    }

    const auto& info = db.get_signal_info(signal_path);
    const std::string query_bits = normalize_query_value_to_bits(value, info.width, radix);
    if (query_bits.empty()) {
        return {{"status", "error"}, {"message", "failed to parse query value"}};
    }

    const auto& trans = db.get_transitions(signal_path);
    std::string current_value = db.get_value_at_time(signal_path, start_time);
    uint64_t interval_start = start_time;
    json intervals = json::array();

    auto emit_interval_if_match = [&](uint64_t seg_start, uint64_t seg_end, const std::string& raw_value) {
        if (seg_start > seg_end) {
            return;
        }
        if (value_matches_query(info, raw_value, query_bits)) {
            intervals.push_back({
                {"start", seg_start},
                {"end", seg_end},
            });
        }
    };

    auto it = std::upper_bound(
        trans.begin(),
        trans.end(),
        start_time,
        [](uint64_t t, const Transition& tr) {
            return t < tr.timestamp;
        });

    while (it != trans.end() && it->timestamp <= end_time) {
        const uint64_t seg_end = it->timestamp == 0 ? 0 : (it->timestamp - 1);
        emit_interval_if_match(interval_start, seg_end, current_value);
        current_value = it->value;
        interval_start = it->timestamp;
        ++it;
    }

    emit_interval_if_match(interval_start, end_time, current_value);

    return {
        {"status", "success"},
        {"data", intervals},
        {"query", {
            {"path", signal_path},
            {"value", value},
            {"start_time", start_time},
            {"end_time", end_time},
            {"radix", normalize_radix(radix)},
        }},
    };
}

json AgentAPI::find_condition(const std::string& expression, uint64_t start_time, const std::string& direction) {
    // Very basic implementation: parse "PATH == VALUE"
    // For a real tool, we'd use a proper AST parser.
    std::istringstream iss(expression);
    std::string path, op, val;
    if (!(iss >> path >> op >> val) || op != "==") {
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
    } else if (direction == "backward") {
        if (db.get_value_at_time(path, start_time) == val) return {{"status", "success"}, {"data", start_time}};

        auto it = std::upper_bound(trans.begin(), trans.end(), start_time,
            [](uint64_t t, const Transition& tr) {
                return t < tr.timestamp;
            });

        if (it == trans.begin()) {
            return {{"status", "success"}, {"data", -1}};
        }
        --it;

        while (true) {
            if (it->value == val) return {{"status", "success"}, {"data", it->timestamp}};
            if (it == trans.begin()) break;
            --it;
        }
    } else {
        return {{"status", "error"}, {"message", "direction must be forward or backward"}};
    }

    return {{"status", "success"}, {"data", -1}};
}

json AgentAPI::get_transitions(const std::string& signal_path, uint64_t start_time, uint64_t end_time, int max_limit) {
    if (!db.has_signal(signal_path)) return {{"status", "error"}, {"message", "Signal not found"}};
    if (start_time > end_time) return {{"status", "error"}, {"message", "start_time must be <= end_time"}};

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

json AgentAPI::count_transitions(
    const std::string& signal_path,
    uint64_t start_time,
    uint64_t end_time,
    const std::string& edge_type) {
    if (!db.has_signal(signal_path)) {
        return {{"status", "error"}, {"message", "Signal not found"}};
    }
    if (start_time > end_time) {
        return {{"status", "error"}, {"message", "start_time must be <= end_time"}};
    }

    const auto& info = db.get_signal_info(signal_path);
    if (info.width <= 1 &&
        edge_type != "posedge" &&
        edge_type != "negedge" &&
        edge_type != "anyedge") {
        return {{"status", "error"}, {"message", "edge_type must be posedge, negedge, or anyedge"}};
    }

    const auto& trans = db.get_transitions(signal_path);
    int64_t count = 0;
    auto it = std::lower_bound(
        trans.begin(),
        trans.end(),
        start_time,
        [](const Transition& tr, uint64_t t) {
            return tr.timestamp < t;
        });

    while (it != trans.end() && it->timestamp <= end_time) {
        const uint64_t before_time = it->timestamp == 0 ? 0 : (it->timestamp - 1);
        const std::string before = it->timestamp == 0 ? "U" : db.get_value_at_time(signal_path, before_time);
        const ClassifiedTransition event = classify_transition_event(info, before, it->value, edge_type);
        if (event.changed && event.matches_requested_edge) {
            ++count;
        }
        ++it;
    }

    return {
        {"status", "success"},
        {"data", {
            {"count", count},
            {"signal", info.path},
            {"width", info.width},
            {"requested_edge_type", edge_type},
            {"effective_mode", effective_count_mode(info, edge_type)},
            {"start_time", start_time},
            {"end_time", end_time},
        }},
    };
}

json AgentAPI::dump_waveform_data(
    const std::vector<std::string>& signal_paths,
    uint64_t start_time,
    uint64_t end_time,
    const std::string& output_path,
    const std::string& mode,
    const json& sample_period,
    const std::string& radix,
    bool overwrite) {
    if (signal_paths.empty()) {
        return {{"status", "error"}, {"message", "signals must not be empty"}};
    }
    if (start_time > end_time) {
        return {{"status", "error"}, {"message", "start_time must be <= end_time"}};
    }
    if (output_path.empty()) {
        return {{"status", "error"}, {"message", "output_path must not be empty"}};
    }
    if (mode != "transitions" && mode != "samples") {
        return {{"status", "error"}, {"message", "mode must be transitions or samples"}};
    }

    if (mode == "samples") {
        if (const auto sample_period_error = parse_sample_period_arg(sample_period)) {
            return {{"status", "error"}, {"message", *sample_period_error}};
        }
    } else if (!sample_period.is_null()) {
        if (const auto sample_period_error = parse_sample_period_arg(sample_period)) {
            return {{"status", "error"}, {"message", *sample_period_error}};
        }
    }

    std::vector<std::string> resolved_signals;
    resolved_signals.reserve(signal_paths.size());
    for (const auto& path : signal_paths) {
        if (!db.has_signal(path)) {
            return {{"status", "error"}, {"message", "Signal not found: " + path}};
        }
        resolved_signals.push_back(db.get_signal_info(path).path);
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path requested_path(output_path);
    const fs::path absolute_output = fs::absolute(requested_path, ec);
    if (ec) {
        return {{"status", "error"}, {"message", "failed to resolve output_path"}};
    }
    const fs::path parent_dir = absolute_output.parent_path();
    if (!parent_dir.empty() && !fs::exists(parent_dir)) {
        return {{"status", "error"}, {"message", "output directory does not exist"}};
    }
    if (fs::exists(absolute_output) && !overwrite) {
        return {{"status", "error"}, {"message", "output_path already exists; pass overwrite=true to replace it"}};
    }

    const fs::path temp_path = absolute_output.string() + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return {{"status", "error"}, {"message", "failed to open temp output file"}};
    }

    uint64_t records_written = 0;
    uint64_t bytes_written = 0;
    const std::string normalized_radix = normalize_radix(radix);

    auto write_line = [&](const json& record) {
        const std::string line = record.dump();
        out << line << '\n';
        bytes_written += static_cast<uint64_t>(line.size() + 1);
        ++records_written;
    };

    std::string write_error;
    if (mode == "transitions") {
        std::vector<DumpTransitionRecord> records;
        for (const auto& path : resolved_signals) {
            const auto& info = db.get_signal_info(path);
            const auto& transitions = db.get_transitions(path);
            auto it = std::lower_bound(
                transitions.begin(),
                transitions.end(),
                start_time,
                [](const Transition& tr, uint64_t t) {
                    return tr.timestamp < t;
                });
            while (it != transitions.end() && it->timestamp <= end_time) {
                const uint64_t before_time = it->timestamp == 0 ? 0 : (it->timestamp - 1);
                const std::string before = it->timestamp == 0 ? "U" : db.get_value_at_time(path, before_time);
                const ClassifiedTransition event = classify_transition_event(info, before, it->value, "anyedge");
                if (event.changed) {
                    records.push_back({
                        it->timestamp,
                        info.path,
                        event.event,
                        event.value_before,
                        event.value_after,
                        it->is_glitch,
                    });
                }
                ++it;
            }
        }

        std::sort(records.begin(), records.end(), dump_transition_record_less);
        for (const auto& record : records) {
            write_line({
                {"t", record.timestamp},
                {"signal", record.signal},
                {"event", record.event},
                {"value_before", record.value_before},
                {"value_after", record.value_after},
                {"glitch", record.glitch},
            });
        }
    } else {
        const uint64_t step = extract_sample_period_arg(sample_period);
        for (uint64_t time = start_time; time <= end_time;) {
            json values = json::object();
            for (const auto& path : resolved_signals) {
                values[path] = format_signal_value_at_time(db, path, time, normalized_radix);
            }
            write_line({
                {"t", time},
                {"values", values},
            });
            if (time > std::numeric_limits<uint64_t>::max() - step) {
                break;
            }
            time += step;
        }
    }

    out.close();
    if (!out) {
        write_error = "failed while writing output file";
    }

    if (write_error.empty()) {
        if (overwrite && fs::exists(absolute_output)) {
            fs::remove(absolute_output, ec);
            if (ec) {
                write_error = "failed to replace existing output file";
            }
        }
    }
    if (write_error.empty()) {
        fs::rename(temp_path, absolute_output, ec);
        if (ec) {
            write_error = "failed to finalize output file";
        }
    }
    if (!write_error.empty()) {
        fs::remove(temp_path, ec);
        return {{"status", "error"}, {"message", write_error}};
    }

    return {
        {"status", "success"},
        {"output_path", absolute_output.string()},
        {"format", "jsonl"},
        {"mode", mode},
        {"records_written", records_written},
        {"bytes_written", bytes_written},
        {"signals", resolved_signals},
        {"start_time", start_time},
        {"end_time", end_time},
        {"radix", mode == "samples" ? json(normalized_radix) : json(nullptr)},
        {"sample_period", mode == "samples" ? json(extract_sample_period_arg(sample_period)) : json(nullptr)},
    };
}

json AgentAPI::get_signal_overview(
    const std::string& signal_path,
    uint64_t start_time,
    uint64_t end_time,
    const json& resolution,
    const std::string& radix) {
    if (!db.has_signal(signal_path)) {
        return {{"status", "error"}, {"message", "Signal not found"}};
    }
    if (start_time > end_time) {
        return {{"status", "error"}, {"message", "start_time must be <= end_time"}};
    }

    std::string requested_resolution;
    const json parsed_resolution = parse_overview_resolution(resolution, requested_resolution);
    if (parsed_resolution.value("status", "error") != "success") {
        return parsed_resolution;
    }

    const auto& info = db.get_signal_info(signal_path);
    const auto& all_transitions = db.get_transitions(signal_path);
    const std::vector<Transition> window_transitions = get_window_transitions(all_transitions, start_time, end_time);
    const std::string start_value = db.get_value_at_time(signal_path, start_time);

    uint64_t resolved_resolution = 0;
    if (parsed_resolution["resolution"].is_string()) {
        resolved_resolution = choose_auto_resolution(
            window_transitions,
            start_time,
            end_time);
    } else {
        resolved_resolution = parsed_resolution["resolution"].get<uint64_t>();
    }

    const auto segments = build_signal_overview_segments(
        info,
        window_transitions,
        start_time,
        end_time,
        resolved_resolution,
        start_value,
        radix);

    return {
        {"status", "success"},
        {"requested_resolution", requested_resolution == "auto" ? json("auto") : json(resolution)},
        {"resolution", resolved_resolution},
        {"timescale", db.get_timescale()},
        {"signal", info.path},
        {"width", info.width},
        {"radix", info.width <= 1 ? json(nullptr) : json(normalize_radix(radix))},
        {"segments", overview_segments_to_json(segments)},
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
