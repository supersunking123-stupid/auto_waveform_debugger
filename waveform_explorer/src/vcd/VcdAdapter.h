#pragma once

#include "FormatAdapter.h"
#include <string>

class VcdAdapter : public FormatAdapter {
public:
    VcdAdapter() = default;
    ~VcdAdapter() override = default;

    bool Load(const std::string& path) override;
    void Close() override;
    std::string GetTimescale() const override;
    void CollectSignals(
        std::unordered_map<std::string, SignalInfo>& signal_info,
        std::unordered_map<std::string, std::vector<Transition>>& id_transitions) override;
    bool LoadSignalTransitions(
        const SignalInfo& info,
        std::unordered_map<std::string, std::vector<Transition>>& id_transitions) override;

private:
    static std::string strip_top_prefix(const std::string& path);

    std::string timescale_;
    std::unordered_map<std::string, SignalInfo> signal_info_;
    std::unordered_map<std::string, std::vector<Transition>> id_transitions_;
    bool loaded_ = false;
};
