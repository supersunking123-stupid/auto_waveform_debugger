#include "vcd/VcdAdapter.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace {

std::string strip_top_prefix_impl(const std::string& path) {
    static const std::string kTop = "TOP.";
    if (path.rfind(kTop, 0) == 0) return path.substr(kTop.size());
    return path;
}

} // namespace

std::string VcdAdapter::strip_top_prefix(const std::string& path) {
    return strip_top_prefix_impl(path);
}

bool VcdAdapter::Load(const std::string& filepath) {
    Close();

    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open VCD file: " << filepath << std::endl;
        return false;
    }

    std::string line;
    std::vector<std::string> current_scope;
    uint64_t current_time = 0;
    std::unordered_map<std::string, uint64_t> last_change_time;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        const char* p = line.c_str();
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (*p == '\0') continue;

        if (*p == '$') {
            std::string token;
            std::istringstream iss(line);
            iss >> token;

            if (token == "$timescale") {
                iss >> timescale_;
                if (timescale_ == "1") {
                    std::string unit;
                    iss >> unit;
                    timescale_ += unit;
                }
            } else if (token == "$scope") {
                std::string type, name;
                iss >> type >> name;
                current_scope.push_back(name);
            } else if (token == "$upscope") {
                if (!current_scope.empty()) current_scope.pop_back();
            } else if (token == "$var") {
                std::string type, width_str, id, name;
                iss >> type >> width_str >> id >> name;

                std::string path = "";
                for (size_t i = 0; i < current_scope.size(); ++i) {
                    path += current_scope[i];
                    path += ".";
                }
                path += name;
                path = strip_top_prefix(path);

                SignalInfo info;
                info.name = name;
                info.path = path;
                try {
                    info.width = std::stoul(width_str);
                } catch(...) {
                    info.width = 1;
                }
                info.type = type;
                info.signal_id = id;

                signal_info_[path] = info;
                if (id_transitions_.find(id) == id_transitions_.end()) {
                    id_transitions_[id] = std::vector<Transition>();
                }
            }
        } else if (*p == '#') {
            try {
                current_time = std::stoull(p + 1);
            } catch (const std::exception&) {
                std::cerr << "Malformed VCD timestamp: " << line << std::endl;
                return false;
            }
        } else {
            // Value change
            std::string value;
            std::string id;

            if (*p == '0' || *p == '1' || *p == 'x' || *p == 'X' || *p == 'z' || *p == 'Z') {
                value = *p;
                id = (p + 1);
                size_t end = id.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) id = id.substr(0, end + 1);
            } else if (*p == 'b' || *p == 'B' || *p == 'r' || *p == 'R') {
                const char* space = strchr(p, ' ');
                if (space) {
                    value = std::string(p, space - p);
                    id = space + 1;
                    size_t end = id.find_last_not_of(" \t\r\n");
                    if (end != std::string::npos) id = id.substr(0, end + 1);
                }
            } else {
                continue;
            }

            auto it_trans = id_transitions_.find(id);
            if (it_trans != id_transitions_.end()) {
                bool is_glitch = false;
                if (last_change_time.count(id) && last_change_time[id] == current_time) {
                    is_glitch = true;
                    if (!it_trans->second.empty() && it_trans->second.back().timestamp == current_time) {
                        it_trans->second.back().is_glitch = true;
                    }
                }
                it_trans->second.push_back({current_time, value, is_glitch});
                last_change_time[id] = current_time;
            }
        }
    }

    loaded_ = true;
    return true;
}

void VcdAdapter::Close() {
    timescale_.clear();
    signal_info_.clear();
    id_transitions_.clear();
    loaded_ = false;
}

std::string VcdAdapter::GetTimescale() const {
    return timescale_;
}

void VcdAdapter::CollectSignals(
    std::unordered_map<std::string, SignalInfo>& signal_info,
    std::unordered_map<std::string, std::vector<Transition>>& id_transitions) {
    signal_info = signal_info_;
    id_transitions = id_transitions_;
}

bool VcdAdapter::LoadSignalTransitions(
    const SignalInfo& /*info*/,
    std::unordered_map<std::string, std::vector<Transition>>& /*id_transitions*/) {
    // VCD loads all transitions eagerly during Load().
    return true;
}
