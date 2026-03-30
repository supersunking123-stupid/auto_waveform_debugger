#pragma once

#include <string>
#include <string_view>
#include <vector>

std::string TrimWhitespace(std::string_view text);
std::vector<std::string> InferAssignmentLhsPathsFromText(const std::string &endpoint_path,
                                                         std::string_view assignment_text);
