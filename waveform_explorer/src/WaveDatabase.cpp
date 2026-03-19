#include "WaveDatabase.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include "fstapi.h"

WaveDatabase::WaveDatabase() {}

bool WaveDatabase::load(const std::string& filepath) {
    if (filepath.size() > 4 && filepath.substr(filepath.size() - 4) == ".fst") {
        return load_fst(filepath);
    }
    return load_vcd(filepath);
}

bool WaveDatabase::load_fst(const std::string& filepath) {
    fstReaderContext* ctx = (fstReaderContext*)fstReaderOpen(filepath.c_str());
    if (!ctx) {
        std::cerr << "Failed to open FST file: " << filepath << std::endl;
        return false;
    }

    // Set timescale
    int8_t ts_exp = fstReaderGetTimescale(ctx);
    // Convert exponent to something like "1ps"
    // -12 is ps, -9 is ns, -6 is us, -3 is ms, 0 is s
    if (ts_exp == -12) timescale = "1ps";
    else if (ts_exp == -9) timescale = "1ns";
    else if (ts_exp == -15) timescale = "1fs";
    else timescale = "10^" + std::to_string(ts_exp) + "s";

    std::vector<std::string> scope_stack;
    struct fstHier* h;

    // First pass: Build signal tree
    while ((h = fstReaderIterateHier(ctx))) {
        switch (h->htyp) {
            case FST_HT_SCOPE: {
                std::string scope_name(h->u.scope.name, h->u.scope.name_length);
                scope_stack.push_back(scope_name);
                break;
            }
            case FST_HT_UPSCOPE:
                if (!scope_stack.empty()) scope_stack.pop_back();
                break;
            case FST_HT_VAR: {
                std::string full_name(h->u.var.name, h->u.var.name_length);
                std::string var_name = full_name;
                size_t space_pos = full_name.find(' ');
                if (space_pos != std::string::npos) {
                    var_name = full_name.substr(0, space_pos);
                }

                std::string path = "";
                for (const auto& s : scope_stack) {
                    path += s + ".";
                }
                path += var_name;

                SignalInfo info;
                info.name = var_name;
                info.path = path;
                info.width = h->u.var.length;
                info.type = "wire"; // FST doesn't specify as granularly as VCD here usually
                
                // Use handle as ID
                std::string id = std::to_string(h->u.var.handle);
                info.signal_id = id;

                signal_info[path] = info;
                if (id_transitions.find(id) == id_transitions.end()) {
                    id_transitions[id] = std::vector<Transition>();
                }
                break;
            }
        }
    }

    // Second pass: Read all transitions
    fstReaderSetFacProcessMaskAll(ctx);
    
    // Using a lambda as callback for C function via a static wrapper is tricky, 
    // so we'll use a static/global or pass 'this' via fstReaderClrFacProcessMask
    // Actually fstReaderIterBlocks is better for this.
    
    // Correct way in C++ to handle the callback:
    auto callback = [](void* user_data, uint64_t time, fstHandle handle, const unsigned char* value) {
        WaveDatabase* _this = static_cast<WaveDatabase*>(user_data);
        std::string id = std::to_string(handle);
        std::string val_str((const char*)value); // FST provides it as a string
        
        auto& trans = _this->id_transitions[id];
        bool is_glitch = false;
        if (!trans.empty() && trans.back().timestamp == time) {
            is_glitch = true;
            trans.back().is_glitch = true;
        }
        trans.push_back({time, val_str, is_glitch});
    };

    fstReaderIterBlocks(ctx, callback, this, nullptr);

    fstReaderClose(ctx);
    return true;
}

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
