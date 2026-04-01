#include "fst/FstAdapter.h"

#include <iostream>
#include <vector>
#include "fstapi.h"

namespace {

std::string strip_top_prefix_impl(const std::string& path) {
    static const std::string kTop = "TOP.";
    if (path.rfind(kTop, 0) == 0) return path.substr(kTop.size());
    return path;
}

} // namespace

std::string FstAdapter::strip_top_prefix(const std::string& path) {
    return strip_top_prefix_impl(path);
}

bool FstAdapter::Load(const std::string& filepath) {
    Close();

    fstReaderContext* ctx = (fstReaderContext*)fstReaderOpen(filepath.c_str());
    if (!ctx) {
        std::cerr << "Failed to open FST file: " << filepath << std::endl;
        return false;
    }

    // Set timescale
    int8_t ts_exp = fstReaderGetTimescale(ctx);
    if (ts_exp == -12) timescale_ = "1ps";
    else if (ts_exp == -9) timescale_ = "1ns";
    else if (ts_exp == -15) timescale_ = "1fs";
    else timescale_ = "10^" + std::to_string(ts_exp) + "s";

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
                path = strip_top_prefix(path);

                SignalInfo info;
                info.name = var_name;
                info.path = path;
                info.width = h->u.var.length;
                info.type = "wire";

                std::string id = std::to_string(h->u.var.handle);
                info.signal_id = id;

                signal_info_[path] = info;
                if (id_transitions_.find(id) == id_transitions_.end()) {
                    id_transitions_[id] = std::vector<Transition>();
                }
                break;
            }
        }
    }

    // Second pass: Read all transitions
    fstReaderSetFacProcessMaskAll(ctx);

    // We need a pointer to this adapter's id_transitions_ for the callback.
    // Use a lambda that captures the map pointer.
    auto* trans_map = &id_transitions_;

    auto callback = [](void* user_data, uint64_t time, fstHandle handle, const unsigned char* value) {
        auto* map = static_cast<std::unordered_map<std::string, std::vector<Transition>>*>(user_data);
        std::string id = std::to_string(handle);
        std::string val_str((const char*)value);

        auto& trans = (*map)[id];
        bool is_glitch = false;
        if (!trans.empty() && trans.back().timestamp == time) {
            is_glitch = true;
            trans.back().is_glitch = true;
        }
        trans.push_back({time, val_str, is_glitch});
    };

    fstReaderIterBlocks(ctx, callback, trans_map, nullptr);

    fstReaderClose(ctx);
    loaded_ = true;
    return true;
}

void FstAdapter::Close() {
    timescale_.clear();
    signal_info_.clear();
    id_transitions_.clear();
    loaded_ = false;
}

std::string FstAdapter::GetTimescale() const {
    return timescale_;
}

void FstAdapter::CollectSignals(
    std::unordered_map<std::string, SignalInfo>& signal_info,
    std::unordered_map<std::string, std::vector<Transition>>& id_transitions) {
    signal_info = signal_info_;
    id_transitions = id_transitions_;
}

bool FstAdapter::LoadSignalTransitions(
    const SignalInfo& /*info*/,
    std::unordered_map<std::string, std::vector<Transition>>& /*id_transitions*/) {
    // FST loads all transitions eagerly during Load().
    return true;
}
