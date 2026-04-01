#pragma once

#include "FormatAdapter.h"
#include <iostream>
#include <string>

// Placeholder adapter for Siemens/Mentor-specific waveform formats.
// To be implemented when Siemens format support is needed.
class SiemensAdapter : public FormatAdapter {
public:
    SiemensAdapter() = default;
    ~SiemensAdapter() override = default;

    bool Load(const std::string& /*path*/) override {
        std::cerr << "Siemens waveform format is not yet supported." << std::endl;
        return false;
    }

    void Close() override {}

    std::string GetTimescale() const override { return ""; }

    void CollectSignals(
        std::unordered_map<std::string, SignalInfo>& /*signal_info*/,
        std::unordered_map<std::string, std::vector<Transition>>& /*id_transitions*/) override {}

    bool LoadSignalTransitions(
        const SignalInfo& /*info*/,
        std::unordered_map<std::string, std::vector<Transition>>& /*id_transitions*/) override {
        return false;
    }
};
