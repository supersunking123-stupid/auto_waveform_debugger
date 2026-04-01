#include "fsdb/FsdbAdapter.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>

#ifdef WAVE_HAS_FSDB
#ifdef NOVAS_FSDB
#undef NOVAS_FSDB
#endif
#include "ffrAPI.h"
#endif

namespace {

std::string strip_top_prefix_impl(const std::string& path) {
    static const std::string kTop = "TOP.";
    if (path.rfind(kTop, 0) == 0) return path.substr(kTop.size());
    return path;
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

    // Fallback for non-logic encodings (real/string/etc.)
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
            if (ctx->normalize_top_prefix) path = strip_top_prefix_impl(path);

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

#endif // WAVE_HAS_FSDB

} // namespace

std::string FsdbAdapter::strip_top_prefix(const std::string& path) {
    return strip_top_prefix_impl(path);
}

FsdbAdapter::~FsdbAdapter() {
    Close();
}

bool FsdbAdapter::Load(const std::string& filepath) {
    Close();
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

        fsdb_obj_ = ffrObject::ffrOpen3(const_cast<char*>(filepath.c_str()));
        if (!fsdb_obj_) {
            ok = false;
            error_message = "Failed to open FSDB file: " + filepath;
        } else {
            const char* scale = fsdb_obj_->ffrGetScaleUnit();
            timescale_ = (scale && *scale) ? scale : "1";

            fsdbTag64 min_tag64{};
            fsdbTag64 max_tag64{};
            if (fsdb_obj_->ffrGetMinFsdbTag64(&min_tag64) == FSDB_RC_SUCCESS &&
                fsdb_obj_->ffrGetMaxFsdbTag64(&max_tag64) == FSDB_RC_SUCCESS) {
                fsdbXTag start_xtag{};
                fsdbXTag close_xtag{};
                start_xtag.hltag.H = min_tag64.H;
                start_xtag.hltag.L = min_tag64.L;
                close_xtag.hltag.H = max_tag64.H;
                close_xtag.hltag.L = max_tag64.L;
                (void)fsdb_obj_->ffrSetViewWindow(&start_xtag, &close_xtag);
            }

            FSDBTreeContext tree_ctx;
            tree_ctx.signal_info = &signal_info_;

            if (fsdb_obj_->ffrSetTreeCBFunc(fsdb_tree_cb, &tree_ctx) != FSDB_RC_SUCCESS) {
                ok = false;
                error_message = "Failed to set FSDB tree callback.";
            } else if (fsdb_obj_->ffrReadScopeVarTree() != FSDB_RC_SUCCESS) {
                ok = false;
                error_message = "Failed to read FSDB scope/var tree.";
            }
        }
    }

    if (!ok) {
        Close();
        std::cerr << error_message << std::endl;
        return false;
    }

    loaded_ = true;
    return true;
#endif
}

void FsdbAdapter::Close() {
#ifdef WAVE_HAS_FSDB
    if (fsdb_obj_ != nullptr) {
        ScopedStdIOSilencer silencer;
        fsdb_obj_->ffrClose();
        fsdb_obj_ = nullptr;
    }
    loaded_signal_ids_.clear();
#endif
    timescale_.clear();
    signal_info_.clear();
    loaded_ = false;
}

std::string FsdbAdapter::GetTimescale() const {
    return timescale_;
}

void FsdbAdapter::CollectSignals(
    std::unordered_map<std::string, SignalInfo>& signal_info,
    std::unordered_map<std::string, std::vector<Transition>>& /*id_transitions*/) {
    signal_info = signal_info_;
    // FSDB does not eagerly load transitions.
}

bool FsdbAdapter::LoadSignalTransitions(
    const SignalInfo& info,
    std::unordered_map<std::string, std::vector<Transition>>& id_transitions) {
#ifndef WAVE_HAS_FSDB
    (void)info;
    (void)id_transitions;
    return false;
#else
    if (loaded_signal_ids_.find(info.signal_id) != loaded_signal_ids_.end()) return true;
    return load_fsdb_signal(info, id_transitions);
#endif
}

#ifdef WAVE_HAS_FSDB
bool FsdbAdapter::load_fsdb_signal(const SignalInfo& info,
                                    std::unordered_map<std::string, std::vector<Transition>>& id_transitions) {
    if (fsdb_obj_ == nullptr) return false;
    if (loaded_signal_ids_.find(info.signal_id) != loaded_signal_ids_.end()) return true;

    fsdbVarIdcode idcode = 0;
    try {
        idcode = static_cast<fsdbVarIdcode>(std::stoll(info.signal_id));
    } catch (const std::exception&) {
        std::cerr << "Invalid FSDB signal id: " << info.signal_id << std::endl;
        return false;
    }
    std::vector<Transition> transitions;
    bool ok = true;

    {
        ScopedStdIOSilencer silencer;

        if (fsdb_obj_->ffrResetSignalList() != FSDB_RC_SUCCESS) {
            ok = false;
        } else if (fsdb_obj_->ffrAddToSignalList(idcode) != FSDB_RC_SUCCESS) {
            ok = false;
        } else if (fsdb_obj_->ffrLoadSignals() != FSDB_RC_SUCCESS) {
            ok = false;
        } else {
            ffrVCTrvsHdl hdl = fsdb_obj_->ffrCreateVCTraverseHandle(idcode);
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

                // Compatibility fallback for FSDB traces without incore VC.
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

            (void)fsdb_obj_->ffrUnloadSignals(idcode);
            (void)fsdb_obj_->ffrResetSignalList();
        }
    }

    if (!ok) return false;

    id_transitions.emplace(info.signal_id, std::move(transitions));
    loaded_signal_ids_.insert(info.signal_id);
    return true;
}
#endif
