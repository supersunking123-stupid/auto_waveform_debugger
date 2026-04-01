#pragma once

#include "FormatAdapter.h"
#include <memory>
#include <string>

// Factory + dispatcher: maps file extensions to FormatAdapter instances.
class FormatRegistry {
public:
    // Create the adapter appropriate for the given file path (based on
    // extension).  Returns nullptr if the extension is not recognised.
    static std::unique_ptr<FormatAdapter> Create(const std::string& filepath);
};
