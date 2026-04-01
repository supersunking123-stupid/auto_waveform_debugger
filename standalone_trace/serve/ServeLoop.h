#pragma once
// ServeLoop.h — Interactive serve mode interface.
#include <string>
#include <vector>

namespace rtl_trace {

bool SplitCommandLine(const std::string &line, std::vector<std::string> &out, std::string &err);

} // namespace rtl_trace
