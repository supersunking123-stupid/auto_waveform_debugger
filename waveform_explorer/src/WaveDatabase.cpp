#include "WaveDatabase.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>

WaveDatabase::WaveDatabase() {}

bool WaveDatabase::load_vcd(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Failed to open VCD file: " << filepath << std::endl;
        return false;
    }

    std::string line;
    std::vector<std::string> current_scope;
    uint64_t current_time = 0;
    
    // To track glitches (multiple changes at the same time)
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
                
                signal_info[path] = info;
                if (id_transitions.find(id) == id_transitions.end()) {
                    id_transitions[id] = std::vector<Transition>();
                }
            }
        } else if (*p == '#') {
            current_time = std::stoull(p + 1);
        } else {
            // Value change
            std::string value;
            std::string id;
            
            if (*p == '0' || *p == '1' || *p == 'x' || *p == 'X' || *p == 'z' || *p == 'Z') {
                value = *p;
                id = (p + 1);
                // remove trailing whitespace from id
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

            auto it_trans = id_transitions.find(id);
            if (it_trans != id_transitions.end()) {
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
    
    return true;
}

bool WaveDatabase::has_signal(const std::string& path) const {
    return signal_info.find(path) != signal_info.end();
}

const SignalInfo& WaveDatabase::get_signal_info(const std::string& path) const {
    return signal_info.at(path);
}

const std::vector<Transition>& WaveDatabase::get_transitions(const std::string& path) const {
    if (!has_signal(path)) {
        static const std::vector<Transition> empty;
        return empty;
    }
    const std::string& id = signal_info.at(path).signal_id;
    return id_transitions.at(id);
}

std::string WaveDatabase::get_value_at_time(const std::string& path, uint64_t time) const {
    if (!has_signal(path)) return "U"; 
    
    const std::string& id = signal_info.at(path).signal_id;
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
