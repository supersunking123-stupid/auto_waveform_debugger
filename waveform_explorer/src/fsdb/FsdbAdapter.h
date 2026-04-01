#pragma once

#include "FormatAdapter.h"
#include <string>
#include <unordered_set>

#ifdef WAVE_HAS_FSDB
class ffrObject;
#endif

class FsdbAdapter : public FormatAdapter {
public:
    FsdbAdapter() = default;
    ~FsdbAdapter() override;

    // Non-copyable
    FsdbAdapter(const FsdbAdapter&) = delete;
    FsdbAdapter& operator=(const FsdbAdapter&) = delete;

    bool Load(const std::string& path) override;
    void Close() override;
    std::string GetTimescale() const override;
    void CollectSignals(
        std::unordered_map<std::string, SignalInfo>& signal_info,
        std::unordered_map<std::string, std::vector<Transition>>& id_transitions) override;
    bool LoadSignalTransitions(
        const SignalInfo& info,
        std::unordered_map<std::string, std::vector<Transition>>& id_transitions) override;

    // Returns true if the FSDB file was loaded successfully.
    bool IsLoaded() const { return loaded_; }

private:
    static std::string strip_top_prefix(const std::string& path);

#ifdef WAVE_HAS_FSDB
    bool load_fsdb_signal(const SignalInfo& info,
                          std::unordered_map<std::string, std::vector<Transition>>& id_transitions);
#endif

    std::string timescale_;
    std::unordered_map<std::string, SignalInfo> signal_info_;
    std::unordered_set<std::string> loaded_signal_ids_;
    bool loaded_ = false;

#ifdef WAVE_HAS_FSDB
    ffrObject* fsdb_obj_ = nullptr;
#endif
};
