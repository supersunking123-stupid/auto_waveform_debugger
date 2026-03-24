#include "WaveDatabase.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <cctype>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include "fstapi.h"

#ifdef WAVE_HAS_FSDB
#ifdef NOVAS_FSDB
#undef NOVAS_FSDB
#endif
#include "ffrAPI.h"
#endif

namespace {

bool ends_with_case_insensitive(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    const size_t start = s.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(s[start + i]);
        const unsigned char b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

std::string strip_top_prefix(const std::string& path) {
    static const std::string kTop = "TOP.";
    if (path.rfind(kTop, 0) == 0) return path.substr(kTop.size());
    return path;
}

std::string strip_bit_suffix(const std::string& path) {
    const size_t bracket = path.find('[');
    if (bracket == std::string::npos) return path;
    return path.substr(0, bracket);
}

#ifdef WAVE_HAS_FSDB
uint64_t fsdb_xtag_to_u64(const fsdbXTag& xtag) {
    return (static_cast<uint64_t>(xtag.hltag.H) << 32) | static_cast<uint64_t>(xtag.hltag.L);
}

char decode_logic(byte_T bit) {
    switch (bit) {
        case FSDB_BT_VCD_0: return '0';
        case FSDB_BT_VCD_1: return '1';
        case FSDB_BT_VCD_X: return 'x';
        case FSDB_BT_VCD_Z: return 'z';
        default: return '?';
    }
}

std::string fsdb_var_type_to_string(byte_T t) {
    switch (t) {
        case FSDB_VT_VCD_WIRE: return "wire";
        case FSDB_VT_VCD_REG: return "reg";
        case FSDB_VT_VCD_REAL: return "real";
        case FSDB_VT_VCD_INTEGER: return "integer";
        case FSDB_VT_VCD_PARAMETER: return "parameter";
        case FSDB_VT_VHDL_SIGNAL: return "signal";
        case FSDB_VT_VHDL_VARIABLE: return "variable";
        case FSDB_VT_VHDL_CONSTANT: return "constant";
        default: return "type_" + std::to_string(static_cast<unsigned int>(t));
    }
}

std::string decode_fsdb_value(ffrVCTrvsHdl hdl, byte_T* vc_ptr) {
    if (vc_ptr == nullptr) return "U";

    const fsdbBytesPerBit bpb = hdl->ffrGetBytesPerBit();
    const uint_T bit_size = hdl->ffrGetBitSize();
    const uint_T byte_count = hdl->ffrGetByteCount();

    if (bpb == FSDB_BYTES_PER_BIT_1B) {
        if (bit_size == 1) {
            return std::string(1, decode_logic(vc_ptr[0]));
        }
        std::string out = "b";
        out.reserve(static_cast<size_t>(bit_size) + 1);
        for (uint_T i = 0; i < bit_size; ++i) {
            out.push_back(decode_logic(vc_ptr[i]));
        }
        return out;
    }

    // Fallback for non-logic encodings (real/string/etc.): keep deterministic hex bytes.
    std::ostringstream oss;
    oss << "h";
    oss << std::hex << std::setfill('0');
    for (uint_T i = 0; i < byte_count; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(vc_ptr[i]);
    }
    return oss.str();
}

struct FSDBTreeContext {
    std::unordered_map<std::string, SignalInfo>* signal_info = nullptr;
    std::vector<std::string> scope_stack;
    bool normalize_top_prefix = true;
};

class ScopedStdIOSilencer {
public:
    ScopedStdIOSilencer() {
        null_fd_ = open("/dev/null", O_WRONLY);
        if (null_fd_ < 0) return;
        saved_stdout_ = dup(STDOUT_FILENO);
        saved_stderr_ = dup(STDERR_FILENO);
        if (saved_stdout_ >= 0) (void)dup2(null_fd_, STDOUT_FILENO);
        if (saved_stderr_ >= 0) (void)dup2(null_fd_, STDERR_FILENO);
    }

    ~ScopedStdIOSilencer() {
        if (saved_stdout_ >= 0) {
            (void)dup2(saved_stdout_, STDOUT_FILENO);
            (void)close(saved_stdout_);
        }
        if (saved_stderr_ >= 0) {
            (void)dup2(saved_stderr_, STDERR_FILENO);
            (void)close(saved_stderr_);
        }
        if (null_fd_ >= 0) (void)close(null_fd_);
    }

private:
    int null_fd_ = -1;
    int saved_stdout_ = -1;
    int saved_stderr_ = -1;
};

bool_T fsdb_tree_cb(fsdbTreeCBType cb_type, void* client_data, void* tree_cb_data) {
    FSDBTreeContext* ctx = static_cast<FSDBTreeContext*>(client_data);
    if (!ctx || !ctx->signal_info) return static_cast<bool_T>(0);

    switch (cb_type) {
        case FSDB_TREE_CBT_SCOPE: {
            auto* scope = static_cast<fsdbTreeCBDataScope*>(tree_cb_data);
            if (scope && scope->name) ctx->scope_stack.emplace_back(scope->name);
            break;
        }
        case FSDB_TREE_CBT_UPSCOPE:
            if (!ctx->scope_stack.empty()) ctx->scope_stack.pop_back();
            break;
        case FSDB_TREE_CBT_VAR: {
            auto* var = static_cast<fsdbTreeCBDataVar*>(tree_cb_data);
            if (!var || !var->name) break;

            std::string path;
            for (const auto& s : ctx->scope_stack) {
                path += s;
                path.push_back('.');
            }
            path += var->name;
            if (ctx->normalize_top_prefix) path = strip_top_prefix(path);

            SignalInfo info;
            info.name = var->name;
            info.path = path;
            info.width = static_cast<uint32_t>(
                (var->lbitnum >= var->rbitnum) ? (var->lbitnum - var->rbitnum + 1) : (var->rbitnum - var->lbitnum + 1));
            if (info.width == 0) info.width = 1;
            info.type = fsdb_var_type_to_string(var->type);
            info.signal_id = std::to_string(static_cast<long long>(var->u.idcode));

            (*ctx->signal_info)[path] = info;
            break;
        }
        default:
            break;
    }
    return static_cast<bool_T>(1);
}
#endif

} // namespace

WaveDatabase::WaveDatabase() {}

WaveDatabase::~WaveDatabase() {
    clear();
}

void WaveDatabase::clear() {
#ifdef WAVE_HAS_FSDB
    if (fsdb_obj != nullptr) {
        ScopedStdIOSilencer silencer;
        fsdb_obj->ffrClose();
        fsdb_obj = nullptr;
    }
    fsdb_loaded_signal_ids.clear();
#endif
    backend_kind = BackendKind::None;
    timescale.clear();
    signal_info.clear();
    id_transitions.clear();
    sorted_signal_paths_cache.clear();
    sorted_signal_paths_valid = false;
    base_signal_path_cache.clear();
    ambiguous_base_signal_paths.clear();
}

bool WaveDatabase::load(const std::string& filepath) {
    if (ends_with_case_insensitive(filepath, ".fst")) {
        return load_fst(filepath);
    }
    if (ends_with_case_insensitive(filepath, ".fsdb")) {
        return load_fsdb(filepath);
    }
    return load_vcd(filepath);
}

bool WaveDatabase::load_fst(const std::string& filepath) {
    clear();
    backend_kind = BackendKind::Fst;
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
                path = normalize_loaded_path(path);

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
    rebuild_base_signal_path_cache();
    return true;
}

bool WaveDatabase::load_fsdb(const std::string& filepath) {
    clear();
#ifndef WAVE_HAS_FSDB
    std::cerr << "FSDB support is not compiled in. Rebuild with Verdi FsdbReader SDK enabled." << std::endl;
    (void)filepath;
    return false;
#else
    bool is_fsdb = false;
    {
        ScopedStdIOSilencer silencer;
        is_fsdb = ffrObject::ffrIsFSDB(const_cast<char*>(filepath.c_str()));
    }
    if (!is_fsdb) {
        std::cerr << "Input is not an FSDB file: " << filepath << std::endl;
        return false;
    }

    std::string error_message;
    bool ok = true;

    {
        ScopedStdIOSilencer silencer;

        fsdb_obj = ffrObject::ffrOpen3(const_cast<char*>(filepath.c_str()));
        if (!fsdb_obj) {
            ok = false;
            error_message = "Failed to open FSDB file: " + filepath;
        } else {
            const char* scale = fsdb_obj->ffrGetScaleUnit();
            timescale = (scale && *scale) ? scale : "1";

            fsdbTag64 min_tag64{};
            fsdbTag64 max_tag64{};
            if (fsdb_obj->ffrGetMinFsdbTag64(&min_tag64) == FSDB_RC_SUCCESS &&
                fsdb_obj->ffrGetMaxFsdbTag64(&max_tag64) == FSDB_RC_SUCCESS) {
                fsdbXTag start_xtag{};
                fsdbXTag close_xtag{};
                start_xtag.hltag.H = min_tag64.H;
                start_xtag.hltag.L = min_tag64.L;
                close_xtag.hltag.H = max_tag64.H;
                close_xtag.hltag.L = max_tag64.L;
                (void)fsdb_obj->ffrSetViewWindow(&start_xtag, &close_xtag);
            }

            FSDBTreeContext tree_ctx;
            tree_ctx.signal_info = &signal_info;

            if (fsdb_obj->ffrSetTreeCBFunc(fsdb_tree_cb, &tree_ctx) != FSDB_RC_SUCCESS) {
                ok = false;
                error_message = "Failed to set FSDB tree callback.";
            } else if (fsdb_obj->ffrReadScopeVarTree() != FSDB_RC_SUCCESS) {
                ok = false;
                error_message = "Failed to read FSDB scope/var tree.";
            }
        }
    }

    if (!ok) {
        clear();
        std::cerr << error_message << std::endl;
        return false;
    }

    backend_kind = BackendKind::Fsdb;
    rebuild_base_signal_path_cache();
    return true;
#endif
}

bool WaveDatabase::load_vcd(const std::string& filepath) {
    clear();
    backend_kind = BackendKind::Vcd;
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
                path = normalize_loaded_path(path);
                
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
    
    rebuild_base_signal_path_cache();
    return true;
}

void WaveDatabase::rebuild_base_signal_path_cache() {
    base_signal_path_cache.clear();
    ambiguous_base_signal_paths.clear();

    for (const auto& entry : signal_info) {
        const std::string& path = entry.first;
        const std::string base_path = strip_bit_suffix(path);
        if (base_path == path) continue;
        if (ambiguous_base_signal_paths.find(base_path) != ambiguous_base_signal_paths.end()) continue;

        const auto it = base_signal_path_cache.find(base_path);
        if (it == base_signal_path_cache.end()) {
            base_signal_path_cache.emplace(base_path, path);
            continue;
        }
        if (it->second != path) {
            base_signal_path_cache.erase(it);
            ambiguous_base_signal_paths.insert(base_path);
        }
    }
}

bool WaveDatabase::has_signal(const std::string& path) const {
    return signal_info.find(resolve_query_path(path)) != signal_info.end();
}

const SignalInfo& WaveDatabase::get_signal_info(const std::string& path) const {
    return signal_info.at(resolve_query_path(path));
}

std::vector<std::string> WaveDatabase::list_signal_paths_page(
    const std::string& prefix,
    const std::string& cursor,
    size_t limit,
    bool& has_more,
    std::string& next_cursor) const {
    if (!sorted_signal_paths_valid) {
        sorted_signal_paths_cache.clear();
        sorted_signal_paths_cache.reserve(signal_info.size());
        for (const auto& pair : signal_info) {
            sorted_signal_paths_cache.push_back(pair.first);
        }
        std::sort(sorted_signal_paths_cache.begin(), sorted_signal_paths_cache.end());
        sorted_signal_paths_valid = true;
    }

    if (limit == 0) limit = 1;

    auto matches_prefix = [&](const std::string& path) {
        return prefix.empty() || path.rfind(prefix, 0) == 0;
    };

    auto begin_it = prefix.empty()
        ? sorted_signal_paths_cache.begin()
        : std::lower_bound(sorted_signal_paths_cache.begin(), sorted_signal_paths_cache.end(), prefix);

    if (!cursor.empty()) {
        auto cursor_it = std::upper_bound(sorted_signal_paths_cache.begin(), sorted_signal_paths_cache.end(), cursor);
        if (cursor_it > begin_it) begin_it = cursor_it;
    }

    std::vector<std::string> page;
    page.reserve(limit);
    auto it = begin_it;
    while (it != sorted_signal_paths_cache.end() && page.size() < limit) {
        if (!matches_prefix(*it)) {
            if (!prefix.empty() && *it > prefix) break;
            ++it;
            continue;
        }
        page.push_back(*it);
        ++it;
    }

    has_more = false;
    next_cursor.clear();
    while (it != sorted_signal_paths_cache.end()) {
        if (!matches_prefix(*it)) {
            if (!prefix.empty() && *it > prefix) break;
            ++it;
            continue;
        }
        has_more = true;
        break;
    }
    if (has_more && !page.empty()) next_cursor = page.back();
    return page;
}

const std::vector<Transition>& WaveDatabase::get_transitions(const std::string& path) const {
    const std::string resolved = resolve_query_path(path);
    if (signal_info.find(resolved) == signal_info.end()) {
        static const std::vector<Transition> empty;
        return empty;
    }
    if (!ensure_signal_transitions_loaded(resolved)) {
        static const std::vector<Transition> empty;
        return empty;
    }
    const std::string& id = signal_info.at(resolved).signal_id;
    return id_transitions.at(id);
}

std::string WaveDatabase::get_value_at_time(const std::string& path, uint64_t time) const {
    const std::string resolved = resolve_query_path(path);
    if (signal_info.find(resolved) == signal_info.end()) return "U";
    if (!ensure_signal_transitions_loaded(resolved)) return "U";

    const std::string& id = signal_info.at(resolved).signal_id;
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

std::string WaveDatabase::normalize_loaded_path(const std::string& path) const {
    return strip_top_prefix(path);
}

std::string WaveDatabase::resolve_query_path(const std::string& path) const {
    if (signal_info.find(path) != signal_info.end()) return path;

    const std::string no_top = strip_top_prefix(path);
    if (signal_info.find(no_top) != signal_info.end()) return no_top;

    const std::string with_top = "TOP." + path;
    if (signal_info.find(with_top) != signal_info.end()) return with_top;

    if (path.find('[') == std::string::npos) {
        const auto base_it = base_signal_path_cache.find(no_top);
        if (base_it != base_signal_path_cache.end()) return base_it->second;
    }

    return path;
}

bool WaveDatabase::ensure_signal_transitions_loaded(const std::string& resolved_path) const {
    const auto it = signal_info.find(resolved_path);
    if (it == signal_info.end()) return false;

    const std::string& signal_id = it->second.signal_id;
    if (id_transitions.find(signal_id) != id_transitions.end()) return true;

    if (backend_kind != BackendKind::Fsdb) return false;

#ifdef WAVE_HAS_FSDB
    return ensure_fsdb_signal_loaded(it->second);
#else
    return false;
#endif
}

#ifdef WAVE_HAS_FSDB
bool WaveDatabase::ensure_fsdb_signal_loaded(const SignalInfo& info) const {
    if (fsdb_obj == nullptr) return false;
    if (fsdb_loaded_signal_ids.find(info.signal_id) != fsdb_loaded_signal_ids.end()) return true;

    const fsdbVarIdcode idcode = static_cast<fsdbVarIdcode>(std::stoll(info.signal_id));
    std::vector<Transition> transitions;
    bool ok = true;

    {
        ScopedStdIOSilencer silencer;

        if (fsdb_obj->ffrResetSignalList() != FSDB_RC_SUCCESS) {
            ok = false;
        } else if (fsdb_obj->ffrAddToSignalList(idcode) != FSDB_RC_SUCCESS) {
            ok = false;
        } else if (fsdb_obj->ffrLoadSignals() != FSDB_RC_SUCCESS) {
            ok = false;
        } else {
            ffrVCTrvsHdl hdl = fsdb_obj->ffrCreateVCTraverseHandle(idcode);
            if (hdl == nullptr) {
                ok = false;
            } else {
                bool loaded_any = false;
                if (hdl->ffrGotoTheFirstVC() == FSDB_RC_SUCCESS) {
                    while (true) {
                        fsdbXTag cur_xtag;
                        byte_T* vc_ptr = nullptr;
                        if (hdl->ffrGetXTag(&cur_xtag) != FSDB_RC_SUCCESS ||
                            hdl->ffrGetVC(&vc_ptr) != FSDB_RC_SUCCESS) {
                            break;
                        }

                        const uint64_t t = fsdb_xtag_to_u64(cur_xtag);
                        bool is_glitch = false;
                        if (!transitions.empty() && transitions.back().timestamp == t) {
                            is_glitch = true;
                            transitions.back().is_glitch = true;
                        }
                        transitions.push_back({t, decode_fsdb_value(hdl, vc_ptr), is_glitch});
                        loaded_any = true;

                        if (hdl->ffrGotoNextVC() != FSDB_RC_SUCCESS) break;
                    }
                }

                // Some FSDB traces do not advertise incore VC, but direct traversal still works.
                // Keep the old min-xtag walk as a compatibility fallback if the first-vc path is empty.
                if (!loaded_any) {
                    fsdbXTag xtag;
                    if (hdl->ffrGetMinXTag(&xtag) == FSDB_RC_SUCCESS &&
                        hdl->ffrGotoXTag(&xtag) == FSDB_RC_SUCCESS) {
                        while (true) {
                            fsdbXTag cur_xtag;
                            byte_T* vc_ptr = nullptr;
                            if (hdl->ffrGetXTag(&cur_xtag) != FSDB_RC_SUCCESS ||
                                hdl->ffrGetVC(&vc_ptr) != FSDB_RC_SUCCESS) {
                                break;
                            }

                            const uint64_t t = fsdb_xtag_to_u64(cur_xtag);
                            bool is_glitch = false;
                            if (!transitions.empty() && transitions.back().timestamp == t) {
                                is_glitch = true;
                                transitions.back().is_glitch = true;
                            }
                            transitions.push_back({t, decode_fsdb_value(hdl, vc_ptr), is_glitch});

                            if (hdl->ffrGotoNextVC() != FSDB_RC_SUCCESS) break;
                        }
                    }
                }
                hdl->ffrFree();
            }

            (void)fsdb_obj->ffrUnloadSignals(idcode);
            (void)fsdb_obj->ffrResetSignalList();
        }
    }

    if (!ok) return false;

    id_transitions.emplace(info.signal_id, std::move(transitions));
    fsdb_loaded_signal_ids.insert(info.signal_id);
    return true;
}
#endif
