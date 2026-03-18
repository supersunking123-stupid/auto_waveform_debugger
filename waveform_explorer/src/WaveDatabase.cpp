#include "WaveDatabase.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

WaveDatabase::WaveDatabase() {}

bool WaveDatabase::load_vcd(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open VCD file: " << filepath << std::endl;
        return false;
    }

    std::string line;
    std::vector<std::string> current_scope;
    std::unordered_map<std::string, std::vector<std::string>> id_to_paths;
    uint64_t current_time = 0;
    
    // To track glitches (multiple changes at the same time)
    std::unordered_map<std::string, uint64_t> last_change_time;

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        // Strip leading whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line[0] == '$') {
            std::istringstream iss(line);
            std::string token;
            iss >> token;

            if (token == "$timescale") {
                iss >> timescale;
                if (timescale == "1") {
                    std::string unit;
                    iss >> unit;
                    timescale += unit;
                }
            } else if (token == "$scope") {
                std::string type, name;
                iss >> type >> name;
                current_scope.push_back(name);
            } else if (token == "$upscope") {
                if (!current_scope.empty()) {
                    current_scope.pop_back();
                }
            } else if (token == "$var") {
                std::string type, width_str, id, name;
                iss >> type >> width_str >> id >> name;
                
                std::string path = "";
                for (size_t i = 0; i < current_scope.size(); ++i) {
                    path += current_scope[i];
                    if (i < current_scope.size() - 1 || !name.empty()) path += ".";
                }
                path += name;
                
                id_to_paths[id].push_back(path);
                
                SignalInfo info;
                info.name = name;
                info.path = path;
                try {
                    info.width = std::stoul(width_str);
                } catch(...) {
                    info.width = 1;
                }
                info.type = type;
                
                signal_info[path] = info;
                transitions[path] = std::vector<Transition>();
            } else if (token == "$enddefinitions") {
                // Done parsing header
            }
        } else if (line[0] == '#') {
            try {
                current_time = std::stoull(line.substr(1));
            } catch(...) {
                std::cerr << "Error parsing time: " << line << std::endl;
            }
        } else {
            // Value change
            std::string value;
            std::string id;
            
            if (line[0] == '0' || line[0] == '1' || line[0] == 'x' || line[0] == 'X' || line[0] == 'z' || line[0] == 'Z') {
                value = line.substr(0, 1);
                id = line.substr(1);
                // remove trailing whitespace from id
                size_t end = id.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) id = id.substr(0, end + 1);
            } else if (line[0] == 'b' || line[0] == 'B' || line[0] == 'r' || line[0] == 'R') {
                std::istringstream iss(line);
                iss >> value >> id;
            } else {
                continue; // ignore unhandled or malformed lines
            }

            if (id_to_paths.find(id) != id_to_paths.end()) {
                for (const auto& path : id_to_paths[id]) {
                    bool is_glitch = false;
                    
                    if (last_change_time.find(path) != last_change_time.end() && last_change_time[path] == current_time) {
                        is_glitch = true;
                        // Mark the previous one at this time as a glitch too if it wasn't
                        if (!transitions[path].empty() && transitions[path].back().timestamp == current_time) {
                            transitions[path].back().is_glitch = true;
                        }
                    }
                    
                    transitions[path].push_back({current_time, value, is_glitch});
                    last_change_time[path] = current_time;
                }
            }
        }
    }
    
    return true;
}

bool WaveDatabase::has_signal(const std::string& path) const {
    return signal_info.find(path) != signal_info.end();
}

const SignalInfo& WaveDatabase::get_signal_info(const std::string& path) const {
    return signal_info.at(path);
}

const std::vector<Transition>& WaveDatabase::get_transitions(const std::string& path) const {
    return transitions.at(path);
}

std::string WaveDatabase::get_value_at_time(const std::string& path, uint64_t time) const {
    if (!has_signal(path)) return "U"; // Unknown signal
    
    const auto& trans = transitions.at(path);
    if (trans.empty()) return "U";
    if (time < trans.front().timestamp) return "U"; // Before first transition
    
    // Binary search for the last transition <= time
    auto it = std::upper_bound(trans.begin(), trans.end(), time,
        [](uint64_t t, const Transition& tr) {
            return t < tr.timestamp;
        });
        
    // it points to the first transition strictly greater than time.
    // So the previous transition is the one we want.
    if (it == trans.begin()) {
        return "U";
    }
    --it;
    
    // If there's a glitch at this exact time, we want the *last* settled value at this time.
    // upper_bound followed by --it already gives us the last transition <= time, 
    // which in case of identical timestamps, points to the last one inserted (the settled value).
    return it->value;
}
