#pragma once

#include "FormatAdapter.h"
#include <iostream>
#include <string>

// Placeholder adapter for Cadence-specific waveform formats.
// To be implemented when Cadence format support is needed.
class CadenceAdapter : public FormatAdapter {
public:
    CadenceAdapter() = default;
    ~CadenceAdapter() override = default;

    bool Load(const std::string& /*path*/) override {
        std::cerr << "Cadence waveform format is not yet supported." << std::endl;
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
